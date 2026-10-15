// SPDX-License-Identifier: AGPL-3.0-only
// Part of Dev CE, a fork of the Civilization VI Community Extension by Wild-W (https://github.com/Wild-W/CivilizationVI_CommunityExtension).
// Copyright (C) 2026 cru121. Licensed under the GNU Affero General Public License v3.0 (see LICENSE.txt). Not endorsed by Wild-W, Firaxis or 2K.
#include "LuaHooks.h"
#include "DevBridge.h"
#include <Windows.h>
#include <TlHelp32.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

// How this works (details and measurements: dev-ce/DESIGN_LUA_HOOKS.md):
//  * lua_sethook (exported) stores hook function / mask / count in the lua_State (+0xc8 / +0xd0 / +0xd4) but the retail interpreter never reads them.
//  * Every VM instruction goes through the fetch site at HavokScript_FinalRelease.dll+0x696aa (`mov [L+0x30], pc ; mov rsi, rax`, L = rdi, pc = rax,
//    frame base = rcx with a spill slot at [rbp+0x498]). Only this site ran in all our measurements (UI, gameplay and worker threads).
//  * On the first debug.sethook we put a 5-byte jmp there to a small generated stub. Fast path: `cmp dword [rdi+0xd0], 0` (hook mask of the state) and,
//    when zero, the two displaced instructions and a jump back: a few cycles per VM instruction. Slow path: save registers, call OnFetch(L, pc), reload the
//    frame base from [L+0x50] into rcx and its spill slot if a hook ran (a hook that calls Lua can reallocate the Lua stack), restore, continue.
//  * OnFetch implements the PUC-Lua hook events: count (+0xd8 counter vs +0xd4), line (new line or backward jump), call (call depth grew),
//    return (instruction word >> 25 == 9). The Lua-level hook function is kept in registry[_DEVCE_HOOKS][lightuserdata(L)] like PUC Lua.
// The DLL is pinned in DllMain (never unloaded), so the stub may call straight into it.
namespace LuaHooks {
namespace {

using hks::lua_State;
typedef void(__cdecl* HookFn)(lua_State*, void*);

constexpr uintptr_t SITE_RVA = 0x696aa;
constexpr uintptr_t CHECK_RVA = 0x696a0;                                       // 20 bytes of context used for the build check
const uint8_t CHECK_BYTES[20] = { 0x48, 0x8b, 0x85, 0x88, 0x04, 0x00, 0x00, 0x4d, 0x8b, 0xde, 0x48, 0x89, 0x47, 0x30, 0x48, 0x8b, 0xf0, 0x4c, 0x8b, 0xc8 };
constexpr size_t SITE_OFFSET_IN_CHECK = 10;                                    // 0x696aa - 0x696a0
constexpr size_t SITE_PATCH_LEN = 5;                                           // jmp rel32
constexpr size_t SITE_DISPLACED_LEN = 7;                                       // mov [rdi+0x30],rax (4) + mov rsi,rax (3): the stub re-executes them

// lua_State field offsets (HavokScript_FinalRelease.dll 15038592 / 15296837; confirmed with Frida, see DESIGN_LUA_HOOKS.md)
constexpr int OFF_CI_BASE = 0x18, OFF_CI_CUR = 0x28, OFF_PC = 0x30, OFF_TOP = 0x48, OFF_BASE = 0x50;
constexpr int OFF_HOOKPC = 0x38, OFF_HOOKIDX = 0x40;   // read by the frame-pc lookup (0x28180): frame index == [L+0x40] -> pc = [L+0x38]
constexpr int OFF_HOOKFN = 0xc8, OFF_MASK = 0xd0, OFF_COUNT = 0xd4, OFF_COUNTER = 0xd8;
constexpr int AR_EVENT = 0x00, AR_CURRENTLINE = 0x28, AR_SIZE = 0x300;         // lua_Debug: event int @0, currentline int @0x28 (name/short_src further on)
constexpr int EV_CALL = 0, EV_RET = 1, EV_LINE = 2, EV_COUNT = 3;
constexpr int MASK_CALL = 1, MASK_RET = 2, MASK_LINE = 4, MASK_COUNT = 8;
constexpr uint32_t OP_RETURN = 9;                                              // instruction word >> 25 (measured: always lowers the call depth)
const char* const HOOKKEY = "_DEVCE_HOOKS";

struct Api {
	int(__cdecl* getstack)(lua_State*, int, void*) = nullptr;
	int(__cdecl* getinfo)(lua_State*, const char*, void*) = nullptr;
	int(__cdecl* sethook)(lua_State*, HookFn, int, int) = nullptr;
	void(__cdecl* checkstack)(lua_State*, int, const char*) = nullptr;
	int(__cdecl* type)(lua_State*, int) = nullptr;
	lua_State*(__cdecl* tothread)(lua_State*, int) = nullptr;
	const char* (__cdecl* typenameOf)(lua_State*, int) = nullptr;
	void(__cdecl* settop)(lua_State*, int) = nullptr;
	int(__cdecl* gettop)(lua_State*) = nullptr;
	void(__cdecl* pushnil)(lua_State*) = nullptr;
	void(__cdecl* rawget)(lua_State*, int) = nullptr;
	void(__cdecl* rawset)(lua_State*, int) = nullptr;
	const char* (__cdecl* tolstring)(lua_State*, int, size_t*) = nullptr;
	double(__cdecl* tonumber)(lua_State*, int) = nullptr;
	int(__cdecl* luaopen_debug)(lua_State*) = nullptr;
	bool ok = false;
};
Api g_api;
HMODULE g_hks = nullptr;
std::once_flag g_apiOnce;

template <typename T> bool Resolve(T& out, const char* name) {
	out = reinterpret_cast<T>(GetProcAddress(g_hks, name));
	if (!out) DevBridge::Log("[LuaHooks] missing export %s", name);
	return out != nullptr;
}

bool InitApi() {
	std::call_once(g_apiOnce, [] {
		g_hks = GetModuleHandleA("HavokScript_FinalRelease.dll");
		if (!g_hks) { DevBridge::Log("[LuaHooks] HavokScript_FinalRelease.dll not loaded"); return; }
		bool ok = true;
		ok &= Resolve(g_api.getstack, "?hksi_lua_getstack@@YAHPEAUlua_State@@HPEAUlua_Debug@@@Z");
		ok &= Resolve(g_api.getinfo, "?hksi_lua_getinfo@@YAHPEAUlua_State@@PEBDPEAUlua_Debug@@@Z");
		ok &= Resolve(g_api.sethook, "?hksi_lua_sethook@@YAHPEAUlua_State@@P6AX0PEAUlua_Debug@@@ZHH@Z");
		ok &= Resolve(g_api.checkstack, "?luaL_checkstack@@YAXPEAUlua_State@@HPEBD@Z");
		ok &= Resolve(g_api.type, "?hksi_lua_type@@YAHPEAUlua_State@@H@Z");
		ok &= Resolve(g_api.typenameOf, "?hksi_lua_typename@@YAPEBDPEAUlua_State@@H@Z");
		ok &= Resolve(g_api.settop, "?hksi_lua_settop@@YAXPEAUlua_State@@H@Z");
		ok &= Resolve(g_api.gettop, "?hksi_lua_gettop@@YAHPEAUlua_State@@@Z");
		ok &= Resolve(g_api.pushnil, "?hksi_lua_pushnil@@YAXPEAUlua_State@@@Z");
		ok &= Resolve(g_api.rawget, "?hksi_lua_rawget@@YAXPEAUlua_State@@H@Z");
		ok &= Resolve(g_api.rawset, "?hksi_lua_rawset@@YAXPEAUlua_State@@H@Z");
		ok &= Resolve(g_api.tolstring, "?hksi_lua_tolstring@@YAPEBDPEAUlua_State@@HPEA_K@Z");
		ok &= Resolve(g_api.tonumber, "?hksi_lua_tonumber@@YANPEAUlua_State@@H@Z");
		ok &= Resolve(g_api.luaopen_debug, "?luaopen_debug@@YAHPEAUlua_State@@@Z");
		g_api.ok = ok;
	});
	return g_api.ok;
}

inline uint8_t* LB(lua_State* L) { return reinterpret_cast<uint8_t*>(L); }
inline int64_t& I64(lua_State* L, int off) { return *reinterpret_cast<int64_t*>(LB(L) + off); }
inline int& I32(lua_State* L, int off) { return *reinterpret_cast<int*>(LB(L) + off); }

bool IsFunction(lua_State* L, int idx) {
	const char* n = g_api.typenameOf(L, g_api.type(L, idx));
	return n && std::strcmp(n, "function") == 0;
}
bool IsTable(lua_State* L, int idx) {
	const char* n = g_api.typenameOf(L, g_api.type(L, idx));
	return n && std::strcmp(n, "table") == 0;
}
bool IsNoneOrNil(lua_State* L, int idx) {
	const char* n = g_api.typenameOf(L, g_api.type(L, idx));
	return !n || std::strcmp(n, "nil") == 0 || std::strcmp(n, "no value") == 0;
}
bool IsThread(lua_State* L, int idx) {
	const char* n = g_api.typenameOf(L, g_api.type(L, idx));
	return n && std::strcmp(n, "thread") == 0;
}
bool IsNumber(lua_State* L, int idx) {
	const char* n = g_api.typenameOf(L, g_api.type(L, idx));
	return n && std::strcmp(n, "number") == 0;
}

// ---- per-state bookkeeping (only states with a hook ever get an entry; entries are never freed so threads may cache the pointer) -------------------------
constexpr int MAX_DEPTH = 256;
struct HookState {
	uint32_t gen = 0;                           // reset when the global generation changes (sethook / hook removal)
	int64_t depth = -1;                         // call depth at the previous fetch of this state
	const uint32_t* lastPc = nullptr;
	int lines[MAX_DEPTH];                       // last reported line per call depth
	HookState() { std::memset(lines, 0xff, sizeof(lines)); }
};
std::mutex g_mu;
std::unordered_map<lua_State*, HookState*> g_states;
std::atomic<uint32_t> g_gen{ 1 };
thread_local int t_inHook = 0;                // no hooks while a hook runs on this thread (PUC: allowhook = 0)
thread_local lua_State* t_cacheL = nullptr;
thread_local HookState* t_cacheSt = nullptr;
thread_local uint32_t t_cacheGen = 0;

// pc -> line cache, lock-free: entry = ((pc >> 2) << 19) | (line + 1) with line in [-1, 524286]
constexpr size_t LINE_CACHE_SIZE = 1u << 16;
std::atomic<uint64_t> g_lineCache[LINE_CACHE_SIZE];

void ClearCaches() {
	g_gen.fetch_add(1);
	for (size_t i = 0; i < LINE_CACHE_SIZE; i++) g_lineCache[i].store(0, std::memory_order_relaxed);
}

HookState* StateOf(lua_State* L) {
	const uint32_t gen = g_gen.load(std::memory_order_relaxed);
	HookState* st;
	if (t_cacheL == L && t_cacheSt) st = t_cacheSt;
	else {
		std::lock_guard<std::mutex> g(g_mu);
		HookState*& slot = g_states[L];
		if (!slot) slot = new HookState();
		st = slot;
		t_cacheL = L; t_cacheSt = st;
	}
	if (st->gen != gen) { st->gen = gen; st->depth = -1; st->lastPc = nullptr; std::memset(st->lines, 0xff, sizeof(st->lines)); }
	return st;
}

int LineOf(lua_State* L, const uint32_t* pc) {
	const uint64_t key = reinterpret_cast<uintptr_t>(pc) >> 2;
	std::atomic<uint64_t>& slot = g_lineCache[key & (LINE_CACHE_SIZE - 1)];
	const uint64_t e = slot.load(std::memory_order_relaxed);
	if (e != 0 && (e >> 19) == key) return static_cast<int>(e & 0x7FFFF) - 1;
	alignas(16) uint8_t ar[AR_SIZE];
	std::memset(ar, 0, sizeof(ar));
	*reinterpret_cast<const void**>(LB(L) + OFF_PC) = pc;                  // the patched instruction (which stores pc) has not run yet
	int line = -1;
	if (g_api.getstack(L, 0, ar) != 0) {
		g_api.getinfo(L, "l", ar);
		line = *reinterpret_cast<int*>(ar + AR_CURRENTLINE);
	}
	const int enc = (line < -1 || line > 524286) ? 0 : line + 1;
	slot.store((key << 19) | static_cast<uint64_t>(enc), std::memory_order_relaxed);
	return line;
}

// ---- the hook function installed with lua_sethook: calls the Lua function registered for this state, protected -----------------------------------------
void __cdecl HookF(lua_State* L, void* ar) {
	static const char* const names[] = { "call", "return", "line", "count", "tail return" };
	const int event = *reinterpret_cast<int*>(static_cast<uint8_t*>(ar) + AR_EVENT);
	const int line = *reinterpret_cast<int*>(static_cast<uint8_t*>(ar) + AR_CURRENTLINE);
	const int top0 = g_api.gettop(L);
	hks::getfield(L, hks::LUA_REGISTRYINDEX, HOOKKEY);
	if (IsTable(L, -1)) {
		hks::pushlightuserdata(L, L);
		g_api.rawget(L, -2);
		if (IsFunction(L, -1)) {
			hks::pushfstring(L, "%s", (event >= 0 && event <= 4) ? names[event] : "?");
			if (line >= 0) hks::pushinteger(L, line); else g_api.pushnil(L);
			if (hks::pcall(L, 2, 0, 0) != 0) {
				size_t len = 0;
				const char* msg = g_api.tolstring(L, -1, &len);
				DevBridge::Log("[LuaHooks] error in hook function, hook removed: %s", msg ? msg : "(no message)");
				g_api.sethook(L, nullptr, 0, 0);                               // PUC would propagate the error; we cannot raise from inside the VM loop safely
			}
		}
	}
	g_api.settop(L, top0);
}

// Calls the hook of state L like Linux hks::debug_hook_call: reserve stack, getstack(0), fill event and currentline, call L+0xc8, restore the top.
void CallHook(lua_State* L, int event, int line, const uint32_t* pc) {
	HookFn hook = *reinterpret_cast<HookFn*>(LB(L) + OFF_HOOKFN);
	if (!hook) return;
	++t_inHook;
	const int64_t topOff = I64(L, OFF_TOP) - I64(L, OFF_BASE);          // as an offset: the Lua stack may be reallocated during the hook
	*reinterpret_cast<const void**>(LB(L) + OFF_PC) = pc;
	// like hks::debug_hook_call: tell the frame-pc lookup which frame is being hooked and where it is, so debug.getinfo(2) inside the hook sees the hooked line
	const void* const prevHookPc = *reinterpret_cast<const void**>(LB(L) + OFF_HOOKPC);
	const int prevHookIdx = I32(L, OFF_HOOKIDX);
	*reinterpret_cast<const void**>(LB(L) + OFF_HOOKPC) = pc;
	I32(L, OFF_HOOKIDX) = static_cast<int>((I64(L, OFF_CI_CUR) - I64(L, OFF_CI_BASE)) / 24);
	g_api.checkstack(L, 40, "hook");
	alignas(16) uint8_t ar[AR_SIZE];
	std::memset(ar, 0, sizeof(ar));
	g_api.getstack(L, 0, ar);
	*reinterpret_cast<int*>(ar + AR_EVENT) = event;
	*reinterpret_cast<int*>(ar + AR_CURRENTLINE) = line;
	hook(L, ar);
	*reinterpret_cast<const void**>(LB(L) + OFF_HOOKPC) = prevHookPc;
	I32(L, OFF_HOOKIDX) = prevHookIdx;
	I64(L, OFF_TOP) = I64(L, OFF_BASE) + topOff;
	--t_inHook;
}

// Called by the stub for every VM instruction of a state whose hook mask is non-zero. Returns non-zero when a hook ran (the stub then reloads the frame base).
int __cdecl OnFetch(lua_State* L, const uint32_t* pc) {
	if (t_inHook) return 0;
	const int mask = I32(L, OFF_MASK);
	if (!mask || !*reinterpret_cast<void**>(LB(L) + OFF_HOOKFN)) return 0;
	const uint32_t op = *pc >> 25;
	const int64_t depth = (I64(L, OFF_CI_CUR) - I64(L, OFF_CI_BASE)) / 24;
	const int slot = depth < 0 ? 0 : (depth >= MAX_DEPTH ? MAX_DEPTH - 1 : static_cast<int>(depth));

	HookState* st = StateOf(L);
	const bool grew = st->depth >= 0 && depth > st->depth;
	const bool callDue = grew && (mask & MASK_CALL);
	if (grew) st->lines[slot] = -1;                                           // entering a new function: its line history starts empty
	st->depth = depth;
	const uint32_t* last = st->lastPc;
	st->lastPc = pc;
	const bool retDue = (mask & MASK_RET) && op == OP_RETURN;
	bool countDue = false;
	if (mask & MASK_COUNT) {
		int c = ++I32(L, OFF_COUNTER);
		if (c >= I32(L, OFF_COUNT)) { I32(L, OFF_COUNTER) = 0; countDue = true; }
	}
	bool lineDue = false;
	int line = -1;
	if (mask & MASK_LINE) {
		line = LineOf(L, pc);
		int& prev = st->lines[slot];
		// PUC Lua: a line event when the line changes or the pc jumps backwards (a loop iteration on the same line)
		if (line != prev || (last && !grew && pc <= last)) { lineDue = true; prev = line; }
	}
	if (!(callDue || lineDue || countDue || retDue)) return 0;
	if (line < 0 && (retDue || callDue || countDue)) line = LineOf(L, pc);
	if (callDue) CallHook(L, EV_CALL, line, pc);
	if (lineDue) CallHook(L, EV_LINE, line, pc);
	if (countDue) CallHook(L, EV_COUNT, line, pc);
	if (retDue) CallHook(L, EV_RET, line, pc);
	return 1;
}

// ---- the stub and the patch ----------------------------------------------------------------------------------------------------------------------------
std::mutex g_installMu;
std::atomic<bool> g_installed{ false };
std::atomic<bool> g_installFailed{ false };

struct Asm {
	std::vector<uint8_t> b;
	void bytes(std::initializer_list<uint8_t> l) { b.insert(b.end(), l.begin(), l.end()); }
	void u32(uint32_t v) { for (int i = 0; i < 4; i++) b.push_back(static_cast<uint8_t>(v >> (8 * i))); }
	size_t pos() const { return b.size(); }
};

void* AllocNear(uintptr_t target, size_t size) {
	SYSTEM_INFO si; GetSystemInfo(&si);
	const uintptr_t gran = si.dwAllocationGranularity;
	const uintptr_t limit = 0x70000000;                                      // stay well inside +-2 GB of the site
	for (uintptr_t off = gran; off < limit; off += gran) {
		for (int dir = 0; dir < 2; dir++) {
			uintptr_t cand = dir == 0 ? (target & ~(gran - 1)) - off : (target & ~(gran - 1)) + off;
			if (dir == 0 && cand > target) continue;
			void* p = VirtualAlloc(reinterpret_cast<void*>(cand), size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
			if (p) return p;
		}
	}
	return nullptr;
}

// Suspends every other thread of the process, checks that none is executing inside [site, site+len), runs `patch`, resumes. Retries a few times.
bool WithThreadsFrozen(uintptr_t site, size_t len, void (*patch)(void*), void* arg) {
	for (int attempt = 0; attempt < 20; attempt++) {
		std::vector<DWORD> ids;
		const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snap == INVALID_HANDLE_VALUE) return false;
		THREADENTRY32 te; te.dwSize = sizeof(te);
		if (Thread32First(snap, &te)) do { if (te.th32OwnerProcessID == pid && te.th32ThreadID != self) ids.push_back(te.th32ThreadID); } while (Thread32Next(snap, &te));
		CloseHandle(snap);
		std::vector<HANDLE> hs; hs.reserve(ids.size());
		bool clash = false;
		for (DWORD id : ids) {                                               // no allocation from here until the threads are resumed
			HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, id);
			if (!h) continue;
			if (SuspendThread(h) == static_cast<DWORD>(-1)) { CloseHandle(h); continue; }
			hs.push_back(h);
			CONTEXT ctx; ctx.ContextFlags = CONTEXT_CONTROL;
			if (GetThreadContext(h, &ctx) && ctx.Rip >= site && ctx.Rip < site + len) clash = true;
		}
		if (!clash) patch(arg);
		for (HANDLE h : hs) { ResumeThread(h); CloseHandle(h); }
		if (!clash) return true;
		Sleep(1);
	}
	return false;
}

struct PatchArg { uint8_t* site; uint8_t bytes[SITE_PATCH_LEN]; };
void DoPatch(void* p) {
	PatchArg* a = static_cast<PatchArg*>(p);
	DWORD old = 0;
	VirtualProtect(a->site, SITE_PATCH_LEN, PAGE_EXECUTE_READWRITE, &old);
	std::memcpy(a->site, a->bytes, SITE_PATCH_LEN);
	FlushInstructionCache(GetCurrentProcess(), a->site, SITE_PATCH_LEN);
	VirtualProtect(a->site, SITE_PATCH_LEN, old, &old);
}

bool Install() {
	std::lock_guard<std::mutex> g(g_installMu);
	if (g_installed) return true;
	if (g_installFailed) return false;
	auto fail = [](const char* why) { DevBridge::Log("[LuaHooks] not installed: %s", why); g_installFailed = true; return false; };
	if (!InitApi()) return fail("HavokScript exports missing");
	uint8_t* base = reinterpret_cast<uint8_t*>(g_hks);
	uint8_t* site = base + SITE_RVA;
	if (std::memcmp(base + CHECK_RVA, CHECK_BYTES, sizeof(CHECK_BYTES)) != 0) return fail("fetch site bytes differ (other HavokScript build, or already patched by a debugger tool)");

	// block: [0x00] magic, [0x08] OnFetch pointer, [0x10] resume address (informational), [0x40...] stub code
	uint8_t* blk = static_cast<uint8_t*>(AllocNear(reinterpret_cast<uintptr_t>(site), 0x1000));
	if (!blk) return fail("no executable memory within +-2 GB of the fetch site");
	std::memcpy(blk, "DEVCEHK1", 8);
	*reinterpret_cast<void**>(blk + 0x08) = reinterpret_cast<void*>(&OnFetch);
	*reinterpret_cast<void**>(blk + 0x10) = site + SITE_DISPLACED_LEN;
	const uintptr_t slotFetch = reinterpret_cast<uintptr_t>(blk) + 0x08;
	const uintptr_t codeBase = reinterpret_cast<uintptr_t>(blk) + 0x40;

	Asm a;
	auto ripRel = [&](uintptr_t slot, size_t instrEndOffset) { return static_cast<uint32_t>(static_cast<int32_t>(slot - (codeBase + instrEndOffset))); };
	auto jmpRel = [&](size_t instrEndOffset) { return static_cast<uint32_t>(static_cast<int32_t>((reinterpret_cast<uintptr_t>(site) + SITE_DISPLACED_LEN) - (codeBase + instrEndOffset))); };
	// entry: fast path when the state has no hook
	a.bytes({ 0x83, 0xBF, 0xD0, 0x00, 0x00, 0x00, 0x00 });                   // cmp dword ptr [rdi+0xd0], 0
	a.bytes({ 0x0F, 0x85 }); const size_t jneFix = a.pos(); a.u32(0);        // jne slow
	a.bytes({ 0x48, 0x89, 0x47, 0x30 });                                     // mov [rdi+0x30], rax   (displaced)
	a.bytes({ 0x48, 0x8B, 0xF0 });                                           // mov rsi, rax          (displaced)
	a.bytes({ 0xE9 }); a.u32(jmpRel(a.pos() + 4));                           // jmp site+7
	const size_t slow = a.pos();
	{ const int32_t rel = static_cast<int32_t>(slow - (jneFix + 4)); std::memcpy(&a.b[jneFix], &rel, 4); }
	a.bytes({ 0x9C, 0x50, 0x51, 0x52, 0x53, 0x56, 0x57 });                   // pushfq; push rax, rcx, rdx, rbx, rsi, rdi
	a.bytes({ 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53 });             // push r8, r9, r10, r11
	a.bytes({ 0x48, 0x89, 0xE3 });                                           // mov rbx, rsp  (rbx+0 r11, +8 r10, +16 r9, +24 r8, +32 rdi, +40 rsi, +48 rbx, +56 rdx, +64 rcx, +72 rax, +80 flags)
	a.bytes({ 0x48, 0x83, 0xE4, 0xF0 });                                     // and rsp, -16
	a.bytes({ 0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00 });                   // sub rsp, 0xa0  (shadow space + xmm save area)
	for (int i = 0; i < 6; i++) a.bytes({ 0xF3, 0x0F, 0x7F, static_cast<uint8_t>(0x44 | (i << 3)), 0x24, static_cast<uint8_t>(0x20 + 0x10 * i) });   // movdqu [rsp+0x20+16i], xmm_i
	a.bytes({ 0x48, 0x89, 0xF9 });                                           // mov rcx, rdi   (L)
	a.bytes({ 0x48, 0x89, 0xC2 });                                           // mov rdx, rax   (pc)
	a.bytes({ 0xFF, 0x15 }); { const size_t at = a.pos(); a.u32(ripRel(slotFetch, at + 4)); }     // call [OnFetch]
	for (int i = 0; i < 6; i++) a.bytes({ 0xF3, 0x0F, 0x6F, static_cast<uint8_t>(0x44 | (i << 3)), 0x24, static_cast<uint8_t>(0x20 + 0x10 * i) });   // movdqu xmm_i, [rsp+...]
	a.bytes({ 0x85, 0xC0 });                                                 // test eax, eax
	a.bytes({ 0x74 }); const size_t jzFix = a.pos(); a.b.push_back(0);       // jz norefix
	a.bytes({ 0x48, 0x8B, 0x7B, 0x20 });                                     // mov rdi, [rbx+0x20]      (saved L)
	a.bytes({ 0x48, 0x8B, 0x4F, 0x50 });                                     // mov rcx, [rdi+0x50]      (frame base, may have moved)
	a.bytes({ 0x48, 0x89, 0x4B, 0x40 });                                     // mov [rbx+0x40], rcx      (saved rcx)
	a.bytes({ 0x48, 0x89, 0x8D, 0x98, 0x04, 0x00, 0x00 });                   // mov [rbp+0x498], rcx     (spill slot of the frame base)
	const size_t norefix = a.pos();
	a.b[jzFix] = static_cast<uint8_t>(norefix - (jzFix + 1));
	a.bytes({ 0x48, 0x89, 0xDC });                                           // mov rsp, rbx
	a.bytes({ 0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58 });             // pop r11, r10, r9, r8
	a.bytes({ 0x5F, 0x5E, 0x5B, 0x5A, 0x59, 0x58, 0x9D });                   // pop rdi, rsi, rbx, rdx, rcx, rax; popfq
	a.bytes({ 0x48, 0x89, 0x47, 0x30 });                                     // mov [rdi+0x30], rax   (displaced)
	a.bytes({ 0x48, 0x8B, 0xF0 });                                           // mov rsi, rax          (displaced)
	a.bytes({ 0xE9 }); a.u32(jmpRel(a.pos() + 4));                           // jmp site+7
	if (a.b.size() > 0x1000 - 0x40) return fail("stub too large");
	std::memcpy(blk + 0x40, a.b.data(), a.b.size());
	FlushInstructionCache(GetCurrentProcess(), blk, 0x1000);

	PatchArg pa; pa.site = site;
	const int64_t rel = static_cast<int64_t>(codeBase) - static_cast<int64_t>(reinterpret_cast<uintptr_t>(site) + SITE_PATCH_LEN);
	if (rel > INT32_MAX || rel < INT32_MIN) return fail("stub out of jmp range");
	pa.bytes[0] = 0xE9;
	const int32_t rel32 = static_cast<int32_t>(rel);
	std::memcpy(pa.bytes + 1, &rel32, 4);
	if (!WithThreadsFrozen(reinterpret_cast<uintptr_t>(site), SITE_DISPLACED_LEN, &DoPatch, &pa)) return fail("could not freeze the other threads outside the patched bytes");
	g_installed = true;
	DevBridge::Log("[LuaHooks] fetch site patched (HavokScript+0x%llx -> stub at %p)", (unsigned long long)SITE_RVA, (void*)(blk + 0x40));
	return true;
}

// ---- Lua API: debug.sethook / debug.gethook (PUC Lua 5.1 semantics for the current thread) -----------------------------------------------------------
int __cdecl l_sethook(lua_State* L) {
	// debug.sethook([thread,] [hook, mask [, count]])  - PUC Lua 5.1 semantics; with a thread argument the hook is set on that coroutine
	// (it is called on the thread that runs the hooked code, so the Lua hook function sees the coroutine as its running state).
	if (!InitApi()) { hks::error(L, "debug.sethook: HavokScript API not available"); return 0; }
	int arg = 0;
	lua_State* L1 = L;
	if (IsThread(L, 1)) { arg = 1; L1 = g_api.tothread(L, 1); if (!L1) { hks::error(L, "debug.sethook: bad thread"); return 0; } }
	if (IsNoneOrNil(L, arg + 1)) {                                             // remove the hook
		g_api.sethook(L1, nullptr, 0, 0);
		hks::getfield(L, hks::LUA_REGISTRYINDEX, HOOKKEY);
		if (IsTable(L, -1)) { hks::pushlightuserdata(L, L1); g_api.pushnil(L); g_api.rawset(L, -3); }
		g_api.settop(L, -2);
		ClearCaches();
		return 0;
	}
	if (!IsFunction(L, arg + 1)) { hks::error(L, "debug.sethook: bad argument (function expected)"); return 0; }
	size_t len = 0;
	const char* smask = g_api.tolstring(L, arg + 2, &len);
	if (!smask) { hks::error(L, "debug.sethook: bad mask argument (string expected)"); return 0; }
	int count = IsNumber(L, arg + 3) ? static_cast<int>(g_api.tonumber(L, arg + 3)) : 0;
	int mask = 0;
	for (const char* p = smask; *p; ++p) {
		if (*p == 'c') mask |= MASK_CALL; else if (*p == 'r') mask |= MASK_RET; else if (*p == 'l') mask |= MASK_LINE;
	}
	if (count > 0) mask |= MASK_COUNT; else count = 0;
	if (mask == 0) { g_api.sethook(L1, nullptr, 0, 0); return 0; }
	if (!Install()) { hks::error(L, "debug.sethook: the VM patch could not be installed (see DevBridge.log)"); return 0; }

	hks::getfield(L, hks::LUA_REGISTRYINDEX, HOOKKEY);                         // registry[HOOKKEY][lightuserdata(L1)] = hook
	if (!IsTable(L, -1)) {
		g_api.settop(L, -2);
		hks::createtable(L, 0, 0);
		hks::pushvalue(L, -1);
		hks::setfield(L, hks::LUA_REGISTRYINDEX, HOOKKEY);
	}
	hks::pushlightuserdata(L, L1);
	hks::pushvalue(L, arg + 1);
	g_api.rawset(L, -3);
	g_api.settop(L, -2);
	ClearCaches();
	g_api.sethook(L1, &HookF, mask, count);
	return 0;
}

int __cdecl l_clockms(lua_State* L) {
	LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
	hks::pushnumber(L, 1000.0 * static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart));
	return 1;
}

