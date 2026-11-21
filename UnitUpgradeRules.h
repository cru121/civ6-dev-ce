// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstdint>
#include "HavokScript.h"

// Extra rules for upgrading units (gold upgrade, UnitCommandTypes.UPGRADE):
//   1. A unit cannot upgrade into a unit type its owner has build-disabled (MODIFIER_PLAYER_UNIT_BUILD_DISABLED)
//   2. Per upgrade step (unit -> upgrade unit) a minimum experience level of the unit (Lua unit:GetExperience():GetLevel():
//      1 for a new unit, +1 per promotion earned with experience; promotions given for free do not count). Set from Lua (the
//      DevCE_UnitUpgrades mod reads its table DevCE_UnitUpgradeRequirements, unique units included).
//   3. (switched on from Lua) An upgrade unit with Unit_BuildingPrereqs needs the unit to stand inside the borders of one of
//      its owner's cities that has one of those buildings (built, not pillaged: the same test as training it there).
//
// The game picks the upgrade target in Rules::Units::GetAvailableUpgrade: the next UnitUpgrades unit (the player's unique
// version), and further along the chain while the next one is unlocked too (Adept -> Archmage when both are). Dev CE walks
// the same chain for a unit and stops before a step the rules block. If the very first step is blocked, the target stays that
// unit, so the unit panel still shows "Upgrade to X" with the reason in red, and CanUpgrade says no.
//
// Hooks: GetAvailableUpgrade (our walk while a unit is known: called through GetUpgradeUnitType, GetUpgradeResourceCost or
// CanUpgrade), and those three to know the unit. Other callers (build queue, start state) keep the game's walk.
namespace UnitUpgradeRules {
	// Addresses in GameCore_XP2_FinalRelease.dll, Steam build 15038592 (same DLL in 15296837).
	constexpr uintptr_t GET_AVAILABLE_UPGRADE_OFFSET = 0x37dbe0;        // Rules::Units::Instance::GetAvailableUpgrade(rules, const Unit** def, player) -> unit or -1 (hooked)
	constexpr uintptr_t GET_NEXT_UPGRADE_OFFSET = 0x37dca0;             // Rules::Units::GetNextUpgrade(const Unit** def, player) -> first UnitUpgrades unit, player's version
	constexpr uintptr_t HAS_PREREQUISITES_OFFSET = 0x37e230;            // Rules::Units::Instance::HasPrerequisites(rules, const Unit** def, player)
	constexpr uintptr_t CHECK_UPGRADE_CYCLE_OFFSET = 0x37d920;          // Rules::Units::CheckForUnitUpgradeCycle(const Unit** def, player)
	constexpr uintptr_t GET_UNIT_DEFINITIONS_OFFSET = 0x7d1b00;         // Definitions::GetUnitDefinitions() -> vector<Unit*>
	constexpr uintptr_t GET_UPGRADE_UNIT_TYPE_OFFSET = 0x3b8010;        // Unit::Upgrade::Manager::GetUpgradeUnitType(manager, unit) (hooked)
	constexpr uintptr_t GET_UPGRADE_RESOURCE_COST_OFFSET = 0x3b7e10;    // Unit::Upgrade::Manager::GetUpgradeResourceCost(manager, unit) (hooked)
	constexpr uintptr_t CAN_UPGRADE_OFFSET = 0x3b7870;                  // Unit::Upgrade::Manager::CanUpgrade(manager, unit, testVisible, results) (hooked)

	// Layout (same build)
	constexpr uintptr_t UNIT_DEF_INDEX = 0x14;              // Definition::Unit: its UnitType index
	constexpr uintptr_t PLAYER_UNITS = 0x6c8;               // Player::Instance -> Player::Units*
	constexpr uintptr_t PLAYER_UNITS_BUILD_DISABLED = 0x198;  // Player::Units: uint8_t[unit type] (Lua PlayerUnits:IsBuildDisabled)
	constexpr uintptr_t UNIT_EXPERIENCE = 0x970;            // Unit::Instance: Experience (value of the FAutoVariable at +0x960)
	constexpr uintptr_t EXPERIENCE_LEVEL = 0x10;            // Experience: int level (Reset sets 1, Promote adds 1; SetLevel writes it)

