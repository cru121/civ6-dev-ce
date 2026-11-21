// SPDX-License-Identifier: AGPL-3.0-only
// Part of Dev CE, a fork of the Civilization VI Community Extension by Wild-W (https://github.com/Wild-W/CivilizationVI_CommunityExtension).
// Licensed under the GNU Affero General Public License v3.0 (see LICENSE.txt). Not endorsed by Wild-W, Firaxis or 2K.
#include "GreatPersonAssign.h"
#include "Runtime.h"
#include "Game.h"
#include "DevBridge.h"
#include <cstring>

namespace GreatPersonAssign {
	typedef void* (__cdecl* GetInstanceFn)(hks::lua_State*, int, bool);
	typedef void* (__cdecl* GetDefinitionsFn)();
	typedef void(__fastcall* SetFn)(void* greatPerson, int individual, int greatPersonClass, int charges);
	typedef void(__fastcall* DispatchFn)(void* signal, void* unit, int individual, int yield, int amount);

	static GetInstanceFn GetInstance = nullptr;
	static GetDefinitionsFn GetIndividualDefinitions = nullptr;
	static SetFn Set = nullptr;
	static DispatchFn DispatchCreated = nullptr;
	static void** CurrentGame = nullptr;   // &Context::Globals::ms_pkCurrentGame
	static bool gOk = false;

