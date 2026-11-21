// SPDX-License-Identifier: AGPL-3.0-only
#include "BuildingGates.h"
#include "Runtime.h"
#include "DevBridge.h"
#include <Windows.h>
#include <intrin.h>
#include <cstring>
#include <string>
#include <vector>

#pragma intrinsic(_ReturnAddress)

namespace BuildingGates {
	typedef void(__fastcall* BuildCollectionFn)(void* builder, void* collection);
	typedef bool(__fastcall* CanConstructFn)(void* queue, int building, bool exclusionTest, void* plot, void* results, bool a, bool b, bool c, bool d, bool e);
	typedef bool(__fastcall* CanConstructInnerFn)(void* queue, int building, void* results, bool a, bool b, bool c, bool d, bool e);
	typedef void(__fastcall* AddStringFn)(void* results, uint32_t key, const void* string);
	typedef void* (__fastcall* StringCtorFn)(void* string, const char* text);   // Localization::String::String(const char*)
	typedef void(__fastcall* StringDtorFn)(void* string);                        // Localization::String::~String()
	typedef uint32_t(__fastcall* MakeHashFn)(const char*);
	typedef const uint8_t* (__fastcall* VariantGetFn)(void* map, uint32_t key);
	// Database::IDatabaseConnection1 / IDatabaseQuery1 (as TechGates / CitySpecialists use them)
	typedef void* (__fastcall* DbPrepareFn)(void* connection, const char* sql, int length);   // +0x18
	typedef void(__fastcall* DbReleaseFn)(void* query);                                        // +0x08
	typedef bool(__fastcall* DbStepFn)(void* query);                                           // +0x20
	typedef void(__fastcall* DbResetFn)(void* query);                                          // +0x38
	typedef const char* (__fastcall* DbTextFn)(void* query, int column);                       // +0xf0
	typedef int(__fastcall* DbIntFn)(void* query, int column);                                 // +0x110

	static BuildCollectionFn orig_BuildConditions;
	static CanConstructFn orig_CanConstruct;
	static CanConstructInnerFn orig_CanConstructInner;
	static AddStringFn AddString;
	static StringCtorFn StringCtor;
	static StringDtorFn StringDtor;
	static MakeHashFn MakeHash;
	static uintptr_t gOuterReturn = 0;
	static bool gOk = false;

	struct Gate { uint32_t key; std::string reason; };
	static std::vector<std::vector<Gate>> gGates;   // per BuildingTypes index
	static volatile bool gAny = false;

	struct Check { const char* name; uintptr_t rva; uint8_t bytes[16]; size_t n; };
	static const Check kChecks[] = {
		{ "BuildCollection<BuildingCondition>", BUILD_CONDITIONS_OFFSET, { 0x40, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x89, 0x5c, 0x24, 0x30, 0x41, 0x83, 0xc8, 0xff, 0x48 }, 16 },
		{ "BuildCollection<BuildingCondition>: builder +0 = connection", BUILD_CONDITIONS_OFFSET + 0x17, { 0x48, 0x8b, 0x09, 0x48, 0x8b, 0xf2 }, 6 },
		{ "BuildQueue::CanConstruct (outer)", CAN_CONSTRUCT_OFFSET, { 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4c, 0x24, 0x08, 0x55, 0x53, 0x41, 0x55, 0x41, 0x56, 0x48 }, 16 },
		{ "CanConstruct (outer): queue +0x1e8 = city, exclusion test r8b", CAN_CONSTRUCT_OFFSET + 0x1e, { 0x48, 0x8b, 0x81, 0xe8, 0x01, 0x00, 0x00, 0x4d, 0x8b, 0xf1, 0x45, 0x0f, 0xb6, 0xe8 }, 14 },
		{ "CanConstruct (outer) calls the inner one", OUTER_CALLS_INNER_RETURN - 5, { 0xe8, 0x49, 0xef, 0xff, 0xff, 0x40, 0x38, 0x7d, 0xb2 }, 9 },
		{ "BuildQueue::CanConstruct (inner)", CAN_CONSTRUCT_INNER_OFFSET, { 0x4c, 0x89, 0x44, 0x24, 0x18, 0x89, 0x54, 0x24, 0x10, 0x53, 0x55, 0x56, 0x41, 0x55, 0x48, 0x81 }, 16 },
		{ "CanConstruct (inner): queue +0x1e8 = city", CAN_CONSTRUCT_INNER_OFFSET + 0x15, { 0x48, 0x8b, 0x81, 0xe8, 0x01, 0x00, 0x00 }, 7 },
		// AddProgress: CanConstruct(front building) false -> OnRemoveFrontEntry + PopFront (progress kept)
		{ "BuildQueue::AddProgress: per-turn check", 0xf5178, { 0xe8, 0x43, 0x10, 0x00, 0x00, 0x84, 0xc0, 0x75, 0x31, 0x49, 0x8b, 0xce, 0xe8, 0xe7, 0xc1, 0x00 }, 16 },
		{ "Reporting::AddString", ADD_STRING_OFFSET, { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x18, 0x55, 0x48, 0x81, 0xec, 0xb0, 0x00, 0x00, 0x00, 0x48 }, 16 },
		{ "Utilities::MakeHash", MAKE_HASH_OFFSET, { 0x48, 0x8b, 0xd1, 0x41, 0x83, 0xc8, 0xff, 0x48 }, 8 },
		{ "Lua City:GetProperty: properties +0x1a0", 0x6c929e, { 0x4c, 0x8d, 0x80, 0xa0, 0x01, 0x00, 0x00 }, 7 },
	};