	// Rule 3 (building prerequisites), same build
	constexpr uintptr_t GET_CITY_OFFSET = 0x42e40;                // City::Instance* GetCity(const CityID*) (Lua Cities.GetPlotPurchaseCity uses it)
	constexpr uintptr_t CITY_BUILDINGS_HAS_BUILDING_OFFSET = 0x10c450;  // City::Buildings::HasBuilding(Buildings*, BuildingTypes)
	constexpr uintptr_t CITY_BUILDINGS_IS_PILLAGED_OFFSET = 0x10ccb0;   // City::Buildings::IsPillaged(Buildings*, BuildingTypes)
	constexpr uintptr_t UNIT_OWNER = 0x128;                 // Unit::Instance: PlayerTypes owner
	constexpr uintptr_t PLOT_VT_CITY_ATTRIBUTES = 0x30;     // Map::Plot vtable: const CityAttributes* (plot)
	constexpr uintptr_t CITY_ATTR_CITY_ID = 0xc;            // CityAttributes: uint32 id of the city that owns the plot (-1: none)
	constexpr uintptr_t CITY_ATTR_OWNER = 0x1c;             // CityAttributes: int8 owner player (-1: none)
	constexpr uintptr_t CITY_BUILDINGS = 0xcb0;             // City::Instance: City::Buildings
	constexpr uintptr_t UNIT_DEF_PREREQ_BUILDINGS = 0x258;  // Definition::Unit: vector<const Building*> (Unit_BuildingPrereqs), begin/end
	constexpr uintptr_t BUILDING_DEF_INDEX = 0x14;          // Definition::Building: BuildingTypes index
	constexpr uintptr_t BUILDING_DEF_NAME = 0x68;           // Definition::Building: Name (LOC key, char*)

	// Alternative upgrades (DevCE_UnitAltUpgrades): extra first steps besides the UnitUpgrades row. The chosen first step travels
	// in the UPGRADE command's generic request int (parameter PARAM_CHOICE, Request +0x18) as unit index + 1; the AI picks the
	// allowed option with the best AI::UnitProduction::GetUnitEfficiency (the score its UpgradeUnits already compares).
	constexpr uintptr_t UPGRADE_COMMAND_CAN_START_OFFSET = 0x663220;   // Unit::Commands::Upgrade::CanStart(handler, unit, const Request&, testOnly, results) (hooked)
	constexpr uintptr_t UPGRADE_COMMAND_START_OFFSET = 0x663460;       // Unit::Commands::Upgrade::Start(handler, unit, const Request&) (vtable +0x20, hooked)
	constexpr uintptr_t UPGRADE_COMMAND_VTABLE = 0xa28038;             // Unit::Commands::Upgrade vtable (+0x10 CanStart, +0x20 Start)
	// Gathering Storm registers its own Upgrade command (XP2): same CanStart, its own Start (a copy of the base one). It is the one
	// the game uses with the expansion; both Starts are hooked.
	constexpr uintptr_t XP2_UPGRADE_COMMAND_START_OFFSET = 0x746680;   // XP2 Upgrade command Start(handler, unit, const Request&) (hooked)
	constexpr uintptr_t XP2_UPGRADE_COMMAND_VTABLE = 0xa432d0;         // its vtable (+0x10 = the shared CanStart, +0x20 = this Start)
	constexpr uintptr_t GET_UPGRADE_MANAGER_OFFSET = 0x3b7cf0;         // Unit::Upgrade::Manager* (game component lookup, no arguments)
	constexpr uintptr_t GET_UPGRADE_COST_OFFSET = 0x3b7d50;            // Unit::Upgrade::Manager::GetUpgradeCost(manager, unit) -> gold
	constexpr uintptr_t GET_UNIT_EFFICIENCY_OFFSET = 0x524e30;         // AI::UnitProduction::GetUnitEfficiency(production, FixedPoint<8>* out, UnitTypes) -> out
	constexpr uintptr_t REQUEST_CHOICE = 0x18;              // Unit::Commands::Request: int copied from parameter PARAM_CHOICE (BaseHandler::MakeRequest)
	constexpr uint32_t PARAM_CHOICE = 0xc65a8ea6;           // command parameter that BaseHandler::MakeRequest copies to Request +0x18
	constexpr uintptr_t PLAYER_UNITS_VERSION = 0x128;       // Player::Units: int[unit type] -> the player's version (-1: itself)
	constexpr uintptr_t PLAYER_AI = 0x670;                  // Player::Instance -> AI component; +0x188 AI::UnitProduction
	constexpr uintptr_t AI_UNIT_PRODUCTION = 0x188;
	// CanUpgrade results added by Dev CE (read by the UI as tResults[key])
	constexpr uint32_t RESULT_UPGRADE_COST = 0x44435543;           // gold cost of this upgrade
	constexpr uint32_t RESULT_UPGRADE_RESOURCE_COST = 0x44435552;  // strategic resource cost of this upgrade

