// SPDX-License-Identifier: AGPL-3.0-only
#include "GoldPurchase.h"
#include "Runtime.h"
#include "DevBridge.h"
#include <cstdio>
#include <cstring>

namespace GoldPurchase {
	typedef bool(__fastcall* CanStartFn)(void* handler, void* city, void* district, void* command, bool testOnly, void* results);
	typedef bool(__fastcall* ValidateFn)(void* contract, void* economic);
	typedef void* (__fastcall* GetDefinitionsFn)();
	typedef uint32_t(__fastcall* MakeHashFn)(const char*);
	typedef const uint8_t* (__fastcall* VariantGetFn)(void* map, uint32_t key);
	typedef void* (__fastcall* ParametersFn)(void* handler, void* raw);

	static ValidateFn base_Validate;
	static GetDefinitionsFn GetUnitDefinitions, GetBuildingDefinitions;
	static MakeHashFn MakeHash;
	static const int* gYieldGold = nullptr;
	static uint32_t gGoldHash = 0, gAllUnits = 0, gAllBuildings = 0;
	static bool gOk = false;

	struct Check { const char* name; uintptr_t rva; uint8_t bytes[16]; size_t n; };
	static const Check kChecks[] = {
		{ "City::Commands::Purchase::CanStart", PURCHASE_CAN_START_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 }, 16 },
		{ "Purchase::CanStart: parameters from Command +8 (vtable +0x40)", PURCHASE_CAN_START_OFFSET + 0x1a, { 0x48, 0x8b, 0xfa, 0x49, 0x8b, 0x51, 0x08, 0xff, 0x50, 0x40 }, 10 },
		{ "City::Commands::XP1::Purchase::CanStart", XP1_PURCHASE_CAN_START_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83 }, 16 },
		{ "XP1 Purchase::CanStart: parameters from Command +8 (vtable +0x40)", XP1_PURCHASE_CAN_START_OFFSET + 0x18, { 0x49, 0x8b, 0x51, 0x08, 0x49, 0x8b, 0xe9, 0x4d, 0x8b, 0xf0, 0x48, 0x8b, 0xf1, 0xff, 0x50, 0x40 }, 16 },
		{ "City::Commands::XP2::Purchase::CanStart", XP2_PURCHASE_CAN_START_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 }, 16 },
		{ "XP2 Purchase::CanStart: parameters from Command +8 (vtable +0x40)", XP2_PURCHASE_CAN_START_OFFSET + 0x22, { 0x49, 0x8b, 0x51, 0x08, 0xff, 0x50, 0x40 }, 7 },
		// building branch: mov eax,[YIELD_GOLD]; ...; mov r8d,[rdi+0x1c]; cmp [rdi+0x24],eax (type +0x1c, yield +0x24)
		{ "XP2 Purchase::CanStart: YIELD_GOLD, parameter type / yield", XP2_PURCHASE_CAN_START_OFFSET + 0x28a, { 0x8b, 0x05, 0x80, 0x04, 0x44, 0x00, 0x41, 0x8b, 0xd6, 0x44, 0x8b, 0x47, 0x1c, 0x39, 0x47, 0x24 }, 16 },
		{ "AI UnitRequestContract::Validate(Economic)", AI_VALIDATE_GOLD_OFFSET, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x83, 0xb9, 0xa8, 0x01, 0x00, 0x00, 0xff, 0x48, 0x8b, 0xd9 }, 16 },
		{ "Validate(Economic): unit +0x178", AI_VALIDATE_GOLD_OFFSET + 0x1f, { 0x8b, 0xb9, 0x78, 0x01, 0x00, 0x00 }, 6 },
		{ "Validate(Economic): unit PurchaseYield == gold", AI_VALIDATE_GOLD_OFFSET + 0x46, { 0x81, 0x7f, 0x2c, 0x4a, 0x63, 0x61, 0xb7 }, 7 },
		{ "Validate(Economic): building +0x190", AI_VALIDATE_GOLD_OFFSET + 0x72, { 0x8b, 0xb9, 0x90, 0x01, 0x00, 0x00 }, 6 },
		{ "Validate(Economic): wonder flag +0x159 & 0x10", AI_VALIDATE_GOLD_OFFSET + 0xa3, { 0xf6, 0x80, 0x59, 0x01, 0x00, 0x00, 0x10 }, 7 },
		{ "Validate(Economic): building PurchaseYield == gold", AI_VALIDATE_GOLD_OFFSET + 0xce, { 0x81, 0x7f, 0x20, 0x4a, 0x63, 0x61, 0xb7 }, 7 },
		{ "AI::Diplomatic::GetPlayerId (Economic +0x98 = player)", 0x4577a0, { 0x48, 0x8b, 0x81, 0x98, 0x00, 0x00, 0x00, 0x8b, 0x80, 0xd8, 0x00, 0x00, 0x00, 0xc3 }, 14 },
		// Validate(Religious): mov rbx,[player+0x6d0]; mov rbx,[rbx+0xd0] (city list), node +0x10 = next
		{ "player cities +0x6d0, list +0xd0", 0x3fb23e, { 0x48, 0x8b, 0x99, 0xd0, 0x06, 0x00, 0x00, 0x48, 0x8b, 0x9b, 0xd0, 0x00, 0x00, 0x00 }, 14 },
		{ "city list node +0x10 = next", 0x3fb279, { 0x48, 0x8b, 0x5b, 0x10 }, 4 },
		{ "Definitions::GetUnitDefinitions", GET_UNIT_DEFINITIONS_OFFSET, { 0x48, 0x83, 0xec, 0x28, 0xe8, 0xc7, 0x32, 0x87, 0xff, 0x48, 0x05, 0x30 }, 12 },
		{ "Definitions::GetBuildingDefinitions", GET_BUILDING_DEFINITIONS_OFFSET, { 0x48, 0x83, 0xec, 0x28, 0xe8, 0x27, 0x49, 0x87, 0xff, 0x48, 0x05, 0x20 }, 12 },
		{ "Utilities::MakeHash", MAKE_HASH_OFFSET, { 0x48, 0x8b, 0xd1, 0x41, 0x83, 0xc8, 0xff, 0x48 }, 8 },
		{ "Unit::SetUnitType: string +0xb0", 0x7dc2e5, { 0x48, 0x81, 0xc1, 0xb0, 0x00, 0x00, 0x00 }, 7 },
		{ "Unit::SetPromotionClass: string +0x170, flag +0x168", 0x7db341, { 0x48, 0x8d, 0x8b, 0x70, 0x01, 0x00, 0x00, 0xc6, 0x83, 0x68, 0x01, 0x00, 0x00, 0x01 }, 14 },
		{ "Unit::SetPurchaseYield: flag +0x188", 0x7db5d8, { 0xc6, 0x83, 0x88, 0x01, 0x00, 0x00, 0x01 }, 7 },
		{ "Unit::SetPurchaseYield: hash +0x2c", 0x7db5fa, { 0x89, 0x43, 0x2c }, 3 },
		{ "Building::Building: type string +0x60", 0x7a2055, { 0x48, 0x89, 0x43, 0x60 }, 4 },
		{ "Building::SetPrereqDistrict: string +0x100, flag +0xf8", 0x7d92c1, { 0x48, 0x8d, 0x8b, 0x00, 0x01, 0x00, 0x00, 0xc6, 0x83, 0xf8, 0x00, 0x00, 0x00, 0x01 }, 14 },
		{ "Building::SetPurchaseYield: flag +0x118", 0x7db528, { 0xc6, 0x83, 0x18, 0x01, 0x00, 0x00, 0x01 }, 7 },
		{ "Building::SetPurchaseYield: hash +0x20", 0x7db54a, { 0x89, 0x43, 0x20 }, 3 },
		{ "Lua City:GetProperty: properties +0x1a0", 0x6c929e, { 0x4c, 0x8d, 0x80, 0xa0, 0x01, 0x00, 0x00 }, 7 },
	};

	// ---- definitions and city properties -----------------------------------------------------------------------------------
	static const char* Def(GetDefinitionsFn get, int index) {
		if (index < 0) return nullptr;
		const char* const* v = (const char* const*)get();
		if (v == nullptr) return nullptr;
		const char* const* begin = (const char* const*)v[0];
		const char* const* end = (const char* const*)v[1];
		if (begin == nullptr || (size_t)index >= (size_t)(end - begin)) return nullptr;
		return begin[index];
	}

	static int CityProperty(void* city, uint32_t key) {
		void* map = (char*)city + CITY_PROPERTIES;
		const uint8_t* v = ((VariantGetFn)(*(void***)map)[0x60 / 8])(map, key);
		if (v == nullptr) return 0;
		const uint16_t type = *(const uint16_t*)v;
		const uint32_t flags = *(const uint32_t*)(v + 0x14);
		const bool isNumber = ((flags >> 29) & 1) == 0 && type <= 0x15 && ((0x30002eu >> type) & 1) != 0;
		return isNumber ? *(const int*)(v + 8) : 0;
	}

	static uint32_t KeyFor(const char* name) {
		char buf[160];
		snprintf(buf, sizeof(buf), "%s%s", PREFIX, name);
		return MakeHash(buf);
	}

	static bool Has(void* city, const char* name) {
		return name != nullptr && name[0] != '\0' && CityProperty(city, KeyFor(name)) >= 1;
	}

	// kind 0 = unit, 1 = building. Build-only (no PurchaseYield), not a wonder.
	static bool Eligible(int kind, const char* def) {
		if (kind == 0) return *(const bool*)(def + UNIT_DEF_HAS_PURCHASE_YIELD) == false;
		return *(const bool*)(def + BUILDING_DEF_HAS_PURCHASE_YIELD) == false && (*(const uint8_t*)(def + BUILDING_DEF_FLAGS) & 0x10) == 0;
	}

	static bool IsGoldItem(int kind, const char* def) {
		if (kind == 0) return *(const bool*)(def + UNIT_DEF_HAS_PURCHASE_YIELD) && *(const uint32_t*)(def + UNIT_DEF_PURCHASE_YIELD_HASH) == gGoldHash;
		return *(const bool*)(def + BUILDING_DEF_HAS_PURCHASE_YIELD) && *(const uint32_t*)(def + BUILDING_DEF_PURCHASE_YIELD_HASH) == gGoldHash;
	}

	static bool Unlocked(void* city, int kind, const char* def) {
		if (city == nullptr) return false;
		if (kind == 0) {
			if (CityProperty(city, gAllUnits) >= 1) return true;
			if (Has(city, *(const char* const*)(def + UNIT_DEF_TYPE))) return true;
			return *(const bool*)(def + UNIT_DEF_HAS_PROMOTION_CLASS) && Has(city, *(const char* const*)(def + UNIT_DEF_PROMOTION_CLASS));
		}
		if (CityProperty(city, gAllBuildings) >= 1) return true;
		if (Has(city, *(const char* const*)(def + BUILDING_DEF_TYPE))) return true;
		return *(const bool*)(def + BUILDING_DEF_HAS_DISTRICT) && Has(city, *(const char* const*)(def + BUILDING_DEF_DISTRICT));
	}

	static const char* DefOf(int kind, int index) {
		return Def(kind == 0 ? GetUnitDefinitions : GetBuildingDefinitions, index);
	}

	// ---- the purchase command ----------------------------------------------------------------------------------------------
	static bool Allowed(void* handler, void* city, void* command) {
		if (command == nullptr) return true;
		const char* p = (const char*)((ParametersFn)(*(void***)handler)[HANDLER_VT_PARAMETERS / 8])(handler, *(void**)((char*)command + 8));
		if (p == nullptr) return true;
		const int kind = *(const int*)(p + PARAM_KIND);
		if (kind != 0 && kind != 1) return true;
		if (*(const int*)(p + PARAM_YIELD) != *gYieldGold) return true;
		const char* def = DefOf(kind, *(const int*)(p + PARAM_TYPE));
		if (def == nullptr || IsGoldItem(kind, def)) return true;
		return Eligible(kind, def) && Unlocked(city, kind, def);
	}

	template <int I> struct CanStartHook {
		static CanStartFn orig;
		static bool __fastcall Hook(void* handler, void* city, void* district, void* command, bool testOnly, void* results) {
			if (!orig(handler, city, district, command, testOnly, results)) return false;
			return Allowed(handler, city, command);
		}
	};
	template <int I> CanStartFn CanStartHook<I>::orig = nullptr;

	// ---- the AI ----------------------------------------------------------------------------------------------------------
	static bool __fastcall Hook_Validate(void* contract, void* economic) {
		if (base_Validate(contract, economic)) return true;
		const char* c = (const char*)contract;
		if (*(const int*)(c + CONTRACT_DISTRICT) != -1 || economic == nullptr) return false;
		int kind = 0, index = *(const int*)(c + CONTRACT_UNIT);
		if (index == -1) { kind = 1; index = *(const int*)(c + CONTRACT_BUILDING); }
		const char* def = DefOf(kind, index);
		if (def == nullptr || !Eligible(kind, def)) return false;
		char* player = *(char**)((char*)economic + ECONOMIC_PLAYER);
		if (player == nullptr) return false;
		char* cities = *(char**)(player + PLAYER_CITIES);
		if (cities == nullptr) return false;
		const uint32_t cityId = *(const uint32_t*)(c + CONTRACT_CITY);
		for (void** node = *(void***)(cities + CITIES_LIST); node != nullptr; node = (void**)node[2]) {
			void* city = node[0];
			if (city == nullptr) continue;
			if (cityId != 0xffffffffu && *(const uint32_t*)((char*)city + CITY_ID) != cityId) continue;
			if (Unlocked(city, kind, def)) return true;
		}
		return false;
	}

	// ---- install ---------------------------------------------------------------------------------------------------------
	static bool Verify(uintptr_t b) {
		for (const Check& c : kChecks) {
			if (memcmp((void*)(b + c.rva), c.bytes, c.n) != 0) {
				DevBridge::Log("[GoldPurchase] %s has unexpected bytes at 0x%llx: gold purchase unlocks are not available", c.name, (unsigned long long)c.rva);
				return false;
			}
		}
		return true;
	}

	template <typename T>
	static bool Hook(uintptr_t b, uintptr_t rva, void* detour, T* original) {
		LPVOID target = (LPVOID)(b + rva);
		if (MH_CreateHook(target, detour, (LPVOID*)original) != MH_OK) return false;
		return MH_EnableHook(target) == MH_OK;
	}

	void Create() {
		if (gOk) return;   // hooks stay installed when Dev CE starts again in the same process
		const uintptr_t b = Runtime::GameCoreAddress;
		if (!Verify(b)) return;
		GetUnitDefinitions = (GetDefinitionsFn)(b + GET_UNIT_DEFINITIONS_OFFSET);
		GetBuildingDefinitions = (GetDefinitionsFn)(b + GET_BUILDING_DEFINITIONS_OFFSET);
		MakeHash = (MakeHashFn)(b + MAKE_HASH_OFFSET);
		gYieldGold = (const int*)(b + YIELD_GOLD_GLOBAL);
		gGoldHash = MakeHash("YIELD_GOLD");
		gAllUnits = KeyFor("ALL_UNITS");
		gAllBuildings = KeyFor("ALL_BUILDINGS");
		const bool ok = Hook(b, PURCHASE_CAN_START_OFFSET, (void*)&CanStartHook<0>::Hook, &CanStartHook<0>::orig)
			&& Hook(b, XP1_PURCHASE_CAN_START_OFFSET, (void*)&CanStartHook<1>::Hook, &CanStartHook<1>::orig)
			&& Hook(b, XP2_PURCHASE_CAN_START_OFFSET, (void*)&CanStartHook<2>::Hook, &CanStartHook<2>::orig)
			&& Hook(b, AI_VALIDATE_GOLD_OFFSET, (void*)&Hook_Validate, &base_Validate);
		if (!ok) {
			DevBridge::Log("[GoldPurchase] a hook could not be installed: gold purchase unlocks are not available");
			return;
		}
		gOk = true;
		DevBridge::Log("[GoldPurchase] hooks installed: city property %s<type / class / district / ALL_UNITS / ALL_BUILDINGS> unlocks gold purchase of build-only items", PREFIX);
	}
}