int __cdecl l_gethook(lua_State* L) {
	// debug.gethook([thread]) -> hook function (or nil), mask string, count  (PUC Lua 5.1 db_gethook)
	if (!InitApi()) { hks::error(L, "debug.gethook: HavokScript API not available"); return 0; }
	lua_State* L1 = L;
	if (IsThread(L, 1)) { L1 = g_api.tothread(L, 1); if (!L1) { hks::error(L, "debug.gethook: bad thread"); return 0; } }
	const int mask = I32(L1, OFF_MASK), count = I32(L1, OFF_COUNT);
	HookFn hook = *reinterpret_cast<HookFn*>(LB(L1) + OFF_HOOKFN);
	if (!hook) g_api.pushnil(L);
	else {
		hks::getfield(L, hks::LUA_REGISTRYINDEX, HOOKKEY);
		if (IsTable(L, -1)) { hks::pushlightuserdata(L, L1); g_api.rawget(L, -2); }     // the registry table stays below the three results
		else g_api.pushnil(L);
	}
	char buf[4]; int n = 0;
	if (mask & MASK_CALL) buf[n++] = 'c';
	if (mask & MASK_RET) buf[n++] = 'r';
	if (mask & MASK_LINE) buf[n++] = 'l';
	buf[n] = 0;
	hks::pushfstring(L, "%s", buf);
	hks::pushinteger(L, hook ? count : 0);
	return 3;
}

} // namespace

void RegisterGlobals(lua_State* L) {
	if (!InitApi()) return;
	// make sure a global `debug` table exists (the main/tuner state has one; mod states do not)
	hks::getfield(L, hks::LUA_GLOBAL, "debug");
	if (!IsTable(L, -1)) {
		g_api.settop(L, -2);
		g_api.luaopen_debug(L);                                                // like luaL_register: creates the global table and leaves it on the stack
		if (!IsTable(L, -1)) { g_api.settop(L, -2); hks::createtable(L, 0, 0); }
		hks::pushvalue(L, -1);
		hks::setfield(L, hks::LUA_GLOBAL, "debug");
	}
	PushLuaMethod(L, l_sethook, "l_sethook", -2, "sethook");
	PushLuaMethod(L, l_gethook, "l_gethook", -2, "gethook");
	g_api.settop(L, -2);
	PushLuaMethod(L, l_clockms, "l_clockms", hks::LUA_GLOBAL, "DevCE_ClockMs");   // high-resolution clock in milliseconds (os.clock is far too coarse here)
}

bool IsInstalled() { return g_installed.load(); }

} // namespace LuaHooks
