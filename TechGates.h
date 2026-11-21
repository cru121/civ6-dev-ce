// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstdint>

// Tech and civic gates. A row in DevCE_TechnologyGates (TechnologyType, Property, Description) or DevCE_CivicGates (CivicType,
// Property, Description) adds a condition to that tech or civic on top of its prereqs: the player's property Property must be >= 1.
// Several rows on one item must all hold. Modifiers set the property with EFFECT_ADJUST_PLAYER_PROPERTY (Key, Amount), so:
//   policy-locked     a PolicyModifiers row adds 1 while the policy is slotted
//   cross-tree        a CivicModifiers row adds 1 to a tech's gate (or a TechnologyModifiers row to a civic's)
//   one of several    no prereq rows; each candidate tech adds 1 through TechnologyModifiers
// "Openers" of a gate = the techs and civics whose TechnologyModifiers / CivicModifiers add a positive Amount to its key through
// EFFECT_ADJUST_PLAYER_PROPERTY on COLLECTION_OWNER (read from the database; no table of their own).
//
// Hooks (all pass through when no gate rows exist):
//   BuildCollection<TechnologyPrereq>      after the game: read the gate tables and openers (the game's database connection)
//   Techs::CanResearch / Culture::CanProgress   after: false while a gate is shut (the AI, choosers, Lua and free techs all ask these)
//   ChangeResearchProgress / ChangeCulturalProgress   before: science or culture for the head of the queue whose gate shut (policy
//                                          unslotted) clears the queue instead, so it goes to overflow and the player chooses again
//   SetResearchingTech / SetProgressingCivic (queue and single)   before: the queue is cut at the first item whose gate is shut and
//                                          not opened by an earlier item of the same queue; nothing left = the request is ignored
//   IPlayerTechs:GetResearchPath / IPlayerCulture:GetCivicPath   replaced: prereqs first, and a shut gate goes through its cheapest
//                                          opener in the same tree; no way through (only civic/policy openers) = no path, the click does nothing
//   AI TechTreeAnalysis::Evaluate / CivicAnalysis::Execute   after: a target whose next step cannot be taken gets the first step of
//                                          our path, or is marked unreachable (else the AI falls back to a random tech)
// UI: ToolTipLoader_DevCE_TechGates.lua adds the shut gates' Description lines to tech and civic tooltips.
namespace TechGates {
	constexpr uintptr_t BUILD_TECH_PREREQS_OFFSET = 0x7690c0;        // DefinitionCollectionBuilder::BuildCollection<TechnologyPrereq>(builder, collection); builder +0 = IDatabaseConnection1*

	constexpr uintptr_t CAN_RESEARCH_OFFSET = 0x333410;              // bool Player::Techs::CanResearch(techs, TechTypes)
	constexpr uintptr_t CAN_PROGRESS_OFFSET = 0x27bc50;              // bool Player::Culture::CanProgress(culture, CivicTypes)
	constexpr uintptr_t CHANGE_RESEARCH_PROGRESS_OFFSET = 0x333660;  // bool Player::Techs::ChangeResearchProgress(techs, TechTypes, FixedPoint* amount)
	constexpr uintptr_t CHANGE_CULTURAL_PROGRESS_OFFSET = 0x27bf30;  // bool Player::Culture::ChangeCulturalProgress(culture, CivicTypes, FixedPoint* amount)
	constexpr uintptr_t SET_RESEARCHING_QUEUE_OFFSET = 0x335f70;     // void Player::Techs::SetResearchingTech(techs, const Queue<TechTypes,4>&, InsertMode)
	constexpr uintptr_t SET_RESEARCHING_TECH_OFFSET = 0x336230;      // void Player::Techs::SetResearchingTech(techs, TechTypes, InsertMode)
	constexpr uintptr_t SET_PROGRESSING_QUEUE_OFFSET = 0x285360;     // void Player::Culture::SetProgressingCivic(culture, const Queue<CivicTypes,4>&, InsertMode)
	constexpr uintptr_t SET_PROGRESSING_CIVIC_OFFSET = 0x2855d0;     // void Player::Culture::SetProgressingCivic(culture, CivicTypes, InsertMode)
	constexpr uintptr_t L_GET_RESEARCH_PATH_OFFSET = 0x6b55b0;       // Cache::Lua::IPlayerTechs::lGetResearchPath(L)
	constexpr uintptr_t L_GET_CIVIC_PATH_OFFSET = 0x6ac720;          // Cache::Lua::IPlayerCulture::lGetCivicPath(L)
	constexpr uintptr_t TECH_ANALYSIS_EVALUATE_OFFSET = 0x4f8ff0;    // AI::TechTreeAnalysis::Evaluate(analysis)
	constexpr uintptr_t CIVIC_ANALYSIS_EXECUTE_OFFSET = 0x414bd0;    // AI::CivicAnalysis::Execute(analysis, unsigned)

