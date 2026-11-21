// SPDX-License-Identifier: AGPL-3.0-only
// Part of Dev CE, a fork of the Civilization VI Community Extension by Wild-W (https://github.com/Wild-W/CivilizationVI_CommunityExtension).
// Licensed under the GNU Affero General Public License v3.0 (see LICENSE.txt). Not endorsed by Wild-W, Firaxis or 2K.
#pragma once
#include "HavokScript.h"
#include <cstdint>

// Custom unit commands whose effect is a great person action.
// A command (a UnitCommands row) is bound to a "template" GreatPersonIndividuals row: the command can start when the template's
// ActionRequires... rules pass at the unit's plot, and starting it attaches the template's GreatPersonIndividualActionModifiers around
// the unit (as a great person activation would), then ends the unit's moves. No charges, no great person needed, once per turn (needs moves).
// Which units may use a command is granted separately (unit type or promotion). AI players, at the start of their turn: a unit uses its
// command where it stands, or moves to the nearest revealed plot within the command's AI search radius where it can (then uses it).
//
// Gameplay Lua (the mod DevCE_UnitCommandActions pushes its database tables with these at game start):
//   UnitCommandActions.Define(commandHash, templateIndividualIndex [, aiSearchRadius = 3]) -> true   (radius 0 = the AI never moves for it)
//   UnitCommandActions.Grant(commandHash, unitTypeIndex)            -> true   (every unit of that type)
//   UnitCommandActions.GrantPromotion(commandHash, promotionIndex)  -> true   (units that have that promotion)
//   UnitCommandActions.GrantAbility(commandHash, abilityIndex)      -> true   (units that have that ability)
//   UnitCommandActions.Clear()
// Gameplay and UI (read only):
//   UnitCommandActions.IsDefined(commandHash) -> boolean
//   UnitCommandActions.GetCommandHashes() -> { hash, ... }
//   UnitCommandActions.GetActivationPlots(commandHash, playerID) -> { plotIndex, ... }   (UI: only when not UI.IsGameCoreBusy())
// The unit panel's tooltip gets the template's action name (when it has ActionNameTextOverride), its effect text and the failure
// reasons, filled as the great person command fills them.
// Background and addresses: dev-ce/UNIT_COMMAND_ACTIONS.md.
namespace UnitCommandActions {
	// Addresses in GameCore_XP2_FinalRelease.dll, Steam build 15038592 (same DLL in 15296837).
	constexpr uintptr_t MANAGER_GET_OFFSET = 0x3e6a0;                // Unit::Commands::Manager::Get()
	constexpr uintptr_t HANDLER_GET_TYPE_HASH_OFFSET = 0x6832d0;     // shared body: return [this+0xc]           (handler vtable slot 1)
	constexpr uintptr_t HANDLER_RETURN_FALSE_OFFSET = 0x3fa120;      // shared body: return false                  (slot 3)
	constexpr uintptr_t BASE_MAKE_REQUEST_OFFSET = 0x3e330;          // Unit::Commands::BaseHandler::MakeRequest   (slot 5)
	constexpr uintptr_t DEFAULT_MAKE_PARAMETERS_OFFSET = 0x13230;    // shared body: empty parameters              (slot 6)
	constexpr uintptr_t BASE_GET_COMMAND_HELP_TEXT_OFFSET = 0x3e2b0; // Unit::Commands::BaseHandler::GetCommandHelpText (slot 7)
	constexpr uintptr_t HANDLER_SET_INDEX_OFFSET = 0x3d0a10;         // shared body: [this+8] = index              (slot 8)
	constexpr uintptr_t GP_CAN_ACTIVATE_OFFSET = 0x397ee0;           // static Unit::GreatPerson::CanActivate(const GreatPersonIndividual**, PlayerTypes, bool, const PlotCoord*, Requirements::Results*)
	constexpr uintptr_t UNIT_GET_PLOT_OFFSET = 0x67a40;              // Unit::IInstance::GetPlot()
	constexpr uintptr_t UNIT_FINISH_MOVES_OFFSET = 0x3a3120;         // Unit::Instance::FinishMoves()
	constexpr uintptr_t ATTACH_ACTION_EFFECTS_OFFSET = 0x966fb0;     // GameEffects::GameEventHandler::OnGreatPersonActivated_AttachGameEffects(Unit*, GreatPersonIndividualTypes)
	constexpr uintptr_t PLAYER_IS_HUMAN_OFFSET = 0x2f9820;           // Player::Instance::IsHuman()
	constexpr uintptr_t PLAYER_IS_BARBARIAN_OFFSET = 0x2f9790;       // Player::Instance::IsBarbarian()
	constexpr uintptr_t HOMELAND_DO_TURN_ACTIVATE_OFFSET = 0x4baff0; // AI::Homeland::DoTurnActivate(PlayerTypes, bool) (hooked: AI use)
	constexpr uintptr_t UNIT_HAS_ABILITY_OFFSET = 0x3a3d60;          // Unit::Instance::HasAbility(hash): checked only, its layout is read directly (UNIT_ABILITY_*)
	constexpr uintptr_t EXPERIENCE_HAS_PROMOTION_OFFSET = 0x397170;  // Unit::Experience::HasPromotion(UnitPromotionTypes): byte array at +0x30, NO bounds check
	constexpr uintptr_t EDIT_MAP_OFFSET = 0x44d40;                   // Context::Globals::EditMap() (= *ms_pkCurrentGame + 0x248)
	constexpr uintptr_t GET_PLAYER_VISIBILITY_OFFSET = 0x44f10;      // Context::Globals::GetPlayerVisibility(PlayerTypes)
	constexpr uintptr_t MAP_GET_PLOT_DISTANCE_OFFSET = 0x15ec0;      // Map::IInstance::GetPlotDistance(x1, y1, x2, y2) (wraps)
	constexpr uintptr_t UNIT_SET_PROCESSED_TURN_OFFSET = 0x3a5ee0;   // Unit::Instance::SetProcessedTurn(bool): the AI's "handled this turn" mark
	constexpr uintptr_t OPERATIONS_MANAGER_GET_OFFSET = 0x408f0;     // Unit::Operations::Manager::Get()
	constexpr uintptr_t MOVE_TO_MAKE_REQUEST_OFFSET = 0x66cd90;      // static Unit::Operations::MoveTo::MakeRequest(int x, int y, int flags) -> Request (hidden return)
	// Unit panel reports, filled as Unit::Commands::ActivateGreatPerson::CanStart (0x6549c0) does
	constexpr uintptr_t REPORTING_ADD_STRING_OFFSET = 0x5fd5a0;      // Reporting::AddString(results, hash, const Localization::String&)
	constexpr uintptr_t GREAT_PEOPLE_GET_OFFSET = 0x20a920;          // Game::GreatPeople::Get()
	constexpr uintptr_t GP_ACTION_NAME_TEXT_OFFSET = 0x20af10;       // Game::GreatPeople::GetActionNameText(individual, String* out, 0)
	constexpr uintptr_t GP_ACTION_EFFECT_TEXT_OFFSET = 0x20ab50;     // Game::GreatPeople::GetActionEffectText(individual, String* out, 0): override, else the action modifiers' text
	constexpr uintptr_t STRING_EMPTY_OFFSET = 0x993b30;              // String::Details::GetUniversalEmptyBasicString() (a String is one char*)
	constexpr uintptr_t STRING_DTOR_OFFSET = 0x993590;               // String::BasicT<...>::~BasicT
	constexpr uintptr_t GP_GET_ACTIVATION_PLOTS_OFFSET = 0x3990c0;   // static Unit::GreatPerson::GetActivationPlots(individual, PlayerTypes, Vector<int>& plotIndices)
	constexpr uintptr_t GET_DISTRICT_DEFINITIONS_OFFSET = 0x7d08e0;  // Definitions::Get...Definitions() (collections; Find = index lookup)
	constexpr uintptr_t GET_BUILDING_DEFINITIONS_OFFSET = 0x7d04a0;
	constexpr uintptr_t GET_FEATURE_DEFINITIONS_OFFSET = 0x7d0aa0;
	constexpr uintptr_t GET_GREAT_WORK_OBJECT_DEFINITIONS_OFFSET = 0x7d0da0;
	constexpr uintptr_t DEFINITION_FIND_OFFSET = 0x2209a0;           // DefinitionCollectionWithTypeIndex<T>::Find(index) (same body for every T)
	constexpr uint32_t RESULT_ACTION_NAME = 0x1f6eef50;              // UnitOperationResults.ACTION_NAME (set with results vtable +0x108)
	constexpr uint32_t RESULT_ADDITIONAL_DESCRIPTION = 0xa3540d8d;   // UnitOperationResults.ADDITIONAL_DESCRIPTION (Reporting::AddString)
	constexpr uint32_t RESULT_FAILURE_REASONS = 0x87150719;          // UnitOperationResults.FAILURE_REASONS (Reporting::AddString)
	constexpr uintptr_t RESULTS_VT_SET_STRING = 0x108;
	constexpr uintptr_t GPI_HAS_NAME_OVERRIDE = 0xa8;               // Definition::GreatPersonIndividual: bool ActionNameTextOverride set (+0xb0 the text)
	constexpr uintptr_t GPI_INDEX = 0x14;

