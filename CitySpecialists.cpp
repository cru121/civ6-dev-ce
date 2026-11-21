// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3.0 (see LICENSE.txt).
#include "CitySpecialists.h"
#include "Runtime.h"
#include "DevBridge.h"
#include "HavokScript.h"
#include <Windows.h>
#include <cstring>
#include <string>
#include <vector>

namespace CitySpecialists {
	// One knapsack candidate (GameCore::City::SimpleScoring): the knapsack only reads food, priority and score, and copies `plot` through.
	struct Item { void* plot; int food; int priority; int score; int pad; };
	static_assert(sizeof(Item) == 0x18, "SimpleScoring is 0x18 bytes");

	typedef void(__fastcall* GlobalParametersInitializeFn)(void* params, void* connection);
	typedef void(__fastcall* RemoveAllFn)(void* citizens, bool send);
	typedef bool(__fastcall* TryAssignFn)(void* citizens, bool send);
	typedef Item* (__fastcall* KnapsackFn)(Item* begin, Item* end, unsigned capacity, int foodNeeded, bool flag);
	typedef int* (__fastcall* GetYieldFromTilesFn)(void* city, int* out, int yield);
	typedef uint32_t(__fastcall* MakeHashFn)(const char* s);
	typedef int(__fastcall* NameToYieldTypeFn)(uint32_t hash);
	typedef void* (__fastcall* PropertyEditFn)(void* object);
	typedef const uint8_t* (__fastcall* VariantGetFn)(void* map, uint32_t key);
	typedef void(__fastcall* VariantSetIntFn)(void* map, uint32_t key, int value);
	struct CityID { int16_t owner; int16_t kind; uint32_t id; };   // kind 2 = city
	typedef void* (__fastcall* GetCityFn)(const CityID* id);
	typedef void(__fastcall* ReallocateFn)(void* citizens);
	typedef int(__fastcall* ReportEventSendFn)(uint64_t type, const CityID* id, int key);
	typedef int(__fastcall* ReportEventSendPlotFn)(uint64_t type, const CityID* id, const void* plotCoord);
	typedef bool(__fastcall* PropertyEffectFn)(void* effect, void* city);
	// Database::IDatabaseConnection1 / IDatabaseQuery1 (vtable slots as GlobalParameters and the name / start-position loaders use them)
	typedef void* (__fastcall* DbPrepareFn)(void* connection, const char* sql, int length);   // +0x18
	typedef void(__fastcall* DbReleaseFn)(void* query);                                        // +0x08
	typedef bool(__fastcall* DbStepFn)(void* query);                                           // +0x20
	typedef void(__fastcall* DbResetFn)(void* query);                                          // +0x38
	typedef void(__fastcall* DbBindTextFn)(void* query, int index, const char* text, int length);   // +0x90
	typedef const char* (__fastcall* DbTextFn)(void* query, int column);                       // +0xf0
	typedef int(__fastcall* DbIntFn)(void* query, int column);                                 // +0x110

	constexpr int MAX_TYPES = 32;
	constexpr int MAX_YIELDS = 8;
	constexpr int NUM_BASE_YIELDS = 6;
	static const char* const kBaseYields[NUM_BASE_YIELDS] = { "YIELD_FOOD", "YIELD_PRODUCTION", "YIELD_GOLD", "YIELD_SCIENCE", "YIELD_CULTURE", "YIELD_FAITH" };

	constexpr int MAX_GPP = 16;

	struct YieldRow { uint32_t hash; int change; };
	struct GppRow { int greatPersonClass; int points; };   // class index = its position in GreatPersonClasses by rowid
	struct SpecialistType {
		char name[64];
		uint32_t slotsKey, countKey, lockedKey;
		YieldRow rows[MAX_YIELDS]; int numRows;
		uint32_t yieldKeyByName[NUM_BASE_YIELDS];   // DEVCE_SPECIALIST_YIELD_<Type>_<kBaseYields[i]>
		int yield[MAX_YIELDS];                      // by yield index (see YieldsReady)
		uint32_t yieldKey[MAX_YIELDS];              // by yield index; 0 = none
		GppRow gpp[MAX_GPP]; int numGpp;
	};
	static SpecialistType gTypes[MAX_TYPES];
	static int gNumTypes = 0;
	static volatile bool gMode = false;
	static volatile bool gYieldsReady = false;
	static char gMarker[MAX_TYPES];   // the candidate's `plot` for type t is &gMarker[t]

	static GlobalParametersInitializeFn orig_GlobalParametersInitialize;
	static RemoveAllFn orig_RemoveAll;
	static TryAssignFn orig_TryAssign;
	static GetYieldFromTilesFn orig_GetYieldFromTiles;
	static PropertyEffectFn orig_PropertyApply;
	static PropertyEffectFn orig_PropertyRemove;
	static KnapsackFn Knapsack;
	static MakeHashFn MakeHash;
	static NameToYieldTypeFn NameToYieldType;
	static PropertyEditFn PropertyEdit;
	static GetCityFn GetCity;
	static ReallocateFn Reallocate;
	static ReportEventSendFn ReportEventSend;
	static ReportEventSendPlotFn ReportEventSendPlot;
	static bool gOk = false;

