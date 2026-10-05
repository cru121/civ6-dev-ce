#include "DevBridge.h"
#include "Runtime.h"
#include <iostream>
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <cmath>

extern "C" __declspec(dllexport) void* DevBridgeStates[4] = { nullptr, nullptr, nullptr, nullptr };
extern "C" __declspec(dllexport) volatile int DevBridgeArmed = 0;
extern "C" __declspec(dllexport) volatile int DevBridgeMarkerLast = 0;

extern "C" __declspec(dllexport) __declspec(noinline) void DevCE_Marker(int code) { DevBridgeMarkerLast = code; }

namespace DevBridge {
	static FILE* gLog = nullptr;
	typedef void* (__fastcall* EditFn)(void*);
	static EditFn gEdit = nullptr;
	typedef void* (__fastcall* PlayerFn)(int);
	static PlayerFn gEditPlayer = nullptr;   // FAutoVariable::edit, null when its address check failed

	static CRITICAL_SECTION gLogLock;
	static INIT_ONCE gLogLockOnce = INIT_ONCE_STATIC_INIT;
	static BOOL CALLBACK InitLogLock(PINIT_ONCE, PVOID, PVOID*) { InitializeCriticalSection(&gLogLock); return TRUE; }

	void Log(const char* fmt, ...) {
		InitOnceExecuteOnce(&gLogLockOnce, InitLogLock, nullptr, nullptr);
		EnterCriticalSection(&gLogLock);
		if (!gLog) {
			char path[MAX_PATH] = { 0 };
			HMODULE self = nullptr;
			GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&DevBridge::Log, &self);
			GetModuleFileNameA(self, path, MAX_PATH);
			char* slash = strrchr(path, '\\');
			if (slash) *(slash + 1) = 0;
			strcat_s(path, "DevBridge.log");
			gLog = _fsopen(path, "a", _SH_DENYNO);   // readable while the game runs
		}
		char buf[1024];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(buf, sizeof(buf), fmt, ap);
		va_end(ap);
		std::cout << buf << "\n";
		if (gLog) { fprintf(gLog, "%s\n", buf); fflush(gLog); }
	}

	// Frida live tool support. DevCE_Marker is exported and does nothing: the tool hooks it to learn when a gameplay-script test window begins (1) and ends (2).
	// DevBridgeArmed is set by the tool; the self-test script only runs its level-2 window while it is non-zero.

	static int lRecordState(hks::lua_State* L) {
		DevBridgeStates[2] = (void*)L;
		DevBridgeStates[3] = (void*)(uintptr_t)GetCurrentThreadId();
		if (hks::isnumber(L, 1)) {
			const int code = hks::checkinteger(L, 1);
			if (code == 5) DevBridgeArmed = 5;   // level-3 run finished: do not repeat
			DevCE_Marker(code);
		}
		return 0;
	}

	// DevCE_Log(text): appends a line to DevBridge.log and flushes it at once (survives a crash: shows the last thing a test tried).
	static int lLog(hks::lua_State* L) {
		unsigned __int64 len = 0;
		const char* t = hks::checklstring(L, 1, &len);
		Log("%s", t ? t : "");
		return 0;
	}

	static int lIsArmed(hks::lua_State* L) {
		hks::pushinteger(L, (int)DevBridgeArmed);   // 0 off, 1 armed by the tool, 2 handshake done (stubs are recording), 3 finished
		return 1;
	}

	void RegisterGlobals(hks::lua_State* L) {
		hks::pushnamedcclosure(L, lRecordState, 0, "lDevCE_RecordState", 0);
		hks::setfield(L, hks::LUA_GLOBAL, "DevCE_RecordState");
		hks::pushnamedcclosure(L, lIsArmed, 0, "lDevCE_IsArmed", 0);
		hks::setfield(L, hks::LUA_GLOBAL, "DevCE_IsArmed");
		hks::pushnamedcclosure(L, lLog, 0, "lDevCE_Log", 0);
		hks::setfield(L, hks::LUA_GLOBAL, "DevCE_Log");
	}

	typedef uint64_t(__fastcall* RawFn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

	// Calls the native function inside an SEH guard (no C++ objects with destructors here, so __try is allowed). A hardware fault (bad pointer from a bad
	// argument, e.g. an out-of-range index) becomes a Lua error instead of killing the game. It cannot catch silent memory corruption or fast-fail aborts.
	static uint64_t CallGuarded(void* fn, const uint64_t* s, unsigned long* fault) {
		*fault = 0;
		__try {
			return ((RawFn)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
		} __except (*fault = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
			return 0;
		}
	}

	int Dispatch(hks::lua_State* L, int index) {
		const FnDesc& d = kFunctions[index];
		FnState& s = gStates[index];
		s.calls++;
		if (!s.ok) {
			hks::error(L, "DevBridge: %s is disabled (address check failed at startup)", d.luaName);
			return 0;
		}

		void* self = nullptr;
		int argBase = 2;                      // Lua stack index of the first argument (object calls: 1 is the object)
		if (d.ownerKind == 2) {
			typedef void* (__fastcall* AccFn)();
			self = ((AccFn)(Runtime::GameCoreAddress + (uintptr_t)d.navOff))();
			argBase = 1;
		} else {
			self = s.getInstance(L, 1, true);
		}
		if (self == nullptr) {
			hks::error(L, "DevBridge: %s needs a valid object as first argument (use ':' not '.')", d.luaName);
			return 0;
		}

		// Owner navigation: the Lua object is the owner (City, Unit, ...), the class of the function is a member of it.
		int pid = -1;
		if (d.ownerKind == 1) {   // Lua IPlayer = PlayerReference { PlayerTypes m_id }
			if (!gEditPlayer) { hks::error(L, "DevBridge: %s needs EditPlayer, which failed its address check", d.luaName); return 0; }
			pid = *(int*)self;
			if (pid < 0 || pid > 63) { hks::error(L, "DevBridge: %s: invalid player", d.luaName); return 0; }
			self = gEditPlayer(pid);
			if (self == nullptr) { hks::error(L, "DevBridge: %s: player not found", d.luaName); return 0; }
		}
		if (d.ownerKind == 2) {
			// this already resolved
		} else if (d.navMode == 3) {
			self = *(void**)((char*)self + d.navOff);
			if (self == nullptr) { hks::error(L, "DevBridge: %s: the player has no such component", d.luaName); return 0; }
		} else if (d.navMode == 4) {
			self = ((PlayerFn)(Runtime::GameCoreAddress + (uintptr_t)d.navOff))(pid);
		} else if (d.navMode == 1) {
			if (!gEdit) { hks::error(L, "DevBridge: %s needs FAutoVariable::edit, which failed its address check", d.luaName); return 0; }
			self = gEdit((char*)self + d.navOff);
		} else if (d.navMode == 2) {
			self = (char*)self + d.navOff;
		}

		// MSVC x64 ABI for member functions: slot 0 = this; a class that is not a plain aggregate (FixedPointT<8> has constructors and a private field) is
		//   * RETURNED through a hidden buffer whose address goes in the slot right after `this` (the callee returns that address in RAX), and
		//   * PASSED BY VALUE as a pointer to a caller-owned temporary copy.
		// (Learned the hard way: passing an argument in the hidden-buffer slot made the callee write its result to address 0.)
		uint64_t slots[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
		int32_t fixedTemp[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };   // copies of by-value FixedPoint arguments (kept alive until the call returns)
		int32_t retFixed = 0;                                   // hidden return buffer for a FixedPoint result
		int n = 0;
		slots[n++] = (uint64_t)self;
		if (d.retKind == R_FIXED) slots[n++] = (uint64_t)&retFixed;
		for (int i = 0; i < d.nargs; i++) {
			const int idx = argBase + i;
			switch (d.argKinds[i]) {
			case A_INT:  slots[n++] = (uint64_t)(int64_t)hks::checkinteger(L, idx); break;
			case A_UINT: slots[n++] = (uint64_t)(uint32_t)hks::checkinteger(L, idx); break;
			case A_BOOL: slots[n++] = hks::toboolean(L, idx) ? 1 : 0; break;
			case A_I64:  slots[n++] = (uint64_t)(int64_t)hks::checknumber(L, idx); break;
			case A_FIXED:
				fixedTemp[i] = (int32_t)llround(hks::checknumber(L, idx) * 256.0);   // Lua number -> raw fixed point (8 fractional bits)
				slots[n++] = (uint64_t)&fixedTemp[i];
				break;
			}
		}

		// Slots beyond the real argument count are ignored by the callee (caller-cleaned x64 convention).
		unsigned long fault = 0;
		const uint64_t r = CallGuarded(s.fn, slots, &fault);
		if (fault) {
			s.faults++;
			Log("[DevBridge] FAULT 0x%08lx in %s (fault #%lu): the call was aborted, game state may be inconsistent", fault, d.cname, s.faults);
			hks::error(L, "DevBridge: %s faulted (0x%08lx); check the arguments", d.luaName, fault);
			return 0;
		}

		switch (d.retKind) {
		case R_BOOL: hks::pushboolean(L, (uint8_t)r != 0); return 1;
		case R_INT:  hks::pushinteger(L, (int)(int32_t)r); return 1;
		case R_UINT: hks::pushnumber(L, (double)(uint32_t)r); return 1;
		case R_I64:  hks::pushnumber(L, (double)(int64_t)r); return 1;
		case R_FIXED: hks::pushnumber(L, (double)retFixed / 256.0); return 1;
		default: return 0;
		}
	}

	// The address table is only valid for one GameCore build (Steam build 15038592). Checked against the PE header of the loaded module, in addition to the
	// per-function prologue check (8 bytes can match a wrong function in another build).
	static const uint32_t kBuildTimeDateStamp = 0x667c6f5b;
	static const uint32_t kBuildSizeOfImage = 0x00c60000;

	static bool BuildMatches() {
		const char* base = (const char*)Runtime::GameCoreAddress;
		const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
		if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
		const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
		Log("[DevBridge] GameCore PE timestamp 0x%08x, image size 0x%08x", (unsigned)nt->FileHeader.TimeDateStamp, (unsigned)nt->OptionalHeader.SizeOfImage);
		return nt->FileHeader.TimeDateStamp == kBuildTimeDateStamp && nt->OptionalHeader.SizeOfImage == kBuildSizeOfImage;
	}

	void Create() {
		if (!BuildMatches()) {
			Log("[DevBridge] this GameCore is not the build the address table was made for (Steam 15038592): ALL Dev CE native functions are disabled");
			return;
		}
		int ok = 0, bad = 0;
		if (memcmp((void*)(Runtime::GameCoreAddress + kEditRva), kEditPrologue, 8) == 0) gEdit = (EditFn)(Runtime::GameCoreAddress + kEditRva);
		else Log("[DevBridge] FAutoVariable::edit has unexpected bytes: member-navigation functions are disabled");
		if (memcmp((void*)(Runtime::GameCoreAddress + kEditPlayerRva), kEditPlayerPrologue, 8) == 0) gEditPlayer = (PlayerFn)(Runtime::GameCoreAddress + kEditPlayerRva);
		else Log("[DevBridge] EditPlayer has unexpected bytes: player-member functions are disabled");
		for (int i = 0; i < kFunctionCount; i++) {
			const FnDesc& d = kFunctions[i];
			FnState& s = gStates[i];
			s.fn = (void*)(Runtime::GameCoreAddress + d.rva);
			s.getInstance = (GetInstanceFn)(Runtime::GameCoreAddress + d.getInstanceRva);
			s.calls = 0;
			const bool fnOk = memcmp(s.fn, d.prologue, 8) == 0;
			const bool giOk = d.ownerKind == 2 || memcmp((void*)s.getInstance, d.giPrologue, 8) == 0;
			const bool navOk = (d.navMode != 1 || gEdit != nullptr) && (d.ownerKind != 1 || gEditPlayer != nullptr) && (d.navMode != 4 && d.ownerKind != 2 || memcmp((void*)(Runtime::GameCoreAddress + (uintptr_t)d.navOff), d.navPrologue, 8) == 0);
			s.ok = fnOk && giOk && navOk;
			if (s.ok) ok++; else {
				bad++;
				Log("[DevBridge] address check failed for %s%s%s", d.cname, fnOk ? "" : " (function)", giOk ? "" : " (GetInstance)");
			}
		}
		Log("[DevBridge] %d native functions ready, %d disabled by the address check", ok, bad);
		InstallGeneratedHooks();
	}
}
