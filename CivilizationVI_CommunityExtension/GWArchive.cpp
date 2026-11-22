#include "GWArchive.h"
#include "Runtime.h"
#include "Features.h"
#include "DevBridge.h"
#include <iostream>
#include <vector>
#include <deque>
#include <set>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <string>
#include <windows.h>
#include <cstring>
#include <cstdint>

namespace GWArchive {
	static Types::CultureGet CultureGet;
	static Types::EditPlayer EditPlayer;
	static Types::PlayerManagerEdit PlayerManagerEdit;
	static Types::IsAlive IsAlive;
	static Types::IsInCity IsInCity;
	static Types::CanAddGreatWork CanAddGreatWork;
	static Types::CitiesAddGreatWork CitiesAddGreatWork;
	static Types::CreateRelic orig_CreateRelic;
	static Types::CreateRelic base_CreateRelic;
	static Types::GetNearestRelicSlot GetNearestRelicSlot;
	static Types::AddRelic AddRelic;
	static Types::PlayerIsHuman PlayerIsHuman;
	static Types::HasBuilding HasBuilding;
	static Types::GetObjectTypeDefinitions GetObjectTypeDefinitions;
	static Types::MakeHash MakeHash;
	static Types::RemoveGreatWorkSlots orig_RemoveGreatWorkSlots;
	static Types::RemoveGreatWorkSlots base_RemoveGreatWorkSlots;
	static Types::ReceiveGreatWork orig_ReceiveGreatWork;
	static Types::EditVariable EditVariable;
	static Types::GreatWorkItemCreate GreatWorkItemCreate;
	static Types::GreatWorkCreatePossible orig_CreatePossible;
	static Types::GreatWorkCreatePossible base_CreatePossible;
	static Types::GreatWorkIsValid orig_IsValid;
	static Types::GreatWorkIsValid base_IsValid;
	static Types::GreatWorkEnact orig_Enact;
	static Types::GreatWorkEnact base_Enact;
	static Types::ItemGetParentType ItemGetParentType;
	static Types::ItemVectorInsert ItemVectorInsert;
	static Types::ReceiveGreatWork base_ReceiveGreatWork;

