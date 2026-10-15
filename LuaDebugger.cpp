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

struct Request { int id; std::string cmd; std::string arg; };

std::mutex g_mu;                               // protects the stop state below
std::condition_variable g_cv;
std::deque<Request> g_reqs;
bool g_paused = false, g_resume = false;
int g_stepMode = 0;                            // 1 = stop at the next line event of the same state
lua_State* g_pausedL = nullptr;
std::string g_pausedReason;

std::atomic<SOCKET> g_client{ INVALID_SOCKET };
std::mutex g_sendMu;
std::atomic<bool> g_started{ false };
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

// Evaluates `expr` as an expression (return <expr>), else as a statement, in the stopped state; returns tostring() of the first result.
std::string Eval(lua_State* L, const Api* A, const std::string& expr, bool& ok) {
	const int top0 = A->gettop(L);
	A->checkstack(L, 20, "dbg");
	std::string res;
	int rc = A->loadstring(L, ("return " + expr).c_str());
	if (rc != 0) { A->settop(L, top0); rc = A->loadstring(L, expr.c_str()); }
	if (rc != 0) { res = LuaError(L, A); ok = false; }
	else if (hks::pcall(L, 0, 1, 0) != 0) { res = LuaError(L, A); ok = false; }
	else {
		hks::getfield(L, hks::LUA_GLOBAL, "tostring");
		hks::pushvalue(L, -2);
		if (hks::pcall(L, 1, 1, 0) == 0) res = LuaError(L, A);          // tolstring of the string on top
		else { res = "<tostring failed: " + LuaError(L, A) + ">"; }
		ok = true;
	}
	A->settop(L, top0);
	return res;
}

void HandleStopped(lua_State* L, const Request& r) {
	const Api* A = LuaHooks::GetApi();
	if (!A) { Reply(r.id, false, "\"error\":\"HavokScript API not available\""); return; }
	if (r.cmd == "stack") Reply(r.id, true, DumpStack(L, A));
	else if (r.cmd == "eval") {
		bool ok = false;
		const std::string res = Eval(L, A, r.arg, ok);
		Reply(r.id, ok, std::string(ok ? "\"result\":\"" : "\"error\":\"") + Esc(res) + "\"");
	} else Reply(r.id, false, "\"error\":\"unknown command\"");
}

void __cdecl DbgHook(lua_State* L, void* ar);

// Blocks the calling thread (which executes L) until the client resumes it. No client or already stopped elsewhere: returns at once.
void StopHere(lua_State* L, const char* reason) {
	if (g_client.load() == INVALID_SOCKET) return;
	std::unique_lock<std::mutex> lk(g_mu);
	if (g_paused) return;                                                    // D0: one stop at a time, a second state just continues
	g_paused = true; g_resume = false; g_stepMode = 0; g_pausedL = L; g_pausedReason = reason; g_reqs.clear();
	lk.unlock();
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
	const int step = g_stepMode;
	g_paused = false; g_pausedL = nullptr;
	lk.unlock();
	g_cv.notify_all();
	if (step == 1) LuaHooks::SetNativeHook(L, DbgHook, LuaHooks::HOOK_LINE, 0);
}

void __cdecl DbgHook(lua_State* L, void* ar) {
	if (*reinterpret_cast<int*>(static_cast<uint8_t*>(ar) + AR_EVENT) != EV_LINE) return;
	LuaHooks::SetNativeHook(L, nullptr, 0, 0);                              // one-shot; `step` re-arms it
	StopHere(L, "step");
}

// ---- server thread ------------------------------------------------------------------------------------------------------------------------------------------
void HandleLine(const std::string& line) {
	int id = 0; GetInt(line, "id", id);
	std::string cmd; GetStr(line, "cmd", cmd);
	if (cmd == "status") {
		std::lock_guard<std::mutex> g(g_mu);
		char st[40]; std::snprintf(st, sizeof(st), "%p", static_cast<void*>(g_pausedL));
		Reply(id, true, std::string("\"paused\":") + (g_paused ? "true" : "false") + ",\"state\":\"" + (g_paused ? st : "") + "\",\"reason\":\"" + Esc(g_pausedReason) + "\"");
	} else if (cmd == "continue" || cmd == "step") {
		std::lock_guard<std::mutex> g(g_mu);
		if (!g_paused) { Reply(id, false, "\"error\":\"not paused\""); return; }
		g_stepMode = cmd == "step" ? 1 : 0;
		g_resume = true;
		g_cv.notify_all();
		Reply(id, true, "");
	} else if (cmd == "stack" || cmd == "eval") {
		std::lock_guard<std::mutex> g(g_mu);
		if (!g_paused) { Reply(id, false, "\"error\":\"not paused\""); return; }
		Request r{ id, cmd, "" };
		GetStr(line, "expr", r.arg);
		g_reqs.push_back(std::move(r));
		g_cv.notify_all();
	} else Reply(id, false, "\"error\":\"unknown command (status, stack, eval, continue, step)\"");
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
		g_cv.notify_all();                                                    // releases a stopped game thread
		DevBridge::Log("[LuaDebugger] client disconnected");
	}
}

void Start(int port) {
	if (g_started.exchange(true)) return;
	g_port = port;
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
	static std::once_flag once;
	std::call_once(once, ReadMarker);
}

} // namespace LuaDebugger
