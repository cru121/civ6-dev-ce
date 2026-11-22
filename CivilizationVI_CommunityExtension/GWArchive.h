#pragma once
#include "HavokScript.h"

// Great Works Archive: native helpers for the archive mod (not part of the upstream Community Extension).
// Adds a gameplay-side global table:  GWArchive.TryPlace(playerID, greatWorkListIndex) -> code
//   0 placed in a city slot            1 no such game-wide record        2 the work belongs to another player
//   3 already held in some slot        4 no city has a free suitable slot   8 internal failure   9 game state unavailable
// Relic archive: Player::Religion::CreateRelic is hooked. When a HUMAN player has no free relic slot (the original does nothing at all),
// a relic is still created through Game::Culture::AddRelic with an out-of-range slot: the game chooses it and records it for the player,
// but it stays in no slot (it is then an "archived" work, seen and placed with the panel on the Great Works screen).
// Every other case calls the original.
// Ownership events: a record's player field is the CREATOR (also used for theming), so who an unslotted work belongs to is stored separately by the
// gameplay script (Game property GWA_OWNERS). The native side only reports the events, in a queue the script drains:
//   GWArchive.PopEvent() -> work, owner, code, turn   (nothing when empty; events for works that are displayed again by then are dropped)
//   GWArchive.PurgeGhosts(playerId) -> entries, works  (manual cleanup of that player's cities: slot entries for buildings the city no longer has are dropped;
//                         works that were inside become unslotted and are reported with event code 4 "found in a leftover slot")
//   GWArchive.SetOwner(listIndex, player) / ClearOwners(): the gameplay script mirrors its GWA_OWNERS table here so the native trade hooks know who owns an archived work.
//   GWArchive.SetTradeArchived(0|1): archived works are offered in, and accepted by, trades (default 1).
//   Trading: the trade screen's list of a player's works is built from city slots only. Hooks on GreatWork::CreatePossible (adds an item for every work the giver owns in the
//   archive), IsValid (accepts such an item) and Enact (hands the work to the receiver with ReceiveGreatWork; no free slot -> it lands in the receiver's archive).
//   GWArchive.IsHeld(listIndex) -> bool                (displayed in a slot of any living player's city)
//   event codes: 1 the building holding it was removed (RemoveGreatWorkSlots)   2 placement for a player failed (ReceiveGreatWork: trade, capture)
//                3 relic created into the archive (CreateRelic hook)   4 found in a leftover (ghost) slot entry and moved out of it
//   GWArchive.TryPlace(player, index [, ownerByProperty]): a non-zero third argument skips the record-player check (the script has verified ownership). Also: GWArchive.ArchivedRelicCount() -> number created this session.
// All offsets are RVAs in GameCore_XP2_FinalRelease.dll, Steam build 26595377 (prologues are checked at start-up; see Create()).
// The behaviour is switched by the game capability CAPABILITY_DEVCE_GW_ARCHIVE (Features.h): without it every hook passes through.
// Background: the "Great works" topic of the GameCore reference (civ6-gamecore-reference).
namespace GWArchive {
	namespace Types {
		typedef void* (__cdecl* CultureGet)(void);                                   // Game::Culture::Get
		typedef void* (__cdecl* EditPlayer)(int playerId);                           // Context::Globals::EditPlayer
		typedef void* (__cdecl* PlayerManagerEdit)(void);                            // Player::Manager::Edit
		typedef bool  (__thiscall* IsAlive)(void* manager, int playerId);            // Player::Manager::IsAlive
		typedef bool  (__thiscall* IsInCity)(void* buildings, int listIndex);        // City::Buildings::IsInCity
		typedef bool  (__thiscall* CanAddGreatWork)(void* buildings, int listIndex, int* outBuilding); // City::Buildings::CanAddGreatWork
		typedef void  (__thiscall* CitiesAddGreatWork)(void* cities, int listIndex); // Player::Cities::AddGreatWork
		typedef void  (__fastcall* CreateRelic)(void* religion, void* plotCoord, unsigned int source);   // Player::Religion::CreateRelic
		typedef void* (__thiscall* GetNearestRelicSlot)(void* religion, void* plotCoord, int* outBuilding, int* outSlot); // Player::Religion::GetNearestRelicSlot
		typedef int   (__thiscall* AddRelic)(void* culture, void* city, int building, int slot, unsigned int source);     // Game::Culture::AddRelic
		typedef bool  (__thiscall* PlayerIsHuman)(void* player);                       // Player::Instance::IsHuman
		typedef bool  (__thiscall* HasBuilding)(void* buildings, int buildingType);    // City::Buildings::HasBuilding
		typedef void* (__cdecl* GetObjectTypeDefinitions)(void);                       // Definitions::GetGreatWorkObjectTypeDefinitions
		typedef unsigned int (__cdecl* MakeHash)(const char* text);                    // Utilities::MakeHash
		typedef void  (__fastcall* RemoveGreatWorkSlots)(void* buildings, int buildingType); // City::Buildings::RemoveGreatWorkSlots (private)
		typedef bool  (__fastcall* ReceiveGreatWork)(void* culture, int listIndex);    // Player::Culture::ReceiveGreatWork
		typedef void* (__cdecl* GreatWorkItemCreate)(void);                           // Diplomacy::Deal::Item::GreatWork::Create (static factory, returns a new item)
		typedef void  (__cdecl* GreatWorkCreatePossible)(void* deal, int giver, int receiver, unsigned int p4, void* itemVector); // ...::GreatWork::CreatePossible (static)
		typedef int   (__fastcall* GreatWorkIsValid)(void* item, void* deal, void* messageMap); // ...::GreatWork::IsValid (virtual): 1 valid, 0 no such work held, 2 receiver has no slots
		typedef void  (__fastcall* GreatWorkEnact)(void* item, unsigned int phase);    // ...::GreatWork::Enact (virtual)
		typedef unsigned int (__fastcall* ItemGetParentType)(void* item);              // ...::Item::Instance::GetParentType
		typedef void  (__fastcall* ItemVectorInsert)(void* vector, void* position, void* uniquePtrRef); // eastl::vector<unique_ptr<Deal::Item::Instance>>::DoInsertValue
		typedef void* (__fastcall* EditVariable)(void* autoVariable);                  // FAutoVariable<VariantMap,Game::Instance>::edit (tracked-state accessor); for a city: edit(city + 0xca0) -> Buildings*
	}