	// Layouts read from the decompiled game code (symbol build; the functions are byte-identical in the installed build):
	//   player + 0x6d0 -> Player::Cities; cities + 0xd0 -> first list node; node[0] = City*, node + 0x10 = next node
	//   city + 0xcb0 = City::Buildings (the object the great work functions take as `this`)
	//   Game::Culture: game-wide great work records from *(culture + 0x118) to *(culture + 0x120), 0x18 bytes each,
	//   record + 4 = the player the work was created for.
	static void* CityList(int playerId) {
		void* player = EditPlayer(playerId);
		if (player == nullptr) return nullptr;
		void* cities = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(player) + 0x6d0);
		if (cities == nullptr) return nullptr;
		return *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(cities) + 0xd0);
	}

	static void* NodeCity(void* node) { return *reinterpret_cast<void**>(node); }
	static void* NodeNext(void* node) { return *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(node) + 0x10); }
	static void* CityBuildings(void* city) { return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(city) + 0xcb0); }

	static bool HeldByAnyone(int listIndex) {
		void* manager = PlayerManagerEdit();
		if (manager == nullptr) return false;
		for (int p = 0; p < 64; ++p) {
			if (!IsAlive(manager, p)) continue;
			for (void* node = CityList(p); node != nullptr; node = NodeNext(node)) {
				void* city = NodeCity(node);
				if (city != nullptr && IsInCity(CityBuildings(city), listIndex)) return true;
			}
		}
		return false;
	}


	// ---- ownership events ----
	struct OwnerEvent { int work; int owner; int code; int turn; };
	static std::deque<OwnerEvent> s_events;

	static int CurrentTurn() {
		uintptr_t game = *reinterpret_cast<uintptr_t*>(Runtime::GameCoreAddress + CURRENT_GAME_OFFSET);
		return game == 0 ? -1 : *reinterpret_cast<int*>(game + 0xc0);
	}

	static void PushEvent(int work, int owner, int code) {
		if (work < 0) return;
		for (const OwnerEvent& e : s_events) if (e.work == work && e.owner == owner && e.code == code) return;
		if (s_events.size() > 4096) s_events.pop_front();
		s_events.push_back({ work, owner, code, CurrentTurn() });
	}

	// Last record in the game-wide list with this great work type (artifacts can have several records per type; a new one is appended last).
	static int LastRecordOfType(int type) {
		void* culture = CultureGet();
		if (culture == nullptr) return -1;
		uintptr_t begin = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(culture) + 0x118);
		uintptr_t end = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(culture) + 0x120);
		if (begin == 0 || end < begin) return -1;
		for (int i = static_cast<int>((end - begin) / 0x18) - 1; i >= 0; --i)
			if (*reinterpret_cast<int*>(begin + static_cast<uintptr_t>(i) * 0x18) == type) return i;
		return -1;
	}

	static int PurgeGhostsInCity(void* city, int* worksOut);   // defined below with the other ghost-slot code

	static int TryPlace(int playerId, int listIndex, bool ownerByProperty) {
		void* culture = CultureGet();
		if (culture == nullptr) return 9;

		uintptr_t begin = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(culture) + 0x118);
		uintptr_t end = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(culture) + 0x120);
		if (begin == 0 || end < begin) return 9;
		if (listIndex < 0 || static_cast<uintptr_t>(listIndex) >= (end - begin) / 0x18) return 1;

		int recordPlayer = *reinterpret_cast<int*>(begin + static_cast<uintptr_t>(listIndex) * 0x18 + 4);
		if (!ownerByProperty && recordPlayer != playerId) return 2;

		// Never place a work that is already displayed anywhere (by any player): that would duplicate it.
		if (HeldByAnyone(listIndex)) return 3;

		void* player = EditPlayer(playerId);
		if (player == nullptr) return 9;
		void* cities = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(player) + 0x6d0);
		if (cities == nullptr) return 9;

		// The game's placement walks slot lists without checking that the building exists: drop this player's leftover (ghost) entries first, or the work could be
		// placed into one and be invisible. (User-initiated action, so the small scan is acceptable.)
		for (void* node = CityList(playerId); node != nullptr; node = NodeNext(node)) {
			void* city = NodeCity(node);
			int worksMoved = 0;
			if (city != nullptr) PurgeGhostsInCity(city, &worksMoved);
		}

		bool anyCityCanTake = false;
		for (void* node = CityList(playerId); node != nullptr; node = NodeNext(node)) {
			void* city = NodeCity(node);
			if (city != nullptr && CanAddGreatWork(CityBuildings(city), listIndex, nullptr)) { anyCityCanTake = true; break; }
		}
		if (!anyCityCanTake) return 4;

		// The game's own placement: first city that can take it, through the tracked-state accessor.
		CitiesAddGreatWork(cities, listIndex);

		// Verify instead of assuming.
		for (void* node = CityList(playerId); node != nullptr; node = NodeNext(node)) {
			void* city = NodeCity(node);
			if (city != nullptr && IsInCity(CityBuildings(city), listIndex)) return 0;
		}
		return 8;
	}



	// Diagnostic log: the shared Dev CE log (DevBridge.log), like the other Dev CE features.
	static void Log(const std::string& text) {
		DevBridge::Log("%s", text.c_str());
	}

	// ---- relic archive ----
	// Layouts (decompiled): Religion + 0x90 -> Player::Instance; player + 0xd8 = player id.
	//   GREATWORKOBJECT_RELIC definition: +0x40..+0x48 = pointers to entries whose building type is at entry + 0x14.
	//   Object type collection: begin at *coll, end at *(coll + 8), 8-byte pointers to definitions, definition + 0x18 = name hash.
	static constexpr int OUT_OF_RANGE_SLOT = 99;   // AddGreatWorkToSlot bounds-checks the slot and silently skips placement
	static int s_archivedRelics = 0;
	static bool s_ready = false;   // the engine functions were found and checked (Create)

	static bool RelicBuildingTypes(std::vector<int>& out) {
		void* coll = GetObjectTypeDefinitions();
		if (coll == nullptr) return false;
		uintptr_t begin = *reinterpret_cast<uintptr_t*>(coll);
		uintptr_t end = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(coll) + 8);
		if (begin == 0 || end < begin) return false;
		unsigned int hash = MakeHash("GREATWORKOBJECT_RELIC");
		for (uintptr_t p = begin; p < end; p += 8) {
			uintptr_t def = *reinterpret_cast<uintptr_t*>(p);
			if (def == 0 || *reinterpret_cast<unsigned int*>(def + 0x18) != hash) continue;
			uintptr_t q = *reinterpret_cast<uintptr_t*>(def + 0x40), qe = *reinterpret_cast<uintptr_t*>(def + 0x48);
			for (; q != 0 && q < qe; q += 8) {
				uintptr_t entry = *reinterpret_cast<uintptr_t*>(q);
				if (entry != 0) out.push_back(*reinterpret_cast<int*>(entry + 0x14));
			}
			return !out.empty();
		}
		return false;
	}

	// Returns true when a relic was created into the archive.
	static bool ArchiveRelic(void* religion, unsigned int source) {
		void* player = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(religion) + 0x90);
		if (player == nullptr) return false;
		int playerId = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(player) + 0xd8);
		bool human = PlayerIsHuman(player);
		{ std::ostringstream o; o << "[GWA] relic archive: no free relic slot for player " << playerId << " (human=" << human << ") source=" << source; Log(o.str()); }
		if (!human) return false;
		std::vector<int> types;
		if (!RelicBuildingTypes(types)) { Log("[GWA] relic archive: relic building list not found"); return false; }
		void* city = nullptr; int building = types[0];
		for (void* node = CityList(playerId); node != nullptr && city == nullptr; node = NodeNext(node)) {
			void* c = NodeCity(node);
			if (c == nullptr) continue;
			for (int t : types) if (HasBuilding(CityBuildings(c), t)) { city = c; building = t; break; }
		}
		if (city == nullptr) {   // no relic building at all: any city (the slot is out of range anyway)
			for (void* node = CityList(playerId); node != nullptr; node = NodeNext(node)) { if (NodeCity(node) != nullptr) { city = NodeCity(node); break; } }
		}
		if (city == nullptr) { Log("[GWA] relic archive: player has no city yet; relic not created"); return false; }
		void* culture = CultureGet();
		if (culture == nullptr) return false;
		{ std::ostringstream o; o << "[GWA] relic archive: calling AddRelic(building " << building << ", slot " << OUT_OF_RANGE_SLOT << ")"; Log(o.str()); }
		int type = AddRelic(culture, city, building, OUT_OF_RANGE_SLOT, source);
		{ std::ostringstream o; o << "[GWA] relic archive: AddRelic returned " << type; Log(o.str()); }
		if (type < 0) return false;
		++s_archivedRelics;
		{ int idx = LastRecordOfType(type); PushEvent(idx, playerId, 3); { std::ostringstream o; o << "[GWA] ownership event: relic record " << idx << " -> player " << playerId << " (code 3)"; Log(o.str()); } }
		return true;
	}

	static void __fastcall Hook_CreateRelic(void* religion, void* plotCoord, unsigned int source) {
		if (!Features::On(Features::GreatWorksArchive)) { base_CreateRelic(religion, plotCoord, source); return; }
		try {
			int b = -1, s = -1;
			void* city = GetNearestRelicSlot(religion, plotCoord, &b, &s);
			if (city == nullptr && ArchiveRelic(religion, source)) return;
		}
		catch (...) { Log("[GWA] relic archive: C++ exception, falling back to the original"); }
		base_CreateRelic(religion, plotCoord, source);
	}

	static int lArchivedRelicCount(hks::lua_State* L) { hks::pushinteger(L, s_archivedRelics); return 1; }


	// ---- ownership hooks ----

	// ---- leftover ("ghost") slot entries ----
	// A slot entry (Buildings + 0xc8, 0x20 bytes each: building type, slot array at +8, slot count at +0x10; slot = 8 bytes, work index at +4) for a building
	// the city does not have. The game's placement code walks the slot vector without checking HasBuilding, so a work can be placed into such an entry, where no
	// screen shows it (see findings/topics/ghost-great-work-slots.md). Dropping the entry with the game's own RemoveGreatWorkSlots (the unhooked original,
	// so no "building removed" event) leaves any work inside it in no slot: it then belongs to the archive (event code 4).
	struct GhostEntry { int buildingType; std::vector<int> works; };

	static std::vector<GhostEntry> FindGhostEntries(void* city) {
		std::vector<GhostEntry> out;
		void* buildings = CityBuildings(city);
		uintptr_t b = reinterpret_cast<uintptr_t>(buildings);
		uintptr_t data = *reinterpret_cast<uintptr_t*>(b + 0xc8);
		unsigned count = *reinterpret_cast<unsigned*>(b + 0xd8);
		for (unsigned k = 0; data != 0 && k < count && k < 200; ++k) {
			uintptr_t entry = data + static_cast<uintptr_t>(k) * 0x20;
			int type = *reinterpret_cast<int*>(entry);
			if (HasBuilding(buildings, type)) continue;
			GhostEntry g; g.buildingType = type;
			uintptr_t slots = *reinterpret_cast<uintptr_t*>(entry + 8);
			unsigned sc = *reinterpret_cast<unsigned*>(entry + 0x10);
			for (unsigned j = 0; slots != 0 && j < sc && j < 100; ++j) {
				int work = *reinterpret_cast<int*>(slots + static_cast<uintptr_t>(j) * 8 + 4);
				if (work != -1) g.works.push_back(work);
			}
			out.push_back(g);
		}
		return out;
	}

	// Drops every ghost entry of one city. Returns the number of entries dropped; worksOut = works that were inside them.
	static int PurgeGhostsInCity(void* city, int* worksOut) {
		std::vector<GhostEntry> ghosts = FindGhostEntries(city);
		int owner = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(city) + 0xd8);
		void* editable = EditVariable(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(city) + 0xca0));
		int works = 0;
		for (const GhostEntry& g : ghosts) {
			for (int w : g.works) {
				PushEvent(w, owner, 4); ++works;
				std::ostringstream o; o << "[GWA] ghost slot: work " << w << " was in a leftover entry for building type " << g.buildingType << " of a city of player " << owner << " -> archive (code 4)"; Log(o.str());
			}
			base_RemoveGreatWorkSlots(editable, g.buildingType);
			std::ostringstream o; o << "[GWA] ghost slot: dropped the leftover entry for building type " << g.buildingType; Log(o.str());
		}
		if (worksOut != nullptr) *worksOut += works;
		return static_cast<int>(ghosts.size());
	}

	// After a successful placement for a player: if the work went into a ghost entry, purge that one city right away.
	static void CheckPlacementIntoGhost(void* culture, int listIndex) {
		void* player = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(culture) + 0x90);
		int owner = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(player) + 0xd8);
		for (void* node = CityList(owner); node != nullptr; node = NodeNext(node)) {
			void* city = NodeCity(node);
			if (city == nullptr || !IsInCity(CityBuildings(city), listIndex)) continue;
			for (const GhostEntry& g : FindGhostEntries(city)) {
				for (int w : g.works) {
					if (w != listIndex) continue;
					std::ostringstream o; o << "[GWA] ghost slot: work " << listIndex << " was just placed into a leftover entry (building type " << g.buildingType << "); purging this city"; Log(o.str());
					int works = 0; PurgeGhostsInCity(city, &works);
					return;
				}
			}
			return;   // found the holding city and it is a normal slot
		}
	}

	static int lPurgeGhosts(hks::lua_State* L) {
		int playerId = hks::checkinteger(L, 1);
		int entries = 0, works = 0;
		if (!s_ready) { hks::pushinteger(L, 0); hks::pushinteger(L, 0); return 2; }
		try {
			for (void* node = CityList(playerId); node != nullptr; node = NodeNext(node)) {
				void* city = NodeCity(node);
				if (city != nullptr) entries += PurgeGhostsInCity(city, &works);
			}
		}
		catch (...) { Log("[GWA] ghost slot: exception during the manual purge"); }
		hks::pushinteger(L, entries); hks::pushinteger(L, works);
		return 2;
	}
	// Building removed: before the game drops the building's slot entry, report every work that was in it (owner = the city's owner).
	static void __fastcall Hook_RemoveGreatWorkSlots(void* buildings, int buildingType) {
		if (!Features::On(Features::GreatWorksArchive)) { base_RemoveGreatWorkSlots(buildings, buildingType); return; }
		try {
			uintptr_t b = reinterpret_cast<uintptr_t>(buildings);
			int owner = *reinterpret_cast<int*>(b - 0xcb0 + 0xd8);        // Buildings sits at city + 0xcb0; the city's owner is at city + 0xd8
			uintptr_t data = *reinterpret_cast<uintptr_t*>(b + 0xc8);
			unsigned count = *reinterpret_cast<unsigned*>(b + 0xd8);
			for (unsigned k = 0; data != 0 && k < count && k < 200; ++k) {
				uintptr_t entry = data + static_cast<uintptr_t>(k) * 0x20;
				if (*reinterpret_cast<int*>(entry) != buildingType) continue;
				uintptr_t slots = *reinterpret_cast<uintptr_t*>(entry + 8);
				unsigned sc = *reinterpret_cast<unsigned*>(entry + 0x10);
				for (unsigned j = 0; slots != 0 && j < sc && j < 100; ++j) {
					int work = *reinterpret_cast<int*>(slots + static_cast<uintptr_t>(j) * 8 + 4);
					if (work != -1) { PushEvent(work, owner, 1); std::ostringstream o; o << "[GWA] ownership event: work " << work << " lost its building (type " << buildingType << ") -> player " << owner << " (code 1)"; Log(o.str()); }
				}
			}
		}
		catch (...) {}
		base_RemoveGreatWorkSlots(buildings, buildingType);
	}

	// Placement for a player failed (trade, capture overflow): the work is meant for that player.
	static bool __fastcall Hook_ReceiveGreatWork(void* culture, int listIndex) {
		bool ok = base_ReceiveGreatWork(culture, listIndex);
		if (!Features::On(Features::GreatWorksArchive)) return ok;
		if (ok) {
			try { CheckPlacementIntoGhost(culture, listIndex); } catch (...) {}
		}
		else {
			try {
				void* player = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(culture) + 0x90);
				int owner = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(player) + 0xd8);
				PushEvent(listIndex, owner, 2);
				std::ostringstream o; o << "[GWA] ownership event: placement of work " << listIndex << " failed for player " << owner << " (code 2)"; Log(o.str());
			}
			catch (...) {}
		}
		return ok;
	}


	// ---- trading archived works ----
	// The trade screen builds a player's list of tradable works from the city slot lists (Deal::Item::GreatWork::CreatePossible), validates an item with IsValid
	// (the giver must hold the work in a city; the receiver must have a free slot) and moves it with Enact (find the holding city, RemoveGreatWork, ReceiveGreatWork).
	// An archived work is held by no city, so none of that sees it. These hooks make it tradable. Ownership comes from the gameplay script's mirror (SetOwner), plus any
	// event still waiting in the queue; without either, the record's creator.
	static std::unordered_map<int, int> s_owner;
	static bool s_tradeArchived = true;

	static int RecordCount() {
		void* culture = CultureGet();
		if (culture == nullptr) return 0;
		uintptr_t begin = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(culture) + 0x118);
		uintptr_t end = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(culture) + 0x120);
		return (begin == 0 || end < begin) ? 0 : static_cast<int>((end - begin) / 0x18);
	}

	static int OwnerOfWork(int idx) {
		for (auto it = s_events.rbegin(); it != s_events.rend(); ++it) if (it->work == idx) return it->owner;   // not yet drained by the script
		auto f = s_owner.find(idx);
		if (f != s_owner.end()) return f->second;
		void* culture = CultureGet();
		uintptr_t begin = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(culture) + 0x118);
		return *reinterpret_cast<int*>(begin + static_cast<uintptr_t>(idx) * 0x18 + 4);   // the creator
	}

	// Every work that some living player's city slot list holds (the game's own view, ghost entries included).
	static std::set<int> CollectHeldWorks() {
		std::set<int> held;
		void* manager = PlayerManagerEdit();
		if (manager == nullptr) return held;
		for (int p = 0; p < 64; ++p) {
			if (!IsAlive(manager, p)) continue;
			for (void* node = CityList(p); node != nullptr; node = NodeNext(node)) {
				void* city = NodeCity(node);
				if (city == nullptr) continue;
				uintptr_t b = reinterpret_cast<uintptr_t>(CityBuildings(city));
				uintptr_t data = *reinterpret_cast<uintptr_t*>(b + 0xc8);
				unsigned count = *reinterpret_cast<unsigned*>(b + 0xd8);
				for (unsigned k = 0; data != 0 && k < count && k < 200; ++k) {
					uintptr_t entry = data + static_cast<uintptr_t>(k) * 0x20;
					uintptr_t slots = *reinterpret_cast<uintptr_t*>(entry + 8);
					unsigned sc = *reinterpret_cast<unsigned*>(entry + 0x10);
					for (unsigned j = 0; slots != 0 && j < sc && j < 100; ++j) {
						int w = *reinterpret_cast<int*>(slots + static_cast<uintptr_t>(j) * 8 + 4);
						if (w != -1) held.insert(w);
					}
				}
			}
		}
		return held;
	}

	static bool IsArchivedOwnedBy(int player, int idx) {
		if (!s_tradeArchived || idx < 0 || idx >= RecordCount()) return false;
		if (OwnerOfWork(idx) != player) return false;
		return !HeldByAnyone(idx);
	}

	// The great work index an item carries: the item's value variant (virtual +0x50), type tag at +0, index at +8, same test the game's own code makes.
	static int ItemWorkIndex(void* item) {
		typedef unsigned short* (__fastcall* GetValueFn)(void*);
		void** vtable = *reinterpret_cast<void***>(item);
		unsigned short* v = reinterpret_cast<GetValueFn>(vtable[0x50 / 8])(item);
		if (v == nullptr) return -1;
		if (((*reinterpret_cast<unsigned*>(v + 10) >> 0x1d) & 1) == 0 && v[0] < 0x16 && ((0x30002eU >> (v[0] & 0x1f)) & 1) != 0)
			return *reinterpret_cast<int*>(v + 4);
		return -1;
	}

	static void ItemSetWorkIndex(void* item, int idx) {
		typedef void(__fastcall* SetValueFn)(void*, void*);
		void** vtable = *reinterpret_cast<void***>(item);
		unsigned char variant[0x40] = {};
		*reinterpret_cast<unsigned*>(variant) = 2;                        // value type tag, as the game's CreatePossible sets it
		*reinterpret_cast<unsigned long long*>(variant + 8) = static_cast<unsigned long long>(static_cast<unsigned>(idx));
		reinterpret_cast<SetValueFn>(vtable[0x58 / 8])(item, variant);
	}

	static void __cdecl Hook_CreatePossible(void* deal, int giver, int receiver, unsigned int p4, void* itemVector) {
		base_CreatePossible(deal, giver, receiver, p4, itemVector);
		if (!s_tradeArchived || !Features::On(Features::GreatWorksArchive)) return;
		try {
			std::set<int> held = CollectHeldWorks();
			int n = RecordCount(), added = 0;
			for (int idx = 0; idx < n; ++idx) {
				if (held.count(idx) != 0 || OwnerOfWork(idx) != giver) continue;
				void* item = GreatWorkItemCreate();
				if (item == nullptr) continue;
				*reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(item) + 0x28) = giver;
				*reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(item) + 0x2c) = receiver;
				ItemSetWorkIndex(item, idx);
				// push the owning pointer into the caller's vector: begin, end (+8), capacity end (+0x10), as the game's own loop does
				uintptr_t vec = reinterpret_cast<uintptr_t>(itemVector);
				uintptr_t end = *reinterpret_cast<uintptr_t*>(vec + 8), cap = *reinterpret_cast<uintptr_t*>(vec + 0x10);
				if (end < cap) { *reinterpret_cast<void**>(end) = item; *reinterpret_cast<uintptr_t*>(vec + 8) = end + 8; }
				else { void* owning = item; ItemVectorInsert(itemVector, reinterpret_cast<void*>(end), &owning); }
				++added;
			}
			if (added > 0) { std::ostringstream o; o << "[GWA] trade: offered " << added << " archived work(s) of player " << giver << " to player " << receiver; Log(o.str()); }
		}
		catch (...) { Log("[GWA] trade: exception while adding archived works to the list"); }
	}

	static int __fastcall Hook_ItemIsValid(void* item, void* deal, void* messageMap) {
		int result = base_IsValid(item, deal, messageMap);
		if ((result == 0 || result == 2) && Features::On(Features::GreatWorksArchive)) {
			try {
				int giver = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(item) + 0x28);
				if (IsArchivedOwnedBy(giver, ItemWorkIndex(item))) return 1;   // an archived work needs no holding city and, being archived, no free slot at the receiver
			}
			catch (...) {}
		}
		return result;
	}

	static void __fastcall Hook_ItemEnact(void* item, unsigned int phase) {
		int idx = -1, giver = -1, receiver = -1;
		bool archived = false;
		if (!Features::On(Features::GreatWorksArchive)) { base_Enact(item, phase); return; }
		try {
			idx = ItemWorkIndex(item);
			giver = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(item) + 0x28);
			receiver = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(item) + 0x2c);
			archived = IsArchivedOwnedBy(giver, idx);   // decided BEFORE the original runs (it resets the item's value)
		}
		catch (...) {}
		base_Enact(item, phase);
		if (!archived) return;
		try {
			if (ItemGetParentType(item) == 0xd384eeb7u) return;
			void* manager = PlayerManagerEdit();
			if (manager == nullptr || !IsAlive(manager, giver) || !IsAlive(manager, receiver)) return;
			void* receiverPlayer = EditPlayer(receiver);
			void* receiverCulture = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(receiverPlayer) + 0x6f0);
			bool placed = orig_ReceiveGreatWork(receiverCulture, idx);   // through our own hook: a failure records the receiver as the owner (event code 2)
			if (!placed) *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(item) + 0x90) = idx;   // like the game: keep the stranded index for PostEnact
			std::ostringstream o; o << "[GWA] trade: archived work " << idx << " handed from player " << giver << " to player " << receiver << (placed ? " (placed in a slot)" : " (no free slot: it is in the receiver's archive)"); Log(o.str());
		}
		catch (...) { Log("[GWA] trade: exception while enacting an archived work"); }
	}

	static int lSetOwner(hks::lua_State* L) { int idx = hks::checkinteger(L, 1); int owner = hks::checkinteger(L, 2); s_owner[idx] = owner; return 0; }
	static int lClearOwners(hks::lua_State*) { s_owner.clear(); return 0; }
	static int lSetTradeArchived(hks::lua_State* L) { s_tradeArchived = hks::checkinteger(L, 1) != 0; return 0; }

	static int lPopEvent(hks::lua_State* L) {
		while (!s_events.empty()) {
			OwnerEvent e = s_events.front();
			s_events.pop_front();
			bool held = true;
			try { held = HeldByAnyone(e.work); } catch (...) {}
			if (held) continue;   // displayed again by now (a replacement or a retried trade): nothing to remember
			hks::pushinteger(L, e.work); hks::pushinteger(L, e.owner); hks::pushinteger(L, e.code); hks::pushinteger(L, e.turn);
			return 4;
		}
		return 0;
	}

	static int lIsHeld(hks::lua_State* L) {
		int idx = hks::checkinteger(L, 1);
		bool held = true;
		if (s_ready) try { held = HeldByAnyone(idx); } catch (...) {}
		hks::pushboolean(L, held);
		return 1;
	}

	static int lTryPlace(hks::lua_State* L) {
		int playerId = hks::checkinteger(L, 1);
		int listIndex = hks::checkinteger(L, 2);
		bool ownerByProperty = hks::gettop(L) >= 3 && hks::checkinteger(L, 3) != 0;
		int code = s_ready ? 8 : 9;
		if (s_ready) try { code = TryPlace(playerId, listIndex, ownerByProperty); }
		catch (...) { code = 8; }
		hks::pushinteger(L, code);
		return 1;
	}

	int Register(hks::lua_State* L) {
		std::cout << "Registering GWArchive!\n";
		hks::createtable(L, 0, 8);
		PushLuaMethod(L, lTryPlace, "lTryPlace", -2, "TryPlace");
		PushLuaMethod(L, lArchivedRelicCount, "lArchivedRelicCount", -2, "ArchivedRelicCount");
		PushLuaMethod(L, lPopEvent, "lPopEvent", -2, "PopEvent");
		PushLuaMethod(L, lIsHeld, "lIsHeld", -2, "IsHeld");
		PushLuaMethod(L, lPurgeGhosts, "lPurgeGhosts", -2, "PurgeGhosts");
		PushLuaMethod(L, lSetOwner, "lSetOwner", -2, "SetOwner");
		PushLuaMethod(L, lClearOwners, "lClearOwners", -2, "ClearOwners");
		PushLuaMethod(L, lSetTradeArchived, "lSetTradeArchived", -2, "SetTradeArchived");
		hks::setfield(L, hks::LUA_GLOBAL, "GWArchive");
		return 0;
	}

	void Create() {
		using namespace Runtime;
		// Build 26595377 prologues (bytes after a relative displacement are not compared). A mismatch means another GameCore build:
		// no hook is installed and the archive stays off.
		struct Check { const char* name; uintptr_t rva; uint8_t bytes[8]; int n; };
		static const Check kChecks[] = {
		{ "CULTURE_GET", CULTURE_GET_OFFSET, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x8b, 0x1d }, 8 },
		{ "EDIT_PLAYER", EDIT_PLAYER_OFFSET, { 0x8b, 0xd1, 0x48, 0x8b, 0x0d, 0x5f, 0x5b, 0xb4 }, 5 },
		{ "PLAYER_MANAGER_EDIT", PLAYER_MANAGER_EDIT_OFFSET, { 0x48, 0x8b, 0x05, 0x79, 0x83, 0x88, 0x00, 0xc3 }, 3 },
		{ "IS_ALIVE", IS_ALIVE_OFFSET, { 0x4c, 0x8b, 0xc9, 0x83, 0xfa, 0x41, 0x73, 0x24 }, 8 },
		{ "IS_IN_CITY", IS_IN_CITY_OFFSET, { 0x44, 0x8b, 0x91, 0xd8, 0x00, 0x00, 0x00, 0x44 }, 8 },
		{ "CAN_ADD_GREAT_WORK", CAN_ADD_GREAT_WORK_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c }, 8 },
		{ "CITIES_ADD_GREAT_WORK", CITIES_ADD_GREAT_WORK_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x56, 0x48, 0x83 }, 8 },
		{ "CREATE_RELIC", CREATE_RELIC_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c }, 8 },
		{ "GET_NEAREST_RELIC_SLOT", GET_NEAREST_RELIC_SLOT_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x4c, 0x89, 0x4c }, 8 },
		{ "ADD_RELIC", ADD_RELIC_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x44, 0x89, 0x4c }, 8 },
		{ "PLAYER_IS_HUMAN", PLAYER_IS_HUMAN_OFFSET, { 0x48, 0x83, 0xec, 0x28, 0x8b, 0x91, 0xd8, 0x00 }, 8 },
		{ "HAS_BUILDING", HAS_BUILDING_OFFSET, { 0x83, 0xfa, 0xff, 0x74, 0x17, 0x3b, 0x51, 0x18 }, 8 },
		{ "OBJECT_TYPE_DEFINITIONS", OBJECT_TYPE_DEFINITIONS_OFFSET, { 0x48, 0x83, 0xec, 0x28, 0xe8, 0xe7, 0x3f, 0x87 }, 5 },
		{ "MAKE_HASH", MAKE_HASH_OFFSET, { 0x48, 0x8b, 0xd1, 0x41, 0x83, 0xc8, 0xff, 0x48 }, 8 },
		{ "REMOVE_GREAT_WORK_SLOTS", REMOVE_GREAT_WORK_SLOTS_OFFSET, { 0x48, 0x8b, 0xc4, 0x89, 0x50, 0x10, 0x55, 0x53 }, 8 },
		{ "RECEIVE_GREAT_WORK", RECEIVE_GREAT_WORK_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74 }, 8 },
		{ "EDIT_VARIABLE", EDIT_VARIABLE_OFFSET, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b }, 8 },
		{ "GW_ITEM_CREATE", GW_ITEM_CREATE_OFFSET, { 0x48, 0x83, 0xec, 0x38, 0xe8, 0x37, 0xd3, 0xe0 }, 5 },
		{ "GW_ITEM_CREATE_POSSIBLE", GW_ITEM_CREATE_POSSIBLE_OFFSET, { 0x44, 0x89, 0x44, 0x24, 0x18, 0x89, 0x54, 0x24 }, 8 },
		{ "GW_ITEM_IS_VALID", GW_ITEM_IS_VALID_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74 }, 8 },
		{ "GW_ITEM_ENACT", GW_ITEM_ENACT_OFFSET, { 0x40, 0x56, 0x48, 0x83, 0xec, 0x50, 0x48, 0x8b }, 8 },
		{ "ITEM_GET_PARENT_TYPE", ITEM_GET_PARENT_TYPE_OFFSET, { 0x48, 0x83, 0xec, 0x28, 0x8b, 0x51, 0x14, 0x85 }, 8 },
		{ "ITEM_VECTOR_INSERT", ITEM_VECTOR_INSERT_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x56, 0x57, 0x41 }, 8 },
		};
		for (const auto& c : kChecks) {
			if (memcmp(reinterpret_cast<const void*>(GameCoreAddress + c.rva), c.bytes, c.n) != 0) {
				Log(std::string("[GWA] unexpected bytes at ") + c.name + ": another GameCore build; the Great Works Archive hooks are not installed");
				return;
			}
		}
		CultureGet = GetGameCoreGlobalAt<Types::CultureGet>(CULTURE_GET_OFFSET);
		EditPlayer = GetGameCoreGlobalAt<Types::EditPlayer>(EDIT_PLAYER_OFFSET);
		PlayerManagerEdit = GetGameCoreGlobalAt<Types::PlayerManagerEdit>(PLAYER_MANAGER_EDIT_OFFSET);
		IsAlive = GetGameCoreGlobalAt<Types::IsAlive>(IS_ALIVE_OFFSET);
		IsInCity = GetGameCoreGlobalAt<Types::IsInCity>(IS_IN_CITY_OFFSET);
		CanAddGreatWork = GetGameCoreGlobalAt<Types::CanAddGreatWork>(CAN_ADD_GREAT_WORK_OFFSET);
		CitiesAddGreatWork = GetGameCoreGlobalAt<Types::CitiesAddGreatWork>(CITIES_ADD_GREAT_WORK_OFFSET);
		GetNearestRelicSlot = GetGameCoreGlobalAt<Types::GetNearestRelicSlot>(GET_NEAREST_RELIC_SLOT_OFFSET);
		AddRelic = GetGameCoreGlobalAt<Types::AddRelic>(ADD_RELIC_OFFSET);
		PlayerIsHuman = GetGameCoreGlobalAt<Types::PlayerIsHuman>(PLAYER_IS_HUMAN_OFFSET);
		HasBuilding = GetGameCoreGlobalAt<Types::HasBuilding>(HAS_BUILDING_OFFSET);
		GetObjectTypeDefinitions = GetGameCoreGlobalAt<Types::GetObjectTypeDefinitions>(OBJECT_TYPE_DEFINITIONS_OFFSET);
		MakeHash = GetGameCoreGlobalAt<Types::MakeHash>(MAKE_HASH_OFFSET);
		orig_CreateRelic = GetGameCoreGlobalAt<Types::CreateRelic>(CREATE_RELIC_OFFSET);
		CreateHook(orig_CreateRelic, &Hook_CreateRelic, &base_CreateRelic);
		EditVariable = GetGameCoreGlobalAt<Types::EditVariable>(EDIT_VARIABLE_OFFSET);
		orig_RemoveGreatWorkSlots = GetGameCoreGlobalAt<Types::RemoveGreatWorkSlots>(REMOVE_GREAT_WORK_SLOTS_OFFSET);
		CreateHook(orig_RemoveGreatWorkSlots, &Hook_RemoveGreatWorkSlots, &base_RemoveGreatWorkSlots);
		GreatWorkItemCreate = GetGameCoreGlobalAt<Types::GreatWorkItemCreate>(GW_ITEM_CREATE_OFFSET);
		ItemGetParentType = GetGameCoreGlobalAt<Types::ItemGetParentType>(ITEM_GET_PARENT_TYPE_OFFSET);
		ItemVectorInsert = GetGameCoreGlobalAt<Types::ItemVectorInsert>(ITEM_VECTOR_INSERT_OFFSET);
		orig_CreatePossible = GetGameCoreGlobalAt<Types::GreatWorkCreatePossible>(GW_ITEM_CREATE_POSSIBLE_OFFSET);
		CreateHook(orig_CreatePossible, &Hook_CreatePossible, &base_CreatePossible);
		orig_IsValid = GetGameCoreGlobalAt<Types::GreatWorkIsValid>(GW_ITEM_IS_VALID_OFFSET);
		CreateHook(orig_IsValid, &Hook_ItemIsValid, &base_IsValid);
		orig_Enact = GetGameCoreGlobalAt<Types::GreatWorkEnact>(GW_ITEM_ENACT_OFFSET);
		CreateHook(orig_Enact, &Hook_ItemEnact, &base_Enact);
		orig_ReceiveGreatWork = GetGameCoreGlobalAt<Types::ReceiveGreatWork>(RECEIVE_GREAT_WORK_OFFSET);
		CreateHook(orig_ReceiveGreatWork, &Hook_ReceiveGreatWork, &base_ReceiveGreatWork);
		s_ready = true;
	}
}
