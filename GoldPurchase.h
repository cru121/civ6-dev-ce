// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstdint>

// Gold purchase of build-only items, unlocked per city by a city property (like the game's faith effects
// EFFECT_ENABLE_UNIT_FAITH_PURCHASE / EFFECT_ENABLE_BUILDING_FAITH_PURCHASE / EFFECT_ENABLE_SPECIFIC_BUILDING_FAITH_PURCHASE).
//
// A unit or building without a PurchaseYield (build only; wonders excluded) can be bought with gold in a city whose property
//   DEVCE_GOLD_PURCHASE_<UnitType>          one unit                (e.g. DEVCE_GOLD_PURCHASE_UNIT_SETTLER)
//   DEVCE_GOLD_PURCHASE_<PromotionClass>    every unit of a class   (e.g. DEVCE_GOLD_PURCHASE_PROMOTION_CLASS_MELEE)
//   DEVCE_GOLD_PURCHASE_ALL_UNITS
//   DEVCE_GOLD_PURCHASE_<BuildingType>      one building            (e.g. DEVCE_GOLD_PURCHASE_BUILDING_MONUMENT)
//   DEVCE_GOLD_PURCHASE_<PrereqDistrict>    every building of that district (e.g. DEVCE_GOLD_PURCHASE_DISTRICT_CAMPUS)
//   DEVCE_GOLD_PURCHASE_ALL_BUILDINGS
// is >= 1 (MODIFIER_SINGLE_CITY_ADJUST_PROPERTY, or EFFECT_ADJUST_CITY_PROPERTY with a city collection).
//
// The game does not check PurchaseYield when a purchase starts (only the production panel and the AI do), so:
//   City::Commands::Purchase::CanStart (base, XP1, XP2)   a gold purchase of a unit / building is allowed only when its
//                                                         PurchaseYield is gold, or it is build-only, not a wonder and unlocked
//                                                         in that city. Faith purchases are unchanged.
//   AI::Control::UnitRequestContract::Validate(Economic&) the AI's "can I buy this with gold" also accepts unlocked items (in the
//                                                         contract's city, or any city when it has none); the purchase then only
//                                                         goes through in a city where it is unlocked (CanStart above).
// The production panel lists them through the DevCE_GoldPurchase mod (wraps ProductionPanel GetData).
namespace GoldPurchase {
	constexpr uintptr_t PURCHASE_CAN_START_OFFSET = 0x6803c0;         // City::Commands::Purchase::CanStart(handler, city, district, const Command&, bool, results)
	constexpr uintptr_t XP1_PURCHASE_CAN_START_OFFSET = 0x71be20;     // City::Commands::XP1::Purchase::CanStart (same signature)
	constexpr uintptr_t XP2_PURCHASE_CAN_START_OFFSET = 0x74ef40;     // City::Commands::XP2::Purchase::CanStart (same signature)
	constexpr uintptr_t AI_VALIDATE_GOLD_OFFSET = 0x3fb0c0;           // AI::Control::UnitRequestContract::Validate(contract, AI::Economic&) (vtable +0x80)
	constexpr uintptr_t GET_UNIT_DEFINITIONS_OFFSET = 0x7d1b00;       // Definitions::GetUnitDefinitions() -> vector<const Unit*>
	constexpr uintptr_t GET_BUILDING_DEFINITIONS_OFFSET = 0x7d04a0;   // Definitions::GetBuildingDefinitions() -> vector<const Building*>
	constexpr uintptr_t MAKE_HASH_OFFSET = 0x606270;
	constexpr uintptr_t YIELD_GOLD_GLOBAL = 0xb8f650;                 // int GameCore::YIELD_GOLD (YieldTypes value, set when the database loads)

	// Purchase command parameters (handler vtable +0x40 maps Command +8 to them)
	constexpr uintptr_t HANDLER_VT_PARAMETERS = 0x40;
	constexpr uintptr_t PARAM_KIND = 0x18;            // 0 unit, 1 building, 2 district, 4 plot
	constexpr uintptr_t PARAM_TYPE = 0x1c;            // UnitTypes / BuildingTypes index
	constexpr uintptr_t PARAM_YIELD = 0x24;           // YieldTypes

	// Definitions
	constexpr uintptr_t UNIT_DEF_TYPE = 0xb0;                 // char* UnitType
	constexpr uintptr_t UNIT_DEF_HAS_PROMOTION_CLASS = 0x168; // bool, char* at +0x170
	constexpr uintptr_t UNIT_DEF_PROMOTION_CLASS = 0x170;
	constexpr uintptr_t UNIT_DEF_HAS_PURCHASE_YIELD = 0x188;  // bool, hash at +0x2c
	constexpr uintptr_t UNIT_DEF_PURCHASE_YIELD_HASH = 0x2c;
	constexpr uintptr_t BUILDING_DEF_TYPE = 0x60;             // char* BuildingType
	constexpr uintptr_t BUILDING_DEF_HAS_DISTRICT = 0xf8;     // bool, char* PrereqDistrict at +0x100
	constexpr uintptr_t BUILDING_DEF_DISTRICT = 0x100;
	constexpr uintptr_t BUILDING_DEF_HAS_PURCHASE_YIELD = 0x118;  // bool, hash at +0x20
	constexpr uintptr_t BUILDING_DEF_PURCHASE_YIELD_HASH = 0x20;
	constexpr uintptr_t BUILDING_DEF_FLAGS = 0x159;           // & 0x10 = wonder (as the AI tests it)

	// City / player / AI contract
	constexpr uintptr_t CITY_ID = 0xa8;
	constexpr uintptr_t CITY_PROPERTIES = 0x1a0;      // Data::VariantMap (vtable +0x60 = get(key) -> Variant*), as Lua City:GetProperty
	constexpr uintptr_t ECONOMIC_PLAYER = 0x98;       // AI::Economic -> Player::Instance*
	constexpr uintptr_t PLAYER_CITIES = 0x6d0;        // Player::Instance -> Player::Cities*; list head +0xd0, node +0 city, +0x10 next
	constexpr uintptr_t CITIES_LIST = 0xd0;
	constexpr uintptr_t CONTRACT_UNIT = 0x178;        // UnitTypes or -1
	constexpr uintptr_t CONTRACT_BUILDING = 0x190;    // BuildingTypes or -1
	constexpr uintptr_t CONTRACT_DISTRICT = 0x1a8;    // must be -1 for a gold purchase
	constexpr uintptr_t CONTRACT_CITY = 0x1d8;        // city id or -1 (any city)

	constexpr const char* PREFIX = "DEVCE_GOLD_PURCHASE_";

	// Verifies the addresses and installs the hooks (call from InitHooks).
	void Create();
}