	struct Check { const char* name; uintptr_t rva; uint8_t bytes[36]; size_t n; };
	static const Check kChecks[] = {
		{ "GlobalParameters::Initialize", GLOBAL_PARAMETERS_INITIALIZE_OFFSET, { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x57, 0x48, 0x81, 0xec, 0x90 }, 16 },
		{ "GlobalParameters::Initialize: release query", 0x1f430e, { 0xff, 0x50, 0x08 }, 3 },
		// query slots: bind text +0x90, step +0x20, reset +0x38 (GlobalParameters::Get); text +0xf0, prepare +0x18 (NameManager); int +0x110 (start positions)
		{ "GlobalParameters::Get: bind text", 0x1f0154, { 0xff, 0x90, 0x90, 0x00, 0x00, 0x00 }, 6 },
		{ "GlobalParameters::Get: step", 0x1f0160, { 0xff, 0x50, 0x20 }, 3 },
		{ "GlobalParameters::Get: reset", 0x1f0182, { 0xff, 0x52, 0x38 }, 3 },
		{ "NameManager::Initialize: prepare", 0x2321db, { 0xff, 0x50, 0x18 }, 3 },
		{ "NameManager::Initialize: text column", 0x232208, { 0xff, 0x90, 0xf0, 0x00, 0x00, 0x00 }, 6 },
		{ "start positions: int column", 0x167868, { 0xff, 0x90, 0x10, 0x01, 0x00, 0x00 }, 6 },
		{ "Utilities::MakeHash", MAKE_HASH_OFFSET, { 0x48, 0x8b, 0xd1, 0x41, 0x83, 0xc8, 0xff, 0x48 }, 8 },
		{ "Definition::Utility::NameToYieldType", NAME_TO_YIELD_TYPE_OFFSET, { 0x81, 0xf9, 0xec, 0x57, 0xf0, 0x24, 0x7f, 0x36, 0x74, 0x2d, 0x81, 0xf9 }, 12 },
		// city properties: AdjustCityProperty::Apply edits city +0x190, reads with the number test, writes with map vt+0x130
		{ "AdjustCityProperty::Apply: city +0x190", 0x8b41e5, { 0x48, 0x8d, 0x8a, 0x90, 0x01, 0x00, 0x00 }, 7 },
		{ "AdjustCityProperty::Apply: number test", 0x8b4201, { 0x44, 0x8b, 0x40, 0x14, 0x41, 0xc1, 0xe8, 0x1d, 0x41, 0xf6, 0xc0, 0x01, 0x75, 0x18, 0x0f, 0xb7 }, 16 },
		{ "AdjustCityProperty::Apply: set", 0x8b4239, { 0x41, 0xff, 0x91, 0x30, 0x01, 0x00, 0x00 }, 7 },
		{ "AdjustCityProperty::Apply", ADJUST_CITY_PROPERTY_APPLY_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x48, 0x8b, 0xf2, 0x48, 0x8d, 0x8a }, 24 },
		{ "AdjustCityProperty::Remove", ADJUST_CITY_PROPERTY_REMOVE_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x48, 0x8b, 0xf2, 0x48, 0x8d, 0x8a }, 24 },
		{ "GetCity(CityID)", GET_CITY_OFFSET, { 0x40, 0x57, 0x48, 0x83, 0xec, 0x20, 0x0f, 0xb7, 0x01, 0x66, 0x83, 0xf8 }, 12 },
		{ "City::Citizens::ReallocateCitizens", REALLOCATE_CITIZENS_OFFSET, { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48 }, 16 },
		// ICity::lSetProperty after a change: Reporting::Event::Send(0x30fbf8d2, &{owner +0xd8, 2, id +0xa8}, key)
		{ "Reporting::Event::Send", REPORT_EVENT_SEND_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x4c, 0x24, 0x08, 0x57, 0x48, 0x81, 0xec, 0xa0, 0x00 }, 16 },
		{ "ICity::lSetProperty: owner +0xd8", 0x6c95fd, { 0x0f, 0xb7, 0x87, 0xd8, 0x00, 0x00, 0x00 }, 7 },
		{ "ICity::lSetProperty: id +0xa8", 0x6c9613, { 0x8b, 0x87, 0xa8, 0x00, 0x00, 0x00 }, 6 },
		{ "ICity::lSetProperty: event type", 0x6c9619, { 0x48, 0xc7, 0x44, 0x24, 0x20, 0xd2, 0xf8, 0xfb, 0x30 }, 9 },
		{ "ICity::lSetProperty: -> Send", 0x6c9630, { 0xe8, 0x5b, 0x22, 0xf3, 0xff }, 5 },
		// ManageCitizen::Start after ForceWorkerToPlot: Send(0x15d11e18c2f385c0, &{owner, 2, id}, &city->plot +0xc0)
		{ "Reporting::Event::Send(plot)", REPORT_EVENT_SEND_PLOT_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x4c, 0x24, 0x08, 0x57, 0x48, 0x81, 0xec, 0xa0, 0x00 }, 16 },
		{ "ManageCitizen::Start: owner +0xd8", 0x67fe4c, { 0x41, 0x0f, 0xb7, 0x86, 0xd8, 0x00, 0x00, 0x00 }, 8 },
		{ "ManageCitizen::Start: city plot +0xc0", 0x67fe54, { 0x4d, 0x8d, 0x86, 0xc0, 0x00, 0x00, 0x00 }, 7 },
		{ "ManageCitizen::Start: event type low", 0x67fe6a, { 0xc7, 0x44, 0x24, 0x20, 0xc0, 0x85, 0xf3, 0xc2 }, 8 },
		{ "ManageCitizen::Start: event type high", 0x67fe7e, { 0xc7, 0x44, 0x24, 0x24, 0x18, 0x1e, 0xd1, 0x15 }, 8 },
		{ "ManageCitizen::Start: -> Send(plot)", 0x67fe8f, { 0xe8, 0x7c, 0xb2, 0xf7, 0xff }, 5 },
		{ "FAutoVariable<VariantMap>::edit", PROPERTY_EDIT_OFFSET, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0x41, 0x08, 0x48, 0x8b, 0xd9, 0x48, 0x85, 0xc0 }, 16 },
		// citizens
		{ "RemoveAllCitizenWorkers", REMOVE_ALL_WORKERS_OFFSET, { 0x40, 0x53, 0x57, 0x41, 0x54, 0x41, 0x55, 0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x01, 0x48, 0x8b }, 16 },
		{ "RemoveAllCitizenWorkers: free = population", 0x11077e, { 0x8b, 0x88, 0x68, 0x02, 0x00, 0x00, 0x89, 0x4b, 0x68 }, 9 },
		{ "TryAssignAllWorkers", TRY_ASSIGN_WORKERS_OFFSET, { 0x48, 0x8b, 0xc4, 0x88, 0x50, 0x10, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56 }, 16 },
		{ "TryAssignAllWorkers: food index", 0x110d4a, { 0x3b, 0x35, 0xf8, 0xe8, 0xa7, 0x00 }, 6 },
		{ "TryAssignAllWorkers: disfavored +0x50", 0x110d5c, { 0x49, 0x8b, 0x4f, 0x50 }, 4 },
		{ "TryAssignAllWorkers: gold index", 0x110d66, { 0x3b, 0x35, 0xe4, 0xe8, 0xa7, 0x00 }, 6 },
		{ "TryAssignAllWorkers: favored +0x38", 0x110d70, { 0x49, 0x8b, 0x47, 0x38 }, 4 },
		// knapsack call: flag 0 at [rsp+0x20], r9d food, r8d = [r15+0x68], rdx = end, then the 8 replaced bytes (mov rcx,rsi; call knapsack)
		{ "TryAssignAllWorkers: knapsack call", 0x110f7e, { 0x45, 0x33, 0xc0, 0x44, 0x8b, 0x4c, 0x24, 0x50, 0x41, 0x8b, 0xc6, 0x44, 0x88, 0x44, 0x24, 0x20, 0x45, 0x8b, 0x47, 0x68,
			0x48, 0x8d, 0x0c, 0x40, 0x48, 0x8d, 0x14, 0xce, 0x48, 0x8b, 0xce, 0xe8, 0xae, 0xe0, 0xff, 0xff }, 36 },
		{ "TryAssignAllWorkers: after knapsack", KNAPSACK_RESUME, { 0x4c, 0x8b, 0xe8 }, 3 },
		{ "bounded_knapsack<SimpleScoring>", KNAPSACK_OFFSET, { 0x4c, 0x8b, 0xdc, 0x45, 0x89, 0x4b, 0x20, 0x49, 0x89, 0x53, 0x10, 0x49, 0x89, 0x4b, 0x08, 0x55 }, 16 },
		{ "City::GetYieldFromTiles", GET_YIELD_FROM_TILES_OFFSET, { 0x44, 0x89, 0x44, 0x24, 0x18, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48 }, 16 },
		{ "City::GetYieldFromTiles: citizens +0xc28", 0x1337d2, { 0x48, 0x8d, 0x8d, 0x28, 0x0c, 0x00, 0x00 }, 7 },
		// great person points: end of the building loop (add ebp,ecx; mov r15d,[rsp+0xc0] = class; ...), then the 7 replaced bytes
		// (mov eax,[r13+0x1500] = the city's GPP %), then test ebp,ebp
		{ "CalculatePointsPerTurn: city points", 0x2e2a6e, { 0x03, 0xe9, 0x44, 0x8b, 0xbc, 0x24, 0xc0, 0x00, 0x00, 0x00, 0x48, 0x83, 0xc3, 0x08, 0x48, 0x3b, 0xdf, 0x75, 0x9f,
			0x41, 0x8b, 0x85, 0x00, 0x15, 0x00, 0x00, 0x85, 0xed }, 28 },
		{ "CalculatePointsPerTurn: city +0x1948 cache", 0x2e2acc, { 0x49, 0x83, 0xbd, 0x48, 0x19, 0x00, 0x00, 0x00 }, 8 },
	};

	// ---- properties -------------------------------------------------------------------------------------------------------
	static int ReadProperty(void* city, uint32_t key) {
		void* map = (char*)city + CITY_PROPERTIES + 0x10;
		const uint8_t* v = ((VariantGetFn)(*(void***)map)[0x60 / 8])(map, key);
		if (v == nullptr) return 0;
		const uint16_t type = *(const uint16_t*)v;
		const uint32_t flags = *(const uint32_t*)(v + 0x14);
		const bool isNumber = ((flags >> 29) & 1) == 0 && type <= 0x15 && ((0x30002eu >> type) & 1) != 0;
		return isNumber ? *(const int*)(v + 8) : 0;
	}

	// Writes like Lua City:SetProperty: edit, set, then the "property changed" report that updates the UI's cached copy of the city.
	static void WriteProperty(void* city, uint32_t key, int value) {
		if (ReadProperty(city, key) == value) return;
		void* map = PropertyEdit((char*)city + CITY_PROPERTIES);
		if (map == nullptr) return;
		((VariantSetIntFn)(*(void***)map)[0x130 / 8])(map, key, value);
		const CityID id = { *(int16_t*)((char*)city + CITY_OWNER), 2, *(uint32_t*)((char*)city + CITY_ID) };
		ReportEventSend(CITY_PROPERTY_CHANGED_EVENT, &id, (int)key);
	}

	static void* CityOf(void* citizens) { return *(void**)citizens; }
	static int& Free(void* citizens) { return *(int*)((char*)citizens + CITIZENS_FREE); }
	static int& Assigned(void* citizens) { return *(int*)((char*)citizens + CITIZENS_ASSIGNED); }
	static int Slots(void* city, const SpecialistType& t) { const int s = ReadProperty(city, t.slotsKey); return s < 0 ? 0 : s; }

	// ---- yields -----------------------------------------------------------------------------------------------------------
	// Yield indices are set when the database loads (all 0 before): resolve the rows once the six base yields give six different indices.
	static bool YieldsReady() {
		if (gYieldsReady) return true;
		bool seen[MAX_YIELDS] = {};
		for (const char* name : kBaseYields) {
			const int index = NameToYieldType(MakeHash(name));
			if (index < 0 || index >= MAX_YIELDS || seen[index]) return false;
			seen[index] = true;
		}
		for (int t = 0; t < gNumTypes; t++) {
			SpecialistType& type = gTypes[t];
			memset(type.yield, 0, sizeof(type.yield));
			memset(type.yieldKey, 0, sizeof(type.yieldKey));
			for (int r = 0; r < type.numRows; r++) {
				const int index = NameToYieldType(type.rows[r].hash);
				if (index >= 0 && index < MAX_YIELDS) type.yield[index] += type.rows[r].change;
			}
			for (int i = 0; i < NUM_BASE_YIELDS; i++) type.yieldKey[NameToYieldType(MakeHash(kBaseYields[i]))] = type.yieldKeyByName[i];
		}
		gYieldsReady = true;
		return true;
	}

	// Yield y of one specialist of type t in this city: the table plus the city's DEVCE_SPECIALIST_YIELD_<Type>_<Yield> (needs YieldsReady).
	static int SpecialistYield(void* city, int t, int y) {
		const SpecialistType& type = gTypes[t];
		return type.yield[y] + (type.yieldKey[y] != 0 ? ReadProperty(city, type.yieldKey[y]) : 0);
	}

	// The candidate a citizen in this type's slot would be, scored as TryAssignAllWorkers scores a tile.
	static Item Candidate(void* citizens, int t) {
		Item item = { &gMarker[t], 0, 0, 0, 0 };
		void* city = CityOf(citizens);
		const int food = *(const int*)(Runtime::GameCoreAddress + YIELD_FOOD_INDEX), gold = *(const int*)(Runtime::GameCoreAddress + YIELD_GOLD_INDEX);
		const char* favored = *(char**)((char*)citizens + CITIZENS_FAVORED);
		const char* disfavored = *(char**)((char*)citizens + CITIZENS_DISFAVORED);
		for (int y = 0; y < NUM_BASE_YIELDS; y++) {
			const int v = SpecialistYield(city, t, y);
			if (v == 0) continue;
			if (y == food) item.food += v;
			if (disfavored != nullptr && disfavored[y]) continue;
			const int score = y == gold ? v : 2 * v;
			if (favored != nullptr && favored[y]) item.priority += score; else item.score += score;
		}
		return item;
	}

	// ---- reallocation -----------------------------------------------------------------------------------------------------
	// From RemoveAllCitizenWorkers to the end of the TryAssignAllWorkers after it, the working counts of that city are kept here and
	// written once at the end, so a count that ends where it started is never written (each write also updates the UI cache).
	struct Pending { void* city; int count[MAX_TYPES]; };
	static thread_local Pending gPending = { nullptr, {} };

	static void FlushPending() {
		if (gPending.city == nullptr) return;
		void* city = gPending.city;
		gPending.city = nullptr;
		bool changed = false;
		for (int t = 0; t < gNumTypes; t++) {
			if (ReadProperty(city, gTypes[t].countKey) == gPending.count[t]) continue;
			WriteProperty(city, gTypes[t].countKey, gPending.count[t]);
			changed = true;
		}
		// The citizens changed: send what the game's citizen command sends, so the UI redraws the tiles and the city's yields
		// (a lock comes through a player operation, which raises no city event of its own).
		if (changed) {
			const CityID id = { *(int16_t*)((char*)city + CITY_OWNER), 2, *(uint32_t*)((char*)city + CITY_ID) };
			ReportEventSendPlot(CITY_CITIZENS_CHANGED_EVENT, &id, (char*)city + CITY_PLOT);
		}
	}

	// Working specialists of type t (the pending count during this city's reallocation).
	static int Working(void* city, int t) {
		return gPending.city == city ? gPending.count[t] : ReadProperty(city, gTypes[t].countKey);
	}

	static void __fastcall Hook_RemoveAll(void* citizens, bool send) {
		if (!gMode || gNumTypes == 0) { orig_RemoveAll(citizens, send); return; }
		void* city = CityOf(citizens);
		// Locked specialists give way before locked tiles: trim them so that population - locked tiles - locked specialists >= 0.
		int lockedTiles = 0;
		const int* locked = *(int**)((char*)citizens + CITIZENS_LOCKED);
		const unsigned n = *(unsigned*)((char*)citizens + CITIZENS_LOCKED_COUNT);
		for (unsigned i = 0; i < n; i++) if (locked[i] > 0) lockedTiles += locked[i];
		int room = *(int*)((char*)city + CITY_POPULATION) - lockedTiles;
		FlushPending();   // a reallocation left without its TryAssignAllWorkers
		orig_RemoveAll(citizens, send);
		gPending.city = city;
		// The working count starts again from the locked request; auto-assign adds to it.
		for (int t = 0; t < gNumTypes; t++) {
			const SpecialistType& type = gTypes[t];
			int keep = ReadProperty(city, type.lockedKey);
			if (keep < 0) keep = 0;
			const int slots = Slots(city, type);
			if (keep > slots) keep = slots;
			if (keep > room) keep = room < 0 ? 0 : room;
			if (keep > Free(citizens)) keep = Free(citizens);
			if (keep != ReadProperty(city, type.lockedKey) && ReadProperty(city, type.lockedKey) > 0) WriteProperty(city, type.lockedKey, keep);   // trimmed, as the game trims locked tiles
			gPending.count[t] = keep;
			room -= keep;
			Free(citizens) -= keep;
			Assigned(citizens) += keep;   // every working specialist counts as assigned (only locked tiles are kept apart)
		}
	}

	static void Place(void* citizens, int t, int count) {
		void* city = CityOf(citizens);
		if (gPending.city == city) gPending.count[t] += count;
		else WriteProperty(city, gTypes[t].countKey, ReadProperty(city, gTypes[t].countKey) + count);
		Free(citizens) -= count;
		Assigned(citizens) += count;
	}

	static int Open(void* city, int t) {
		const int open = Slots(city, gTypes[t]) - Working(city, t);
		return open < 0 ? 0 : open;
	}

	// Replaces the knapsack call in TryAssignAllWorkers (called from the jump stub; citizens = r15).
	static Item* __fastcall KnapsackWithSpecialists(Item* begin, Item* end, unsigned capacity, int foodNeeded, void* citizens) {
		if (!gMode || gNumTypes == 0 || capacity == 0 || !YieldsReady()) return Knapsack(begin, end, capacity, foodNeeded, false);
		void* city = CityOf(citizens);
		static thread_local std::vector<Item> items;
		items.assign(begin, end);
		const size_t tiles = items.size();
		for (int t = 0; t < gNumTypes; t++) {
			int open = Open(city, t);
			if (open > (int)capacity) open = (int)capacity;
			if (open <= 0) continue;
			const Item c = Candidate(citizens, t);
			for (int i = 0; i < open; i++) items.push_back(c);
		}
		if (items.size() == tiles) return Knapsack(begin, end, capacity, foodNeeded, false);
		Item* chosenEnd = Knapsack(items.data(), items.data() + items.size(), capacity, foodNeeded, false);
		int placed[MAX_TYPES] = {};
		Item* out = begin;
		for (Item* it = items.data(); it != chosenEnd; ++it) {
			const char* p = (const char*)it->plot;
			if (p >= gMarker && p < gMarker + MAX_TYPES) placed[p - gMarker]++;
			else *out++ = *it;   // tiles go back to the game's list, which the game then applies
		}
		for (int t = 0; t < gNumTypes; t++) if (placed[t] > 0) Place(citizens, t, placed[t]);
		return out;
	}

	// Free citizens can be at most population - assigned (auto tiles and every working specialist) - locked tiles. A safety net: growth
	// runs TryAssignAllWorkers without RemoveAllCitizenWorkers first, so a bad count would otherwise place a citizen the city does not have.
	static void ClampFree(void* citizens) {
		void* city = CityOf(citizens);
		int used = Assigned(citizens);
		const int* locked = *(int**)((char*)citizens + CITIZENS_LOCKED);
		const unsigned n = *(unsigned*)((char*)citizens + CITIZENS_LOCKED_COUNT);
		for (unsigned i = 0; i < n; i++) if (locked[i] > 0) used += locked[i];
		int expected = *(int*)((char*)city + CITY_POPULATION) - used;
		if (expected < 0) expected = 0;
		if (Free(citizens) > expected) Free(citizens) = expected;
	}

	// Fills open slots with the citizens still free (the game skips the knapsack when there are no tile candidates).
	static bool __fastcall Hook_TryAssign(void* citizens, bool send) {
		if (gMode && gNumTypes > 0) ClampFree(citizens);
		const bool r = orig_TryAssign(citizens, send);
		if (!gMode || gNumTypes == 0) return r;
		void* city = CityOf(citizens);
		while (Free(citizens) > 0 && YieldsReady()) {
			int best = -1; Item bestItem = {};
			for (int t = 0; t < gNumTypes; t++) {
				if (Open(city, t) <= 0) continue;
				const Item c = Candidate(citizens, t);
				if (c.priority + c.score + c.food <= 0) continue;
				if (best < 0 || c.priority > bestItem.priority || (c.priority == bestItem.priority && c.score > bestItem.score)) { best = t; bestItem = c; }
			}
			if (best < 0) break;
			int n = Open(city, best);
			if (n > Free(citizens)) n = Free(citizens);
			Place(citizens, best, n);
		}
		if (gPending.city == city) FlushPending();
		return r;
	}

	static int* __fastcall Hook_GetYieldFromTiles(void* city, int* out, int yield) {
		int* r = orig_GetYieldFromTiles(city, out, yield);
		if (!gMode || gNumTypes == 0 || yield < 0 || yield >= MAX_YIELDS || !YieldsReady()) return r;
		int sum = 0;
		for (int t = 0; t < gNumTypes; t++) {
			int n = Working(city, t);   // working specialists only (a locked request counts once a reallocation applied it)
			if (n <= 0) continue;
			const int slots = Slots(city, gTypes[t]);
			if (n > slots) n = slots;
			if (n > 0) sum += n * SpecialistYield(city, t, yield);
		}
		*r += sum * 0x100;   // FixedPoint<8>
		return r;
	}

	// ---- great person points ------------------------------------------------------------------------------------------------
	// Called from the stub in CalculatePointsPerTurn for each city: the points of its specialists for this class, FixedPoint<8>, as the
	// game adds District_CitizenGreatPersonPoints (PointsPerTurn x citizens x 0x100). The city's and player's % modifiers follow.
	static int __fastcall SpecialistGreatPersonPoints(void* city, int greatPersonClass) {
		if (!gMode || city == nullptr) return 0;
		int sum = 0;
		for (int t = 0; t < gNumTypes; t++) {
			const SpecialistType& type = gTypes[t];
			if (type.numGpp == 0) continue;
			int n = Working(city, t);
			if (n <= 0) continue;
			const int slots = Slots(city, type);
			if (n > slots) n = slots;
			for (int g = 0; g < type.numGpp; g++) if (type.gpp[g].greatPersonClass == greatPersonClass) sum += type.gpp[g].points * n * 0x100;
		}
		return sum;
	}

	// ---- slots lost ---------------------------------------------------------------------------------------------------------
	// After an EFFECT_ADJUST_CITY_PROPERTY Apply or Remove: when a slot key now holds fewer slots than the working specialists of its
	// type (a modifier removed, the city changed owner), reallocate the city at once, as City::ChangeNumCitizens does when a
	// population loss leaves too few citizens. Slots gained are filled at the city's next reallocation (growth, focus, turn).
	static void AfterSlotEffect(void* effect, void* city) {
		if (!gMode || city == nullptr || gPending.city != nullptr) return;
		const uint32_t key = *(const uint32_t*)((char*)effect + 8);
		int t = -1;
		for (int i = 0; i < gNumTypes; i++) if (gTypes[i].slotsKey == key) { t = i; break; }
		if (t < 0) return;
		const CityID id = { *(int16_t*)((char*)city + CITY_OWNER), 2, *(uint32_t*)((char*)city + CITY_ID) };
		if (GetCity(&id) != city) return;   // only a registered city
		if (ReadProperty(city, gTypes[t].countKey) <= Slots(city, gTypes[t])) return;
		Reallocate((char*)city + CITY_CITIZENS);   // our RemoveAll hook trims the locks and counts to the new slots
	}

	static bool __fastcall Hook_PropertyApply(void* effect, void* city) {
		const bool r = orig_PropertyApply(effect, city);
		AfterSlotEffect(effect, city);
		return r;
	}

	static bool __fastcall Hook_PropertyRemove(void* effect, void* city) {
		const bool r = orig_PropertyRemove(effect, city);
		AfterSlotEffect(effect, city);
		return r;
	}

	// ---- database ---------------------------------------------------------------------------------------------------------
	template <typename T> static T Slot(void* object, size_t offset) { return (T)(*(void***)object)[offset / 8]; }

	static void LoadTables(void* connection) {
		gMode = false; gYieldsReady = false; gNumTypes = 0;
		if (connection == nullptr) return;
		void* q = Slot<DbPrepareFn>(connection, 0x18)(connection, "SELECT Value FROM GlobalParameters WHERE Name = ? LIMIT 1", -1);
		if (q == nullptr) return;
		Slot<DbBindTextFn>(q, 0x90)(q, 1, MODE_PARAMETER, -1);
		const bool on = Slot<DbStepFn>(q, 0x20)(q) && Slot<DbIntFn>(q, 0x110)(q, 0) == 1;
		Slot<DbResetFn>(q, 0x38)(q);
		Slot<DbReleaseFn>(q, 0x08)(q);
		if (!on) { DevBridge::Log("[CitySpecialists] %s is not 1: normal specialists", MODE_PARAMETER); return; }

		q = Slot<DbPrepareFn>(connection, 0x18)(connection, "SELECT SpecialistType FROM DevCE_Specialists ORDER BY SortIndex, rowid", -1);
		if (q == nullptr) { DevBridge::Log("[CitySpecialists] table DevCE_Specialists missing: city-centre specialists off"); return; }
		while (gNumTypes < MAX_TYPES && Slot<DbStepFn>(q, 0x20)(q)) {
			const char* name = Slot<DbTextFn>(q, 0xf0)(q, 0);
			if (name == nullptr || strlen(name) >= sizeof(gTypes[0].name)) continue;
			SpecialistType& t = gTypes[gNumTypes++];
			memset(&t, 0, sizeof(t));
			strcpy_s(t.name, name);
			t.slotsKey = MakeHash((std::string(SLOTS_PREFIX) + name).c_str());
			t.countKey = MakeHash((std::string(COUNT_PREFIX) + name).c_str());
			t.lockedKey = MakeHash((std::string(LOCKED_PREFIX) + name).c_str());
			for (int i = 0; i < NUM_BASE_YIELDS; i++) t.yieldKeyByName[i] = MakeHash((std::string(YIELD_PREFIX) + name + "_" + kBaseYields[i]).c_str());
		}
		Slot<DbResetFn>(q, 0x38)(q);
		Slot<DbReleaseFn>(q, 0x08)(q);

		q = Slot<DbPrepareFn>(connection, 0x18)(connection, "SELECT SpecialistType, YieldType, YieldChange FROM DevCE_Specialist_Yields", -1);
		if (q != nullptr) {
			while (Slot<DbStepFn>(q, 0x20)(q)) {
				const char* type = Slot<DbTextFn>(q, 0xf0)(q, 0);
				const char* yield = Slot<DbTextFn>(q, 0xf0)(q, 1);
				const int change = Slot<DbIntFn>(q, 0x110)(q, 2);
				if (type == nullptr || yield == nullptr) continue;
				for (int t = 0; t < gNumTypes; t++) {
					if (strcmp(gTypes[t].name, type) != 0 || gTypes[t].numRows >= MAX_YIELDS) continue;
					gTypes[t].rows[gTypes[t].numRows++] = { MakeHash(yield), change };
				}
			}
			Slot<DbResetFn>(q, 0x38)(q);
			Slot<DbReleaseFn>(q, 0x08)(q);
		}
		// Great person class index = position in GreatPersonClasses by rowid, as the game builds its GreatPersonClass definitions.
		q = Slot<DbPrepareFn>(connection, 0x18)(connection,
			"SELECT s.SpecialistType, (SELECT count(*) FROM GreatPersonClasses g2 WHERE g2.rowid < g.rowid), s.PointsPerTurn "
			"FROM DevCE_Specialist_GreatPersonPoints s JOIN GreatPersonClasses g ON g.GreatPersonClassType = s.GreatPersonClassType", -1);
		if (q != nullptr) {
			while (Slot<DbStepFn>(q, 0x20)(q)) {
				const char* type = Slot<DbTextFn>(q, 0xf0)(q, 0);
				const int greatPersonClass = Slot<DbIntFn>(q, 0x110)(q, 1);
				const int points = Slot<DbIntFn>(q, 0x110)(q, 2);
				if (type == nullptr || points == 0) continue;
				for (int t = 0; t < gNumTypes; t++) {
					if (strcmp(gTypes[t].name, type) != 0 || gTypes[t].numGpp >= MAX_GPP) continue;
					gTypes[t].gpp[gTypes[t].numGpp++] = { greatPersonClass, points };
				}
			}
			Slot<DbResetFn>(q, 0x38)(q);
			Slot<DbReleaseFn>(q, 0x08)(q);
		}
		gMode = gNumTypes > 0;
		DevBridge::Log("[CitySpecialists] city-centre specialists %s: %d types", gMode ? "ON" : "off (no types)", gNumTypes);
		for (int t = 0; t < gNumTypes; t++) DevBridge::Log("[CitySpecialists]   %s: %d yield rows, %d great person point rows", gTypes[t].name, gTypes[t].numRows, gTypes[t].numGpp);
	}

	static void __fastcall Hook_GlobalParametersInitialize(void* params, void* connection) {
		orig_GlobalParametersInitialize(params, connection);
		__try { LoadTables(connection); }
		__except (EXCEPTION_EXECUTE_HANDLER) { gMode = false; gNumTypes = 0; DevBridge::Log("[CitySpecialists] reading the tables faulted: city-centre specialists off"); }
	}

	// ---- Lua (gameplay state) ---------------------------------------------------------------------------------------------
	// CitySpecialists.SetLocked(playerId, cityId, specialistType, count): sets the lock request and reallocates the city at once.
	// Returns the working count of that type afterwards, or -1 (mode off, unknown type or city).
	static int __cdecl lSetLocked(hks::lua_State* L) {
		const int player = hks::checkinteger(L, 1);
		const int cityId = hks::checkinteger(L, 2);
		const char* name = hks::checklstring(L, 3, nullptr);
		int count = hks::checkinteger(L, 4);
		int t = -1;
		if (gMode && name != nullptr) for (int i = 0; i < gNumTypes; i++) if (strcmp(gTypes[i].name, name) == 0) { t = i; break; }
		void* city = nullptr;
		if (t >= 0) { const CityID id = { (int16_t)player, 2, (uint32_t)cityId }; city = GetCity(&id); }
		if (city == nullptr) { hks::pushinteger(L, -1); return 1; }
		if (count < 0) count = 0;
		WriteProperty(city, gTypes[t].lockedKey, count);
		Reallocate((char*)city + CITY_CITIZENS);   // RemoveAllCitizenWorkers (our hook applies the request) + TryAssignAllWorkers
		hks::pushinteger(L, ReadProperty(city, gTypes[t].countKey));
		return 1;
	}

	static int __cdecl lIsEnabled(hks::lua_State* L) {
		hks::pushboolean(L, gOk && gMode);
		return 1;
	}

	int Register(hks::lua_State* L) {
		hks::createtable(L, 0, 2);
		PushLuaMethod(L, lSetLocked, "lSetLocked", -2, "SetLocked");
		PushLuaMethod(L, lIsEnabled, "lIsEnabled", -2, "IsEnabled");
		hks::setfield(L, hks::LUA_GLOBAL, "CitySpecialists");
		return 0;
	}

	// ---- the knapsack jump ------------------------------------------------------------------------------------------------------
	// Executable memory within +-2 GB of the patch, for a rel32 jump (as LuaHooks' AllocNear).
	static void* AllocNear(uintptr_t target, size_t size) {
		SYSTEM_INFO si; GetSystemInfo(&si);
		const uintptr_t gran = si.dwAllocationGranularity;
		for (uintptr_t off = gran; off < 0x70000000; off += gran) {
			for (int dir = 0; dir < 2; dir++) {
				const uintptr_t cand = dir == 0 ? (target & ~(gran - 1)) - off : (target & ~(gran - 1)) + off;
				if (dir == 0 && cand > target) continue;
				if (cand < 0x10000) continue;
				void* p = VirtualAlloc((void*)cand, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
				if (p == nullptr) continue;
				const intptr_t dist = (intptr_t)(uintptr_t)p - (intptr_t)target;
				if (dist > 0x7FF00000 || dist < -0x7FF00000) { VirtualFree(p, 0, MEM_RELEASE); continue; }
				return p;
			}
		}
		return nullptr;
	}

	// Replaces `mov rcx,rsi; call knapsack` (8 bytes) with `jmp stub; nop x3`. The stub:
	//   mov rcx,rsi; sub rsp,0x30; mov [rsp+0x20],r15; mov rax,fn; call rax; add rsp,0x30; mov r11,resume; jmp r11
	// rdx, r8d, r9d are already the knapsack's arguments; rsp is 16-aligned at the site; the result stays in rax.
	static bool PatchKnapsackCall(uintptr_t b) {
		const uintptr_t site = b + KNAPSACK_PATCH, resume = b + KNAPSACK_RESUME;
		uint8_t* stub = (uint8_t*)AllocNear(site, 0x1000);
		if (stub == nullptr) return false;
		void* fn = (void*)&KnapsackWithSpecialists;
		uint8_t* p = stub;
		const uint8_t head[] = { 0x48, 0x8b, 0xce, 0x48, 0x83, 0xec, 0x30, 0x4c, 0x89, 0x7c, 0x24, 0x20 };
		memcpy(p, head, sizeof(head)); p += sizeof(head);
		*p++ = 0x48; *p++ = 0xb8; memcpy(p, &fn, 8); p += 8;        // mov rax, fn
		*p++ = 0xff; *p++ = 0xd0;                                  // call rax
		*p++ = 0x48; *p++ = 0x83; *p++ = 0xc4; *p++ = 0x30;        // add rsp, 0x30
		*p++ = 0x49; *p++ = 0xbb; memcpy(p, &resume, 8); p += 8;   // mov r11, resume
		*p++ = 0x41; *p++ = 0xff; *p++ = 0xe3;                     // jmp r11
		FlushInstructionCache(GetCurrentProcess(), stub, p - stub);

		const int64_t rel = (int64_t)(uintptr_t)stub - (int64_t)(site + 5);
		if (rel > INT32_MAX || rel < INT32_MIN) return false;
		const int32_t rel32 = (int32_t)rel;
		uint8_t jump[8] = { 0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90 };
		memcpy(jump + 1, &rel32, 4);
		DWORD old = 0;
		if (!VirtualProtect((void*)site, 8, PAGE_EXECUTE_READWRITE, &old)) return false;
		memcpy((void*)site, jump, 8);
		VirtualProtect((void*)site, 8, old, &old);
		FlushInstructionCache(GetCurrentProcess(), (void*)site, 8);
		return true;
	}

	// Replaces `mov eax,[r13+0x1500]` (7 bytes) in CalculatePointsPerTurn with `jmp stub; nop x2`. The stub:
	//   sub rsp,0x20; mov rcx,r13; mov edx,r15d; mov rax,fn; call rax; add rsp,0x20; add ebp,eax; mov eax,[r13+0x1500]; mov r11,resume; jmp r11
	// rsp is 16-aligned at the site (6 pushes + sub 0x78); no volatile register is live there (the next instruction sets the flags).
	static bool PatchGreatPersonPoints(uintptr_t b) {
		const uintptr_t site = b + GPP_PATCH, resume = b + GPP_RESUME;
		uint8_t* stub = (uint8_t*)AllocNear(site, 0x1000);
		if (stub == nullptr) return false;
		void* fn = (void*)&SpecialistGreatPersonPoints;
		uint8_t* p = stub;
		const uint8_t head[] = { 0x48, 0x83, 0xec, 0x20, 0x49, 0x8b, 0xcd, 0x41, 0x8b, 0xd7 };   // sub rsp,0x20; mov rcx,r13; mov edx,r15d
		memcpy(p, head, sizeof(head)); p += sizeof(head);
		*p++ = 0x48; *p++ = 0xb8; memcpy(p, &fn, 8); p += 8;                                     // mov rax, fn
		*p++ = 0xff; *p++ = 0xd0;                                                               // call rax
		const uint8_t tail[] = { 0x48, 0x83, 0xc4, 0x20, 0x03, 0xe8, 0x41, 0x8b, 0x85, 0x00, 0x15, 0x00, 0x00 };   // add rsp,0x20; add ebp,eax; mov eax,[r13+0x1500]
		memcpy(p, tail, sizeof(tail)); p += sizeof(tail);
		*p++ = 0x49; *p++ = 0xbb; memcpy(p, &resume, 8); p += 8;                                // mov r11, resume
		*p++ = 0x41; *p++ = 0xff; *p++ = 0xe3;                                                  // jmp r11
		FlushInstructionCache(GetCurrentProcess(), stub, p - stub);

		const int64_t rel = (int64_t)(uintptr_t)stub - (int64_t)(site + 5);
		if (rel > INT32_MAX || rel < INT32_MIN) return false;
		const int32_t rel32 = (int32_t)rel;
		uint8_t jump[7] = { 0xe9, 0, 0, 0, 0, 0x90, 0x90 };
		memcpy(jump + 1, &rel32, 4);
		DWORD old = 0;
		if (!VirtualProtect((void*)site, 7, PAGE_EXECUTE_READWRITE, &old)) return false;
		memcpy((void*)site, jump, 7);
		VirtualProtect((void*)site, 7, old, &old);
		FlushInstructionCache(GetCurrentProcess(), (void*)site, 7);
		return true;
	}

	template <typename T> static bool Hook(uintptr_t rva, void* detour, T* original) {
		LPVOID target = (LPVOID)(Runtime::GameCoreAddress + rva);
		if (MH_CreateHook(target, detour, (LPVOID*)original) != MH_OK) return false;
		return MH_EnableHook(target) == MH_OK;
	}

	// ---- setup -------------------------------------------------------------------------------------------------------------
	void Create() {
		if (gOk) return;   // the hooks stay installed when Dev CE starts again in the same process
		const uintptr_t b = Runtime::GameCoreAddress;
		for (const Check& c : kChecks) {
			if (memcmp((void*)(b + c.rva), c.bytes, c.n) != 0) {
				DevBridge::Log("[CitySpecialists] %s has unexpected bytes at 0x%llx: city-centre specialists not available", c.name, (unsigned long long)c.rva);
				return;
			}
		}
		MakeHash = (MakeHashFn)(b + MAKE_HASH_OFFSET);
		NameToYieldType = (NameToYieldTypeFn)(b + NAME_TO_YIELD_TYPE_OFFSET);
		PropertyEdit = (PropertyEditFn)(b + PROPERTY_EDIT_OFFSET);
		GetCity = (GetCityFn)(b + GET_CITY_OFFSET);
		Reallocate = (ReallocateFn)(b + REALLOCATE_CITIZENS_OFFSET);
		ReportEventSend = (ReportEventSendFn)(b + REPORT_EVENT_SEND_OFFSET);
		ReportEventSendPlot = (ReportEventSendPlotFn)(b + REPORT_EVENT_SEND_PLOT_OFFSET);
		Knapsack = (KnapsackFn)(b + KNAPSACK_OFFSET);
		gOk = true;
		const bool ok = Hook(GLOBAL_PARAMETERS_INITIALIZE_OFFSET, (void*)&Hook_GlobalParametersInitialize, &orig_GlobalParametersInitialize)
			&& Hook(REMOVE_ALL_WORKERS_OFFSET, (void*)&Hook_RemoveAll, &orig_RemoveAll)
			&& Hook(TRY_ASSIGN_WORKERS_OFFSET, (void*)&Hook_TryAssign, &orig_TryAssign)
			&& Hook(GET_YIELD_FROM_TILES_OFFSET, (void*)&Hook_GetYieldFromTiles, &orig_GetYieldFromTiles)
			&& Hook(ADJUST_CITY_PROPERTY_APPLY_OFFSET, (void*)&Hook_PropertyApply, &orig_PropertyApply)
			&& Hook(ADJUST_CITY_PROPERTY_REMOVE_OFFSET, (void*)&Hook_PropertyRemove, &orig_PropertyRemove)
			&& PatchKnapsackCall(b)
			&& PatchGreatPersonPoints(b);
		DevBridge::Log("[CitySpecialists] hooks %s (mode is read from %s when the database loads)", ok ? "installed" : "FAILED", MODE_PARAMETER);
	}
}