	// ---- gates -----------------------------------------------------------------------------------------------------------
	static int CityProperty(void* city, uint32_t key) {
		void* map = (char*)city + CITY_PROPERTIES;
		const uint8_t* v = ((VariantGetFn)(*(void***)map)[0x60 / 8])(map, key);
		if (v == nullptr) return 0;
		const uint16_t type = *(const uint16_t*)v;
		const uint32_t flags = *(const uint32_t*)(v + 0x14);
		const bool isNumber = ((flags >> 29) & 1) == 0 && type <= 0x15 && ((0x30002eu >> type) & 1) != 0;
		return isNumber ? *(const int*)(v + 8) : 0;
	}

	// The first shut gate of this building in the queue's city, or null.
	static const Gate* Shut(void* queue, int building) {
		if (!gAny || queue == nullptr || building < 0 || building >= (int)gGates.size()) return nullptr;
		const std::vector<Gate>& gates = gGates[building];
		if (gates.empty()) return nullptr;
		void* city = *(void**)((char*)queue + QUEUE_CITY);
		if (city == nullptr) return nullptr;
		for (const Gate& g : gates) if (CityProperty(city, g.key) < 1) return &g;
		return nullptr;
	}

	static void AddReason(void* results, const char* key) {
		if (results == nullptr || StringCtor == nullptr || StringDtor == nullptr) return;
		alignas(16) char string[0x100];
		StringCtor(string, key);
		AddString(results, FAILURE_REASONS, string);
		StringDtor(string);
	}

	static bool __fastcall Hook_CanConstruct(void* queue, int building, bool exclusionTest, void* plot, void* results, bool a, bool b, bool c, bool d, bool e) {
		const bool r = orig_CanConstruct(queue, building, exclusionTest, plot, results, a, b, c, d, e);
		if (!r || exclusionTest) return r;   // listing test: a gated building stays listed (greyed out)
		const Gate* g = Shut(queue, building);
		if (g == nullptr) return r;
		AddReason(results, g->reason.c_str());
		return false;
	}

	static bool __fastcall Hook_CanConstructInner(void* queue, int building, void* results, bool a, bool b, bool c, bool d, bool e) {
		const bool r = orig_CanConstructInner(queue, building, results, a, b, c, d, e);
		if (!r || (uintptr_t)_ReturnAddress() == gOuterReturn) return r;   // the outer one decides for its callers
		return Shut(queue, building) == nullptr;
	}

	// ---- database ---------------------------------------------------------------------------------------------------------
	template <typename T> static T Slot(void* object, size_t offset) { return (T)(*(void***)object)[offset / 8]; }