	constexpr uintptr_t HAS_TECH_OFFSET = 0x334cc0;                  // bool (techs, TechTypes): researched
	constexpr uintptr_t HAS_CIVIC_OFFSET = 0x281280;                 // bool (culture, CivicTypes): completed
	constexpr uintptr_t GAME_TECHS_OFFSET = 0x2512c0;                // Game::Techs* ()
	constexpr uintptr_t GAME_CULTURE_OFFSET = 0x1c8250;              // Game::Culture* ()
	constexpr uintptr_t TECH_PREREQS_OFFSET = 0x2514d0;              // const {int* begin, end}* (Game::Techs*, TechTypes)
	constexpr uintptr_t CIVIC_PREREQS_OFFSET = 0x1c84d0;             // Game::Culture::GetCivicPrerequisites(Game::Culture*, CivicTypes)
	constexpr uintptr_t TECH_COST_OFFSET = 0x251340;                 // int Game::Techs::GetTechCost(Game::Techs*, TechTypes, bool)
	constexpr uintptr_t CIVIC_COST_OFFSET = 0x1c82b0;                // int Game::Culture::GetCivicCost(Game::Culture*, CivicTypes, bool)
	constexpr uintptr_t TECH_INDEX_OFFSET = 0x1b0f40;                // TechTypes (index or hash)
	constexpr uintptr_t CIVIC_INDEX_OFFSET = 0x1b0360;               // CivicTypes (index or hash)
	constexpr uintptr_t EDIT_PLAYER_OFFSET = 0x44f00;                // Player::Instance* Context::Globals::EditPlayer(PlayerTypes)
	constexpr uintptr_t EVENT_SEND_OFFSET = 0x5fc4f0;                // Reporting::Event::Send(type, PlayerTypes)
	constexpr uint64_t RESEARCH_QUEUE_CHANGED_EVENT = 0x4378a305;    //   what SetResearchingTech sends after a queue change
	constexpr uint64_t CIVIC_QUEUE_CHANGED_EVENT = 0xac2769a3;       //   what SetProgressingCivic sends
	constexpr uintptr_t TECHS_LUA_INSTANCE_OFFSET = 0x6b4f30;        // Cache::Player::Techs* GetInstance(L, index, bool); +0 = PlayerTypes*
	constexpr uintptr_t CULTURE_LUA_INSTANCE_OFFSET = 0x6ab620;      // Cache::Player::Culture* GetInstance(L, index, bool); +0 = PlayerTypes*
	constexpr uintptr_t NUMBER_OR_HASH_OFFSET = 0x254f0;             // int Lua::Utility::GetNumberOrHash(L, index)
	constexpr uintptr_t TECH_DEFINITIONS_OFFSET = 0x7d1920;          // DefinitionCollection* (): begin +0, end +8
	constexpr uintptr_t CIVIC_DEFINITIONS_OFFSET = 0x7d05e0;
	constexpr uintptr_t PROPERTY_EDIT_OFFSET = 0x72a920;             // FAutoVariable<VariantMap>::edit(object) -> value

	constexpr uintptr_t PLAYER_ID = 0xd8;
	constexpr uintptr_t PLAYER_PROPERTIES = 0x518;   // Player::Instance: properties FAutoVariable (map at +0x10)
	constexpr uintptr_t PLAYER_CULTURE = 0x6f0;
	constexpr uintptr_t PLAYER_TECHS = 0x718;
	constexpr uintptr_t OWNER_PLAYER = 0x90;         // Techs / Culture: Player::Instance*
	constexpr uintptr_t TECHS_QUEUE = 0xb0;          // queue FAutoVariable; value +0x10: data +0x20, count +0x30 (techs +0xe0 / +0xf0)
	constexpr uintptr_t CULTURE_QUEUE = 0x3d8;       // (culture +0x408 / +0x418)
	constexpr uintptr_t QUEUE_DATA = 0x20;           // Queue<T,4>: T* data
	constexpr uintptr_t QUEUE_COUNT = 0x30;          //   unsigned count

	void Create();
}
