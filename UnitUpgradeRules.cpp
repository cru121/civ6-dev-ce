// SPDX-License-Identifier: AGPL-3.0-only
// Part of Dev CE, a fork of the Civilization VI Community Extension by Wild-W (https://github.com/Wild-W/CivilizationVI_CommunityExtension).
// Licensed under the GNU Affero General Public License v3.0 (see LICENSE.txt). Not endorsed by Wild-W, Firaxis or 2K.
#include "UnitUpgradeRules.h"
#include "UnitCommandActions.h"
#include "UnitPropertyRequirement.h"
#include "Runtime.h"
#include "DevBridge.h"
#include <cstring>
#include <unordered_map>
#include <vector>

namespace UnitUpgradeRules {
	struct UnitVector { void** begin; void** end; };
	typedef int(__fastcall* GetAvailableUpgradeFn)(void* rules, void** def, void* player);
	typedef int(__fastcall* GetNextUpgradeFn)(void** def, void* player);
	typedef bool(__fastcall* HasPrerequisitesFn)(void* rules, void** def, void* player);
	typedef bool(__fastcall* CheckCycleFn)(void** def, void* player);
	typedef UnitVector* (__fastcall* GetUnitDefinitionsFn)();
	typedef int(__fastcall* ManagerUnitFn)(void* manager, void* unit);
	typedef bool(__fastcall* CanUpgradeFn)(void* manager, void* unit, bool testVisible, void* results);
	typedef void(__fastcall* AddStringFn)(void* results, uint32_t hash, const void* locString);
	typedef void* (__fastcall* LocLookupFn)(void* out, const char* key);
	typedef void(__fastcall* LocDtorFn)(void* self);
	typedef bool(__fastcall* LocPushIntFn)(void* self, int n);
	typedef bool(__fastcall* LocPushStrFn)(void* self, const char* s);
	typedef void* (__fastcall* UnitGetPlotFn)(void* unit);
	typedef void* (__fastcall* PlotCityAttributesFn)(void* plot);
	struct CityID { int16_t owner; int16_t kind; uint32_t id; };   // kind 2 = city
	typedef void* (__fastcall* GetCityFn)(const CityID* id);
	typedef bool(__fastcall* BuildingsTestFn)(void* buildings, int buildingType);
	typedef bool(__fastcall* CmdCanStartFn)(void* handler, void* unit, const void* request, bool testOnly, void* results);
	typedef void(__fastcall* CmdStartFn)(void* handler, void* unit, const void* request);
	typedef int(__fastcall* GetUpgradeCostFn)(void* manager, void* unit);
	typedef int* (__fastcall* GetUnitEfficiencyFn)(void* production, int* out, int unitType);
	typedef bool(__fastcall* PlayerBoolFn)(void* player);
	typedef void(__fastcall* ResultsSetIntFn)(void* results, uint32_t key, int value);

	static GetAvailableUpgradeFn base_GetAvailableUpgrade;
	static GetNextUpgradeFn GetNextUpgrade;
	static HasPrerequisitesFn HasPrerequisites;
	static CheckCycleFn CheckCycle;
	static GetUnitDefinitionsFn GetUnitDefinitions;
	static ManagerUnitFn base_GetUpgradeUnitType, base_GetUpgradeResourceCost;
	static CanUpgradeFn base_CanUpgrade;
	static AddStringFn ReportAddString;
	static LocLookupFn LocLookup;
	static LocDtorFn LocDtor;
	static LocPushIntFn LocPushInt;
	static LocPushStrFn LocPushStr;
	static UnitGetPlotFn UnitGetPlot;
	static GetCityFn GetCity;
	static BuildingsTestFn HasBuilding, IsPillaged;
	static CmdCanStartFn base_CmdCanStart;
	static CmdStartFn base_CmdStart, base_CmdStartXP2;
	typedef void* (__fastcall* GetUpgradeManagerFn)();
	static GetUpgradeManagerFn GetUpgradeManager;
	static GetUpgradeCostFn GetUpgradeCost;
	static GetUnitEfficiencyFn GetUnitEfficiency;
	static PlayerBoolFn PlayerIsHuman;
	static bool gOk = false, gReportsOk = false, gBuildingRuleOk = false, gAltOk = false;

