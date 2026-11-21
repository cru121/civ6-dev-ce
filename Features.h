// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstdint>

// Feature switches. A Dev CE feature that changes the game's own behaviour, or that sits in a hot engine path, only acts in a game
// whose database has its game capability (a Types row of KIND_CAPABILITY and a GameCapabilities row, added by any mod, with
// INSERT OR IGNORE so several mods can add it). Without the capability the hooks pass straight through to the game's own code, so
// loading Dev CE alone never changes how a game plays.
//
//   CAPABILITY_DEVCE_UPGRADE_RULES       unit upgrade rules: no upgrade into a build-disabled unit, minimum levels, building prerequisite,
//                                        alternative upgrades (UnitUpgradeRules; the property-driven "upgrade outside territory" effect needs no switch)
//   CAPABILITY_DEVCE_UPGRADE_FALLBACK    a unit stays buildable while its city cannot train its upgrade (UnitUpgradeFallback)
//   CAPABILITY_DEVCE_GOLD_PURCHASE       gold purchase of build-only items, and the check that a gold purchase is for a gold item
//   CAPABILITY_DEVCE_FOOD_PRODUCTION     excess food into unit production (FoodProduction)
//   CAPABILITY_DEVCE_DISTRICT_CITIZENS   district citizen slots and yields from district properties (DistrictCitizens)
//   CAPABILITY_DEVCE_CITY_RADIUS         city radius up to 5 (CityRadius)
//
// The answer is read once per game and kept; Features::NewGame() (a new gameplay context) forgets it. A capability must already be in
// the database when the game starts, as with every game capability.
namespace Features {
	enum Id { UpgradeRules, UpgradeFallback, GoldPurchase, FoodProduction, DistrictCitizens, CityRadius, COUNT };

	// Resolves the capability hashes and the engine functions that read them. False (every feature then stays off) when the
	// engine's code is not the expected one.
	bool Init(uintptr_t gameCore);

	// True when the current game has the feature's capability. Cheap after the first call of a game.
	bool On(Id id);

	// A new game context: ask again.
	void NewGame();

	constexpr uintptr_t CAPABILITIES_GET_OFFSET = 0x48230;       // Capabilities::Manager* Capabilities::Get()
	constexpr uintptr_t HAS_CAPABILITY_OFFSET = 0x484e0;         // bool Capabilities::Manager::HasCapability(unsigned hash)
	constexpr uintptr_t MAKE_HASH_OFFSET = 0x6062a0;             // uint32 Utilities::MakeHash(const char*)
}
