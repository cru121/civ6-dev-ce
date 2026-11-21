// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstdint>

// Building gates. A row in DevCE_BuildingGates (BuildingType, Property, Description) makes that building or wonder buildable
// only in a city whose property Property is >= 1 (MODIFIER_SINGLE_CITY_ADJUST_PROPERTY, or EFFECT_ADJUST_CITY_PROPERTY with
// COLLECTION_PLAYER_CITIES; a modifier with requirements takes the amount off again when they stop holding). Several rows on
// one building must all hold. While a gate is shut the building is listed greyed out in the production panel with its
// Description (a LOC key; default LOC_DEVCE_BUILDING_GATE_SHUT) as the reason, and cannot be queued, bought or granted.
//
// Progress is kept: the game re-checks the front item every turn (BuildQueue::AddProgress) and when it reaches the front
// (OnNewFrontBuilding); a building that fails is only taken off the queue (OnRemoveFrontEntry + PopFront), its progress and a
// wonder's partly built model on its tile stay. Only BuildQueue::JustRemoveBuilding clears them, and the gates never call it
// (MODIFIER_PLAYER_ADJUST_VALID_BUILDING and the World Congress production ban do, through RemoveBuilding).
//
// Hooks (pass through when there are no gate rows):
//   BuildCollection<BuildingCondition>   after the game: read DevCE_BuildingGates (the game's database connection)
//   BuildQueue::CanConstruct (outer: UI, queueing, purchases, free buildings)   gate shut: the listing test (exclusion test)
//                                        passes, the full test fails with the Description as failure reason
//   BuildQueue::CanConstruct (inner: per-turn check, new front item, AI)        gate shut: false (except when the outer one asks)
namespace BuildingGates {
	constexpr uintptr_t BUILD_CONDITIONS_OFFSET = 0x755480;     // DefinitionCollectionBuilder::BuildCollection<BuildingCondition>(builder, collection); builder +0 = IDatabaseConnection1*
	constexpr uintptr_t CAN_CONSTRUCT_OFFSET = 0xf7100;         // bool BuildQueue::CanConstruct(queue, BuildingTypes, bool exclusionTest, PlotCoord*, TypedVariantMap* results, bool x5)
	constexpr uintptr_t CAN_CONSTRUCT_INNER_OFFSET = 0xf61c0;   // bool BuildQueue::CanConstruct(queue, BuildingTypes, Results*, bool x5)
	constexpr uintptr_t OUTER_CALLS_INNER_RETURN = 0xf7277;     // return address of the outer CanConstruct's call of the inner one
	constexpr uintptr_t ADD_STRING_OFFSET = 0x5fd5a0;           // Reporting::AddString(TypedVariantMap*, key, const Localization::String&)
	constexpr uint32_t FAILURE_REASONS = 0x87150719;            // results key of the failure reason strings (CityCommandResults.FAILURE_REASONS)
	constexpr uintptr_t MAKE_HASH_OFFSET = 0x606270;

	constexpr uintptr_t QUEUE_CITY = 0x1e8;          // City::BuildQueue -> City::Instance*
	constexpr uintptr_t CITY_PROPERTIES = 0x1a0;     // Data::VariantMap (vtable +0x60 = get(key) -> Variant*)

	constexpr const char* DEFAULT_REASON = "LOC_DEVCE_BUILDING_GATE_SHUT";

	void Create();
}