	struct Check { const char* name; uintptr_t rva; uint8_t bytes[16]; size_t n; };
	static const Check kChecks[] = {
		{ "Rules::Units::GetAvailableUpgrade", GET_AVAILABLE_UPGRADE_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48 }, 12 },
		{ "Rules::Units::GetNextUpgrade", GET_NEXT_UPGRADE_OFFSET, { 0x48, 0x8b, 0x09, 0x48, 0x8b, 0x81, 0xd8, 0x02, 0x00, 0x00, 0x48, 0x8b }, 12 },
		{ "Rules::Units::CheckForUnitUpgradeCycle", CHECK_UPGRADE_CYCLE_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48 }, 12 },
		{ "Definitions::GetUnitDefinitions", GET_UNIT_DEFINITIONS_OFFSET, { 0x48, 0x83, 0xec, 0x28, 0xe8, 0xc7, 0x32, 0x87, 0xff, 0x48, 0x05, 0x30 }, 12 },
		{ "Unit::Upgrade::Manager::GetUpgradeUnitType", GET_UPGRADE_UNIT_TYPE_OFFSET, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x85, 0xd2, 0x74, 0x59, 0x48 }, 12 },
		{ "Unit::Upgrade::Manager::GetUpgradeResourceCost", GET_UPGRADE_RESOURCE_COST_OFFSET, { 0x40, 0x56, 0x57, 0x48, 0x83, 0xec, 0x48, 0x48, 0x8b, 0xb2, 0x98, 0x00 }, 12 },
		{ "Unit::Upgrade::Manager::CanUpgrade", CAN_UPGRADE_OFFSET, { 0x48, 0x89, 0x4c, 0x24, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56, 0x48 }, 12 },
		// GetAvailableUpgrade calls HasPrerequisites (call at 0x37dc3a: e8 + rel32 to 0x37e230)
		{ "GetAvailableUpgrade -> HasPrerequisites", 0x37dc3a, { 0xe8, 0xf1, 0x05, 0x00, 0x00 }, 5 },
	};
	// Rule 3 only (a mismatch switches off just that rule). The layout is checked where the game itself uses it.
	static const Check kBuildingChecks[] = {
		// CanUpgrade: unit->GetPlot(), plot->vt[0x30] CityAttributes, owner +0x1c, city id +0xc, GetCity(CityID)
		{ "CanUpgrade -> Unit::GetPlot", 0x3b79a3, { 0xe8, 0x98, 0x00, 0xcb, 0xff }, 5 },
		{ "CanUpgrade: plot CityAttributes owner / city id", 0x3b7a83, { 0xff, 0x50, 0x30, 0x0f, 0xbe, 0x48, 0x1c, 0x8b, 0x50, 0x0c }, 10 },
		{ "CanUpgrade -> GetCity(CityID)", 0x3b7ab8, { 0xe8, 0x83, 0xb3, 0xc8, 0xff }, 5 },
		{ "GetCity(CityID)", GET_CITY_OFFSET, { 0x40, 0x57, 0x48, 0x83, 0xec, 0x20, 0x0f, 0xb7, 0x01, 0x66, 0x83, 0xf8 }, 12 },
		// CanTrain: Unit_BuildingPrereqs vector (def +0x260 / +0x258), city Buildings +0xcb0 -> HasBuilding / IsPillaged
		{ "CanTrain: unit def building prereqs", 0xf8a18, { 0x48, 0x8b, 0xbe, 0x60, 0x02, 0x00, 0x00, 0x48, 0x8b, 0x9e, 0x58, 0x02, 0x00, 0x00 }, 14 },
		{ "CanTrain -> Buildings::HasBuilding", 0xf8a62, { 0x48, 0x81, 0xc1, 0xb0, 0x0c, 0x00, 0x00, 0xe8, 0xe2, 0x39, 0x01, 0x00 }, 12 },
		{ "CanTrain -> Buildings::IsPillaged", 0xf8a7f, { 0x48, 0x81, 0xc1, 0xb0, 0x0c, 0x00, 0x00, 0xe8, 0x25, 0x42, 0x01, 0x00 }, 12 },
		{ "City::Buildings::HasBuilding", CITY_BUILDINGS_HAS_BUILDING_OFFSET, { 0x83, 0xfa, 0xff, 0x74, 0x17, 0x3b, 0x51, 0x18, 0x73, 0x12, 0x48, 0x8b }, 12 },
		{ "City::Buildings::IsPillaged", CITY_BUILDINGS_IS_PILLAGED_OFFSET, { 0x85, 0xd2, 0x78, 0x11, 0x3b, 0x51, 0x30, 0x7d, 0x0c, 0x48, 0x8b, 0x41 }, 12 },
		// Buildings loader: column 30 (Name) -> building +0x68
		{ "Building definition Name at +0x68", 0x7b6494, { 0x48, 0x85, 0xc0, 0x74, 0x0e, 0x48, 0x8d, 0x4b, 0x68 }, 9 },
	};
	// Alternative upgrades only (a mismatch switches off just them)
	static const Check kAltChecks[] = {
		{ "Unit::Commands::Upgrade::CanStart", UPGRADE_COMMAND_CAN_START_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x41, 0x0f }, 12 },
		{ "Unit::Commands::Upgrade::Start", UPGRADE_COMMAND_START_OFFSET, { 0x4c, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89 }, 12 },
		{ "XP2 Upgrade command Start", XP2_UPGRADE_COMMAND_START_OFFSET, { 0x4c, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89 }, 12 },
		// both Starts begin with Upgrade::Manager (0x3b7cf0) -> GetUpgradeUnitType (the target the guard below checks first)
		{ "Upgrade::Start -> Upgrade manager, GetUpgradeUnitType", 0x66349c, { 0xe8, 0x4f, 0x48, 0xd5, 0xff, 0x48, 0x8b, 0xc8, 0x48, 0x8b, 0xd6, 0xe8, 0x64, 0x4b, 0xd5, 0xff }, 16 },
		{ "XP2 Start -> Upgrade manager, GetUpgradeUnitType", 0x7466bc, { 0xe8, 0x2f, 0x16, 0xc7, 0xff, 0x48, 0x8b, 0xc8, 0x48, 0x8b, 0xd6, 0xe8, 0x44, 0x19, 0xc7, 0xff }, 16 },
		{ "Unit::Upgrade manager lookup", GET_UPGRADE_MANAGER_OFFSET, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x8b, 0x1d }, 8 },
		// (the Upgrade command's vtable entries +0x10 / +0x20 are checked in Create: they hold relocated addresses)
		// BaseHandler::MakeRequest: parameter 0xc65a8ea6 -> Request +0x18
		{ "BaseHandler::MakeRequest PARAM_CHOICE", 0x3e3a6, { 0xba, 0xa6, 0x8e, 0x5a, 0xc6 }, 5 },
		{ "BaseHandler::MakeRequest -> Request +0x18", 0x3e3be, { 0x89, 0x45, 0x18 }, 3 },
		// GetNextUpgrade: player Units +0x6c8 -> version table +0x128
		{ "GetNextUpgrade: player's unit version table", 0x37dce2, { 0x48, 0x8b, 0x82, 0xc8, 0x06, 0x00, 0x00, 0x48, 0x8b, 0x88, 0x28, 0x01, 0x00, 0x00 }, 14 },
		{ "Unit::Upgrade::Manager::GetUpgradeCost", GET_UPGRADE_COST_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83 }, 12 },
		// AI::Military::UpgradeUnits: player +0x670 AI, +0x188 UnitProduction, GetUnitEfficiency(production, out, type)
		{ "UpgradeUnits: player AI +0x670", 0x4d2e46, { 0x4c, 0x8b, 0xa3, 0x70, 0x06, 0x00, 0x00 }, 7 },
		{ "UpgradeUnits -> GetUnitEfficiency(+0x188)", 0x4d2eea, { 0x49, 0x8d, 0x8c, 0x24, 0x88, 0x01, 0x00, 0x00, 0xe8, 0x39, 0x1f, 0x05, 0x00 }, 13 },
		{ "AI::UnitProduction::GetUnitEfficiency", GET_UNIT_EFFICIENCY_OFFSET, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x8b, 0x41, 0x20, 0x48, 0x8b, 0xda }, 12 },
		{ "Player::Instance::IsHuman", UnitCommandActions::PLAYER_IS_HUMAN_OFFSET, { 0x48, 0x83, 0xec, 0x28, 0x8b, 0x91, 0xd8, 0x00, 0x00, 0x00, 0x48, 0x8b }, 12 },
		// CanUpgrade: results->vt[0x128](results, UNIT_TYPE, int) (the setter Dev CE uses for its cost results)
		{ "CanUpgrade: results set int", 0x3b78e2, { 0xba, 0x5b, 0xec, 0x0d, 0x3c, 0x48, 0x8b, 0xce, 0xff, 0x90, 0x28, 0x01, 0x00, 0x00 }, 14 },
	};

	// ---- rules (written by the gameplay script at game start, read from any thread afterwards) ----------------------------
	static std::unordered_map<uint64_t, int> gMinLevel;   // (from << 32 | to) -> minimum experience level
	static uint64_t Key(int from, int to) { return ((uint64_t)(uint32_t)from << 32) | (uint32_t)to; }
	static int MinLevel(int from, int to) {
		if (gMinLevel.empty()) return 0;
		auto it = gMinLevel.find(Key(from, to));
		return it != gMinLevel.end() ? it->second : 0;
	}

	static bool gBuildingRule = false;   // rule 3, switched on from Lua

	// ---- the walk ---------------------------------------------------------------------------------------------------------
	enum BlockKind { BLOCK_NONE = 0, BLOCK_UNAVAILABLE = 1, BLOCK_LEVEL = 2, BLOCK_BUILDING = 4 };   // bits: a step can fail several
	struct Block { int kind; int need; int have; int to; };

	// The unit whose upgrade the game is asking about on this thread (set by our GetUpgradeUnitType / GetUpgradeResourceCost /
	// CanUpgrade hooks), and what blocked its first upgrade step last time (read by our CanUpgrade hook).
	static thread_local void* tUnit = nullptr;
	static thread_local Block tFirstBlock = {};

	static bool IsBuildDisabled(void* player, int unitType) {
		__try {
			char* units = *(char**)((char*)player + PLAYER_UNITS);
			if (units == nullptr) return false;
			const uint8_t* flags = *(const uint8_t**)(units + PLAYER_UNITS_BUILD_DISABLED);
			return flags != nullptr && flags[unitType] != 0;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}
	static int UnitLevel(void* unit) {
		__try {
			return *(const int*)((const char*)unit + UNIT_EXPERIENCE + EXPERIENCE_LEVEL);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return 0;
		}
	}
	static void* UnitDefinition(int index) {
		UnitVector* v = GetUnitDefinitions();
		if (index < 0 || v == nullptr || (size_t)index >= (size_t)(v->end - v->begin)) return nullptr;
		return v->begin[index];
	}

	// Unit_BuildingPrereqs of a unit definition (null entries skipped by the callers, as in CanTrain)
	struct BuildingVector { void** begin; void** end; };
	static const BuildingVector* PrereqBuildings(void* unitDef) {
		return (const BuildingVector*)((char*)unitDef + UNIT_DEF_PREREQ_BUILDINGS);
	}

	// The city whose borders the unit stands in, if it belongs to the unit's owner (an enemy, allied or city-state city never counts).
	static void* OwnCityAtUnit(void* unit) {
		void* plot = UnitGetPlot(unit);
		if (plot == nullptr) return nullptr;
		const char* attrs = (const char*)(*(PlotCityAttributesFn*)(*(char**)plot + PLOT_VT_CITY_ATTRIBUTES))(plot);
		if (attrs == nullptr) return nullptr;
		const int owner = *(const int8_t*)(attrs + CITY_ATTR_OWNER);
		const uint32_t id = *(const uint32_t*)(attrs + CITY_ATTR_CITY_ID);
		if (owner < 0 || id == 0xffffffffu || owner != *(const int*)((const char*)unit + UNIT_OWNER)) return nullptr;
		const CityID cid = { (int16_t)owner, 2, id };
		return GetCity(&cid);
	}

	// Rule 3: true when the upgrade unit has no Unit_BuildingPrereqs, or the unit is in one of its owner's cities with one of them
	// (built and not pillaged, like City::BuildQueue::CanTrain; its NumSupported limit is not applied to upgrades).
	static bool HasUpgradeBuilding(void* unit, int to) {
		void* toDef = UnitDefinition(to);
		if (toDef == nullptr) return true;
		__try {
			const BuildingVector* v = PrereqBuildings(toDef);
			bool any = false;
			for (void** p = v->begin; p != v->end; ++p) if (*p != nullptr) { any = true; break; }
			if (!any) return true;
			void* city = OwnCityAtUnit(unit);
			if (city == nullptr) return false;
			void* buildings = (char*)city + CITY_BUILDINGS;
			for (void** p = v->begin; p != v->end; ++p) {
				if (*p == nullptr) continue;
				const int b = *(const int*)((const char*)*p + BUILDING_DEF_INDEX);
				if (HasBuilding(buildings, b) && !IsPillaged(buildings, b)) return true;
			}
			return false;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return true;   // unreadable: leave the game's decision alone
		}
	}

	static Block Gate(void* unit, void* player, int from, int to) {
		Block b = { BLOCK_NONE, 0, 0, to };
		if (IsBuildDisabled(player, to)) b.kind |= BLOCK_UNAVAILABLE;
		const int need = MinLevel(from, to);
		if (need > 1) {
			const int have = UnitLevel(unit);
			if (have < need) { b.kind |= BLOCK_LEVEL; b.need = need; b.have = have; }
		}
		if (gBuildingRule && gBuildingRuleOk && !HasUpgradeBuilding(unit, to)) b.kind |= BLOCK_BUILDING;
		return b;
	}

	// ---- alternative upgrades (written by the gameplay script at game start) ----------------------------------------------
	static std::unordered_map<int, std::vector<int>> gAlt;   // unit type -> extra first steps, in table order
	static thread_local int tChoice = -1;                    // first step chosen by the UPGRADE command on this thread (-1: none)
	constexpr int MAX_OPTIONS = 16;

	// The player's version of a unit type (its unique unit), as GetNextUpgrade maps the UnitUpgrades row.
	static int PlayerVersion(void* player, int t) {
		if (t < 0) return t;
		__try {
			const int* table = *(const int* const*)(*(char**)((char*)player + PLAYER_UNITS) + PLAYER_UNITS_VERSION);
			const int v = table[t];
			return v != -1 ? v : t;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return t;
		}
	}

	// The first steps of a unit: [0] the UnitUpgrades one (-1 if none), then its alternative upgrades (player's versions, no repeats).
	static int FirstSteps(void* def, void* player, int out[MAX_OPTIONS]) {
		void* from = def;
		int n = 0;
		out[n++] = GetNextUpgrade(&from, player);
		if (!gAltOk || gAlt.empty()) return n;
		auto it = gAlt.find(*(int*)((char*)def + UNIT_DEF_INDEX));
		if (it == gAlt.end()) return n;
		for (int t : it->second) {
			const int v = PlayerVersion(player, t);
			bool seen = false;
			for (int i = 0; i < n; ++i) if (out[i] == v) seen = true;
			if (!seen && n < MAX_OPTIONS) out[n++] = v;
		}
		return n;
	}

	// GetAvailableUpgrade (0x37dbe0) with the rules, from a given first step: the furthest unlocked step that the rules allow.
	// A blocked first step is still returned (with *firstBlock set) so the panel can name it; a blocked later step ends the walk
	// before it. Later steps follow the UnitUpgrades chain only.
	static int Walk(void* rules, void* def, void* player, void* unit, int depth, Block* firstBlock);
	static int WalkFrom(void* rules, void* def, int next, void* player, void* unit, int depth, Block* firstBlock) {
		if (def == nullptr || next < 0) return -1;
		void* nextDef = UnitDefinition(next);
		if (nextDef == nullptr || !HasPrerequisites(rules, &nextDef, player)) return -1;
		const Block b = Gate(unit, player, *(int*)((char*)def + UNIT_DEF_INDEX), next);
		if (b.kind != BLOCK_NONE) {
			if (depth == 0) { *firstBlock = b; return next; }
			return -1;
		}
		if (depth < 16 && !CheckCycle(&nextDef, player)) {
			Block ignored = {};
			const int deeper = Walk(rules, nextDef, player, unit, depth + 1, &ignored);
			if (deeper != -1) return deeper;
		}
		return next;
	}
	static int Walk(void* rules, void* def, void* player, void* unit, int depth, Block* firstBlock) {
		if (def == nullptr) return -1;
		void* from = def;
		return WalkFrom(rules, def, GetNextUpgrade(&from, player), player, unit, depth, firstBlock);
	}

	// The AI's own production score for a unit type (0 for one it does not rate), as AI::Military::UpgradeUnits compares them.
	static int Efficiency(void* player, int unitType) {
		__try {
			char* ai = *(char**)((char*)player + PLAYER_AI);
			if (ai == nullptr) return 0;
			int out = 0;
			const int* r = GetUnitEfficiency(ai + AI_UNIT_PRODUCTION, &out, unitType);
			return r != nullptr ? *r : 0;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return 0;
		}
	}

	// The target for a unit with these first steps: the chosen one (tChoice), else for the AI the allowed option with the best
	// efficiency, else the UnitUpgrades one (the game's default; humans pick the others with their own buttons).
	static int Choose(void* rules, void* def, void* player, void* unit, Block* firstBlock) {
		int steps[MAX_OPTIONS];
		const int n = FirstSteps(def, player, steps);
		if (tChoice >= 0) {
			const int want = PlayerVersion(player, tChoice);
			for (int i = 0; i < n; ++i) if (steps[i] == want && want >= 0) return WalkFrom(rules, def, want, player, unit, 0, firstBlock);
			return -1;   // not an option of this unit
		}
		if (n > 1 && !PlayerIsHuman(player)) {
			int best = -1, bestScore = 0;
			for (int i = 0; i < n; ++i) {
				if (steps[i] < 0) continue;
				Block b = {};
				const int r = WalkFrom(rules, def, steps[i], player, unit, 0, &b);
				if (r < 0 || b.kind != BLOCK_NONE) continue;
				const int score = Efficiency(player, r);
				if (best < 0 || score > bestScore) { best = r; bestScore = score; }
			}
			if (best >= 0) return best;
		}
		return WalkFrom(rules, def, steps[0], player, unit, 0, firstBlock);
	}

	static int __fastcall Hook_GetAvailableUpgrade(void* rules, void** def, void* player) {
		void* unit = tUnit;
		if (unit == nullptr || def == nullptr) return base_GetAvailableUpgrade(rules, def, player);
		tUnit = nullptr;   // the game's own calls below us (none today) see the plain walk
		Block b = {};
		const int r = Choose(rules, *def, player, unit, &b);
		tUnit = unit;
		tFirstBlock = b;
		return r;
	}

	// The UPGRADE command: its request's generic int (parameter PARAM_CHOICE) is the chosen first step + 1 (0: the default).
	static int RequestChoice(const void* request) {
		if (!gAltOk || request == nullptr) return -1;
		__try {
			const int v = *(const int*)((const char*)request + REQUEST_CHOICE);
			return v > 0 ? v - 1 : -1;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return -1;
		}
	}
	static bool __fastcall Hook_CmdCanStart(void* handler, void* unit, const void* request, bool testOnly, void* results) {
		const int saved = tChoice;
		tChoice = RequestChoice(request);
		const bool r = base_CmdCanStart(handler, unit, request, testOnly, results);
		tChoice = saved;
		return r;
	}
	static int __fastcall Hook_GetUpgradeUnitType(void* manager, void* unit);
	// Both Upgrade Starts remove the unit before creating the new one, and do not check the target: with no target (-1) the unit
	// would just disappear. The command's message runs CanStart first, but other callers need not, so Start checks it again.
	static void CmdStart(CmdStartFn base, void* handler, void* unit, const void* request) {
		const int saved = tChoice;
		tChoice = RequestChoice(request);
		void* manager = unit != nullptr ? GetUpgradeManager() : nullptr;
		if (manager != nullptr && Hook_GetUpgradeUnitType(manager, unit) >= 0) {
			base(handler, unit, request);
		} else {
			DevBridge::Log("[UnitUpgradeRules] UPGRADE command with no upgrade target (choice %d): not started", tChoice);
		}
		tChoice = saved;
	}
	static void __fastcall Hook_CmdStart(void* handler, void* unit, const void* request) { CmdStart(base_CmdStart, handler, unit, request); }
	static void __fastcall Hook_CmdStartXP2(void* handler, void* unit, const void* request) { CmdStart(base_CmdStartXP2, handler, unit, request); }

	static int __fastcall Hook_GetUpgradeUnitType(void* manager, void* unit) {
		void* saved = tUnit;
		tUnit = unit;
		const int r = base_GetUpgradeUnitType(manager, unit);
		tUnit = saved;
		return r;
	}
	static int __fastcall Hook_GetUpgradeResourceCost(void* manager, void* unit) {
		void* saved = tUnit;
		tUnit = unit;
		const int r = base_GetUpgradeResourceCost(manager, unit);
		tUnit = saved;
		return r;
	}

	// Names of the upgrade unit's prerequisite buildings (LOC keys, the game localizes string arguments), at most 2.
	static int PrereqBuildingNames(int to, const char* names[2]) {
		void* toDef = UnitDefinition(to);
		if (toDef == nullptr) return 0;
		int n = 0;
		__try {
			const BuildingVector* v = PrereqBuildings(toDef);
			for (void** p = v->begin; p != v->end; ++p) {
				if (*p == nullptr) continue;
				if (n == 2) return 3;   // more than 2: the text without names
				const char* name = *(const char* const*)((const char*)*p + BUILDING_DEF_NAME);
				if (name == nullptr || *name == '\0') return 3;
				names[n++] = name;
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return 3;
		}
		return n;
	}

	static void AddText(void* results, const char* key, int i1, int i2, const char* s1, const char* s2) {
		alignas(16) uint8_t s[0x100];   // Localization::String (smaller than this)
		LocLookup(s, key);
		if (i1 >= 0) LocPushInt(s, i1);
		if (i2 >= 0) LocPushInt(s, i2);
		if (s1 != nullptr) LocPushStr(s, s1);
		if (s2 != nullptr) LocPushStr(s, s2);
		ReportAddString(results, UnitCommandActions::RESULT_FAILURE_REASONS, s);
		LocDtor(s);
	}

	static void AddReason(void* results, const Block& b) {
		if (!gReportsOk || results == nullptr) return;
		if (b.kind & BLOCK_UNAVAILABLE) AddText(results, "LOC_DEVCE_UPGRADE_UNIT_UNAVAILABLE", -1, -1, nullptr, nullptr);
		if (b.kind & BLOCK_LEVEL) AddText(results, "LOC_DEVCE_UPGRADE_NEEDS_LEVEL", b.need, b.have, nullptr, nullptr);
		if (b.kind & BLOCK_BUILDING) {
			const char* names[2] = {};
			const int n = PrereqBuildingNames(b.to, names);
			if (n == 1) AddText(results, "LOC_DEVCE_UPGRADE_NEEDS_BUILDING", -1, -1, names[0], nullptr);
			else if (n == 2) AddText(results, "LOC_DEVCE_UPGRADE_NEEDS_BUILDING_2", -1, -1, names[0], names[1]);
			else AddText(results, "LOC_DEVCE_UPGRADE_NEEDS_BUILDING_ANY", -1, -1, nullptr, nullptr);
		}
	}

	// The game's CanUpgrade (its GetUpgradeUnitType call runs our walk), then: a blocked first step keeps the button (testVisible)
	// but the real check fails, with the reason.
	static bool __fastcall Hook_CanUpgrade(void* manager, void* unit, bool testVisible, void* results) {
		void* saved = tUnit;
		tUnit = unit;
		tFirstBlock = {};
		bool r = base_CanUpgrade(manager, unit, testVisible, results);
		const Block b = tFirstBlock;
		if (gAltOk && results != nullptr && tChoice >= 0 && Hook_GetUpgradeUnitType(manager, unit) >= 0) {
			// the cost of this option (pUnit:GetUpgradeCost() in Lua knows only the default one)
			__try {
				ResultsSetIntFn set = *(ResultsSetIntFn*)(*(char**)results + 0x128);
				set(results, RESULT_UPGRADE_COST, GetUpgradeCost(manager, unit));
				set(results, RESULT_UPGRADE_RESOURCE_COST, Hook_GetUpgradeResourceCost(manager, unit));
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
		}
		tUnit = saved;
		if (b.kind != BLOCK_NONE && !testVisible) {
			AddReason(results, b);
			r = false;
		}
		return r;
	}

	// ---- upgrading outside the owner's territory (EFFECT_DEVCE_UNIT_UPGRADE_OUTSIDE_TERRITORY) --------------------------------
	struct FactoryVector { void** begin; void** end; void** capacity; };   // std::vector<IModifierEffectFactory*>
	typedef FactoryVector* (__fastcall* GetTypesFn)();
	typedef void(__fastcall* VectorGrowFn)(FactoryVector* v, size_t n);
	typedef void* (__fastcall* OperatorNewFn)(size_t size);
	typedef uint32_t(__fastcall* MakeHashFn)(const char* s);
	typedef void** (__fastcall* FactoryCreateFn)(void* factory, void** out, void* definition);
	typedef int(__fastcall* TerritoryFn)(void* rules, int player, void* plot);
	typedef const uint8_t* (__fastcall* VariantGetFn)(void* map, uint32_t key);

	constexpr int FACTORY_SLOTS = 7;
	static void* gOutsideFactoryVtable[FACTORY_SLOTS];
	static FactoryCreateFn base_UnitPropertyCreate;
	static TerritoryFn GetDiplomaticTerritory;
	static uint32_t gOutsidePropertyKey = 0;
	static bool gOutsideOk = false;

		static const Check kOutsideChecks[] = {
		{ "Registry<IModifierEffectFactory>::GetTypes", EFFECT_REGISTRY_GET_TYPES_OFFSET, { 0x48, 0x83, 0xec, 0x28, 0x65, 0x48, 0x8b, 0x04, 0x25, 0x58 }, 10 },
		// Register<EffectFactory<AdjustUnitProperty>>: new(0x10), vtable 0xa92560, MakeHash -> +8, GetTypes, grow when full
		{ "Register<AdjustUnitProperty>: operator new", 0x90eccf, { 0xe8, 0x2c, 0x1c, 0x08, 0x00 }, 5 },
		{ "Register<AdjustUnitProperty>: GetTypes", 0x90ecf9, { 0xe8, 0xc2, 0xc3, 0x04, 0x00 }, 5 },
		{ "Register<AdjustUnitProperty>: vector grow", 0x90ed13, { 0xe8, 0x38, 0xa2, 0x9f, 0xff }, 5 },
		{ "EffectFactory<AdjustUnitProperty>::Create", UNIT_PROPERTY_FACTORY_CREATE_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10 }, 10 },
		// AdjustUnitProperty::Apply reads the key at +8 and the amount at +0xc
		{ "AdjustUnitProperty::Apply key +8", 0x8d9d19, { 0x8b, 0x57, 0x08 }, 3 },
		{ "AdjustUnitProperty::Apply amount +0xc", 0x8d9d4e, { 0x44, 0x8b, 0x47, 0x0c }, 4 },
		{ "Rules::Movement::GetDiplomaticTerritory", GET_DIPLOMATIC_TERRITORY_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x49, 0x8b }, 12 },
		// CanUpgrade: call GetDiplomaticTerritory; cmp eax, 1 (own territory)
		{ "CanUpgrade -> GetDiplomaticTerritory", CAN_UPGRADE_TERRITORY_CALL, { 0xe8, 0x41, 0x0a, 0xfc, 0xff, 0x83, 0xf8, 0x01 }, 8 },
	};

	// EffectFactory<AdjustUnitProperty>::Create, then the fixed key and amount (Initialize found no Key/Amount arguments).
	static void** __fastcall OutsideFactoryCreate(void* factory, void** out, void* definition) {
		void** r = base_UnitPropertyCreate(factory, out, definition);
		if (r != nullptr && r[0] != nullptr) {
			char* effect = (char*)r[0];
			*(uint32_t*)(effect + EFFECT_KEY) = gOutsidePropertyKey;
			*(int32_t*)(effect + EFFECT_AMOUNT) = 1;
		}
		return r;
	}

	static bool UnitMayUpgradeOutside(void* unit) {
		__try {
			void* map = (char*)unit + UNIT_PROPERTIES;
			const uint8_t* v = ((VariantGetFn)(*(void***)map)[0x60 / 8])(map, gOutsidePropertyKey);
			if (v == nullptr) return false;
			const uint16_t type = *(const uint16_t*)v;
			const uint32_t flags = *(const uint32_t*)(v + 0x14);
			const bool isNumber = ((flags >> 29) & 1) == 0 && type <= 0x15 && ((0x30002eu >> type) & 1) != 0;
			return isNumber && *(const int*)(v + 8) > 0;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// Called instead of GetDiplomaticTerritory by CanUpgrade only (its call instruction is rewritten). tUnit = the unit being
	// checked (set by our CanUpgrade hook).
	static int __fastcall TerritoryForUpgrade(void* rules, int player, void* plot) {
		void* unit = tUnit;
		if (unit != nullptr && UnitMayUpgradeOutside(unit)) return 1;
		return GetDiplomaticTerritory(rules, player, plot);
	}

	// Executable memory within +-2 GB of the call site, for the jump to TerritoryForUpgrade (as LuaHooks' AllocNear).
	static void* AllocNear(uintptr_t target, size_t size) {
		SYSTEM_INFO si; GetSystemInfo(&si);
		const uintptr_t gran = si.dwAllocationGranularity;
		for (uintptr_t off = gran; off < 0x70000000; off += gran) {
			for (int dir = 0; dir < 2; dir++) {
				const uintptr_t cand = dir == 0 ? (target & ~(gran - 1)) - off : (target & ~(gran - 1)) + off;
				if (dir == 0 && cand > target) continue;
				if (cand < 0x10000) continue;
				void* p = VirtualAlloc((void*)cand, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
				if (p == nullptr) continue;
				const intptr_t dist = (intptr_t)(uintptr_t)p - (intptr_t)target;
				if (dist > 0x7FF00000 || dist < -0x7FF00000) { VirtualFree(p, 0, MEM_RELEASE); continue; }
				return p;
			}
		}
		return nullptr;
	}

	// CanUpgrade's `call GetDiplomaticTerritory` (e8 rel32) -> `call stub`, stub = `mov rax, TerritoryForUpgrade; jmp rax`.
	static bool PatchTerritoryCall(uintptr_t b) {
		uint8_t* site = (uint8_t*)(b + CAN_UPGRADE_TERRITORY_CALL);
		uint8_t* stub = (uint8_t*)AllocNear((uintptr_t)site, 0x1000);
		if (stub == nullptr) return false;
		const uint64_t target = (uint64_t)&TerritoryForUpgrade;
		stub[0] = 0x48; stub[1] = 0xb8; memcpy(stub + 2, &target, 8);   // mov rax, imm64
		stub[10] = 0xff; stub[11] = 0xe0;                                // jmp rax
		FlushInstructionCache(GetCurrentProcess(), stub, 12);
		const int64_t rel = (int64_t)(uintptr_t)stub - (int64_t)((uintptr_t)site + 5);
		if (rel > INT32_MAX || rel < INT32_MIN) return false;
		const int32_t rel32 = (int32_t)rel;
		DWORD old = 0;
		if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
		memcpy(site + 1, &rel32, 4);   // keep the e8 opcode, new target
		VirtualProtect(site, 5, old, &old);
		FlushInstructionCache(GetCurrentProcess(), site, 5);
		return true;
	}

	static bool AlreadyRegistered(FactoryVector* v) {
		for (void** it = v->begin; it != v->end; ++it) if (*it != nullptr && *(void**)*it == (void*)gOutsideFactoryVtable) return true;
		return false;
	}

	// Registers the effect as Registry<IModifierEffectFactory>::Register<T> does, and redirects CanUpgrade's territory check.
	static void CreateOutsideTerritory(uintptr_t b) {
		for (const Check& c : kOutsideChecks) {
			if (memcmp((void*)(b + c.rva), c.bytes, c.n) != 0) {
				DevBridge::Log("[UnitUpgradeRules] %s has unexpected bytes at 0x%llx: %s is not available", c.name, (unsigned long long)c.rva, OUTSIDE_TERRITORY_EFFECT);
				return;
			}
		}
		const uintptr_t* vt = (const uintptr_t*)(b + UNIT_PROPERTY_FACTORY_VTABLE);
		if (vt[5] != b + UNIT_PROPERTY_FACTORY_CREATE_OFFSET) {
			DevBridge::Log("[UnitUpgradeRules] EffectFactory<AdjustUnitProperty> vtable is not as expected: %s is not available", OUTSIDE_TERRITORY_EFFECT);
			return;
		}
		MakeHashFn makeHash = (MakeHashFn)(b + UnitPropertyRequirement::MAKE_HASH_OFFSET);
		OperatorNewFn operatorNew = (OperatorNewFn)(b + UnitPropertyRequirement::OPERATOR_NEW_OFFSET);
		VectorGrowFn grow = (VectorGrowFn)(b + UnitPropertyRequirement::FACTORY_VECTOR_GROW_OFFSET);
		GetTypesFn getTypes = (GetTypesFn)(b + EFFECT_REGISTRY_GET_TYPES_OFFSET);
		base_UnitPropertyCreate = (FactoryCreateFn)(b + UNIT_PROPERTY_FACTORY_CREATE_OFFSET);
		GetDiplomaticTerritory = (TerritoryFn)(b + GET_DIPLOMATIC_TERRITORY_OFFSET);
		gOutsidePropertyKey = makeHash(OUTSIDE_TERRITORY_PROPERTY);

		memcpy(gOutsideFactoryVtable, (void*)vt, sizeof(gOutsideFactoryVtable));
		gOutsideFactoryVtable[5] = (void*)&OutsideFactoryCreate;

		FactoryVector* types = getTypes();
		if (types == nullptr) return;
		if (!AlreadyRegistered(types)) {
			void** factory = (void**)operatorNew(0x10);
			if (factory == nullptr) return;
			factory[0] = gOutsideFactoryVtable;
			*(uint32_t*)(factory + 1) = makeHash(OUTSIDE_TERRITORY_EFFECT);
			if (types->end == types->capacity) grow(types, 1);
			*types->end = factory;
			types->end++;
		}
		if (!PatchTerritoryCall(b)) {
			DevBridge::Log("[UnitUpgradeRules] could not redirect CanUpgrade's territory check: %s has no effect", OUTSIDE_TERRITORY_EFFECT);
			return;
		}
		gOutsideOk = true;
	}

	// ---- Lua (gameplay state) ---------------------------------------------------------------------------------------------
	// UnitUpgradeRules.SetMinLevel(fromUnitIndex, toUnitIndex, level): level <= 1 removes the rule
	static int lSetMinLevel(hks::lua_State* L) {
		const int from = hks::checkinteger(L, 1), to = hks::checkinteger(L, 2), n = hks::checkinteger(L, 3);
		if (from < 0 || to < 0 || UnitDefinition(from) == nullptr || UnitDefinition(to) == nullptr) {
			hks::error(L, "UnitUpgradeRules.SetMinLevel: invalid unit index %d or %d", from, to);
			return 0;
		}
		if (n > 1) gMinLevel[Key(from, to)] = n; else gMinLevel.erase(Key(from, to));
		hks::pushboolean(L, true);
		return 1;
	}
	// UnitUpgradeRules.GetMinLevel(fromUnitIndex, toUnitIndex) -> level (0 = no rule)
	static int lGetMinLevel(hks::lua_State* L) {
		hks::pushinteger(L, MinLevel(hks::checkinteger(L, 1), hks::checkinteger(L, 2)));
		return 1;
	}
	// UnitUpgradeRules.Clear(): the minimum levels
	static int lClear(hks::lua_State* L) {
		gMinLevel.clear();
		return 0;
	}
	// UnitUpgradeRules.SetBuildingRule(on) -> true if the rule is available in this build (rule 3: Unit_BuildingPrereqs)
	static int lSetBuildingRule(hks::lua_State* L) {
		gBuildingRule = hks::toboolean(L, 1) != 0;
		hks::pushboolean(L, gBuildingRuleOk);
		return 1;
	}
	// UnitUpgradeRules.GetBuildingRule() -> on (and available)
	static int lGetBuildingRule(hks::lua_State* L) {
		hks::pushboolean(L, gBuildingRule && gBuildingRuleOk);
		return 1;
	}

	// UnitUpgradeRules.AddAltUpgrade(fromUnitIndex, toUnitIndex) -> true if alternative upgrades are available in this build
	static int lAddAltUpgrade(hks::lua_State* L) {
		const int from = hks::checkinteger(L, 1), to = hks::checkinteger(L, 2);
		if (UnitDefinition(from) == nullptr || UnitDefinition(to) == nullptr || from == to) {
			hks::error(L, "UnitUpgradeRules.AddAltUpgrade: invalid unit index %d or %d", from, to);
			return 0;
		}
		std::vector<int>& v = gAlt[from];
		bool seen = false;
		for (int t : v) if (t == to) seen = true;
		if (!seen) v.push_back(to);
		hks::pushboolean(L, gAltOk);
		return 1;
	}
	// UnitUpgradeRules.ClearAltUpgrades()
	static int lClearAltUpgrades(hks::lua_State* L) {
		gAlt.clear();
		return 0;
	}
	// UnitUpgradeRules.GetAltUpgrades(fromUnitIndex) -> { toUnitIndex, ... } (table order)
	static int lGetAltUpgrades(hks::lua_State* L) {
		auto it = gAlt.find(hks::checkinteger(L, 1));
		const int n = it != gAlt.end() ? (int)it->second.size() : 0;
		hks::createtable(L, n, 0);
		for (int i = 0; i < n; ++i) {
			hks::pushinteger(L, i + 1);
			hks::pushinteger(L, it->second[i]);
			hks::settable(L, -3);
		}
		return 1;
	}

	int Register(hks::lua_State* L) {
		if (!gOk) return 0;
		hks::createtable(L, 0, 8);
		PushLuaMethod(L, lSetMinLevel, "lUnitUpgradeRulesSetMinLevel", -2, "SetMinLevel");
		PushLuaMethod(L, lGetMinLevel, "lUnitUpgradeRulesGetMinLevel", -2, "GetMinLevel");
		PushLuaMethod(L, lClear, "lUnitUpgradeRulesClear", -2, "Clear");
		PushLuaMethod(L, lSetBuildingRule, "lUnitUpgradeRulesSetBuildingRule", -2, "SetBuildingRule");
		PushLuaMethod(L, lGetBuildingRule, "lUnitUpgradeRulesGetBuildingRule", -2, "GetBuildingRule");
		PushLuaMethod(L, lAddAltUpgrade, "lUnitUpgradeRulesAddAltUpgrade", -2, "AddAltUpgrade");
		PushLuaMethod(L, lClearAltUpgrades, "lUnitUpgradeRulesClearAltUpgrades", -2, "ClearAltUpgrades");
		PushLuaMethod(L, lGetAltUpgrades, "lUnitUpgradeRulesGetAltUpgrades", -2, "GetAltUpgrades");
		hks::setfield(L, hks::LUA_GLOBAL, "UnitUpgradeRules");
		return 0;
	}

	// UI state: the keys a unit panel needs to offer the alternative upgrades through the normal UPGRADE command:
	//   UnitManager.CanStartCommand(unit, UnitCommandTypes.UPGRADE, { [UnitUpgradeRules.PARAM_CHOICE] = firstStepIndex + 1 }, testOnly, true)
	//   -> tResults[UnitCommandResults.UNIT_TYPE], tResults[UnitUpgradeRules.RESULT_COST], tResults[UnitUpgradeRules.RESULT_RESOURCE_COST]
	//   UnitManager.RequestCommand(unit, UnitCommandTypes.UPGRADE, { [UnitUpgradeRules.PARAM_CHOICE] = firstStepIndex + 1 })
	int RegisterUI(hks::lua_State* L) {
		if (!gOk || !gAltOk) return 0;
		hks::createtable(L, 0, 3);
		hks::pushinteger(L, (int32_t)PARAM_CHOICE);
		hks::setfield(L, -2, "PARAM_CHOICE");
		hks::pushinteger(L, (int32_t)RESULT_UPGRADE_COST);
		hks::setfield(L, -2, "RESULT_COST");
		hks::pushinteger(L, (int32_t)RESULT_UPGRADE_RESOURCE_COST);
		hks::setfield(L, -2, "RESULT_RESOURCE_COST");
		hks::setfield(L, hks::LUA_GLOBAL, "UnitUpgradeRules");
		return 0;
	}

	void ResetForNewGame() { gMinLevel.clear(); gBuildingRule = false; gAlt.clear(); }

	// ---- setup -------------------------------------------------------------------------------------------------------------
	void Create() {
		if (gOk) return;   // hooks stay installed when Dev CE starts again in the same process
		const uintptr_t b = Runtime::GameCoreAddress;
		for (const Check& c : kChecks) {
			if (memcmp((void*)(b + c.rva), c.bytes, c.n) != 0) {
				DevBridge::Log("[UnitUpgradeRules] %s has unexpected bytes at 0x%llx: upgrade rules are not available", c.name, (unsigned long long)c.rva);
				return;
			}
		}
		GetNextUpgrade = (GetNextUpgradeFn)(b + GET_NEXT_UPGRADE_OFFSET);
		HasPrerequisites = (HasPrerequisitesFn)(b + HAS_PREREQUISITES_OFFSET);
		CheckCycle = (CheckCycleFn)(b + CHECK_UPGRADE_CYCLE_OFFSET);
		GetUnitDefinitions = (GetUnitDefinitionsFn)(b + GET_UNIT_DEFINITIONS_OFFSET);
		ReportAddString = (AddStringFn)(b + UnitCommandActions::REPORTING_ADD_STRING_OFFSET);
		if (HMODULE loc = GetModuleHandleW(L"Localization_FinalRelease.dll")) {
			LocDtor = (LocDtorFn)GetProcAddress(loc, "??1String@Localization@@UEAA@XZ");
			LocLookup = (LocLookupFn)GetProcAddress(loc, "?Lookup@Localization@@YA?AVString@1@PEBD@Z");
			LocPushInt = (LocPushIntFn)GetProcAddress(loc, "?PushArgument@String@Localization@@QEAA_NH@Z");
			LocPushStr = (LocPushStrFn)GetProcAddress(loc, "?PushArgument@String@Localization@@QEAA_NPEBD@Z");
			gReportsOk = LocDtor && LocLookup && LocPushInt && LocPushStr;
		}
		gBuildingRuleOk = true;
		for (const Check& c : kBuildingChecks) {
			if (memcmp((void*)(b + c.rva), c.bytes, c.n) != 0) {
				DevBridge::Log("[UnitUpgradeRules] %s has unexpected bytes at 0x%llx: the building prerequisite rule is not available", c.name, (unsigned long long)c.rva);
				gBuildingRuleOk = false;
				break;
			}
		}
		gAltOk = true;
		for (const Check& c : kAltChecks) {
			if (memcmp((void*)(b + c.rva), c.bytes, c.n) != 0) {
				DevBridge::Log("[UnitUpgradeRules] %s has unexpected bytes at 0x%llx: alternative upgrades are not available", c.name, (unsigned long long)c.rva);
				gAltOk = false;
				break;
			}
		}
		if (gAltOk) {
			const uintptr_t* vt = (const uintptr_t*)(b + UPGRADE_COMMAND_VTABLE);
			const uintptr_t* vt2 = (const uintptr_t*)(b + XP2_UPGRADE_COMMAND_VTABLE);
			if (vt[2] != b + UPGRADE_COMMAND_CAN_START_OFFSET || vt[4] != b + UPGRADE_COMMAND_START_OFFSET
				|| vt2[2] != b + UPGRADE_COMMAND_CAN_START_OFFSET || vt2[4] != b + XP2_UPGRADE_COMMAND_START_OFFSET) {
				DevBridge::Log("[UnitUpgradeRules] Upgrade command vtables are not as expected: alternative upgrades are not available");
				gAltOk = false;
			}
		}
		GetUpgradeManager = (GetUpgradeManagerFn)(b + GET_UPGRADE_MANAGER_OFFSET);
		GetUpgradeCost = (GetUpgradeCostFn)(b + GET_UPGRADE_COST_OFFSET);
		GetUnitEfficiency = (GetUnitEfficiencyFn)(b + GET_UNIT_EFFICIENCY_OFFSET);
		PlayerIsHuman = (PlayerBoolFn)(b + UnitCommandActions::PLAYER_IS_HUMAN_OFFSET);
		UnitGetPlot = (UnitGetPlotFn)(b + UnitCommandActions::UNIT_GET_PLOT_OFFSET);
		GetCity = (GetCityFn)(b + GET_CITY_OFFSET);
		HasBuilding = (BuildingsTestFn)(b + CITY_BUILDINGS_HAS_BUILDING_OFFSET);
		IsPillaged = (BuildingsTestFn)(b + CITY_BUILDINGS_IS_PILLAGED_OFFSET);
		Runtime::CreateHook((LPVOID)(b + GET_AVAILABLE_UPGRADE_OFFSET), (LPVOID)&Hook_GetAvailableUpgrade, &base_GetAvailableUpgrade);
		Runtime::CreateHook((LPVOID)(b + GET_UPGRADE_UNIT_TYPE_OFFSET), (LPVOID)&Hook_GetUpgradeUnitType, &base_GetUpgradeUnitType);
		Runtime::CreateHook((LPVOID)(b + GET_UPGRADE_RESOURCE_COST_OFFSET), (LPVOID)&Hook_GetUpgradeResourceCost, &base_GetUpgradeResourceCost);
		Runtime::CreateHook((LPVOID)(b + CAN_UPGRADE_OFFSET), (LPVOID)&Hook_CanUpgrade, &base_CanUpgrade);
		if (gAltOk) {
			Runtime::CreateHook((LPVOID)(b + UPGRADE_COMMAND_CAN_START_OFFSET), (LPVOID)&Hook_CmdCanStart, &base_CmdCanStart);
			Runtime::CreateHook((LPVOID)(b + UPGRADE_COMMAND_START_OFFSET), (LPVOID)&Hook_CmdStart, &base_CmdStart);
			Runtime::CreateHook((LPVOID)(b + XP2_UPGRADE_COMMAND_START_OFFSET), (LPVOID)&Hook_CmdStartXP2, &base_CmdStartXP2);
		}
		gOk = true;
		CreateOutsideTerritory(b);   // after the CanUpgrade hook: it sets the unit the redirected check reads
		DevBridge::Log("[UnitUpgradeRules] hooks installed (no upgrading into build-disabled units; minimum levels from Lua; building prerequisite rule %s; alternative upgrades %s; EFFECT_DEVCE_UNIT_UPGRADE_OUTSIDE_TERRITORY %s)%s",
			gBuildingRuleOk ? "available" : "NOT available", gAltOk ? "available" : "NOT available", gOutsideOk ? "available" : "NOT available",
			gReportsOk ? "" : "; Localization exports not found: no failure reasons in the unit panel");
	}
}