	constexpr uintptr_t CULTURE_GET_OFFSET = 0x1c8910;
	constexpr uintptr_t EDIT_PLAYER_OFFSET = 0x44f00;
	constexpr uintptr_t PLAYER_MANAGER_EDIT_OFFSET = 0x305dc0;
	constexpr uintptr_t IS_ALIVE_OFFSET = 0x306ac0;
	constexpr uintptr_t IS_IN_CITY_OFFSET = 0x10cc60;
	constexpr uintptr_t CAN_ADD_GREAT_WORK_OFFSET = 0x109a70;
	constexpr uintptr_t CITIES_ADD_GREAT_WORK_OFFSET = 0x264390;
	constexpr uintptr_t CREATE_RELIC_OFFSET = 0x313950;
	constexpr uintptr_t GET_NEAREST_RELIC_SLOT_OFFSET = 0x3144b0;
	constexpr uintptr_t ADD_RELIC_OFFSET = 0x1c7240;
	constexpr uintptr_t PLAYER_IS_HUMAN_OFFSET = 0x2f9830;   // mapped through the call graph ("resolved"): confirm with the log line on the first relic
	constexpr uintptr_t HAS_BUILDING_OFFSET = 0x10c450;
	constexpr uintptr_t OBJECT_TYPE_DEFINITIONS_OFFSET = 0x7d0de0;
	constexpr uintptr_t MAKE_HASH_OFFSET = 0x6062a0;
	constexpr uintptr_t REMOVE_GREAT_WORK_SLOTS_OFFSET = 0x10d3e0;
	constexpr uintptr_t RECEIVE_GREAT_WORK_OFFSET = 0x2832f0;
	constexpr uintptr_t EDIT_VARIABLE_OFFSET = 0x72a960;
	constexpr uintptr_t GW_ITEM_CREATE_OFFSET = 0x585ba0;
	constexpr uintptr_t GW_ITEM_CREATE_POSSIBLE_OFFSET = 0x585bf0;
	constexpr uintptr_t GW_ITEM_IS_VALID_OFFSET = 0x5863f0;
	constexpr uintptr_t GW_ITEM_ENACT_OFFSET = 0x585f40;
	constexpr uintptr_t ITEM_GET_PARENT_TYPE_OFFSET = 0x586cd0;
	constexpr uintptr_t ITEM_VECTOR_INSERT_OFFSET = 0x4f1ee0;
	constexpr uintptr_t CURRENT_GAME_OFFSET = 0xb8aa68;        // data: Context::Globals::ms_pkCurrentGame; the game turn is an int at game + 0xc0

	extern int Register(hks::lua_State* L);
	extern void Create();
}
