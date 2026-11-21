// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstdint>

// A unit stays buildable in a city when that city cannot train the unit it upgrades to (UnitUpgrades).
//
// Units can no longer be built when their UnitUpgrade is unlocked via tech or civic for major civs. This can be a problem
// if the upgraded unit is no longer buildable, because of MODIFIER_PLAYER_UNIT_BUILD_DISABLED, or Unit_BuildingPrereqs.
// The game does have a system for this for unbuildable units because of missing strategics. This expands it
//
// GameCore::City::BuildQueue::CanTrain runs the check inline, in a loop over the unit's upgrades. Per upgrade it calls
// Rules::Units::HasAvailableReplacementForUnit (unique unit swap) and, when the upgrade is unlocked, Player::Instance::IsMajor.
// Both are hooked and act only when called from those two places in CanTrain (return address): the first records which unit
// the upgrade is, the second answers "not major" (= keep the unit) when CanTrain says the city cannot train the upgrade.
namespace UnitUpgradeFallback {
	constexpr uintptr_t CAN_TRAIN_OFFSET = 0xf86d0;                    // City::BuildQueue::CanTrain(this, unit, testVisible, results, formation, ignoreRequirements, p6) (hooked)
	constexpr uintptr_t HAS_REPLACEMENT_OFFSET = 0x37e110;             // Rules::Units::Instance::HasAvailableReplacementForUnit(rules, unit, player) -> unit or -1 (hooked)
	constexpr uintptr_t IS_MAJOR_OFFSET = 0x2f9860;                    // Player::Instance::IsMajor(player) (hooked)
	constexpr uintptr_t CAN_TRAIN_REPLACEMENT_RETURN = 0xf88d7;        // return address of CanTrain's call to HasAvailableReplacementForUnit (upgrade loop)
	constexpr uintptr_t CAN_TRAIN_IS_MAJOR_RETURN = 0xf898a;           // return address of CanTrain's call to IsMajor (upgrade loop: upgrade unlocked)

	// Verifies the addresses and installs the hooks (call from InitHooks).
	void Create();
}
