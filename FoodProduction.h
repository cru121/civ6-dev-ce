// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstdint>

// Food into unit production. While a city whose property DEVCE_FOOD_TO_PRODUCTION is >= 1 produces a unit, its excess food
// (food yield minus what its citizens eat) is added to its production instead of its growth:
//   production   + excess food, added to the base before the modifiers, so production bonuses (city %, % toward units) apply
//   food         lowered to exactly what the citizens eat (no growth, no starvation)
// Both show as their own line in the yield tooltips (LOC_DEVCE_PRODUCTION_FROM_FOOD / LOC_DEVCE_FOOD_TO_PRODUCTION).
//
// The engine computes every city yield in City::Instance::CalculateYield(yield, production kind, track tooltip steps); the
// production kind is the current item's (unit / building / district), so the build step, GetYield (UI, AI, growth) and the
// tooltips all go through it. That function is hooked for food and production when the kind is "unit".
// The turn runs the build step before growth: when the unit finishes, growth already sees the next item. So the build step
// (BuildQueue::DoTurnActivate) records whether this turn's food went into a unit, and the growth step (Growth::DoCityGrowth ->
// AddGrowth) adds growth from the unconverted food unless it did. This also makes growth independent of the city yield cache.
namespace FoodProduction {
	constexpr uintptr_t CALCULATE_YIELD_OFFSET = 0x12ff20;         // GameAttribute* City::Instance::CalculateYield(city, GameAttribute* out, YieldTypes, TypeHash kind, bool track)
	constexpr uintptr_t BUILD_QUEUE_TURN_OFFSET = 0xfc690;         // void City::BuildQueue::DoTurnActivate(queue): -> DoBuilds (adds this turn's production)
	constexpr uintptr_t DO_CITY_GROWTH_OFFSET = 0x123460;          // void City::Growth::DoCityGrowth(growth)
	constexpr uintptr_t ADD_GROWTH_OFFSET = 0x123160;              // void City::Growth::AddGrowth(growth, const FixedPoint* amount)
	constexpr uintptr_t GROWTH_MODIFIER_OFFSET = 0x1247c0;         // FixedPoint* City::Growth::GetOverallGrowthModifier(growth, FixedPoint* out)
	constexpr uintptr_t CURRENT_KIND_OFFSET = 0xfd0e0;             // TypeHash City::BuildQueue::GetCurrentProductionKind(queue)
	constexpr uintptr_t ADD_STEP_OFFSET = 0x12fce0;                // void TrackedValue::AddStep(tracked, const FixedPoint* amount, const char* locKey)
	constexpr uintptr_t ECONOMIC_GET_OFFSET = 0x370f30;            // Rules::Economic::Instance* Rules::Economic::Get()
	constexpr uintptr_t FOOD_CONSUMPTION_OFFSET = 0x371060;        // int Rules::Economic::Instance::GetFoodConsumption(rules, population)
	constexpr uintptr_t MAKE_HASH_OFFSET = 0x606270;
	constexpr uintptr_t YIELD_FOOD_GLOBAL = 0xb8f648;              // int GameCore::YIELD_FOOD
	constexpr uintptr_t YIELD_PRODUCTION_GLOBAL = 0xb8f64c;        // int GameCore::YIELD_PRODUCTION
	constexpr uint32_t KIND_UNIT = 0x15d11e18;                     // GetCurrentProductionKind: a unit

	// Layout
	constexpr uintptr_t ATTR_MODIFIER = 0x30;          // GameAttribute: TrackedValue base at +0, modifier (percent, FixedPoint) at +0x30
	constexpr uintptr_t TV_HAS_MIN = 0x0, TV_MIN = 0x4, TV_HAS_MAX = 0x8, TV_MAX = 0xc, TV_VALUE = 0x10;
	constexpr uintptr_t CITY_POPULATION = 0x268;
	constexpr uintptr_t CITY_PROPERTIES = 0x1a0;       // Data::VariantMap (vtable +0x60 = get(key) -> Variant*)
	constexpr uintptr_t CITY_BUILD_QUEUE = 0x10d0;
	constexpr uintptr_t QUEUE_CITY = 0x1e8;            // City::BuildQueue -> City::Instance*
	constexpr uintptr_t GROWTH_CITY = 0x0;             // City::Growth -> City::Instance*

	constexpr const char* PROPERTY = "DEVCE_FOOD_TO_PRODUCTION";
	constexpr const char* LOC_PRODUCTION_FROM_FOOD = "LOC_DEVCE_PRODUCTION_FROM_FOOD";
	constexpr const char* LOC_FOOD_TO_PRODUCTION = "LOC_DEVCE_FOOD_TO_PRODUCTION";

	// Verifies the addresses and installs the hooks (call from InitHooks).
	void Create();
}