	// Upgrading outside the owner's territory: effect EFFECT_DEVCE_UNIT_UPGRADE_OUTSIDE_TERRITORY (unit subject, no arguments).
	// It is the game's EFFECT_ADJUST_UNIT_PROPERTY with a fixed key and amount: Apply adds 1 to the unit property
	// DEVCE_UPGRADE_OUTSIDE_TERRITORY, Remove takes it away (a count, so two sources and one removed still leave it on). Unit
	// properties are in the save, so it survives save and load like the game's own effects. CanUpgrade's territory check
	// (its one call of Rules::Movement::GetDiplomaticTerritory) is redirected at that call site only: a unit with the property > 0
	// counts as in its own territory.
	constexpr uintptr_t EFFECT_REGISTRY_GET_TYPES_OFFSET = 0x95b0c0;  // Registry<IModifierEffectFactory>::GetTypes() -> std::vector<IModifierEffectFactory*>&
	constexpr uintptr_t UNIT_PROPERTY_FACTORY_VTABLE = 0xa92560;      // EffectFactory<AdjustUnitProperty> vtable (7 slots; 5 = Create)
	constexpr uintptr_t UNIT_PROPERTY_FACTORY_CREATE_OFFSET = 0x8a3850;  // its Create(factory, shared_ptr<IEffect>* out, definition) -> out
	constexpr uintptr_t UNIT_PROPERTY_EFFECT_VTABLE = 0xa9e440;       // AdjustUnitProperty vtable (0 = Initialize, 12 = Apply(unit), 13 = Remove(unit))
	constexpr uintptr_t EFFECT_KEY = 0x8;                   // AdjustUnitProperty: MakeHash(Key argument)
	constexpr uintptr_t EFFECT_AMOUNT = 0xc;                // AdjustUnitProperty: Amount argument
	constexpr uintptr_t GET_DIPLOMATIC_TERRITORY_OFFSET = 0x378410;   // Rules::Movement::Instance::GetDiplomaticTerritory(rules, player, plot) -> TerritoryTypes (1 = own)
	constexpr uintptr_t CAN_UPGRADE_TERRITORY_CALL = 0x3b79ca;        // CanUpgrade: call GetDiplomaticTerritory (rewritten to call Dev CE)
	constexpr uintptr_t UNIT_PROPERTIES = 0x1f0;            // Unit::Instance: Data::VariantMap (vtable +0x60 = get(key) -> Variant*)
	constexpr const char* OUTSIDE_TERRITORY_EFFECT = "EFFECT_DEVCE_UNIT_UPGRADE_OUTSIDE_TERRITORY";
	constexpr const char* OUTSIDE_TERRITORY_PROPERTY = "DEVCE_UPGRADE_OUTSIDE_TERRITORY";

	void Create();
	void ResetForNewGame();
	int Register(hks::lua_State* L);     // gameplay state: UnitUpgradeRules.SetMinLevel / GetMinLevel / Clear / SetBuildingRule / GetBuildingRule / AddAltUpgrade / ClearAltUpgrades / GetAltUpgrades
	int RegisterUI(hks::lua_State* L);   // UI state: UnitUpgradeRules.PARAM_CHOICE / RESULT_COST / RESULT_RESOURCE_COST / IsAltAvailable
}
