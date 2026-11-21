#include "UnitManager.h"
#include "DevBridge.h"
#include "Runtime.h"
#include "ProxyTypes.h"

namespace UnitManager {
	Types::ChangeOwner ChangeOwner;
	Types::Get Get;
	Types::ChangeBuildCharges ChangeBuildCharges;

	ProxyTypes::RegisterMembers base_RegisterMembers;
	ProxyTypes::RegisterMembers orig_RegisterMembers;

	int lChangeOwner(hks::lua_State* L) {
		Manager* manager = Get();

		Unit::Instance* unit = Unit::GetInstance(L, 1, true);
		Unit::Instance** unitRef = &unit;

		int playerId = hks::checkplayerid(L, 2);
		bool b1 = hks::toboolean(L, 3);
		bool b2 = hks::toboolean(L, 4);

		ChangeOwner(manager, unit, playerId, b1, b2, unitRef);

		Unit::Push(L, *unitRef);
		return 1;
	}

	int lGetInstance(hks::lua_State* L) {
		Unit::Instance* unit = (Unit::Instance*)static_cast<uintptr_t>(hks::checknumber(L, 1));

		Unit::Push(L, unit);
		return 1;
	}

	// UnitManager.ChangeBuildCharges(unit, delta): adds delta (may be negative) to the unit's build charges; clamps at 0.
	int lChangeBuildCharges(hks::lua_State* L) {
		Unit::Instance* unit = Unit::GetInstance(L, 1, true);
		int delta = hks::checkinteger(L, 2);

		ChangeBuildCharges(unit, delta);
		return 0;
	}

	void RegisterMembers(hks::lua_State* L) {
		std::cout << "Hooked UnitManager::PushMethods!\n";

		DevBridge::PushExtra_IUnitManager(L, -2);   // Dev CE bridge methods
		PushLuaMethod(L, lChangeOwner, "lChangeOwner", -2, "ChangeOwner");
		PushLuaMethod(L, lGetInstance, "lGetInstance", -2, "GetInstance");
		PushLuaMethod(L, lChangeBuildCharges, "lChangeBuildCharges", -2, "ChangeBuildCharges");

		base_RegisterMembers(L);
	}

	void Create() {
		using namespace Runtime;

		ChangeOwner = GetGameCoreGlobalAt<Types::ChangeOwner>(CHANGE_OWNER_OFFSET);
		Get = GetGameCoreGlobalAt<Types::Get>(GET_OFFSET);
		ChangeBuildCharges = GetGameCoreGlobalAt<Types::ChangeBuildCharges>(CHANGE_BUILD_CHARGES_OFFSET);

		orig_RegisterMembers = GetGameCoreGlobalAt<ProxyTypes::RegisterMembers>(REGISTER_MEMBERS_OFFSET);
		CreateHook(orig_RegisterMembers, &RegisterMembers, &base_RegisterMembers);
	}
}