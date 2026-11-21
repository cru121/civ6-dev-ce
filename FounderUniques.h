// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstdint>

// Founder uniques. A city whose property DEVCE_BUILD_FOUNDER_UNIQUES is >= 1 (MODIFIER_SINGLE_CITY_ADJUST_PROPERTY, or
// EFFECT_ADJUST_CITY_PROPERTY with COLLECTION_PLAYER_CITIES) and that was founded by another player can also train the units
// and construct the buildings its founder could, as if its owner were the founder's civilization and leader: the founder's
// unique units and buildings (and generics the founder has not replaced). Nothing is replaced for the owner: a city founded by
// a civilization whose unique unit replaces the Swordsman offers that unit and the owner's Swordsman.
// All other conditions (techs, civics, districts, resources, limits, ...) are the owner's as usual.
//
// The engine keeps per player "which version of this unit / building can I ever build" (Player::Units +0x128,
// Player::Cities +0x150: the item itself, the player's replacement, or -1 for another civilization's unique). The one test of
// it in City::BuildQueue::CanTrain and in the inner City::BuildQueue::CanConstruct is `cmp [version + item*4], item / jne fail`;
// both jne are redirected through a small stub that asks Dev CE first: the city has the property and its founder's table maps
// the item to itself -> go on as if the owner's did. Every caller of those two (production panel, queueing, purchase, the
// per-turn check) follows. The AI does not look for founder uniques by itself (its unit / building lists use the owner's table).
namespace FounderUniques {
	constexpr uintptr_t CAN_TRAIN_OFFSET = 0xf86d0;              // bool City::BuildQueue::CanTrain(queue, UnitTypes, ...): queue in r14, unit in edi
	constexpr uintptr_t CAN_TRAIN_VERSION_JNE = 0xf8762;         //   jne fail after cmp [r9 + rcx], edi (version[unit] != unit)
	constexpr uintptr_t CAN_TRAIN_PASS = 0xf8768;                //   next instruction
	constexpr uintptr_t CAN_TRAIN_FAIL = 0xf979f;                //   jne target
	constexpr uintptr_t CAN_CONSTRUCT_OFFSET = 0xf61c0;          // bool City::BuildQueue::CanConstruct(queue, BuildingTypes, Results*, ...) (inner): queue in r13, building in ebp
	constexpr uintptr_t CAN_CONSTRUCT_VERSION_JNE = 0xf62c0;     //   jne fail after cmp [r8 + rax], ebp
	constexpr uintptr_t CAN_CONSTRUCT_PASS = 0xf62c6;
	constexpr uintptr_t CAN_CONSTRUCT_FAIL = 0xf70e5;
	constexpr uintptr_t EDIT_PLAYER_OFFSET = 0x44f00;            // Player::Instance* Context::Globals::EditPlayer(PlayerTypes)
	constexpr uintptr_t MAKE_HASH_OFFSET = 0x606270;

	constexpr uintptr_t QUEUE_CITY = 0x1e8;          // City::BuildQueue -> City::Instance*
	constexpr uintptr_t CITY_OWNER = 0xd8;
	constexpr uintptr_t CITY_ORIGINAL_OWNER = 0xf0;  // founder (Lua City:GetOriginalOwner)
	constexpr uintptr_t CITY_PROPERTIES = 0x1a0;     // Data::VariantMap (vtable +0x60 = get(key) -> Variant*)
	constexpr uintptr_t PLAYER_UNITS = 0x6c8;        // Player::Units*: version table data +0x128, count +0x138
	constexpr uintptr_t PLAYER_CITIES = 0x6d0;       // Player::Cities*: version table data +0x150, count +0x160
	constexpr uintptr_t UNITS_VERSION = 0x128, UNITS_VERSION_COUNT = 0x138;
	constexpr uintptr_t CITIES_VERSION = 0x150, CITIES_VERSION_COUNT = 0x160;

	constexpr const char* PROPERTY = "DEVCE_BUILD_FOUNDER_UNIQUES";

	void Create();
}