	struct Check { const char* name; uintptr_t rva; uint8_t bytes[8]; size_t n; };
	static const Check kChecks[] = {
		{ "Definitions::GetGreatPersonIndividualDefinitions", GET_INDIVIDUAL_DEFINITIONS_OFFSET, { 0x48, 0x83, 0xec, 0x28, 0xe8, 0x67, 0x40, 0x87 }, 8 },
		{ "Unit::GreatPerson::Set", GREAT_PERSON_SET_OFFSET, { 0x48, 0x83, 0xec, 0x38, 0x48, 0x8b, 0x01, 0x89 }, 8 },
		{ "ProtoSignal<GreatPersonCreated>::Dispatch", CREATED_SIGNAL_DISPATCH_OFFSET, { 0x48, 0x8b, 0xc4, 0x53, 0x55, 0x41, 0x54, 0x41 }, 8 },
		{ "ScopedInstance<IUnitGreatPerson>::GetInstance", GET_INSTANCE_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c }, 8 },
		{ "FAutoVariable::edit", Game::F_AUTO_VARIABLE_EDIT_OFFSET, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b }, 8 },
		{ "read of ms_pkCurrentGame", CURRENT_GAME_READ_SITE, { 0x48, 0x8b, 0x0d, 0x52, 0x53, 0x53, 0x00 }, 7 },
	};

	void Create() {
		for (const Check& c : kChecks) {
			if (memcmp((void*)(Runtime::GameCoreAddress + c.rva), c.bytes, c.n) != 0) {
				DevBridge::Log("[GreatPersonAssign] %s has unexpected bytes at 0x%llx: SetGreatPersonType is disabled", c.name, (unsigned long long)c.rva);
				return;
			}
		}
		GetInstance = Runtime::GetGameCoreGlobalAt<GetInstanceFn>(GET_INSTANCE_OFFSET);
		GetIndividualDefinitions = Runtime::GetGameCoreGlobalAt<GetDefinitionsFn>(GET_INDIVIDUAL_DEFINITIONS_OFFSET);
		Set = Runtime::GetGameCoreGlobalAt<SetFn>(GREAT_PERSON_SET_OFFSET);
		DispatchCreated = Runtime::GetGameCoreGlobalAt<DispatchFn>(CREATED_SIGNAL_DISPATCH_OFFSET);
		CurrentGame = Runtime::GetGameCoreGlobalAt<void**>(CURRENT_GAME_OFFSET);
		gOk = true;
		DevBridge::Log("[GreatPersonAssign] address checks passed: UnitGreatPerson:SetGreatPersonType is available");
	}

	enum Result { R_OK = 0, R_NO_UNIT, R_HAS_INDIVIDUAL, R_BAD_INDEX, R_NO_CLASS, R_EDIT_MISMATCH, R_NO_GAME, R_FAULT };

	// All native work, inside an SEH guard (no C++ objects with destructors in this function, so __try is allowed).
	// A hardware fault becomes a Lua error instead of killing the game; it cannot catch silent memory corruption.
	static Result Assign(void* gp, int individual, int* outClass, int* outUnitId, unsigned long* fault) {
		*fault = 0;
		__try {
			char* unit = *(char**)((char*)gp + GP_UNIT);
			if (unit == nullptr) return R_NO_UNIT;
			*outUnitId = *(int*)(unit + UNIT_ID);
			// Re-assigning would attach a second set of birth modifiers; the old ones cannot be removed yet.
			if (*(int*)((char*)gp + GP_INDIVIDUAL) != -1) return R_HAS_INDIVIDUAL;

			char* defs = (char*)GetIndividualDefinitions();                   // DefinitionCollection: [0] begin, [8] end of a vector of definition pointers
			char** begin = *(char***)(defs);
			char** end = *(char***)(defs + 8);
			if (individual < 0 || individual >= (int)(end - begin) || begin[individual] == nullptr) return R_BAD_INDEX;
			char* classDef = *(char**)(begin[individual] + INDIVIDUAL_CLASS_DEF);
			if (classDef == nullptr) return R_NO_CLASS;
			const int greatPersonClass = *(int*)(classDef + CLASS_INDEX);
			*outClass = greatPersonClass;

			// edit() marks the synced variable as changed; it must hand back the object Lua gave us.
			void* edited = Game::FAutoVariable_edit(unit + UNIT_GREAT_PERSON_VAR);
			if (edited != gp) return R_EDIT_MISMATCH;
			char* game = (char*)*CurrentGame;
			if (game == nullptr) return R_NO_GAME;
			char* events = *(char**)(game + GAME_EVENTS);
			if (events == nullptr) return R_NO_GAME;

			const int charges = *(int*)((char*)gp + GP_CHARGES);              // left unchanged
			Set(edited, individual, greatPersonClass, charges);
			// Same arguments as Game::GreatPeople::CreatePerson: no yield, amount 0.
			DispatchCreated(events + CREATED_SIGNAL, unit, individual, -1, 0);
			return R_OK;
		} __except (*fault = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
			return R_FAULT;
		}
	}

	// greatPerson:SetGreatPersonType(individualIndex) -> true
	// individualIndex = GameInfo.GreatPersonIndividuals[...].Index
	static int lSetGreatPersonType(hks::lua_State* L) {
		if (!gOk) { hks::error(L, "SetGreatPersonType is disabled (address check failed at startup, see DevBridge.log)"); return 0; }
		void* gp = GetInstance(L, 1, true);
		if (gp == nullptr) { hks::error(L, "SetGreatPersonType needs a great person object (unit:GetGreatPerson():SetGreatPersonType(index), with ':')"); return 0; }
		const int individual = hks::checkinteger(L, 2);

		int greatPersonClass = -1, unitId = -1;
		unsigned long fault = 0;
		const Result r = Assign(gp, individual, &greatPersonClass, &unitId, &fault);
		switch (r) {
		case R_OK:
			DevBridge::Log("[GreatPersonAssign] unit %d is now great person individual %d (class %d)", unitId, individual, greatPersonClass);
			hks::pushboolean(L, true);
			return 1;
		case R_NO_UNIT: hks::error(L, "SetGreatPersonType: the great person object has no unit"); return 0;
		case R_HAS_INDIVIDUAL: hks::error(L, "SetGreatPersonType: unit %d is already a great person (changing it is not supported: its first passive cannot be removed)", unitId); return 0;
		case R_BAD_INDEX: hks::error(L, "SetGreatPersonType: %d is not a GreatPersonIndividuals index", individual); return 0;
		case R_NO_CLASS: hks::error(L, "SetGreatPersonType: individual %d has no great person class", individual); return 0;
		case R_EDIT_MISMATCH: hks::error(L, "SetGreatPersonType: unexpected unit layout (edit() did not return the great person object); nothing was changed"); return 0;
		case R_NO_GAME: hks::error(L, "SetGreatPersonType: no game is running"); return 0;
		default:
			DevBridge::Log("[GreatPersonAssign] FAULT 0x%08lx in SetGreatPersonType(unit %d, individual %d): the call was aborted, game state may be inconsistent", fault, unitId, individual);
			hks::error(L, "SetGreatPersonType faulted (0x%08lx)", fault);
			return 0;
		}
	}

	void PushMethods(hks::lua_State* L, int t) {
		if (!gOk) return;
		PushLuaMethod(L, lSetGreatPersonType, "lDevSetGreatPersonType", t, "SetGreatPersonType");
	}
}

namespace DevBridge {
	// Declared in DevBridge.h; called by the generated IUnitGreatPerson hook (DevNativeTable.cpp).
	void HandExtra_IUnitGreatPerson(hks::lua_State* L, int t) { GreatPersonAssign::PushMethods(L, t); }
}