	// Layout (same build)
	constexpr uintptr_t MANAGER_VT_REGISTER = 0xb8;        // Unit::Commands::Manager vtable: Register(handler)
	constexpr uintptr_t MANAGER_VT_INDEX_FROM_HASH = 0x88; //   index of a command hash, -1 if none
	constexpr uintptr_t MANAGER_VT_GET_HANDLER = 0x70;     //   handler of an index
	constexpr uintptr_t OPERATIONS_VT_REQUEST = 0x60;      // Unit::Operations::Manager vtable: (unit, const Request&, int) as AI::BehaviorTree::MoveUnitNode uses it
	constexpr uintptr_t MAP_PLOT_COUNT = 0x30;             // Map::Instance: number of plots;  vtable +0x60 = plot by index
	constexpr uintptr_t MAP_VT_PLOT_BY_INDEX = 0x60;
	constexpr uintptr_t VIS_WIDTH = 0xc8;                  // Player visibility: map width, revealed flags (one byte per plot, index = width * y + x)
	constexpr uintptr_t VIS_REVEALED = 0x138;
	constexpr uintptr_t GAME_EFFECTS = 0x260;              // Game::Instance: GameEffects::Instance*
	constexpr uintptr_t GAME_EFFECTS_EVENT_HANDLER = 0x20; // GameEffects::Instance: embedded GameEventHandler
	constexpr uintptr_t UNIT_DEFINITION = 0x98;            // Unit::Instance: Definition::Unit*
	constexpr uintptr_t UNIT_ID = 0xb0;
	constexpr uintptr_t UNIT_TYPE = 0xc8;
	constexpr uintptr_t UNIT_X = 0xf8;
	constexpr uintptr_t UNIT_Y = 0xfc;
	constexpr uintptr_t UNIT_MOVES = 0x4f0;                // moves remaining (Lua Unit:GetMovesRemaining reads it; FinishMoves sets it to 0)
	constexpr uintptr_t UNIT_OWNER = 0x128;
	constexpr uintptr_t UNIT_EXPERIENCE = 0x970;            // Unit::Instance: embedded Unit::Experience (callers pass unit + 0x970)
	constexpr uintptr_t UNIT_ABILITY_COUNTS = 0x348;       // Unit::Instance: int* count per UnitAbilities index (> 0 = has it)
	constexpr uintptr_t UNIT_ABILITY_COUNT = 0x358;        //   number of entries
	constexpr uintptr_t UNIT_DYING = 0x2e8;                // bool: the unit is being destroyed
	constexpr uintptr_t UNIT_DEF_GP_FLAGS = 0x1b9;         // Definition::Unit: bit 2 is passed to CanActivate (as the game does)
	constexpr uintptr_t PLOT_X = 0x28;                     // Map::Plot::Instance: int16 x, int16 y
	constexpr uintptr_t PLOT_Y = 0x2a;
	constexpr uintptr_t PLAYER_UNITS = 0x6c8;              // Player::Instance: Player::Units*
	constexpr uintptr_t UNITS_LIST = 0xd0;                 // Player::Units: list head; node = { Unit*, ?, next }
	constexpr uintptr_t HOMELAND_PLAYER = 0x98;            // AI::Homeland: Player::Instance*
	constexpr uintptr_t HOMELAND_PLAYER_ID = 0xd8;

	// Verifies the addresses and hooks the AI turn (call after Runtime::Create()). On a mismatch the feature stays off.
	void Create();
	// A new gameplay context: the game deletes its command handlers with the old game; forget ours.
	void ResetForNewGame();
	// Adds the global table UnitCommandActions to the gameplay Lua state.
	int Register(hks::lua_State* L);
	// Adds the read-only part (IsDefined, GetCommandHashes, GetActivationPlots) to the UI Lua state.
	int RegisterUI(hks::lua_State* L);
}