	static void LoadTables(void* connection) {
		gAny = false;
		gGates.clear();
		if (connection == nullptr) return;
		void* q = Slot<DbPrepareFn>(connection, 0x18)(connection, "SELECT count(*) FROM Buildings", -1);
		if (q == nullptr) return;
		const int count = Slot<DbStepFn>(q, 0x20)(q) ? Slot<DbIntFn>(q, 0x110)(q, 0) : 0;
		Slot<DbResetFn>(q, 0x38)(q);
		Slot<DbReleaseFn>(q, 0x08)(q);
		gGates.assign(count > 0 ? count : 0, {});
		// building index = position in Buildings by rowid, as the game numbers them
		q = Slot<DbPrepareFn>(connection, 0x18)(connection,
			"SELECT (SELECT count(*) FROM Buildings x WHERE x.rowid < b.rowid), g.Property, g.Description "
			"FROM DevCE_BuildingGates g JOIN Buildings b ON b.BuildingType = g.BuildingType", -1);
		if (q == nullptr) {
			DevBridge::Log("[BuildingGates] no DevCE_BuildingGates table: building gates off");
			return;
		}
		int rows = 0;
		while (Slot<DbStepFn>(q, 0x20)(q)) {
			const int building = Slot<DbIntFn>(q, 0x110)(q, 0);
			const char* property = Slot<DbTextFn>(q, 0xf0)(q, 1);
			const char* reason = Slot<DbTextFn>(q, 0xf0)(q, 2);
			if (property == nullptr || property[0] == '\0' || building < 0 || building >= (int)gGates.size()) continue;
			gGates[building].push_back({ MakeHash(property), (reason != nullptr && reason[0] != '\0') ? reason : DEFAULT_REASON });
			rows++;
		}
		Slot<DbResetFn>(q, 0x38)(q);
		Slot<DbReleaseFn>(q, 0x08)(q);
		gAny = rows > 0;
		DevBridge::Log("[BuildingGates] %s: %d gate rows", gAny ? "ON" : "no gate rows, off", rows);
	}

	static void SafeLoadTables(void* connection) {
		__try { LoadTables(connection); }
		__except (EXCEPTION_EXECUTE_HANDLER) { gAny = false; DevBridge::Log("[BuildingGates] reading the table faulted: building gates off"); }
	}

	static void __fastcall Hook_BuildConditions(void* builder, void* collection) {
		orig_BuildConditions(builder, collection);
		SafeLoadTables(builder != nullptr ? *(void**)builder : nullptr);
	}

	// ---- setup ------------------------------------------------------------------------------------------------------------
	template <typename T> static bool Hook(uintptr_t rva, void* detour, T* original) {
		LPVOID target = (LPVOID)(Runtime::GameCoreAddress + rva);
		if (MH_CreateHook(target, detour, (LPVOID*)original) != MH_OK) return false;
		return MH_EnableHook(target) == MH_OK;
	}

	void Create() {
		if (gOk) return;   // the hooks stay installed when Dev CE starts again in the same process
		const uintptr_t b = Runtime::GameCoreAddress;
		for (const Check& c : kChecks) {
			if (memcmp((void*)(b + c.rva), c.bytes, c.n) != 0) {
				DevBridge::Log("[BuildingGates] %s has unexpected bytes at 0x%llx: building gates not available", c.name, (unsigned long long)c.rva);
				return;
			}
		}
		AddString = (AddStringFn)(b + ADD_STRING_OFFSET);
		MakeHash = (MakeHashFn)(b + MAKE_HASH_OFFSET);
		gOuterReturn = b + OUTER_CALLS_INNER_RETURN;
		if (HMODULE loc = GetModuleHandleA("Localization_FinalRelease.dll")) {
			StringCtor = (StringCtorFn)GetProcAddress(loc, "??0String@Localization@@QEAA@PEBD@Z");
			StringDtor = (StringDtorFn)GetProcAddress(loc, "??1String@Localization@@UEAA@XZ");
		}
		if (StringCtor == nullptr || StringDtor == nullptr)
			DevBridge::Log("[BuildingGates] Localization::String not found: gated buildings show no reason");
		gOk = true;
		const bool ok = Hook(BUILD_CONDITIONS_OFFSET, (void*)&Hook_BuildConditions, &orig_BuildConditions)
			&& Hook(CAN_CONSTRUCT_OFFSET, (void*)&Hook_CanConstruct, &orig_CanConstruct)
			&& Hook(CAN_CONSTRUCT_INNER_OFFSET, (void*)&Hook_CanConstructInner, &orig_CanConstructInner);
		DevBridge::Log("[BuildingGates] hooks %s (gates are read when the database loads)", ok ? "installed" : "FAILED");
	}
}
