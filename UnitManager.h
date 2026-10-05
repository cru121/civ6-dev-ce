#pragma once
#include "HavokScript.h"
#include "Unit.h"

namespace UnitManager {
	typedef class Manager;
	namespace Types {
		typedef void(__thiscall* ChangeOwner)(Manager* manager, Unit::Instance* unit, int playerId, bool b1, bool b2, Unit::Instance** unit2Ref);
		typedef Manager* (__cdecl* Get)(void);
		typedef void(__thiscall* ChangeBuildCharges)(Unit::Instance* unit, int delta);
	}

	extern int lChangeOwner(hks::lua_State* L);
	extern int lGetInstance(hks::lua_State* L);
	extern int lChangeBuildCharges(hks::lua_State* L);

	// GameCore::Unit::Instance::ChangeBuildCharges(int delta) - installed build 15038592
	constexpr uintptr_t CHANGE_BUILD_CHARGES_OFFSET = 0x3a2980;
	extern Types::ChangeBuildCharges ChangeBuildCharges;

	constexpr uintptr_t CHANGE_OWNER_OFFSET = 0x64f420;
	extern Types::ChangeOwner ChangeOwner;

	constexpr uintptr_t GET_OFFSET = 0x64fff0;
	extern Types::Get Get;

	constexpr uintptr_t REGISTER_MEMBERS_OFFSET = 0x700980;
	extern void RegisterMembers(hks::lua_State* L);

	extern void Create();
}