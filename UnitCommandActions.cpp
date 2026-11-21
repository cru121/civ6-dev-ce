// SPDX-License-Identifier: AGPL-3.0-only
// Part of Dev CE, a fork of the Civilization VI Community Extension by Wild-W (https://github.com/Wild-W/CivilizationVI_CommunityExtension).
// Licensed under the GNU Affero General Public License v3.0 (see LICENSE.txt). Not endorsed by Wild-W, Firaxis or 2K.
#include "UnitCommandActions.h"
#include "GreatPersonAssign.h"
#include "Runtime.h"
#include "DevBridge.h"
#include <cstring>
#include <cstddef>

namespace UnitCommandActions {
	// ---- engine functions -------------------------------------------------------------------------------------------------
	typedef void* (__fastcall* ManagerGetFn)();
	typedef void(__fastcall* ManagerRegisterFn)(void* manager, void* handler);
	typedef int(__fastcall* ManagerIndexFromHashFn)(void* manager, uint32_t hash);
	typedef void* (__fastcall* ManagerGetHandlerFn)(void* manager, int index);
	typedef void* (__fastcall* GetDefinitionsFn)();
	typedef bool(__fastcall* CanActivateFn)(void** individualDef, int owner, bool flag, const int* plotCoord, void* results);
	typedef void* (__fastcall* UnitGetPlotFn)(void* unit);
	typedef void(__fastcall* UnitFinishMovesFn)(void* unit);
	typedef void(__fastcall* AttachFn)(void* eventHandler, void* unit, int individual);
	typedef bool(__fastcall* PlayerBoolFn)(void* player);
	typedef void(__fastcall* DoTurnActivateFn)(void* homeland, int player, bool activate);
	typedef bool(__fastcall* HasPromotionFn)(void* experience, int promotion);
	typedef void* (__fastcall* EditMapFn)();
	typedef void* (__fastcall* GetPlayerVisibilityFn)(int player);
	typedef int(__fastcall* PlotDistanceFn)(void* map, int x1, int y1, int x2, int y2);
	typedef void* (__fastcall* PlotByIndexFn)(void* map, int index);
	typedef void(__fastcall* SetProcessedTurnFn)(void* unit, bool processed);
	typedef void* (__fastcall* OperationsManagerGetFn)();
	typedef void* (__fastcall* MoveToMakeRequestFn)(void* outRequest, int x, int y, int flags);
	typedef void(__fastcall* OperationsRequestFn)(void* manager, void* unit, void* request, int flags);
	typedef void(__fastcall* AddStringFn)(void* results, uint32_t hash, const void* locString);
	typedef void* (__fastcall* GreatPeopleGetFn)();
	typedef void(__fastcall* GpTextFn)(void* greatPeople, int individual, char** outString, bool flag);
	typedef char* (__fastcall* StringEmptyFn)();
	typedef void(__fastcall* StringDtorFn)(char** s);
	typedef void(__fastcall* GetActivationPlotsFn)(int individual, int player, void* outVector);
	typedef void* (__fastcall* DefinitionFindFn)(void* collection, int index);
	typedef void(__fastcall* ResultsSetStringFn)(void* results, uint32_t hash, const char* s);
	// Localization_FinalRelease.dll (Localization::String is a C++ object; we keep it in a raw buffer)
	typedef void* (__fastcall* LocCtorFn)(void* self, const char* s);
	typedef void(__fastcall* LocDtorFn)(void* self);
	typedef void* (__fastcall* LocLookupFn)(void* out, const char* key);
	typedef bool(__fastcall* LocPushStrFn)(void* self, const char* s);
	typedef bool(__fastcall* LocPushIntFn)(void* self, int n);

	static ManagerGetFn ManagerGet;
	static GetDefinitionsFn GetIndividualDefinitions;
	static CanActivateFn CanActivate;
	static UnitGetPlotFn UnitGetPlot;
	static UnitFinishMovesFn UnitFinishMoves;
	static AttachFn AttachActionEffects;
	static PlayerBoolFn PlayerIsHuman, PlayerIsBarbarian;
	static DoTurnActivateFn base_DoTurnActivate;
	static HasPromotionFn HasPromotion;
	static EditMapFn EditMap;
	static GetPlayerVisibilityFn GetPlayerVisibility;
	static PlotDistanceFn PlotDistance;
	static SetProcessedTurnFn SetProcessedTurn;
	static OperationsManagerGetFn OperationsManagerGet;
	static MoveToMakeRequestFn MoveToMakeRequest;
	static AddStringFn ReportAddString;
	static GreatPeopleGetFn GreatPeopleGet;
	static GpTextFn GpActionNameText, GpActionEffectText;
	static StringEmptyFn StringEmpty;
	static StringDtorFn StringDtor;
	static GetActivationPlotsFn GetActivationPlots;
	static GetDefinitionsFn GetDistrictDefinitions, GetBuildingDefinitions, GetFeatureDefinitions, GetGreatWorkObjectDefinitions;
	static DefinitionFindFn DefinitionFind;
	static LocCtorFn LocCtor;
	static LocDtorFn LocDtor;
	static LocLookupFn LocLookup;
	static LocPushStrFn LocPushStr;
	static LocPushIntFn LocPushInt;
	static void** CurrentGame;   // &Context::Globals::ms_pkCurrentGame
	static bool gOk = false;
	static bool gReportsOk = false;   // the Localization exports were found: the unit panel gets name, effect and failure reasons

