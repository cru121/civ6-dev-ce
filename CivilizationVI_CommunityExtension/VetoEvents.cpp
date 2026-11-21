// SPDX-License-Identifier: AGPL-3.0-only
// Part of Dev CE, a fork of the Civilization VI Community Extension by Wild-W (https://github.com/Wild-W/CivilizationVI_CommunityExtension).
// Copyright (C) 2026 cru121. Licensed under the GNU Affero General Public License v3.0 (see LICENSE.txt). Not endorsed by Wild-W, Firaxis or 2K.
#include "VetoEvents.h"
#include "GameProcessor.h"
#include "Runtime.h"
#include "DevBridge.h"
#include <cstdint>

namespace VetoEvents {
	// Offsets for installed build 15038592 (GameCore_XP2_FinalRelease.dll; same DLL in 15296837). Names are from the leaked-symbol build, mapped to this build.
	constexpr uintptr_t CITY_OPERATION_CAN_START_OFFSET = 0x14070;   // GameCore::City::Operations::Manager::CanStart(Manager*, City::Instance*, Operation*, bool isExclusionTest, TypedVariantMap* results)
	constexpr uintptr_t CITY_COMMAND_CAN_START_OFFSET = 0x13560;     // GameCore::City::Commands::Manager::CanStart(Manager*, City::Instance*, District::Instance*, Command*, bool, TypedVariantMap* results)
	// City::Instance layout (from City::Instance::SetComponentID): owner PlayerTypes at +0xd8, city id at +0xa8. Operation / Command: the type is the first int.
	constexpr uintptr_t CITY_OWNER_OFFSET = 0xd8;
	constexpr uintptr_t CITY_ID_OFFSET = 0xa8;

	typedef bool(__fastcall* CityOperationCanStart)(void* manager, void* city, void* operation, bool isExclusionTest, void* results);
	typedef bool(__fastcall* CityCommandCanStart)(void* manager, void* city, void* district, void* command, bool isExclusionTest, void* results);
	static CityOperationCanStart orig_CityOperationCanStart = nullptr;
	static CityCommandCanStart orig_CityCommandCanStart = nullptr;

	// A handler that calls back into a CanStart check (for example CityManager.CanStartOperation) must not trigger the event again on the same thread.
	static thread_local bool inDispatch = false;

	// Returns true when a Lua handler vetoed.
	static bool Vetoed(const char* eventName, void* city, void* typed) {
		if (inDispatch || city == nullptr || typed == nullptr) return false;
		int args[3] = {
			*reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(city) + CITY_OWNER_OFFSET),
			static_cast<int>(*reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(city) + CITY_ID_OFFSET)),
			*reinterpret_cast<int*>(typed)
		};
		inDispatch = true;
		bool result = true;
		bool called = GameProcessor::TestAll(eventName, args, 3, result);
		inDispatch = false;
		return called && !result;
	}

	static bool __fastcall Hook_CityOperationCanStart(void* manager, void* city, void* operation, bool isExclusionTest, void* results) {
		if (!orig_CityOperationCanStart(manager, city, operation, isExclusionTest, results)) return false;
		return !Vetoed("CanStartCityOperation", city, operation);
	}

	static bool __fastcall Hook_CityCommandCanStart(void* manager, void* city, void* district, void* command, bool isExclusionTest, void* results) {
		if (!orig_CityCommandCanStart(manager, city, district, command, isExclusionTest, results)) return false;
		return !Vetoed("CanStartCityCommand", city, command);
	}

	void Create() {
		using namespace Runtime;
		CreateHook(GetGameCoreGlobalAt<LPVOID>(CITY_OPERATION_CAN_START_OFFSET), reinterpret_cast<LPVOID>(&Hook_CityOperationCanStart), &orig_CityOperationCanStart);
		CreateHook(GetGameCoreGlobalAt<LPVOID>(CITY_COMMAND_CAN_START_OFFSET), reinterpret_cast<LPVOID>(&Hook_CityCommandCanStart), &orig_CityCommandCanStart);
		DevBridge::Log("[VetoEvents] hooks installed: GameEvents.CanStartCityOperation, GameEvents.CanStartCityCommand");
	}
}
