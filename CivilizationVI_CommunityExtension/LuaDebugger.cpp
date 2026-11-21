// SPDX-License-Identifier: AGPL-3.0-only
// Part of Dev CE, a fork of the Civilization VI Community Extension by Wild-W (https://github.com/Wild-W/CivilizationVI_CommunityExtension).
// Copyright (C) 2026 cru121. Licensed under the GNU Affero General Public License v3.0 (see LICENSE.txt). Not endorsed by Wild-W, Firaxis or 2K.
#include <winsock2.h>
#include <ws2tcpip.h>
#include "LuaDebugger.h"
#include "LuaHooks.h"
#include "DevBridge.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <vector>
#include <algorithm>
#include <string>
#include <thread>
#pragma comment(lib, "ws2_32.lib")

// D0 of dev-ce/DESIGN_LUA_DEBUGGER.md. Threads: a server thread (sockets only, never touches a lua_State) and the game thread that stops. A stop blocks the thread that
// executes the Lua state inside a request loop: stack / eval requests run on that thread (the only place where Lua may be touched), continue / step release it.
namespace LuaDebugger {
namespace {

using hks::lua_State;
using LuaHooks::Api;

constexpr int AR_EVENT = 0x00, AR_CURRENTLINE = 0x28, AR_SHORTSRC = 0x40, AR_SIZE = 0x300;
constexpr int EV_LINE = 2;
constexpr int DEFAULT_PORT = 4320;
constexpr int IDLE_RESUME_SECONDS = 120;       // a stopped game thread resumes by itself when the client sends nothing for this long

struct Request { int id; std::string cmd; std::string arg; int level; };

std::mutex g_mu;                               // protects the stop state below
std::condition_variable g_cv;
std::deque<Request> g_reqs;
bool g_paused = false, g_resume = false;
int g_resumeMode = 0;                          // set by continue / step / next / out while stopped: 0 run, 1 step in, 2 step over, 3 step out
lua_State* g_pausedL = nullptr;
std::string g_pausedReason;

// stepping / breakpoints (read by the hook on any game thread, written under the stop or by the server thread)
struct Bp {                                            // file: lower case, '/' separators; matches as a path suffix
	std::string file; int line;
	std::string cond;                                  // Lua expression, evaluated in the hooked frame; the breakpoint only counts when it is true
	std::string log;                                   // Lua expression: when set the breakpoint does not stop but sends an `output` event with the value
	int hit = 0;                                       // stop only from the hit-th time on (0 = every time)
	int hits = 0;
};
std::mutex g_bpMu;
std::vector<Bp> g_bps;
std::atomic<bool> g_hasBps{ false };
std::atomic<bool> g_pauseReq{ false };
std::atomic<int> g_stepMode{ 0 };                     // 0 none, 1 in, 2 over, 3 out
std::atomic<lua_State*> g_stepL{ nullptr };
std::atomic<long long> g_stepDepth{ 0 };
std::atomic<uint64_t> g_lineBits[1024];                 // bit (line & 0xFFFF) set for every breakpoint line: lets the VM skip the hook for all other lines
std::atomic<int> g_armed{ 0 };                         // states that armed themselves (diagnostics)

std::atomic<SOCKET> g_client{ INVALID_SOCKET };
std::mutex g_sendMu;
std::atomic<bool> g_started{ false };
std::atomic<bool> g_demo{ false };                    // marker file line 2 == "demo": the demo script (DevCE_DebugDemo.lua) runs
std::atomic<int> g_port{ DEFAULT_PORT };

// ---- tiny JSON helpers (requests are flat objects with string / integer members) ---------------------------------------------------------------------------
std::string Esc(const std::string& s) {
	std::string o; o.reserve(s.size() + 8);
	for (unsigned char c : s) {
		switch (c) {
		case '"': o += "\\\""; break;
		case '\\': o += "\\\\"; break;
		case '\n': o += "\\n"; break;
		case '\r': o += "\\r"; break;
		case '\t': o += "\\t"; break;
		default:
			if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; } else o += static_cast<char>(c);
		}
	}
	return o;
}

bool GetStr(const std::string& line, const char* key, std::string& out) {
	const std::string k = std::string("\"") + key + "\"";
	size_t p = line.find(k);
	if (p == std::string::npos) return false;
	p = line.find(':', p + k.size());
	if (p == std::string::npos) return false;
	p = line.find('"', p + 1);
	if (p == std::string::npos) return false;
	out.clear();
	for (++p; p < line.size() && line[p] != '"'; ++p) {
		if (line[p] == '\\' && p + 1 < line.size()) {
			const char e = line[++p];
			if (e == 'n') out += '\n'; else if (e == 'r') out += '\r'; else if (e == 't') out += '\t';
			else if (e == 'u' && p + 4 < line.size()) { out += static_cast<char>(std::strtol(line.substr(p + 1, 4).c_str(), nullptr, 16)); p += 4; }
			else out += e;
		} else out += line[p];
	}
	return true;
}

bool GetInt(const std::string& line, const char* key, int& out) {
	const std::string k = std::string("\"") + key + "\"";
	size_t p = line.find(k);
	if (p == std::string::npos) return false;
	p = line.find(':', p + k.size());
	if (p == std::string::npos) return false;
	out = std::atoi(line.c_str() + p + 1);
	return true;
}

void Send(const std::string& s) {
	const SOCKET c = g_client.load();
	if (c == INVALID_SOCKET) return;
	std::lock_guard<std::mutex> g(g_sendMu);
	const std::string t = s + "\n";
	send(c, t.data(), static_cast<int>(t.size()), 0);
}

void Reply(int id, bool ok, const std::string& members) {
	Send("{\"id\":" + std::to_string(id) + ",\"ok\":" + (ok ? "true" : "false") + (members.empty() ? "" : "," + members) + "}");
}

void __cdecl DbgHook(lua_State* L, void* ar);

std::string StatsJson() {
	const LuaHooks::Stats s = LuaHooks::GetStats();
	return ",\"fetchSlow\":" + std::to_string(s.fetchSlow) + ",\"lineDelivered\":" + std::to_string(s.lineDelivered) + ",\"lineFiltered\":" + std::to_string(s.lineFiltered);
}

long long CallDepth(lua_State* L) {                    // callinfo entries (24 bytes): current - base, same rule as LuaHooks
	const uint8_t* b = reinterpret_cast<const uint8_t*>(L);
	return (*reinterpret_cast<const long long*>(b + 0x28) - *reinterpret_cast<const long long*>(b + 0x18)) / 24;
}

std::string NormPath(const char* p, size_t n) {
	std::string o(p, n);
	for (char& c : o) { if (c == '\\') c = '/'; else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
	return o;
}

bool BpMatches(const std::string& src, const Bp& b) {
	if (src.size() < b.file.size()) return false;
	if (src.compare(src.size() - b.file.size(), b.file.size(), b.file) != 0) return false;
	return src.size() == b.file.size() || b.file[0] == '/' || src[src.size() - b.file.size() - 1] == '/';
}

void RebuildLineBits() {                               // call with g_bpMu held
	for (auto& w : g_lineBits) w.store(0, std::memory_order_relaxed);
	for (const Bp& b : g_bps) { const unsigned l = static_cast<unsigned>(b.line) & 0xFFFF; g_lineBits[l >> 6].fetch_or(1ull << (l & 63), std::memory_order_relaxed); }
}

// Arm mode: while breakpoints, a pause request or a step exist, every state that runs Lua gets the line hook ON ITS OWN THREAD (LuaHooks arm handler). The
// server thread never touches another thread's lua_State. Idle hooks remove themselves at their next line event (DbgHook).
void UpdateArm() { LuaHooks::SetArmFlag(g_hasBps.load() || g_pauseReq.load() || g_stepMode.load() != 0); }

void __cdecl ArmSelf(lua_State* L) {
	LuaHooks::SetNativeHook(L, DbgHook, LuaHooks::HOOK_LINE, 0, false);
	g_armed++;
}

void ResetSession() {                                  // client gone: no breakpoints, no stepping; armed hooks remove themselves
	{ std::lock_guard<std::mutex> g(g_bpMu); g_bps.clear(); RebuildLineBits(); }
	g_hasBps = false; g_pauseReq = false; g_stepMode = 0; g_stepL = nullptr;
	UpdateArm();
}

// ---- requests that run on the stopped thread --------------------------------------------------------------------------------------------------------------
std::string DumpStack(lua_State* L, const Api* A) {
	std::string o = "\"frames\":[";
	for (int lvl = 0; lvl < 64; ++lvl) {
		alignas(16) uint8_t ar[AR_SIZE];
		std::memset(ar, 0, sizeof(ar));
		if (!A->getstack(L, lvl, ar)) break;
		A->getinfo(L, "Sl", ar);
		const char* src = reinterpret_cast<const char*>(ar + AR_SHORTSRC);
		const int line = *reinterpret_cast<int*>(ar + AR_CURRENTLINE);
		if (lvl) o += ",";
		o += "{\"level\":" + std::to_string(lvl) + ",\"src\":\"" + Esc(std::string(src, strnlen(src, 0x200))) + "\",\"line\":" + std::to_string(line) + "}";
	}
	return o + "]";
}

std::string LuaError(lua_State* L, const Api* A) {
	size_t len = 0;
	const char* m = A->tolstring(L, -1, &len);
	return m ? std::string(m, len) : std::string("(no message)");
}

// Lua helper chunk run in the stopped state for eval / vars / children / cond. It receives the frame's locals and upvalues as arrays (names, values, counts) and builds an
// chunk prefix `local a, b = ...` (upvalues, then locals; globals stay the real globals; assigning to a frame variable changes the copy only).
// Returns a string: 'O' + text (eval), 'T' / 'F' (cond), 'J' + JSON members (vars, children), 'E' + message.
const char* const HELPER_SRC = R"LUA(
local mode, expr, lnames, lvals, unames, uvals, nl, nu = ...
local function esc(s)
  s = tostring(s)
  s = s:gsub('[\\"]', '\\%0')
  s = s:gsub('[^ -~]', function(c) return string.format('\\u%04x', string.byte(c)) end)
  return s
end
local function short(v)
  local t = type(v)
  local s
  if t == 'string' then s = '"' .. v .. '"' else s = tostring(v) end
  if #s > 120 then s = s:sub(1, 117) .. '...' end
  return s
end
local function entry(k, v, path)
  local t = type(v)
  local e = '{"name":"' .. esc(k) .. '","type":"' .. t .. '","value":"' .. esc(short(v)) .. '"'
  if t == 'table' then e = e .. ',"expandable":true' end
  if path then e = e .. ',"path":"' .. esc(path) .. '"' end
  return e .. '}'
end
-- No setfenv in this Lua: the frame's variables become real locals of the compiled chunk (`local a, b = ...`), called with their values. Upvalues first, locals override.
local map, order = {}, {}
local function add(n, v)
  if type(n) == 'string' and n:match('^[%a_][%w_]*$') and #order < 180 then
    if map[n] == nil and not order[n] then order[#order + 1] = n; order[n] = true end
    map[n] = v
  end
end
for i = 1, nu do add(unames[i], uvals[i]) end
for i = 1, nl do add(lnames[i], lvals[i]) end
local args = {}
for i = 1, #order do args[i] = map[order[i]] end
local nargs = #order
local function compile(code)
  local pre = ''
  if nargs > 0 then pre = 'local ' .. table.concat(order, ',') .. ' = ...; ' end
  return loadstring(pre .. code)
end
if mode == 'vars' then
  local a, b = {}, {}
  for i = 1, nl do a[#a + 1] = entry(lnames[i], lvals[i], lnames[i]) end
  for i = 1, nu do b[#b + 1] = entry(unames[i], uvals[i], unames[i]) end
  return 'J"locals":[' .. table.concat(a, ',') .. '],"upvalues":[' .. table.concat(b, ',') .. ']'
end
local f, err = compile('return ' .. expr)
if not f and mode ~= 'cond' then f, err = compile(expr) end
if not f then return 'E' .. tostring(err) end
local ok, v = pcall(f, unpack(args, 1, nargs))
if not ok then return 'E' .. tostring(v) end
if mode == 'cond' then return v and 'T' or 'F' end
if mode == 'eval' then
  local s = tostring(v)
  if #s > 4000 then s = s:sub(1, 4000) .. '...' end
  return 'O' .. s
end
if type(v) ~= 'table' then return 'J"children":[],"count":0,"truncated":false' end
local keys, n, trunc = {}, 0, false
for k in pairs(v) do
  n = n + 1
  if n > 200 then trunc = true break end
  keys[n] = k
end
table.sort(keys, function(x, y)
  local tx, ty = type(x), type(y)
  if tx ~= ty then return tx < ty end
  if tx == 'number' or tx == 'string' then return x < y end
  return tostring(x) < tostring(y)
end)
local parts = {}
for i = 1, #keys do
  local k = keys[i]
  local val = v[k]
  local path
  if type(val) == 'table' then
    if type(k) == 'string' then path = '(' .. expr .. ')[' .. string.format('%q', k) .. ']'
    elseif type(k) == 'number' then path = '(' .. expr .. ')[' .. tostring(k) .. ']' end
  end
  parts[#parts + 1] = entry(type(k) == 'string' and k or ('[' .. tostring(k) .. ']'), val, path)
end
local mt = getmetatable(v)
if type(mt) == 'table' then parts[#parts + 1] = entry('<metatable>', mt, 'getmetatable(' .. expr .. ')') end
return 'J"children":[' .. table.concat(parts, ',') .. '],"count":' .. n .. ',"truncated":' .. tostring(trunc)
)LUA";

// Runs the helper for `level` (stack level, as in `stack`; < 0 = no frame variables). Returns the helper's result string (first char = status) or an error message.
std::string RunHelper(lua_State* L, const Api* A, const char* mode, const std::string& expr, int level) {
	const int top0 = A->gettop(L);
	A->checkstack(L, 60, "dbg");
	if (A->loadstring(L, HELPER_SRC) != 0) { const std::string m = LuaError(L, A); A->settop(L, top0); return "Ehelper: " + m; }
	hks::pushfstring(L, "%s", mode);
	hks::pushfstring(L, "%s", expr.c_str());
	for (int i = 0; i < 4; i++) hks::createtable(L, 0, 0);
	const int tLn = top0 + 4, tLv = top0 + 5, tUn = top0 + 6, tUv = top0 + 7;
	int nl = 0, nu = 0;
	if (level >= 0) {
		alignas(16) uint8_t ar[AR_SIZE];
		std::memset(ar, 0, sizeof(ar));
		if (A->getstack(L, level, ar)) {
			for (int i = 1; i < 250; i++) {
				const char* nm = A->getlocal(L, ar, i);
				if (!nm) break;
				if (nm[0] != '(') {
					++nl;
					hks::pushinteger(L, nl); hks::pushfstring(L, "%s", nm); A->rawset(L, tLn);
					hks::pushinteger(L, nl); hks::pushvalue(L, -2); A->rawset(L, tLv);
				}
				A->settop(L, -2);                                            // pop the local's value
			}
			const int before = A->gettop(L);
			A->getinfo(L, "f", ar);                                            // pushes the function of the frame
			if (A->gettop(L) == before + 1) {
				for (int i = 1; i < 250; i++) {
					const char* nm = A->getupvalue(L, before + 1, i);
					if (!nm) break;
					++nu;
					hks::pushinteger(L, nu); hks::pushfstring(L, "%s", nm); A->rawset(L, tUn);
					hks::pushinteger(L, nu); hks::pushvalue(L, -2); A->rawset(L, tUv);
					A->settop(L, -2);
				}
				A->settop(L, before);
			}
		}
	}
	hks::pushinteger(L, nl);
	hks::pushinteger(L, nu);
	if (hks::pcall(L, 8, 1, 0) != 0) { const std::string m = LuaError(L, A); A->settop(L, top0); return "E" + m; }
	size_t len = 0;
	const char* r = A->tolstring(L, -1, &len);
	std::string res = r ? std::string(r, len) : std::string("E(no result)");
	A->settop(L, top0);
	return res;
}

// The first stack level that is a Lua function (a stop inside c_breakpoint has a C function at level 0).
int FirstLuaLevel(lua_State* L, const Api* A) {
	for (int lvl = 0; lvl < 64; ++lvl) {
		alignas(16) uint8_t ar[AR_SIZE];
		std::memset(ar, 0, sizeof(ar));
		if (!A->getstack(L, lvl, ar)) break;
		A->getinfo(L, "S", ar);
		if (std::strncmp(reinterpret_cast<const char*>(ar + AR_SHORTSRC), "[C]", 3) != 0) return lvl;
	}
	return 0;
}

void HandleStopped(lua_State* L, const Request& r) {
	const Api* A = LuaHooks::GetApi();
	if (!A) { Reply(r.id, false, "\"error\":\"HavokScript API not available\""); return; }
	if (r.cmd == "stack") { Reply(r.id, true, DumpStack(L, A)); return; }
	if (r.cmd != "eval" && r.cmd != "vars") { Reply(r.id, false, "\"error\":\"unknown command\""); return; }
	const int level = r.level >= 0 ? r.level : FirstLuaLevel(L, A);
	const bool vars = r.cmd == "vars";
	const std::string res = RunHelper(L, A, vars ? (r.arg.empty() ? "vars" : "children") : "eval", r.arg, level);
	const char st = res.empty() ? 'E' : res[0];
	if (st == (vars ? 'J' : 'O')) {
		if (vars) Reply(r.id, true, res.substr(1) + ",\"level\":" + std::to_string(level));
		else Reply(r.id, true, "\"result\":\"" + Esc(res.substr(1)) + "\",\"level\":" + std::to_string(level));
	} else Reply(r.id, false, "\"error\":\"" + Esc(st == 'E' ? res.substr(1) : res) + "\"");
}

void __cdecl DbgHook(lua_State* L, void* ar);

// Blocks the calling thread (which executes L) until the client resumes it. No client or already stopped elsewhere: returns at once.
void StopHere(lua_State* L, const char* reason) {
	if (g_client.load() == INVALID_SOCKET) return;
	std::unique_lock<std::mutex> lk(g_mu);
	if (g_paused) return;                                                    // D0: one stop at a time, a second state just continues
	g_paused = true; g_resume = false; g_resumeMode = 0; g_pausedL = L; g_pausedReason = reason; g_reqs.clear();
	lk.unlock();
	long long stopDepth = CallDepth(L);                                      // step baseline: the innermost Lua frame (a stop inside a C function such as c_breakpoint counts its caller)
	{
		const Api* A = LuaHooks::GetApi();
		alignas(16) uint8_t ar[AR_SIZE];
		std::memset(ar, 0, sizeof(ar));
		if (A && A->getstack(L, 0, ar)) {
			A->getinfo(L, "S", ar);
			if (std::strncmp(reinterpret_cast<const char*>(ar + AR_SHORTSRC), "[C]", 3) == 0) stopDepth -= 1;
		}
	}
	char st[40]; std::snprintf(st, sizeof(st), "%p", static_cast<void*>(L));
	Send(std::string("{\"event\":\"stopped\",\"reason\":\"") + reason + "\",\"state\":\"" + st + "\"}");
	lk.lock();
	auto last = std::chrono::steady_clock::now();
	for (;;) {
		g_cv.wait_for(lk, std::chrono::milliseconds(100), [] { return g_resume || !g_reqs.empty() || g_client.load() == INVALID_SOCKET; });
		if (g_resume || g_client.load() == INVALID_SOCKET) break;
		if (!g_reqs.empty()) {
			Request r = std::move(g_reqs.front()); g_reqs.pop_front();
			lk.unlock();
			HandleStopped(L, r);
			lk.lock();
			last = std::chrono::steady_clock::now();
			continue;
		}
		if (std::chrono::steady_clock::now() - last > std::chrono::seconds(IDLE_RESUME_SECONDS)) { DevBridge::Log("[LuaDebugger] no request for %d s: resuming the game thread", IDLE_RESUME_SECONDS); break; }
	}
	const int mode = g_client.load() == INVALID_SOCKET ? 0 : g_resumeMode;
	g_paused = false; g_pausedL = nullptr;
	lk.unlock();
	g_cv.notify_all();
	g_stepDepth = stopDepth; g_stepL = mode ? L : nullptr; g_stepMode = mode;
	if (mode || g_hasBps.load() || g_pauseReq.load()) LuaHooks::SetNativeHook(L, DbgHook, LuaHooks::HOOK_LINE, 0, false); else LuaHooks::SetNativeHook(L, nullptr, 0, 0, false);
	UpdateArm();
}

void __cdecl DbgHook(lua_State* L, void* ar) {
	if (*reinterpret_cast<int*>(static_cast<uint8_t*>(ar) + AR_EVENT) != EV_LINE) return;
	const bool pause = g_pauseReq.load();
	const int sm = g_stepMode.load();
	const bool stepping = sm != 0 && g_stepL.load() == L;
	const bool bps = g_hasBps.load();
	if (!pause && !stepping && !bps) { LuaHooks::SetNativeHook(L, nullptr, 0, 0); return; }   // stale hook (inherited by a coroutine, or nothing to do any more)
	if (pause && g_pauseReq.exchange(false)) { StopHere(L, "pause"); return; }
	if (stepping) {
		const long long d = CallDepth(L), base = g_stepDepth.load();
		if (sm == 1 || (sm == 2 && d <= base) || (sm == 3 && d < base)) { StopHere(L, "step"); return; }
	}
	if (bps) {
		const Api* A = LuaHooks::GetApi();
		if (!A) return;
		A->getinfo(L, "S", ar);
		const char* src = reinterpret_cast<const char*>(static_cast<uint8_t*>(ar) + AR_SHORTSRC);
		const int line = *reinterpret_cast<int*>(static_cast<uint8_t*>(ar) + AR_CURRENTLINE);
					Bp hitBp; bool hit = false; size_t hitIdx = 0;
			{
				std::lock_guard<std::mutex> g(g_bpMu);
				if (!g_bps.empty()) {
					const std::string ns = NormPath(src, strnlen(src, 0x200));
					for (size_t i = 0; i < g_bps.size(); i++) if (g_bps[i].line == line && BpMatches(ns, g_bps[i])) { hit = true; hitBp = g_bps[i]; hitIdx = i; break; }
				}
			}
			if (hit && !hitBp.cond.empty()) {                                    // condition: evaluated in the hooked frame (level 0); an error stops, with a message
				const std::string r = RunHelper(L, A, "cond", hitBp.cond, 0);
				if (!r.empty() && r[0] == 'E') Send("{\"event\":\"output\",\"text\":\"breakpoint condition error: " + Esc(r.substr(1)) + "\"}");
				else if (r != "T") hit = false;
			}
			if (hit) {                                                           // hit count
				std::lock_guard<std::mutex> g(g_bpMu);
				if (hitIdx < g_bps.size() && g_bps[hitIdx].line == line) { const int n = ++g_bps[hitIdx].hits; if (hitBp.hit > 0 && n < hitBp.hit) hit = false; }
			}
			if (hit && !hitBp.log.empty()) {                                     // logpoint: report the value, do not stop
				const std::string r = RunHelper(L, A, "eval", hitBp.log, 0);
				Send("{\"event\":\"output\",\"text\":\"" + Esc(r.empty() ? std::string() : r.substr(1)) + "\",\"line\":" + std::to_string(line) + "}");
				hit = false;
			}
if (hit) StopHere(L, "breakpoint");
	}
}

// Line filter used by the VM stub (LuaHooks::SetLineFilter): idle hooks get one event so DbgHook can remove them; otherwise only stepping / pause / breakpoint lines.
bool __cdecl LineWanted(lua_State* L, int line) {
	const bool pause = g_pauseReq.load();
	const int sm = g_stepMode.load();
	const bool bps = g_hasBps.load();
	if (!bps && !pause && sm == 0) return true;
	if (pause || (sm != 0 && g_stepL.load() == L)) return true;
	const unsigned l = static_cast<unsigned>(line) & 0xFFFF;
	return (g_lineBits[l >> 6].load(std::memory_order_relaxed) >> (l & 63)) & 1;
}

// ---- server thread ------------------------------------------------------------------------------------------------------------------------------------------
void HandleLine(const std::string& line) {
	int id = 0; GetInt(line, "id", id);
	std::string cmd; GetStr(line, "cmd", cmd);
	if (cmd == "status") {
		std::lock_guard<std::mutex> g(g_mu);
		char st[40]; std::snprintf(st, sizeof(st), "%p", static_cast<void*>(g_pausedL));
		Reply(id, true, std::string("\"paused\":") + (g_paused ? "true" : "false") + ",\"state\":\"" + (g_paused ? st : "") + "\",\"reason\":\"" + Esc(g_pausedReason) + "\",\"armedStates\":" + std::to_string(g_armed.load()) + StatsJson());
	} else if (cmd == "break" || cmd == "clear" || cmd == "breaks") {
		std::string file; int ln = 0; GetStr(line, "file", file); GetInt(line, "line", ln);
		{
			std::lock_guard<std::mutex> g(g_bpMu);
							if (cmd == "break") {
					if (file.empty() || ln <= 0) { Reply(id, false, "\"error\":\"break needs file and line\""); return; }
					const std::string nf = NormPath(file.data(), file.size());
					Bp nb{ nf, ln };
					GetStr(line, "cond", nb.cond); GetStr(line, "log", nb.log); GetInt(line, "hit", nb.hit);
					bool replaced = false;
					for (Bp& b : g_bps) if (b.line == ln && b.file == nf) { b = nb; replaced = true; }
					if (!replaced) g_bps.push_back(nb);
} else if (cmd == "clear") {
				if (file.empty()) g_bps.clear();
				else { const std::string nf = NormPath(file.data(), file.size()); g_bps.erase(std::remove_if(g_bps.begin(), g_bps.end(), [&](const Bp& b) { return b.file == nf && (ln <= 0 || b.line == ln); }), g_bps.end()); }
			}
			g_hasBps = !g_bps.empty();
			RebuildLineBits();
			std::string list = "\"breakpoints\":[";
			for (size_t i = 0; i < g_bps.size(); i++) list += (i ? "," : "") + std::string("{\"file\":\"") + Esc(g_bps[i].file) + "\",\"line\":" + std::to_string(g_bps[i].line) + ",\"cond\":\"" + Esc(g_bps[i].cond) + "\",\"log\":\"" + Esc(g_bps[i].log) + "\",\"hit\":" + std::to_string(g_bps[i].hit) + ",\"hits\":" + std::to_string(g_bps[i].hits) + "}";
			Reply(id, true, list + "]");
		}
		if (cmd != "breaks") UpdateArm();
	} else if (cmd == "pause") {
		g_pauseReq = true;
		UpdateArm();
		Reply(id, true, "");
	} else if (cmd == "continue" || cmd == "step" || cmd == "next" || cmd == "out") {
		std::lock_guard<std::mutex> g(g_mu);
		if (!g_paused) { Reply(id, false, "\"error\":\"not paused\""); return; }
		g_resumeMode = cmd == "step" ? 1 : cmd == "next" ? 2 : cmd == "out" ? 3 : 0;
		g_resume = true;
		g_cv.notify_all();
		Reply(id, true, "");
	} else if (cmd == "stack" || cmd == "eval" || cmd == "vars") {
		std::lock_guard<std::mutex> g(g_mu);
		if (!g_paused) { Reply(id, false, "\"error\":\"not paused\""); return; }
		Request r{ id, cmd, "", -1 };
		GetStr(line, "expr", r.arg);
		GetInt(line, "level", r.level);
		g_reqs.push_back(std::move(r));
		g_cv.notify_all();
	} else Reply(id, false, "\"error\":\"unknown command (status, stack, vars, eval, break, clear, breaks, pause, continue, step, next, out)\"");
}

void ServerMain() {
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { DevBridge::Log("[LuaDebugger] WSAStartup failed"); return; }
	SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	sockaddr_in a; std::memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET; a.sin_port = htons(static_cast<u_short>(g_port.load()));
	inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
	BOOL yes = TRUE; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
	if (bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(ls, 1) != 0) {
		DevBridge::Log("[LuaDebugger] cannot listen on 127.0.0.1:%d (error %d)", g_port.load(), WSAGetLastError());
		closesocket(ls); g_started = false; return;
	}
	DevBridge::Log("[LuaDebugger] listening on 127.0.0.1:%d", g_port.load());
	for (;;) {
		SOCKET c = accept(ls, nullptr, nullptr);
		if (c == INVALID_SOCKET) { Sleep(100); continue; }
		BOOL nd = TRUE; setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nd), sizeof(nd));
		g_client = c;
		DevBridge::Log("[LuaDebugger] client connected");
		Send("{\"event\":\"hello\",\"version\":1}");
		std::string buf; char tmp[4096];
		for (;;) {
			const int n = recv(c, tmp, sizeof(tmp), 0);
			if (n <= 0) break;
			buf.append(tmp, n);
			size_t p;
			while ((p = buf.find('\n')) != std::string::npos) { std::string line = buf.substr(0, p); buf.erase(0, p + 1); HandleLine(line); }
		}
		g_client = INVALID_SOCKET;
		closesocket(c);
		ResetSession();
		g_cv.notify_all();                                                    // releases a stopped game thread
		DevBridge::Log("[LuaDebugger] client disconnected");
	}
}

void Start(int port) {
	if (g_started.exchange(true)) return;
	g_port = port;
	LuaHooks::SetArmHandler(ArmSelf);
	LuaHooks::SetLineFilter(DbgHook, LineWanted);
	std::thread(ServerMain).detach();
}

// ---- Lua API ---------------------------------------------------------------------------------------------------------------------------------------------
int __cdecl l_cbreakpoint(lua_State* L) { StopHere(L, "c_breakpoint"); return 0; }

int __cdecl l_listen(lua_State* L) {
	const Api* A = LuaHooks::GetApi();
	int port = DEFAULT_PORT;
	if (A && A->gettop(L) >= 1) { const double d = A->tonumber(L, 1); if (d >= 1024 && d <= 65535) port = static_cast<int>(d); }
	Start(port);
	return 0;
}

int __cdecl l_wait(lua_State* L) {                                           // DevCE_DebugWait(seconds): returns 1 when a client is connected, else 0
	const Api* A = LuaHooks::GetApi();
	double sec = 10;
	if (A && A->gettop(L) >= 1) sec = A->tonumber(L, 1);
	const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<long long>(sec * 1000));
	while (g_client.load() == INVALID_SOCKET && g_started.load() && std::chrono::steady_clock::now() < end) Sleep(50);
	hks::pushnumber(L, g_client.load() != INVALID_SOCKET ? 1.0 : 0.0);
	return 1;
}

int __cdecl l_mark(lua_State* L) {                                           // DevCE_DebugMark(name): log + event with the slow-path counters (turn-cost measurements)
	const Api* A = LuaHooks::GetApi();
	std::string name = "mark";
	if (A && A->gettop(L) >= 1) { size_t n = 0; const char* s = A->tolstring(L, 1, &n); if (s) name.assign(s, n); }
	const LuaHooks::Stats s = LuaHooks::GetStats();
	const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	DevBridge::Log("[Mark] %s t=%lld fetchSlow=%llu lineDelivered=%llu lineFiltered=%llu", name.c_str(), ms, s.fetchSlow, s.lineDelivered, s.lineFiltered);
	Send("{\"event\":\"mark\",\"name\":\"" + Esc(name) + "\",\"t\":" + std::to_string(ms) + StatsJson() + "}");
	return 0;
}

int __cdecl l_demo(lua_State* L) { hks::pushnumber(L, g_demo.load() ? 1.0 : 0.0); return 1; }

int __cdecl l_active(lua_State* L) { hks::pushnumber(L, g_started.load() ? 1.0 : 0.0); return 1; }

void ReadMarker() {
	char path[MAX_PATH] = { 0 };
	HMODULE self = nullptr;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&ReadMarker), &self);
	GetModuleFileNameA(self, path, MAX_PATH);
	char* slash = std::strrchr(path, '\\');
	if (!slash) return;
	*(slash + 1) = 0;
	strcat_s(path, MAX_PATH, "DevCE_Debug.txt");
	FILE* f = nullptr;
	if (fopen_s(&f, path, "r") != 0 || !f) return;
	char line[64] = { 0 };
	int port = DEFAULT_PORT;
	if (std::fgets(line, sizeof(line), f)) { const int p = std::atoi(line); if (p >= 1024 && p <= 65535) port = p; }
	char l2[32] = { 0 };
	if (std::fgets(l2, sizeof(l2), f) && std::strncmp(l2, "demo", 4) == 0) g_demo = true;
	std::fclose(f);
	Start(port);
}

} // namespace

void RegisterGlobals(lua_State* L) {
	const Api* A = LuaHooks::GetApi();
	if (!A) return;
	hks::getfield(L, hks::LUA_GLOBAL, "debug");
	const char* tn = A->typenameOf(L, A->type(L, -1));
	if (tn && std::strcmp(tn, "table") == 0) { PushLuaMethod(L, l_cbreakpoint, "l_cbreakpoint", -2, "c_breakpoint"); }
	A->settop(L, -2);
	PushLuaMethod(L, l_listen, "l_debuglisten", hks::LUA_GLOBAL, "DevCE_DebugListen");
	PushLuaMethod(L, l_wait, "l_debugwait", hks::LUA_GLOBAL, "DevCE_DebugWait");
	PushLuaMethod(L, l_active, "l_debugactive", hks::LUA_GLOBAL, "DevCE_DebugActive");
	PushLuaMethod(L, l_demo, "l_debugdemo", hks::LUA_GLOBAL, "DevCE_DebugDemo");
	PushLuaMethod(L, l_mark, "l_debugmark", hks::LUA_GLOBAL, "DevCE_DebugMark");
	static std::once_flag once;
	std::call_once(once, ReadMarker);
}

} // namespace LuaDebugger