	struct Check { const char* name; uintptr_t rva; uint8_t bytes[16]; size_t n; };
	static const Check kChecks[] = {
		{ "Unit::Commands::Manager::Get", MANAGER_GET_OFFSET, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x8b, 0x1d }, 8 },
		{ "handler GetTypeHash", HANDLER_GET_TYPE_HASH_OFFSET, { 0x8b, 0x41, 0x0c, 0xc3 }, 4 },
		{ "handler return false", HANDLER_RETURN_FALSE_OFFSET, { 0x32, 0xc0, 0xc3 }, 3 },
		{ "BaseHandler::MakeRequest", BASE_MAKE_REQUEST_OFFSET, { 0x48, 0x89, 0x74, 0x24, 0x20, 0x41, 0x56, 0x48 }, 8 },
		{ "default MakeParameters", DEFAULT_MAKE_PARAMETERS_OFFSET, { 0x33, 0xc0, 0x48, 0x89, 0x02, 0x48, 0x89, 0x42 }, 8 },
		{ "BaseHandler::GetCommandHelpText", BASE_GET_COMMAND_HELP_TEXT_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x81 }, 8 },
		{ "handler SetIndex", HANDLER_SET_INDEX_OFFSET, { 0x89, 0x51, 0x08, 0xc3 }, 4 },
		{ "Unit::GreatPerson::CanActivate (static)", GP_CAN_ACTIVATE_OFFSET, { 0x89, 0x54, 0x24, 0x10, 0x55, 0x53, 0x41, 0x54 }, 8 },
		{ "Unit::IInstance::GetPlot", UNIT_GET_PLOT_OFFSET, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b }, 8 },
		{ "Unit::Instance::FinishMoves", UNIT_FINISH_MOVES_OFFSET, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x83, 0xb9 }, 8 },
		{ "OnGreatPersonActivated_AttachGameEffects", ATTACH_ACTION_EFFECTS_OFFSET, { 0x48, 0x85, 0xd2, 0x0f, 0x84, 0x68, 0x09, 0x00 }, 8 },
		{ "Player::Instance::IsHuman", PLAYER_IS_HUMAN_OFFSET, { 0x48, 0x83, 0xec, 0x28, 0x8b, 0x91, 0xd8, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x0d, 0x47, 0x12, 0x89 }, 16 },
		{ "Player::Instance::IsBarbarian", PLAYER_IS_BARBARIAN_OFFSET, { 0x48, 0x83, 0xec, 0x28, 0x8b, 0x91, 0xd8, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x0d, 0xd7, 0x12, 0x89 }, 16 },
		{ "AI::Homeland::DoTurnActivate", HOMELAND_DO_TURN_ACTIVATE_OFFSET, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b }, 8 },
		{ "Unit::Instance::HasAbility (reads of the ability counts +0x358 / +0x348)", UNIT_HAS_ABILITY_OFFSET + 0x15, { 0x3b, 0x83, 0x58, 0x03, 0x00, 0x00, 0x7d, 0x17, 0x48, 0x63, 0xd0, 0x48, 0x8b, 0x83, 0x48, 0x03 }, 16 },
		{ "Unit::Experience::HasPromotion", EXPERIENCE_HAS_PROMOTION_OFFSET, { 0x48, 0x8b, 0x41, 0x30, 0x48, 0x63, 0xd2, 0x0f }, 8 },
		{ "Context::Globals::EditMap", EDIT_MAP_OFFSET, { 0x48, 0x8b, 0x05, 0x21, 0x5d, 0xb4, 0x00 }, 7 },
		{ "Context::Globals::GetPlayerVisibility", GET_PLAYER_VISIBILITY_OFFSET, { 0x48, 0x8b, 0x05, 0x51, 0x5b, 0xb4, 0x00, 0x8b }, 8 },
		{ "Map::IInstance::GetPlotDistance", MAP_GET_PLOT_DISTANCE_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x45, 0x8b }, 8 },
		{ "Unit::Instance::SetProcessedTurn", UNIT_SET_PROCESSED_TURN_OFFSET, { 0x88, 0x54, 0x24, 0x10, 0x53, 0x48, 0x83, 0xec }, 8 },
		{ "Unit::Operations::Manager::Get", OPERATIONS_MANAGER_GET_OFFSET, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x8b, 0x1d }, 8 },
		{ "Unit::Operations::MoveTo::MakeRequest", MOVE_TO_MAKE_REQUEST_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c }, 8 },
		{ "Definitions::GetGreatPersonIndividualDefinitions", GreatPersonAssign::GET_INDIVIDUAL_DEFINITIONS_OFFSET, { 0x48, 0x83, 0xec, 0x28, 0xe8, 0x67, 0x40, 0x87 }, 8 },
		{ "read of ms_pkCurrentGame", GreatPersonAssign::CURRENT_GAME_READ_SITE, { 0x48, 0x8b, 0x0d, 0x52, 0x53, 0x53, 0x00 }, 7 },
		{ "Reporting::AddString", REPORTING_ADD_STRING_OFFSET, { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x18, 0x55 }, 8 },
		{ "Game::GreatPeople::Get", GREAT_PEOPLE_GET_OFFSET, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x8b, 0x1d }, 8 },
		{ "GreatPeople::GetActionNameText", GP_ACTION_NAME_TEXT_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c }, 8 },
		{ "GreatPeople::GetActionEffectText", GP_ACTION_EFFECT_TEXT_OFFSET, { 0x44, 0x88, 0x4c, 0x24, 0x20, 0x55, 0x53, 0x41 }, 8 },
		{ "String::Details::GetUniversalEmptyBasicString", STRING_EMPTY_OFFSET, { 0x48, 0x83, 0xec, 0x28, 0x65, 0x48, 0x8b, 0x04 }, 8 },
		{ "String::BasicT::~BasicT", STRING_DTOR_OFFSET, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b }, 8 },
		{ "Unit::GreatPerson::GetActivationPlots", GP_GET_ACTIVATION_PLOTS_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74 }, 8 },
		{ "Definitions::GetDistrictDefinitions", GET_DISTRICT_DEFINITIONS_OFFSET, { 0x48, 0x83, 0xec, 0x28, 0xe8, 0xe7, 0x44, 0x87 }, 8 },
		{ "Definitions::GetBuildingDefinitions", GET_BUILDING_DEFINITIONS_OFFSET, { 0x48, 0x83, 0xec, 0x28, 0xe8, 0x27, 0x49, 0x87 }, 8 },
		{ "Definitions::GetFeatureDefinitions", GET_FEATURE_DEFINITIONS_OFFSET, { 0x48, 0x83, 0xec, 0x28, 0xe8, 0x27, 0x43, 0x87 }, 8 },
		{ "Definitions::GetGreatWorkObjectTypeDefinitions", GET_GREAT_WORK_OBJECT_DEFINITIONS_OFFSET, { 0x48, 0x83, 0xec, 0x28, 0xe8, 0x27, 0x40, 0x87 }, 8 },
		{ "DefinitionCollectionWithTypeIndex::Find", DEFINITION_FIND_OFFSET, { 0x48, 0x8b, 0x01, 0x4c, 0x8b, 0x41, 0x08, 0x4c }, 8 },
	};

	// ---- our command handlers ---------------------------------------------------------------------------------------------
	// Same layout as the game's handlers (Unit::Commands::BaseHandler: vtable, index, hash). The manager owns them once registered and
	// deletes them through vtable slot 0 when the game ends, so slot 0 is ours (allocated with our new, freed with our delete).
	constexpr int MAX_GRANTS = 1024;
	enum GrantKind : int32_t { GRANT_UNIT_TYPE = 0, GRANT_PROMOTION = 1, GRANT_ABILITY = 2 };
	struct Grant { int32_t kind; int32_t index; };   // index = UnitTypes / UnitPromotionTypes / UnitAbilityTypes (validated by the caller against GameInfo)
	struct Handler {
		void** vtable;          // +0x00
		int32_t index;          // +0x08 set by the manager (slot 8)
		uint32_t hash;          // +0x0c UnitCommands Types.Hash (slot 1 returns it)
		int32_t individual;     // template GreatPersonIndividuals index
		int32_t aiRadius;       // AI: how far (plots) a unit may move to a plot where it can use the command; 0 = never moves for it
		int32_t grantCount;
		Grant grants[MAX_GRANTS];
	};
	static_assert(offsetof(Handler, index) == 8 && offsetof(Handler, hash) == 0xc, "handler layout must match Unit::Commands::BaseHandler");

	constexpr int MAX_COMMANDS = 256;
	static Handler* gHandlers[MAX_COMMANDS];
	static int gHandlerCount = 0;
	static void* gVtable[9];

	static void Forget(Handler* h) {
		for (int i = 0; i < gHandlerCount; i++) {
			if (gHandlers[i] == h) { gHandlers[i] = gHandlers[--gHandlerCount]; gHandlers[gHandlerCount] = nullptr; return; }
		}
	}
	static Handler* FindOurs(uint32_t hash) {
		for (int i = 0; i < gHandlerCount; i++) if (gHandlers[i]->hash == hash) return gHandlers[i];
		return nullptr;
	}
	static bool HasGrant(const Handler* h, GrantKind kind, int index) {
		for (int i = 0; i < h->grantCount; i++) if (h->grants[i].kind == kind && h->grants[i].index == index) return true;
		return false;
	}
	static bool UnitHasPromotion(char* unit, int promotion) {
		__try {
			return HasPromotion(unit + UNIT_EXPERIENCE, promotion);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}
	// Unit::Instance::HasAbility (0x3a3d60) without the hash lookup: ability counts per UnitAbilities index, range-checked as the game does.
	static bool UnitHasAbility(char* unit, int ability) {
		__try {
			const int count = *(int*)(unit + UNIT_ABILITY_COUNT);
			const int* counts = *(const int**)(unit + UNIT_ABILITY_COUNTS);
			return ability >= 0 && ability < count && counts != nullptr && counts[ability] > 0;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}
	// A unit may use the command if its type is granted, or it has a granted promotion or ability.
	static bool IsGranted(const Handler* h, char* unit) {
		const int unitType = *(int*)(unit + UNIT_TYPE);
		for (int i = 0; i < h->grantCount; i++) {
			const Grant& g = h->grants[i];
			if (g.kind == GRANT_UNIT_TYPE && g.index == unitType) return true;
			if (g.kind == GRANT_PROMOTION && UnitHasPromotion(unit, g.index)) return true;
			if (g.kind == GRANT_ABILITY && UnitHasAbility(unit, g.index)) return true;
		}
		return false;
	}

	// Requirements::Results, initialised as ActivateGreatPerson::CanStart does (104 bytes; 0 = no need, -1 = no type needed)
	static void InitResults(uint8_t* r) {
		memset(r, 0, 256);
		const uintptr_t minusOne4[] = { 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c, 0x24, 0x3c, 0x44, 0x4c, 0x50, 0x5c };
		for (uintptr_t off : minusOne4) *(int32_t*)(r + off) = -1;
	}

	static char* IndividualDefinition(int individual) {
		char* defs = (char*)GetIndividualDefinitions();
		char** begin = *(char***)defs;
		char** end = *(char***)(defs + 8);
		if (individual < 0 || individual >= (int)(end - begin)) return nullptr;
		return begin[individual];
	}

	// The template's ActionRequires... rules for this unit at plot (x, y) (the unit does not need to be there). The Requirements::Results
	// (which rules failed) go to resultsOut when given (256 bytes). SEH-guarded (no C++ objects with destructors here).
	static bool RequirementsMetAt(const Handler* h, char* unit, int x, int y, uint8_t* resultsOut = nullptr) {
		uint8_t results[256];
		InitResults(results);
		bool ok = false;
		__try {
			void* def = IndividualDefinition(h->individual);
			if (def != nullptr) {
				const int coord[2] = { x, y };
				char* unitDef = *(char**)(unit + UNIT_DEFINITION);
				const bool flag = unitDef != nullptr && ((*(uint8_t*)(unitDef + UNIT_DEF_GP_FLAGS) >> 2) & 1) != 0;
				ok = CanActivate(&def, *(int*)(unit + UNIT_OWNER), flag, coord, results);
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			ok = false;
		}
		if (resultsOut != nullptr) memcpy(resultsOut, results, sizeof(results));
		return ok;
	}
	// ... at the unit's own plot
	static bool RequirementsMet(const Handler* h, char* unit, uint8_t* resultsOut = nullptr) {
		int x = 0, y = 0;
		__try {
			char* plot = (char*)UnitGetPlot(unit);
			if (plot == nullptr) return false;
			x = *(int16_t*)(plot + PLOT_X);
			y = *(int16_t*)(plot + PLOT_Y);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
		return RequirementsMetAt(h, unit, x, y, resultsOut);
	}

	// ---- unit panel reports (as ActivateGreatPerson::CanStart fills them) ------------------------------------------------------
	// One localized line into results under hash (ADDITIONAL_DESCRIPTION / FAILURE_REASONS), with optional arguments.
	static void AddLoc(void* results, uint32_t hash, const char* key, const char* arg1 = nullptr, const char* arg2 = nullptr, bool hasInt = false, int n = 0) {
		alignas(16) uint8_t s[0x100];   // Localization::String (smaller than this)
		LocLookup(s, key);
		if (arg1 != nullptr) LocPushStr(s, arg1);
		if (arg2 != nullptr) LocPushStr(s, arg2);
		if (hasInt) LocPushInt(s, n);
		ReportAddString(results, hash, s);
		LocDtor(s);
	}
	// A const char* field of a definition, or nullptr.
	static const char* DefinitionText(GetDefinitionsFn collection, int index, uintptr_t field) {
		char* def = (char*)DefinitionFind(collection(), index);
		return def != nullptr ? *(const char**)(def + field) : nullptr;
	}

	// Title (only when the template has ActionNameTextOverride, else the command's own Description stays) and effect text
	// (ActionEffectTextOverride, else the text of the template's action modifiers). SEH-guarded.
	static void FillDescription(const Handler* h, void* results) {
		__try {
			if (*CurrentGame == nullptr) return;
			char* def = IndividualDefinition(h->individual);
			void* greatPeople = GreatPeopleGet();
			if (def == nullptr || greatPeople == nullptr) return;
			if (*(uint8_t*)(def + GPI_HAS_NAME_OVERRIDE) != 0) {
				char* name = StringEmpty();
				GpActionNameText(greatPeople, h->individual, &name, false);
				if (name != nullptr && name[0] != 0) ((ResultsSetStringFn)(*(void***)results)[RESULTS_VT_SET_STRING / 8])(results, RESULT_ACTION_NAME, name);
				StringDtor(&name);
			}
			char* effect = StringEmpty();
			GpActionEffectText(greatPeople, h->individual, &effect, false);
			if (effect != nullptr && effect[0] != 0) {
				alignas(16) uint8_t s[0x100];
				LocCtor(s, effect);
				ReportAddString(results, RESULT_ADDITIONAL_DESCRIPTION, s);
				LocDtor(s);
			}
			StringDtor(&effect);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			DevBridge::Log("[UnitCommandActions] FAULT 0x%08lx writing the description of command 0x%08x", GetExceptionCode(), h->hash);
		}
	}

	// The failed ActionRequires... rules as the unit panel's red lines, in the order and with the texts ActivateGreatPerson::CanStart uses.
	// r = the Requirements::Results from CanActivate. Returns how many lines were added. SEH-guarded.
	struct FlagReason { uint16_t offset; const char* key; };
	static const FlagReason kTerritoryReasons[] = {
		{ 0x34, "LOC_UNITCOMMAND_ACTIVATE_GREAT_PERSON_REQUIRES_OWN_TILE" },
		{ 0x36, "LOC_UNITCOMMAND_ACTIVATE_GREAT_PERSON_REQUIRES_UNOWNED_TILE" },
		{ 0x61, "LOC_UNITCOMMAND_ACTIVATE_GREAT_PERSON_REQUIRES_ENEMY_TERRITORY" },
		{ 0x63, "LOC_UNITCOMMAND_ACTIVATE_GREAT_PERSON_REQUIRES_NONHOSTILE_TERRITORY" },
		{ 0x62, "LOC_UNITCOMMAND_ACTIVATE_GREAT_PERSON_REQUIRES_CITY_STATE_TERRITORY" },
		{ 0x64, "LOC_UNITCOMMAND_ACTIVATE_GREAT_PERSON_REQUIRES_SUZERAIN_TERRITORY" },
		{ 0x40, "LOC_UNITCOMMAND_ACTIVATE_GREAT_PERSON_REQUIRES_ADJACENT_BARBARIAN_UNIT" },
		{ 0x49, "LOC_UNITCOMMAND_ACTIVATE_GREAT_PERSON_REQUIRES_ADJACENT_MOUNTAIN" },
		{ 0x37, "LOC_UNITCOMMAND_ACTIVATE_GREAT_PERSON_REQUIRES_ADJACENT_TO_OWNED_TILE" },
	};
	static void AddFailureReasonsBody(void* results, const uint8_t* r, int& added) {
		{
			auto flag = [r](uintptr_t off) { return r[off] != 0; };
			auto num = [r](uintptr_t off) { return *(const int32_t*)(r + off); };
			auto add = [&](const char* key, const char* a1 = nullptr, const char* a2 = nullptr, bool hasInt = false, int n = 0) {
				AddLoc(results, RESULT_FAILURE_REASONS, key, a1, a2, hasInt, n);
				added++;
			};
			for (const FlagReason& f : kTerritoryReasons) if (flag(f.offset)) add(f.key);
			if (num(0x50) != -1) add("LOC_UNITCOMMAND_ACTIVATE_GREAT_PERSON_REQUIRES_CITY_GREAT_WORK_OBJECT_TYPE",
				DefinitionText(GetGreatWorkObjectDefinitions, num(0x50), 0x30), DefinitionText(GetGreatWorkObjectDefinitions, num(0x50), 0x28));
			if (num(0x18) != -1) add("LOC_UNITCOMMAND_ACTIVATE_GREAT_PERSON_INVALID_DISTRICT", DefinitionText(GetDistrictDefinitions, num(0x18), 0x80));
			if (flag(0x48)) add("LOC_UNITCOMMAND_ACTIVATE_GREAT_PERSON_REQUIRES_INCOMPLETE_SPACE_RACE_PROJECT");
			if (flag(0x54)) add("LOC_UNITCOMMAND_ACTIVATE_GREAT_PERSON_REQUIRES_INCOMPLETE_WONDER");
			if (flag(0x3a) || num(0x3c) != -1) {   // a military unit / a unit of a domain nearby
				const char* key = "LOC_UNITCOMMAND_ACTIVATE_GREAT_PERSON_REQUIRES_MILITARY_UNIT";
				if (num(0x3c) == 2) key = "LOC_UNITCOMMAND_ACTIVATE_GREAT_PERSON_REQUIRES_DOMAIN_TYPE_LAND";
				else if (num(0x3c) == 0) key = "LOC_UNITCOMMAND_ACTIVATE_GREAT_PERSON_REQUIRES_DOMAIN_TYPE_SEA";
				else if (num(0x3c) != -1) key = "LOC_UNITCOMMAND_ACTIVATE_GREAT_PERSON_REQUIRES_DOMAIN_TYPE";
				add(key);
			}
			if (flag(0x65)) add("LOC_UNITCOMMAND_ACTIVATE_GREAT_PERSON_REQUIRES_EXPERIENCE_UNIT");
			if (num(0x5c) >= 0) add("LOC_GREATPERSON_ACTION_USAGE_ON_MILITARY_LAND_UNIT_WITHIN_RANGE", nullptr, nullptr, true, num(0x5c));
			if (flag(0x60)) add("LOC_UNITCOMMAND_ACTIVATE_GREAT_PERSON_REQUIRES_NEARBY_VALID_UNIT");
			if (flag(0x39)) add("LOC_UNITCOMMAND_ACTIVATE_GREAT_PERSON_REQUIRES_NO_MILITARY_UNIT");
			if (num(0x4c) != -1) add("LOC_UNITCOMMAND_ACTIVATE_GREAT_PERSON_REQUIRES_ON_OR_ADJACENT_FEATURE_TYPE", DefinitionText(GetFeatureDefinitions, num(0x4c), 0x58));
			if (flag(0x4a)) add("LOC_UNITCOMMAND_ACTIVATE_GREAT_PERSON_REQUIRES_ON_OR_ADJACENT_NATURAL_WONDER");
			switch (num(0x44)) {   // military formation
			case -1: break;
			case 0: add("LOC_UNITCOMMAND_ACTIVATE_GREAT_PERSON_REQUIRES_STANDARD_MILITARY_FORMATION"); break;
			case 1: add("LOC_UNITCOMMAND_ACTIVATE_GREAT_PERSON_REQUIRES_CORPS_MILITARY_FORMATION"); break;
			case 2: add("LOC_UNITCOMMAND_ACTIVATE_GREAT_PERSON_REQUIRES_ARMY_MILITARY_FORMATION"); break;
			default: add("LOC_UNITCOMMAND_ACTIVATE_GREAT_PERSON_REQUIRES_UNIT_WITH_MILITARY_FORMATION"); break;
			}
			if (flag(0x38)) add("LOC_UNITCOMMAND_ACTIVATE_GREAT_PERSON_REQUIRES_VISIBLE_LUXURY");
			if (num(0x30) > 0) {   // gold needed, fixed point 24.8, rounded up
				const int v = num(0x30);
				add("LOC_UNITCOMMAND_ACTIVATE_GREAT_PERSON_INSUFFICIENT_GOLD", nullptr, nullptr, true, (v >> 8) + ((v & 0xff) != 0 ? 1 : 0));
			}
			if (flag(0x2d)) add("LOC_UNITCOMMAND_ACTIVATE_GREAT_PERSON_NO_GREATWORK_SLOT");
			if (flag(0x2e)) add("LOC_UNITCOMMAND_ACTIVATE_GREAT_PERSON_REQUIRES_OPEN_PLAYER_RELIC_SLOT");
			if (num(0x10) != -1) add("LOC_UNITCOMMAND_ACTIVATE_GREAT_PERSON_REQUIRES_ON_MISSING_BUILDING", DefinitionText(GetBuildingDefinitions, num(0x10), 0x68));
		}
	}
	static int AddFailureReasons(void* results, const uint8_t* r) {
		int added = 0;
		__try {
			AddFailureReasonsBody(results, r, added);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			DevBridge::Log("[UnitCommandActions] FAULT 0x%08lx writing failure reasons", GetExceptionCode());
		}
		return added;
	}
	static void AddFailure(void* results, const char* key) {
		__try { AddLoc(results, RESULT_FAILURE_REASONS, key); } __except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	// results: the unit panel's Reporting results (nullptr when nobody asked for them)
	static bool CanStartImpl(const Handler* h, char* unit, bool testOnly, void* results = nullptr) {
		if (!gOk || unit == nullptr) return false;
		if (!IsGranted(h, unit)) return false;
		const bool report = results != nullptr && gReportsOk;
		if (report) FillDescription(h, results);
		if (testOnly) return true;                      // the unit panel's "could this unit ever do it" check: show the button
		if (*(uint8_t*)(unit + UNIT_DYING) != 0) return false;
		if (*(int32_t*)(unit + UNIT_MOVES) <= 0) {      // once per turn: Start ends the unit's moves
			if (report) AddFailure(results, "LOC_DEVCE_UNITCOMMAND_NO_MOVES");
			return false;
		}
		uint8_t r[256];
		const bool ok = RequirementsMet(h, unit, r);
		if (!ok && report && AddFailureReasons(results, r) == 0) AddFailure(results, "LOC_DEVCE_UNITCOMMAND_REQUIREMENTS_NOT_MET");
		return ok;
	}

	// Attaches the template's action modifiers around the unit and ends its moves. SEH-guarded.
	static unsigned long ApplyAction(const Handler* h, char* unit) {
		__try {
			char* game = (char*)*CurrentGame;
			if (game == nullptr) return 1;
			char* gameEffects = *(char**)(game + GAME_EFFECTS);
			if (gameEffects == nullptr) return 1;
			AttachActionEffects(gameEffects + GAME_EFFECTS_EVENT_HANDLER, unit, h->individual);
			UnitFinishMoves(unit);
			return 0;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return GetExceptionCode();
		}
	}

	static bool StartImpl(Handler* h, char* unit) {
		if (!CanStartImpl(h, unit, false)) {
			DevBridge::Log("[UnitCommandActions] command 0x%08x on unit %d: requirements not met, nothing done", h->hash, unit ? *(int*)(unit + UNIT_ID) : -1);
			return false;
		}
		const unsigned long fault = ApplyAction(h, unit);
		if (fault) {
			DevBridge::Log("[UnitCommandActions] FAULT 0x%08lx applying command 0x%08x (individual %d) on unit %d", fault, h->hash, h->individual, *(int*)(unit + UNIT_ID));
			return false;
		}
		DevBridge::Log("[UnitCommandActions] unit %d (player %d) used command 0x%08x (individual %d)", *(int*)(unit + UNIT_ID), *(int*)(unit + UNIT_OWNER), h->hash, h->individual);
		return true;
	}

	// vtable slot 0: vector deleting destructor (flags bit 0 = free the memory)
	static void* __fastcall HandlerDestroy(Handler* self, unsigned int flags) {
		Forget(self);
		if (flags & 1) delete self;
		return self;
	}
	// slot 2: CanStart(unit, request, testOnly, results)
	static bool __fastcall HandlerCanStart(Handler* self, char* unit, void* /*request*/, bool testOnly, void* results) {
		return CanStartImpl(self, unit, testOnly, results);
	}
	// slot 4: Start(unit, request)
	static void __fastcall HandlerStart(Handler* self, char* unit, void* /*request*/) {
		StartImpl(self, unit);
	}

	// ---- AI ------------------------------------------------------------------------------------------------------------------
	// Nearest plot within h->aiRadius that the unit's owner has revealed and where the template's requirements pass.
	// Same idea as Unit::GreatPerson::GetActivationPlots (all revealed plots where CanActivate passes), limited by distance.
	static bool FindTarget(const Handler* h, char* unit, int* outX, int* outY) {
		__try {
			char* map = (char*)EditMap();
			char* vis = (char*)GetPlayerVisibility(*(int*)(unit + UNIT_OWNER));
			if (map == nullptr || vis == nullptr) return false;
			const int plots = *(int*)(map + MAP_PLOT_COUNT);
			const int width = *(int*)(vis + VIS_WIDTH);
			const uint8_t* revealed = *(const uint8_t**)(vis + VIS_REVEALED);
			const int ux = *(int*)(unit + UNIT_X), uy = *(int*)(unit + UNIT_Y);
			PlotByIndexFn plotByIndex = (PlotByIndexFn)(*(void***)map)[MAP_VT_PLOT_BY_INDEX / 8];
			int best = h->aiRadius + 1;
			for (int i = 0; i < plots; i++) {
				char* plot = (char*)plotByIndex(map, i);
				if (plot == nullptr) continue;
				const int x = *(int16_t*)(plot + PLOT_X), y = *(int16_t*)(plot + PLOT_Y);
				if (revealed == nullptr || !revealed[width * y + x]) continue;
				const int d = PlotDistance(map, ux, uy, x, y);
				if (d < 1 || d >= best) continue;
				if (!RequirementsMetAt(h, unit, x, y)) continue;
				best = d; *outX = x; *outY = y;
			}
			return best <= h->aiRadius;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// A move order exactly as AI::BehaviorTree::MoveUnitNode gives it: MoveTo::MakeRequest, Operations::Manager vtable +0x60, then release the
	// request's shared parameters (the same reference counting the game does inline).
	static bool MoveUnitTo(char* unit, int x, int y) {
		__try {
			void* operations = OperationsManagerGet();
			if (operations == nullptr) return false;
			alignas(16) uint8_t request[0x40];
			memset(request, 0, sizeof(request));
			MoveToMakeRequest(request, x, y, 0);
			((OperationsRequestFn)(*(void***)operations)[OPERATIONS_VT_REQUEST / 8])(operations, unit, request, 0);
			void** control = *(void***)(request + 0x10);
			if (control != nullptr && InterlockedDecrement((volatile long*)((char*)control + 8)) == 0) {
				((void(__fastcall*)(void*))((*(void***)control)[0]))(control);
				if (InterlockedDecrement((volatile long*)((char*)control + 0xc)) == 0) ((void(__fastcall*)(void*))((*(void***)control)[1]))(control);
			}
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	static void MarkProcessed(char* unit) {
		__try { SetProcessedTurn(unit, true); } __except (EXCEPTION_EXECUTE_HANDLER) {}
	}

	// At the start of an AI player's turn, for each unit with a command: use it where the unit stands, or move to the nearest plot within the
	// command's AI search radius where it can be used, and use it there if moves are left. Units sent somewhere are marked as processed for the
	// turn (as the game's MoveUnitNode does), so the homeland/tactical AI leaves them alone. This version treats the unit's first granted
	// command as its goal (one command per unit is the expected case); choosing the most valuable of several comes later.
	static void UseCommandsForPlayer(char* player) {
		char* units = *(char**)(player + PLAYER_UNITS);
		if (units == nullptr) return;
		// Snapshot first: an action may create or remove units.
		constexpr int MAX_UNITS = 2048;
		static char* snapshot[MAX_UNITS];
		int n = 0;
		for (void** node = *(void***)(units + UNITS_LIST); node != nullptr && n < MAX_UNITS; node = (void**)node[2]) snapshot[n++] = (char*)node[0];
		int used = 0, moved = 0;
		for (int i = 0; i < n; i++) {
			char* unit = snapshot[i];
			bool stillThere = false;   // re-check that the unit still exists before touching it
			for (void** node = *(void***)(units + UNITS_LIST); node != nullptr; node = (void**)node[2]) if (node[0] == unit) { stillThere = true; break; }
			if (!stillThere) continue;
			if (*(uint8_t*)(unit + UNIT_DYING) != 0 || *(int32_t*)(unit + UNIT_MOVES) <= 0) continue;
			Handler* goal = nullptr;
			bool done = false;
			for (int c = 0; c < gHandlerCount; c++) {
				Handler* h = gHandlers[c];
				if (!IsGranted(h, unit)) continue;
				if (goal == nullptr) goal = h;
				if (CanStartImpl(h, unit, false)) { if (StartImpl(h, unit)) used++; done = true; break; }   // one command per unit (its moves are used up)
			}
			if (done || goal == nullptr || goal->aiRadius <= 0) continue;
			int tx = 0, ty = 0;
			if (!FindTarget(goal, unit, &tx, &ty)) continue;
			const int fromX = *(int*)(unit + UNIT_X), fromY = *(int*)(unit + UNIT_Y);
			if (!MoveUnitTo(unit, tx, ty)) continue;
			MarkProcessed(unit);
			moved++;
			DevBridge::Log("[UnitCommandActions] AI unit %d (player %d) moves (%d,%d) -> (%d,%d) to use command 0x%08x", *(int*)(unit + UNIT_ID), *(int*)(unit + UNIT_OWNER), fromX, fromY, tx, ty, goal->hash);
			if (CanStartImpl(goal, unit, false) && StartImpl(goal, unit)) used++;   // arrived with moves left
		}
		if (used || moved) DevBridge::Log("[UnitCommandActions] AI player %d: %d unit commands used, %d units moved towards a target", *(int*)(player + 0xd8), used, moved);
	}

	static bool IsAiTurn(char* homeland, int playerId, char** outPlayer) {
		__try {
			if (*(int*)(homeland + HOMELAND_PLAYER_ID) != playerId) return false;
			char* player = *(char**)(homeland + HOMELAND_PLAYER);
			if (player == nullptr || PlayerIsBarbarian(player) || PlayerIsHuman(player)) return false;
			*outPlayer = player;
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	static void __fastcall Hook_DoTurnActivate(void* homeland, int playerId, bool activate) {
		base_DoTurnActivate(homeland, playerId, activate);
		if (!gOk || !activate || gHandlerCount == 0) return;
		char* player = nullptr;
		if (IsAiTurn((char*)homeland, playerId, &player)) UseCommandsForPlayer(player);
	}

	// ---- Lua ---------------------------------------------------------------------------------------------------------------
	static void* Manager() { return ManagerGet ? ManagerGet() : nullptr; }
	static void* VtFn(void* object, uintptr_t slot) { return (*(void***)object)[slot / 8]; }

	// UnitCommandActions.Define(commandHash, templateIndividualIndex) -> true
	static int lDefine(hks::lua_State* L) {
		if (!gOk) { hks::error(L, "UnitCommandActions is disabled (address check failed at startup, see DevBridge.log)"); return 0; }
		const uint32_t hash = (uint32_t)(int64_t)hks::checknumber(L, 1);   // Types.Hash: signed or unsigned in Lua, same 32 bits
		const int individual = hks::checkinteger(L, 2);
		int radius = 3;
		if (hks::gettop(L) >= 3 && hks::isnumber(L, 3)) radius = hks::checkinteger(L, 3);
		if (radius < 0) radius = 0;
		char* defs = (char*)GetIndividualDefinitions();
		char** begin = *(char***)defs;
		char** end = *(char***)(defs + 8);
		if (individual < 0 || individual >= (int)(end - begin) || begin[individual] == nullptr) { hks::error(L, "UnitCommandActions.Define: %d is not a GreatPersonIndividuals index", individual); return 0; }

		if (Handler* h = FindOurs(hash)) { h->individual = individual; h->aiRadius = radius; hks::pushboolean(L, true); return 1; }   // redefinition: update in place
		void* manager = Manager();
		if (manager == nullptr) { hks::error(L, "UnitCommandActions.Define: no unit command manager (is a game running?)"); return 0; }
		const int index = ((ManagerIndexFromHashFn)VtFn(manager, MANAGER_VT_INDEX_FROM_HASH))(manager, hash);
		if (index != -1 && ((ManagerGetHandlerFn)VtFn(manager, MANAGER_VT_GET_HANDLER))(manager, index) != nullptr) {
			hks::error(L, "UnitCommandActions.Define: command 0x%08x already has a game handler (only new UnitCommands rows can be defined)", hash);
			return 0;
		}
		if (gHandlerCount >= MAX_COMMANDS) { hks::error(L, "UnitCommandActions.Define: more than %d commands", MAX_COMMANDS); return 0; }

		Handler* h = new Handler();
		memset(h, 0, sizeof(Handler));
		h->vtable = gVtable;
		h->index = -1;
		h->hash = hash;
		h->individual = individual;
		h->aiRadius = radius;
		gHandlers[gHandlerCount++] = h;
		((ManagerRegisterFn)VtFn(manager, MANAGER_VT_REGISTER))(manager, h);   // the manager sets h->index and owns h from now on
		DevBridge::Log("[UnitCommandActions] command 0x%08x defined: template individual %d, AI search radius %d, manager index %d", hash, individual, radius, h->index);
		hks::pushboolean(L, true);
		return 1;
	}

	static int AddGrant(hks::lua_State* L, GrantKind kind, const char* fnName) {
		const uint32_t hash = (uint32_t)(int64_t)hks::checknumber(L, 1);   // Types.Hash: signed or unsigned in Lua, same 32 bits
		const int index = hks::checkinteger(L, 2);
		if (index < 0) { hks::error(L, "UnitCommandActions.%s: invalid index %d", fnName, index); return 0; }
		Handler* h = FindOurs(hash);
		if (h == nullptr) { hks::error(L, "UnitCommandActions.%s: command 0x%08x is not defined (call Define first)", fnName, hash); return 0; }
		if (!HasGrant(h, kind, index)) {
			if (h->grantCount >= MAX_GRANTS) { hks::error(L, "UnitCommandActions.%s: more than %d grants for one command", fnName, MAX_GRANTS); return 0; }
			h->grants[h->grantCount].kind = kind;
			h->grants[h->grantCount].index = index;
			h->grantCount++;
		}
		hks::pushboolean(L, true);
		return 1;
	}
	// UnitCommandActions.Grant(commandHash, unitTypeIndex) -> true
	static int lGrant(hks::lua_State* L) { return AddGrant(L, GRANT_UNIT_TYPE, "Grant"); }
	// UnitCommandActions.GrantPromotion(commandHash, promotionIndex) -> true. The index MUST be a GameInfo.UnitPromotions index
	// (the game's HasPromotion does not check the range).
	static int lGrantPromotion(hks::lua_State* L) { return AddGrant(L, GRANT_PROMOTION, "GrantPromotion"); }
	// UnitCommandActions.GrantAbility(commandHash, abilityIndex) -> true (a GameInfo.UnitAbilities index; units that have the ability, active count > 0)
	static int lGrantAbility(hks::lua_State* L) { return AddGrant(L, GRANT_ABILITY, "GrantAbility"); }

	// UnitCommandActions.Clear(): removes every grant (the commands stay registered with the game but no unit can use them)
	static int lClear(hks::lua_State* L) {
		for (int i = 0; i < gHandlerCount; i++) gHandlers[i]->grantCount = 0;
		return 0;
	}

	// ---- read-only functions (gameplay and UI) ------------------------------------------------------------------------------
	// UnitCommandActions.IsDefined(commandHash) -> boolean
	static int lIsDefined(hks::lua_State* L) {
		const uint32_t hash = (uint32_t)(int64_t)hks::checknumber(L, 1);
		hks::pushboolean(L, gOk && FindOurs(hash) != nullptr);
		return 1;
	}
	// UnitCommandActions.GetCommandHashes() -> { hash, ... } (as Types.Hash: signed 32 bit, like GameInfo)
	static int lGetCommandHashes(hks::lua_State* L) {
		hks::createtable(L, gHandlerCount, 0);
		for (int i = 0; i < gHandlerCount; i++) {
			hks::pushinteger(L, i + 1);
			hks::pushinteger(L, (int)gHandlers[i]->hash);
			hks::settable(L, -3);
		}
		return 1;
	}

	// Plot indices where the template's requirements pass for the player (Unit::GreatPerson::GetActivationPlots: the whole map, the
	// player's revealed plots). The game's Vector<int> is { data, capacity, ?, count, ? }; we hand it a buffer as large as the map, so it
	// never grows (and never frees) it. Returns the count, or -1 on a fault.
	struct GameIntVector { int* data; uint32_t capacity; uint32_t pad; uint32_t count; uint32_t flags; };
	static int CollectActivationPlots(int individual, int player, int* buffer, int capacity) {
		__try {
			GameIntVector v = { buffer, (uint32_t)capacity, 0, 0, 0 };
			GetActivationPlots(individual, player, &v);
			if (v.data != buffer || (int)v.count > capacity) return -1;   // not expected: the game reallocated
			return (int)v.count;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return -1;
		}
	}
	static int PlotCount() {
		__try {
			char* map = (char*)EditMap();
			return map != nullptr ? *(int*)(map + MAP_PLOT_COUNT) : 0;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return 0;
		}
	}
	// UnitCommandActions.GetActivationPlots(commandHash, playerID) -> { plotIndex, ... } (empty for an unknown command)
	// UI: call it only when not UI.IsGameCoreBusy(), as the base game's highlight code does.
	static int lGetActivationPlots(hks::lua_State* L) {
		const uint32_t hash = (uint32_t)(int64_t)hks::checknumber(L, 1);
		const int player = hks::checkinteger(L, 2);
		const Handler* h = gOk ? FindOurs(hash) : nullptr;
		const int plots = h != nullptr ? PlotCount() : 0;
		if (h == nullptr || plots <= 0 || player < 0) { hks::createtable(L, 0, 0); return 1; }
		int* buffer = new int[plots + 16];
		const int n = CollectActivationPlots(h->individual, player, buffer, plots + 16);
		if (n < 0) DevBridge::Log("[UnitCommandActions] GetActivationPlots failed for command 0x%08x, player %d", hash, player);
		hks::createtable(L, n > 0 ? n : 0, 0);
		for (int i = 0; i < n; i++) {
			hks::pushinteger(L, i + 1);
			hks::pushinteger(L, buffer[i]);
			hks::settable(L, -3);
		}
		delete[] buffer;
		return 1;
	}

	static void PushReadOnly(hks::lua_State* L) {
		PushLuaMethod(L, lIsDefined, "lUnitCommandActionsIsDefined", -2, "IsDefined");
		PushLuaMethod(L, lGetCommandHashes, "lUnitCommandActionsGetCommandHashes", -2, "GetCommandHashes");
		PushLuaMethod(L, lGetActivationPlots, "lUnitCommandActionsGetActivationPlots", -2, "GetActivationPlots");
	}

	int Register(hks::lua_State* L) {
		hks::createtable(L, 0, 8);
		PushLuaMethod(L, lDefine, "lUnitCommandActionsDefine", -2, "Define");
		PushLuaMethod(L, lGrant, "lUnitCommandActionsGrant", -2, "Grant");
		PushLuaMethod(L, lGrantPromotion, "lUnitCommandActionsGrantPromotion", -2, "GrantPromotion");
		PushLuaMethod(L, lGrantAbility, "lUnitCommandActionsGrantAbility", -2, "GrantAbility");
		PushLuaMethod(L, lClear, "lUnitCommandActionsClear", -2, "Clear");
		PushReadOnly(L);
		hks::setfield(L, hks::LUA_GLOBAL, "UnitCommandActions");
		return 0;
	}

	int RegisterUI(hks::lua_State* L) {
		hks::createtable(L, 0, 3);
		PushReadOnly(L);
		hks::setfield(L, hks::LUA_GLOBAL, "UnitCommandActions");
		return 0;
	}

	void ResetForNewGame() {
		// The old game's manager deleted our handlers (slot 0 removes them from the list); anything left is stale.
		for (int i = 0; i < MAX_COMMANDS; i++) gHandlers[i] = nullptr;
		gHandlerCount = 0;
	}

	void Create() {
		for (const Check& c : kChecks) {
			if (memcmp((void*)(Runtime::GameCoreAddress + c.rva), c.bytes, c.n) != 0) {
				DevBridge::Log("[UnitCommandActions] %s has unexpected bytes at 0x%llx: UnitCommandActions is disabled", c.name, (unsigned long long)c.rva);
				return;
			}
		}
		const uintptr_t b = Runtime::GameCoreAddress;
		ManagerGet = (ManagerGetFn)(b + MANAGER_GET_OFFSET);
		GetIndividualDefinitions = (GetDefinitionsFn)(b + GreatPersonAssign::GET_INDIVIDUAL_DEFINITIONS_OFFSET);
		CanActivate = (CanActivateFn)(b + GP_CAN_ACTIVATE_OFFSET);
		UnitGetPlot = (UnitGetPlotFn)(b + UNIT_GET_PLOT_OFFSET);
		UnitFinishMoves = (UnitFinishMovesFn)(b + UNIT_FINISH_MOVES_OFFSET);
		AttachActionEffects = (AttachFn)(b + ATTACH_ACTION_EFFECTS_OFFSET);
		PlayerIsHuman = (PlayerBoolFn)(b + PLAYER_IS_HUMAN_OFFSET);
		PlayerIsBarbarian = (PlayerBoolFn)(b + PLAYER_IS_BARBARIAN_OFFSET);
		HasPromotion = (HasPromotionFn)(b + EXPERIENCE_HAS_PROMOTION_OFFSET);
		EditMap = (EditMapFn)(b + EDIT_MAP_OFFSET);
		GetPlayerVisibility = (GetPlayerVisibilityFn)(b + GET_PLAYER_VISIBILITY_OFFSET);
		PlotDistance = (PlotDistanceFn)(b + MAP_GET_PLOT_DISTANCE_OFFSET);
		SetProcessedTurn = (SetProcessedTurnFn)(b + UNIT_SET_PROCESSED_TURN_OFFSET);
		OperationsManagerGet = (OperationsManagerGetFn)(b + OPERATIONS_MANAGER_GET_OFFSET);
		MoveToMakeRequest = (MoveToMakeRequestFn)(b + MOVE_TO_MAKE_REQUEST_OFFSET);
		CurrentGame = (void**)(b + GreatPersonAssign::CURRENT_GAME_OFFSET);
		ReportAddString = (AddStringFn)(b + REPORTING_ADD_STRING_OFFSET);
		GreatPeopleGet = (GreatPeopleGetFn)(b + GREAT_PEOPLE_GET_OFFSET);
		GpActionNameText = (GpTextFn)(b + GP_ACTION_NAME_TEXT_OFFSET);
		GpActionEffectText = (GpTextFn)(b + GP_ACTION_EFFECT_TEXT_OFFSET);
		StringEmpty = (StringEmptyFn)(b + STRING_EMPTY_OFFSET);
		StringDtor = (StringDtorFn)(b + STRING_DTOR_OFFSET);
		GetActivationPlots = (GetActivationPlotsFn)(b + GP_GET_ACTIVATION_PLOTS_OFFSET);
		GetDistrictDefinitions = (GetDefinitionsFn)(b + GET_DISTRICT_DEFINITIONS_OFFSET);
		GetBuildingDefinitions = (GetDefinitionsFn)(b + GET_BUILDING_DEFINITIONS_OFFSET);
		GetFeatureDefinitions = (GetDefinitionsFn)(b + GET_FEATURE_DEFINITIONS_OFFSET);
		GetGreatWorkObjectDefinitions = (GetDefinitionsFn)(b + GET_GREAT_WORK_OBJECT_DEFINITIONS_OFFSET);
		DefinitionFind = (DefinitionFindFn)(b + DEFINITION_FIND_OFFSET);

		// The same Localization exports GameCore imports (by their decorated names)
		if (HMODULE loc = GetModuleHandleW(L"Localization_FinalRelease.dll")) {
			LocCtor = (LocCtorFn)GetProcAddress(loc, "??0String@Localization@@QEAA@PEBD@Z");
			LocDtor = (LocDtorFn)GetProcAddress(loc, "??1String@Localization@@UEAA@XZ");
			LocLookup = (LocLookupFn)GetProcAddress(loc, "?Lookup@Localization@@YA?AVString@1@PEBD@Z");
			LocPushStr = (LocPushStrFn)GetProcAddress(loc, "?PushArgument@String@Localization@@QEAA_NPEBD@Z");
			LocPushInt = (LocPushIntFn)GetProcAddress(loc, "?PushArgument@String@Localization@@QEAA_NH@Z");
			gReportsOk = LocCtor && LocDtor && LocLookup && LocPushStr && LocPushInt;
		}
		if (!gReportsOk) DevBridge::Log("[UnitCommandActions] Localization exports not found: the unit panel shows no effect text or failure reasons");

		gVtable[0] = (void*)&HandlerDestroy;
		gVtable[1] = (void*)(b + HANDLER_GET_TYPE_HASH_OFFSET);
		gVtable[2] = (void*)&HandlerCanStart;
		gVtable[3] = (void*)(b + HANDLER_RETURN_FALSE_OFFSET);
		gVtable[4] = (void*)&HandlerStart;
		gVtable[5] = (void*)(b + BASE_MAKE_REQUEST_OFFSET);
		gVtable[6] = (void*)(b + DEFAULT_MAKE_PARAMETERS_OFFSET);
		gVtable[7] = (void*)(b + BASE_GET_COMMAND_HELP_TEXT_OFFSET);
		gVtable[8] = (void*)(b + HANDLER_SET_INDEX_OFFSET);

		Runtime::CreateHook((LPVOID)(b + HOMELAND_DO_TURN_ACTIVATE_OFFSET), (LPVOID)&Hook_DoTurnActivate, &base_DoTurnActivate);
		gOk = true;
		DevBridge::Log("[UnitCommandActions] address checks passed: UnitCommandActions is available, AI turn hook installed");
	}
}
