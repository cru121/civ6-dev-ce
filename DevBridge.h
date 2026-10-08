// SPDX-License-Identifier: AGPL-3.0-only
// Part of Dev CE, a fork of the Civilization VI Community Extension by Wild-W (https://github.com/Wild-W/CivilizationVI_CommunityExtension).
// Copyright (C) 2026 cru121. Licensed under the GNU Affero General Public License v3.0 (see LICENSE.txt). Not endorsed by Wild-W, Firaxis or 2K.
#pragma once
#include "HavokScript.h"
#include <cstdint>

// Dev CE native bridge: exposes engine member functions to Lua from a generated table (DevNativeTable.cpp, tools/gen_bridge.py).
// The Lua call  object:Method(a, b)  becomes  GameCore::Class::Method(this, a, b)  where `this` comes from the game's own
// ScopedInstance<I, Class>::GetInstance(L, 1) for the Lua object the method is called on.
// Every argument is passed as one 64-bit slot (Windows x64: ints, bools and enums share the integer registers), so the same
// generic call works for all exposed functions.
// Exported so tools can find Lua states: [0] = RegisterScriptData argument (NOT reliable as a gameplay state), [1] = UI cache state (RegisterScriptDataForUI),
// [2] = lua_State of the last DevCE_RecordState() call from a gameplay script, [3] = thread id of that call.
extern "C" __declspec(dllexport) extern void* DevBridgeStates[4];

namespace DevBridge {
	enum ArgKind : uint8_t { A_INT = 1, A_UINT, A_BOOL, A_I64, A_FIXED };   // A_FIXED: FixedPointT<8> (4-byte raw value, 8 fractional bits) passed by value in a register
	enum RetKind : uint8_t { R_VOID = 0, R_BOOL, R_INT, R_UINT, R_I64, R_FIXED };

	typedef void* (__cdecl* GetInstanceFn)(hks::lua_State*, int, bool);

	struct FnDesc {
		const char* cname;          // qualified C++ name, for logs
		const char* luaName;        // method name added to the Lua object
		uintptr_t rva;              // function, installed build
		uintptr_t getInstanceRva;   // ScopedInstance<I, C>::GetInstance
		uint8_t nargs;              // arguments after `this` (max 7)
		uint8_t argKinds[7];
		uint8_t retKind;
		uint8_t prologue[8];        // expected first bytes of the function (address check at startup)
		uint8_t giPrologue[8];      // same for GetInstance
		int32_t navOff;             // owner navigation: the class is a member of the object GetInstance returns, at this offset (installed build)
		uint8_t navMode;            // 0 = `this` is the Lua object itself, 1 = member wrapped in FAutoVariable (this = edit(owner + navOff)), 2 = embedded member (this = owner + navOff),
		                            // 3 = pointer member (this = *(owner + navOff)), 4 = the game's static accessor Get(PlayerTypes) at rva navOff (player root only)
		uint8_t ownerKind;          // 1 = the Lua object is a PlayerReference: the owner is the Player::Instance found with Context::Globals::EditPlayer(id)
		                            // 2 = static Lua table (Game, Map): no object argument; this = the game's zero-argument accessor at rva navOff, Lua arguments start at 1
		uint8_t navPrologue[8];     // mode 4: expected first bytes of the accessor
	};
	struct FnState {
		void* fn;
		GetInstanceFn getInstance;
		bool ok;                    // address checks passed
		unsigned long calls;        // how often the Lua wrapper was entered
		unsigned long faults;       // native calls aborted by the SEH guard
	};

	extern const FnDesc kFunctions[];   // generated
	extern FnState gStates[];           // generated storage
	extern const int kFunctionCount;    // generated
	extern const uintptr_t kEditRva;    // generated: FAutoVariable<T,Owner>::edit (one shared body for every T): takes the wrapper, returns the value
	extern const uint8_t kEditPrologue[8];
	extern const uintptr_t kEditPlayerRva;   // generated: Context::Globals::EditPlayer(PlayerTypes) -> Player::Instance&
	extern const uint8_t kEditPlayerPrologue[8];

	// Interfaces whose registration function CE already hooks (MinHook cannot hook one address twice): CE's own hook calls these (generated) right before it
	// calls the original registration function.
	void PushExtra_IPlayerInfluence(hks::lua_State* L, int t);
	void PushExtra_IPlayerCities(hks::lua_State* L, int t);
	void PushExtra_IPlayerGovernors(hks::lua_State* L, int t);
	void PushExtra_IMapPlot(hks::lua_State* L, int t);
	void PushExtra_IUnitManager(hks::lua_State* L, int t);

	int Dispatch(hks::lua_State* L, int index);   // called by the generated thin wrappers
	void InstallGeneratedHooks();                 // generated: hooks the PushMethods/RegisterMembers of each interface
	void Log(const char* fmt, ...);               // appends to DevBridge.log next to the DLL (and the CE console)
	void RegisterGlobals(hks::lua_State* L);      // adds the global function DevCE_RecordState() to a state
	void Create();                                // verify addresses, install hooks (call after Runtime::Create())
}
