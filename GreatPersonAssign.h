// SPDX-License-Identifier: AGPL-3.0-only
// Part of Dev CE, a fork of the Civilization VI Community Extension by Wild-W (https://github.com/Wild-W/CivilizationVI_CommunityExtension).
// Licensed under the GNU Affero General Public License v3.0 (see LICENSE.txt). Not endorsed by Wild-W, Firaxis or 2K.
#pragma once
#include "HavokScript.h"
#include <cstdint>

// UnitGreatPerson:SetGreatPersonType(individualIndex) (gameplay scripts only).
// Makes the unit behind the object a great person of the given GreatPersonIndividuals row, the way the game's own
// Game::GreatPeople::CreatePerson does it, but on an existing unit of any type:
//   1. Unit::GreatPerson::Set(individual, its class, current charges)    (the charge count is left as it is)
//   2. the GreatPersonCreated signal, which attaches the individual's GreatPersonIndividualBirthModifiers (owner = the unit),
//      grants the Diplomatic Favor and the "earned a great person" boosts, like a recruit does.
// Background and addresses: dev-ce/GREAT_PERSON_ASSIGN.md.
namespace GreatPersonAssign {
	// Addresses in GameCore_XP2_FinalRelease.dll, Steam build 15038592 (same DLL in 15296837).
	constexpr uintptr_t GET_INDIVIDUAL_DEFINITIONS_OFFSET = 0x7d0d60;   // Definitions::GetGreatPersonIndividualDefinitions()
	constexpr uintptr_t GREAT_PERSON_SET_OFFSET = 0x399650;            // Unit::GreatPerson::Set(GreatPersonIndividualTypes, GreatPersonClassTypes, int)
	constexpr uintptr_t CREATED_SIGNAL_DISPATCH_OFFSET = 0x27cfc0;     // ProtoSignal<void(Unit::Instance*, GreatPersonIndividualTypes, YieldTypes, int)>::Dispatch
	constexpr uintptr_t GET_INSTANCE_OFFSET = 0x7005b0;                // Lua::ScopedInstance<Lua::IUnitGreatPerson, Unit::GreatPerson>::GetInstance
	constexpr uintptr_t CURRENT_GAME_OFFSET = 0xb8aa68;                // Context::Globals::ms_pkCurrentGame (data)
	constexpr uintptr_t CURRENT_GAME_READ_SITE = 0x65570f;             // `mov rcx, [rip+0x535352]` = a read of ms_pkCurrentGame in ActivateGreatPerson::Start

	// Layout (same build)
	constexpr uintptr_t UNIT_GREAT_PERSON_VAR = 0x9c0;    // Unit::Instance: FAutoVariable<Unit::GreatPerson>; edit() returns the GreatPerson at unit + 0x9d0
	constexpr uintptr_t UNIT_ID = 0xb0;                   // Unit::Instance: unit id (for the log)
	constexpr uintptr_t GP_UNIT = 0x0;                    // Unit::GreatPerson: back pointer to its unit
	constexpr uintptr_t GP_INDIVIDUAL = 0x8;              // Unit::GreatPerson: GreatPersonIndividualTypes (-1 = none)
	constexpr uintptr_t GP_CLASS = 0xc;                   // Unit::GreatPerson: GreatPersonClassTypes (-1 = none)
	constexpr uintptr_t GP_CHARGES = 0x10;                // Unit::GreatPerson: action charges
	constexpr uintptr_t INDIVIDUAL_CLASS_DEF = 0x188;     // Definition::GreatPersonIndividual: GreatPersonClass definition
	constexpr uintptr_t CLASS_INDEX = 0x14;               // Definition::GreatPersonClass: its index
	constexpr uintptr_t GAME_EVENTS = 0x280;              // Game::Instance: signal hub
	constexpr uintptr_t CREATED_SIGNAL = 0x7b0;           // signal hub: GreatPersonCreated

	// Verifies the addresses (call after Runtime::Create()). On a mismatch the method is not added.
	void Create();
	// Adds the method to the IUnitGreatPerson table at stack index t (called from the generated IUnitGreatPerson hook).
	void PushMethods(hks::lua_State* L, int t);
}
