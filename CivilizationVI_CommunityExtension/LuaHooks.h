// SPDX-License-Identifier: AGPL-3.0-only
// Part of Dev CE, a fork of the Civilization VI Community Extension by Wild-W (https://github.com/Wild-W/CivilizationVI_CommunityExtension).
// Copyright (C) 2026 cru121. Licensed under the GNU Affero General Public License v3.0 (see LICENSE.txt). Not endorsed by Wild-W, Firaxis or 2K.
#pragma once
#include "HavokScript.h"
#include <cstddef>

// Working Lua hooks for the retail HavokScript VM: debug.sethook / debug.gethook with call, return, line and count events.
// The retail interpreter never reads the hook fields that lua_sethook stores in the lua_State, so we patch the VM's per-instruction fetch site
// (HavokScript_FinalRelease.dll, only builds 15038592 / 15296837, byte-pattern checked) on the first debug.sethook call. Design notes and the
// measurements behind it: dev-ce/DESIGN_LUA_HOOKS.md (M0 results).
namespace LuaHooks {
	// Raw HavokScript exports used by Dev CE's hook and debugger code (resolved on first use; GetApi() returns nullptr when one is missing).
	typedef void(__cdecl* HookFnRaw)(hks::lua_State*, void*);
	struct Api {
		int(__cdecl* getstack)(hks::lua_State*, int, void*) = nullptr;
		int(__cdecl* getinfo)(hks::lua_State*, const char*, void*) = nullptr;
		int(__cdecl* sethook)(hks::lua_State*, HookFnRaw, int, int) = nullptr;
		void(__cdecl* checkstack)(hks::lua_State*, int, const char*) = nullptr;
		int(__cdecl* type)(hks::lua_State*, int) = nullptr;
		const void*(__cdecl* topointer)(hks::lua_State*, int) = nullptr;
		const char* (__cdecl* typenameOf)(hks::lua_State*, int) = nullptr;
		void(__cdecl* settop)(hks::lua_State*, int) = nullptr;
		int(__cdecl* gettop)(hks::lua_State*) = nullptr;
		void(__cdecl* pushnil)(hks::lua_State*) = nullptr;
		void(__cdecl* rawget)(hks::lua_State*, int) = nullptr;
		void(__cdecl* rawset)(hks::lua_State*, int) = nullptr;
		const char* (__cdecl* tolstring)(hks::lua_State*, int, size_t*) = nullptr;
		double(__cdecl* tonumber)(hks::lua_State*, int) = nullptr;
		int(__cdecl* luaopen_debug)(hks::lua_State*) = nullptr;
		int(__cdecl* loadstring)(hks::lua_State*, const char*) = nullptr;
		const char*(__cdecl* getlocal)(hks::lua_State*, void*, int) = nullptr;
		const char*(__cdecl* getupvalue)(hks::lua_State*, int, int) = nullptr;
		bool ok = false;
	};
	const Api* GetApi();

	// Mask bits of a hook (same as PUC Lua): call, return, line, count.
	enum { HOOK_CALL = 1, HOOK_RET = 2, HOOK_LINE = 4, HOOK_COUNT = 8 };
	// Native hook for one state (used by the debugger): fn(L, lua_Debug*) runs on the thread that executes L, inside the VM loop. fn == nullptr or mask == 0 removes it.
	// Installs the VM patch on first use. Does not touch the Lua-level hook table of debug.sethook (do not mix both on one state).
	typedef void(__cdecl* NativeHook)(hks::lua_State*, void* luaDebug);
	// clearCaches = false is for arming a fresh state from the arm handler (the pc -> line cache stays valid).
	bool SetNativeHook(hks::lua_State* L, NativeHook fn, int mask, int count, bool clearCaches = true);

	// Arm mode (debugger): while the flag is set, every VM instruction of a state that has no hook calls the handler on the thread running that state, so the handler can
	// give it a native hook without touching states of other threads. SetArmFlag(true) installs the VM patch; the flag is checked in the stub's fast path.
	typedef void(__cdecl* ArmHandler)(hks::lua_State*);
	void SetArmHandler(ArmHandler fn);
	bool SetArmFlag(bool on);

	// Statistics of the slow path (plain counters, races only lose counts): VM instructions that went through the C++ side (hooked states, or any state while the arm
	// flag is set), line events delivered to the hook, line events suppressed by the line filter.
	struct Stats { unsigned long long fetchSlow, lineDelivered, lineFiltered; };
	Stats GetStats();
	// Line filter for ONE native hook function: while hook `forHook` is installed on a state, line events are only delivered when fn(L, line) is true. Saves the cost of
	// building the lua_Debug record and entering the hook for lines that cannot matter (breakpoints elsewhere, no stepping).
	typedef bool(__cdecl* LineFilter)(hks::lua_State*, int line);
	void SetLineFilter(NativeHook forHook, LineFilter fn);

	// Adds debug.sethook / debug.gethook to the global `debug` table of this state (creating the table with HavokScript's own luaopen_debug if it is missing).
	void RegisterGlobals(hks::lua_State* L);

	// True once the VM patch is installed (first successful debug.sethook with a non-zero mask).
	bool IsInstalled();
}
