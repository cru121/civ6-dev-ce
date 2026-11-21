// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3.0 (see LICENSE.txt). Not endorsed by Wild-W, Firaxis or 2K.
#include "DistrictCitizens.h"
#include "Runtime.h"
#include "DevBridge.h"
#include <Windows.h>
#include <cstring>
#include <string>

namespace DistrictCitizens {
	struct CityID { int16_t owner; int16_t kind; uint32_t id; };            // kind 2 = city
	typedef uint32_t(__fastcall* MakeHashFn)(const char* s);
	typedef int(__fastcall* NameToYieldTypeFn)(uint32_t hash);
	typedef const uint8_t* (__fastcall* VariantGetFn)(void* map, uint32_t key);
	typedef void* (__fastcall* GetCityFn)(const CityID* id);
	typedef void* (__fastcall* GetDistrictAtFn)(void* districts, const int* coord);
	typedef void* (__fastcall* PlotCityAttributesFn)(void* plot);

	constexpr int NUM_YIELDS = 6;
	static const char* const kYieldNames[NUM_YIELDS] = { "YIELD_FOOD", "YIELD_PRODUCTION", "YIELD_GOLD", "YIELD_SCIENCE", "YIELD_CULTURE", "YIELD_FAITH" };

	static MakeHashFn MakeHash;
	static NameToYieldTypeFn NameToYieldType;
	static GetCityFn GetCity;
	static GetDistrictAtFn GetDistrictAt;
	static uint32_t gYieldHash[NUM_YIELDS];      // MakeHash(name), in kYieldNames order
	static uint32_t gYieldNameKey[NUM_YIELDS];   // MakeHash("DEVCE_CITIZEN_" + name), in kYieldNames order
	static uint32_t gYieldKey[NUM_YIELDS];       // the same keys by yield index (see YieldKeysReady)
	static volatile bool gYieldKeysReady = false;
	static uint32_t gSlotsKey = 0;
	static bool gOk = false, gYieldOk = false, gSlotsOk = false;

	struct Check { const char* name; uintptr_t rva; uint8_t bytes[16]; size_t n; };
	// Both readers (a mismatch switches off both)
	static const Check kCommonChecks[] = {
		{ "Utilities::MakeHash", MAKE_HASH_OFFSET, { 0x48, 0x8b, 0xd1, 0x41, 0x83, 0xc8, 0xff, 0x48 }, 8 },
		// AdjustDistrictProperty::Apply: properties at district +0x368 (map +0x10), and the number test the readers repeat
		{ "AdjustDistrictProperty::Apply district +0x368", 0x8dbb7d, { 0x48, 0x8d, 0x8a, 0x68, 0x03, 0x00, 0x00 }, 7 },
		{ "AdjustDistrictProperty::Apply number test", 0x8dbb99, { 0x8b, 0x50, 0x14, 0xc1, 0xea, 0x1d, 0xf6, 0xc2, 0x01, 0x75, 0x18, 0x0f, 0xb7, 0x08, 0x66, 0x83 }, 16 },
		{ "FAutoVariable<VariantMap>::edit", 0x72a920, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0x41, 0x08, 0x48, 0x8b, 0xd9, 0x48, 0x85, 0xc0 }, 16 },
		{ "City::Districts::GetDistrictAt", GET_DISTRICT_AT_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89 }, 12 },
	};
	// The yield reader in Rules::Yields::GetYield
	static const Check kYieldChecks[] = {
		{ "Definition::Utility::NameToYieldType", NAME_TO_YIELD_TYPE_OFFSET, { 0x81, 0xf9, 0xec, 0x57, 0xf0, 0x24, 0x7f, 0x36, 0x74, 0x2d, 0x81, 0xf9 }, 12 },
		// frame: 7 pushes + sub rsp,0xa0 (rsp 16-aligned at the patch); r12d = yield, r14 = plot
		{ "GetYield: pushes", GET_YIELD_OFFSET + 5, { 0x55, 0x53, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 }, 11 },
		{ "GetYield: frame", GET_YIELD_OFFSET + 0x10, { 0x48, 0x8d, 0x6c, 0x24, 0xe1, 0x48, 0x81, 0xec, 0xa0, 0x00, 0x00, 0x00 }, 12 },
		{ "GetYield: r12d = yield, r14 = plot", 0x37ec7f, { 0x45, 0x8b, 0xe0, 0x4c, 0x8b, 0xf2 }, 6 },
		// district definition's citizen yields (+0x160 / +0x168) summed into esi
		{ "GetYield: District_CitizenYieldChanges", 0x37edcb, { 0x48, 0x8b, 0xbb, 0x68, 0x01, 0x00, 0x00, 0x48, 0x8b, 0x9b, 0x60, 0x01, 0x00, 0x00 }, 14 },
		{ "GetYield: -> NameToYieldType", 0x37edeb, { 0xe8, 0x80, 0x9f, 0x45, 0x00 }, 5 },
		{ "GetYield: esi += YieldChange", 0x37edf0, { 0x44, 0x3b, 0xe0, 0x75, 0x06, 0x48, 0x8b, 0x03, 0x03, 0x70, 0x1c }, 11 },
		// patch site (replayed: mov rax,[r14]; mov rcx,r14), then CityAttributes city id +0xc / owner +0x1c and GetCity
		{ "GetYield: patch site", GET_YIELD_PATCH, { 0x49, 0x8b, 0x06, 0x49, 0x8b, 0xce, 0xff, 0x50, 0x30, 0x8b, 0x48, 0x0c, 0x0f, 0xbe, 0x40, 0x1c }, 16 },
		{ "GetYield: -> GetCity(CityID)", 0x37ee28, { 0xe8, 0x13, 0x40, 0xcc, 0xff }, 5 },
		{ "GetCity(CityID)", GET_CITY_OFFSET, { 0x40, 0x57, 0x48, 0x83, 0xec, 0x20, 0x0f, 0xb7, 0x01, 0x66, 0x83, 0xf8 }, 12 },
	};
	// The slots reader in City::Citizens::GetPlotWorkerCapacity
	static const Check kSlotsChecks[] = {
		// frame: 3 pushes + sub rsp,0x80 (rsp 16-aligned at the patch); rsi = plot, r15 = citizens
		{ "GetPlotWorkerCapacity: frame, rsi = plot, r15 = citizens", WORKER_CAPACITY_OFFSET, { 0x40, 0x53, 0x56, 0x41, 0x57, 0x48, 0x81, 0xec, 0x80, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xf2, 0x4c }, 16 },
		// citizens -> city, +0xb58 Districts, GetDistrictAt
		{ "GetPlotWorkerCapacity: city +0xb58", 0x1101cf, { 0x48, 0x81, 0xc1, 0x58, 0x0b, 0x00, 0x00 }, 7 },
		{ "GetPlotWorkerCapacity: -> GetDistrictAt", 0x1101e1, { 0xe8, 0x3a, 0xe3, 0x00, 0x00 }, 5 },
		// ebp = Districts.CitizenSlots (definition +0x54)
		{ "GetPlotWorkerCapacity: district CitizenSlots", 0x11022f, { 0x40, 0x38, 0x78, 0x50, 0x74, 0x03, 0x8b, 0x68, 0x54 }, 9 },
		// patch site (replayed: mov rbx,[r15]; mov rcx,rsi)
		{ "GetPlotWorkerCapacity: patch site", WORKER_CAPACITY_PATCH, { 0x49, 0x8b, 0x1f, 0x48, 0x8b, 0xce, 0x48, 0x89, 0x7c, 0x24, 0x5c }, 11 },
	};

	static bool CheckAll(const Check* checks, size_t count, const char* what) {
		const uintptr_t b = Runtime::GameCoreAddress;
		for (size_t i = 0; i < count; i++) {
			const Check& c = checks[i];
			if (memcmp((void*)(b + c.rva), c.bytes, c.n) != 0) {
				DevBridge::Log("[DistrictCitizens] %s has unexpected bytes at 0x%llx: %s off", c.name, (unsigned long long)c.rva, what);
				return false;
			}
		}
		return true;
	}

	// Yield indices are not fixed: NameToYieldType returns globals that IndexSerializer::Initialize fills from the Yields table
	// when the database loads (they are all 0 when Dev CE starts). So the index -> key table is built on first use, once the six
	// names give six different indices.
	static bool YieldKeysReady() {
		if (gYieldKeysReady) return true;
		uint32_t keys[NUM_YIELDS] = {};
		bool seen[NUM_YIELDS] = {};
		for (int i = 0; i < NUM_YIELDS; i++) {
			const int index = NameToYieldType(gYieldHash[i]);
			if (index < 0 || index >= NUM_YIELDS || seen[index]) return false;
			seen[index] = true;
			keys[index] = gYieldNameKey[i];
		}
		memcpy(gYieldKey, keys, sizeof(keys));
		gYieldKeysReady = true;
		return true;
	}

	// ---- reading the district's value -------------------------------------------------------------------------------------
	static int DistrictProperty(void* district, uint32_t key) {
		if (district == nullptr) return 0;
		void* map = (char*)district + DISTRICT_PROPERTIES;
		const uint8_t* v = ((VariantGetFn)(*(void***)map)[0x60 / 8])(map, key);
		if (v == nullptr) return 0;
		const uint16_t type = *(const uint16_t*)v;
		const uint32_t flags = *(const uint32_t*)(v + 0x14);
		const bool isNumber = ((flags >> 29) & 1) == 0 && type <= 0x15 && ((0x30002eu >> type) & 1) != 0;
		return isNumber ? *(const int*)(v + 8) : 0;
	}

	static void* DistrictAtPlot(void* city, void* plot) {
		if (city == nullptr) return nullptr;
		const int coord[2] = { *(const int16_t*)((char*)plot + 0x28), *(const int16_t*)((char*)plot + 0x2a) };
		return GetDistrictAt((char*)city + CITY_DISTRICTS, coord);
	}

	// Called from GetYield's district branch with the District_CitizenYieldChanges sum; returns the new sum.
	static int __fastcall CitizenYield(void* plot, int yield, int sum) {
		if (yield < 0 || yield >= NUM_YIELDS || !YieldKeysReady()) return sum;
		__try {
			const char* attrs = (const char*)((PlotCityAttributesFn)(*(void***)plot)[0x30 / 8])(plot);
			const CityID id = { (int16_t)*(const int8_t*)(attrs + 0x1c), 2, *(const uint32_t*)(attrs + 0xc) };
			return sum + DistrictProperty(DistrictAtPlot(GetCity(&id), plot), gYieldKey[yield]);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return sum;
		}
	}

	// Called from GetPlotWorkerCapacity with Districts.CitizenSlots; returns the new count (never below 0).
	static int __fastcall CitizenSlots(void* citizens, void* plot, int slots) {
		__try {
			const int total = slots + DistrictProperty(DistrictAtPlot(*(void**)citizens, plot), gSlotsKey);
			return total < 0 ? 0 : total;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return slots;
		}
	}

	// ---- the two jumps --------------------------------------------------------------------------------------------------------
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

	// Replaces the 6 bytes at `site` (two register moves) with `jmp stub; nop`. The stub (50 bytes):
	//   sub rsp,0x20; <3 moves: arguments>; mov rax,fn; call rax; <mov result>; add rsp,0x20; <the 2 replaced moves>;
	//   mov r11,resume; jmp r11
	// rsp is 16-aligned at both sites (checked frames), so the call keeps the ABI alignment.
	static bool PatchSite(uintptr_t site, uintptr_t resume, const uint8_t args[9], const uint8_t result[2], void* fn) {
		uint8_t* stub = (uint8_t*)AllocNear(site, 0x1000);
		if (stub == nullptr) return false;
		uint8_t* p = stub;
		const uint8_t subRsp[] = { 0x48, 0x83, 0xec, 0x20 }, addRsp[] = { 0x48, 0x83, 0xc4, 0x20 };
		memcpy(p, subRsp, 4); p += 4;
		memcpy(p, args, 9); p += 9;
		*p++ = 0x48; *p++ = 0xb8; memcpy(p, &fn, 8); p += 8;     // mov rax, fn
		*p++ = 0xff; *p++ = 0xd0;                               // call rax
		memcpy(p, result, 2); p += 2;
		memcpy(p, addRsp, 4); p += 4;
		memcpy(p, (const void*)site, 6); p += 6;                // the replaced instructions
		*p++ = 0x49; *p++ = 0xbb; memcpy(p, &resume, 8); p += 8; // mov r11, resume
		*p++ = 0x41; *p++ = 0xff; *p++ = 0xe3;                  // jmp r11
		FlushInstructionCache(GetCurrentProcess(), stub, p - stub);

		const int64_t rel = (int64_t)(uintptr_t)stub - (int64_t)(site + 5);
		if (rel > INT32_MAX || rel < INT32_MIN) return false;
		const int32_t rel32 = (int32_t)rel;
		uint8_t jump[6] = { 0xe9, 0, 0, 0, 0, 0x90 };
		memcpy(jump + 1, &rel32, 4);
		DWORD old = 0;
		if (!VirtualProtect((void*)site, 6, PAGE_EXECUTE_READWRITE, &old)) return false;
		memcpy((void*)site, jump, 6);
		VirtualProtect((void*)site, 6, old, &old);
		FlushInstructionCache(GetCurrentProcess(), (void*)site, 6);
		return true;
	}

	// ---- setup -------------------------------------------------------------------------------------------------------------
	void Create() {
		if (gOk) return;   // patches stay installed when Dev CE starts again in the same process
		const uintptr_t b = Runtime::GameCoreAddress;
		if (!CheckAll(kCommonChecks, sizeof(kCommonChecks) / sizeof(kCommonChecks[0]), "district citizen properties")) return;
		MakeHash = (MakeHashFn)(b + MAKE_HASH_OFFSET);
		NameToYieldType = (NameToYieldTypeFn)(b + NAME_TO_YIELD_TYPE_OFFSET);
		GetCity = (GetCityFn)(b + GET_CITY_OFFSET);
		GetDistrictAt = (GetDistrictAtFn)(b + GET_DISTRICT_AT_OFFSET);

		for (int i = 0; i < NUM_YIELDS; i++) {
			gYieldHash[i] = MakeHash(kYieldNames[i]);
			gYieldNameKey[i] = MakeHash((std::string(YIELD_PROPERTY_PREFIX) + kYieldNames[i]).c_str());
		}
		gSlotsKey = MakeHash(SLOTS_PROPERTY);
		gOk = true;

		// yield: mov rcx,r14 (plot); mov edx,r12d (yield); mov r8d,esi (sum) -> mov esi,eax
		static const uint8_t yieldArgs[9] = { 0x49, 0x8b, 0xce, 0x41, 0x8b, 0xd4, 0x44, 0x8b, 0xc6 }, yieldResult[2] = { 0x8b, 0xf0 };
		// slots: mov rcx,r15 (citizens); mov rdx,rsi (plot); mov r8d,ebp (slots) -> mov ebp,eax
		static const uint8_t slotsArgs[9] = { 0x49, 0x8b, 0xcf, 0x48, 0x8b, 0xd6, 0x44, 0x8b, 0xc5 }, slotsResult[2] = { 0x8b, 0xe8 };

		gYieldOk = CheckAll(kYieldChecks, sizeof(kYieldChecks) / sizeof(kYieldChecks[0]), "DEVCE_CITIZEN_<YieldType>")
			&& PatchSite(b + GET_YIELD_PATCH, b + GET_YIELD_RESUME, yieldArgs, yieldResult, (void*)&CitizenYield);
		gSlotsOk = CheckAll(kSlotsChecks, sizeof(kSlotsChecks) / sizeof(kSlotsChecks[0]), SLOTS_PROPERTY)
			&& PatchSite(b + WORKER_CAPACITY_PATCH, b + WORKER_CAPACITY_RESUME, slotsArgs, slotsResult, (void*)&CitizenSlots);
		DevBridge::Log("[DistrictCitizens] district properties DEVCE_CITIZEN_<YieldType> %s; %s %s",
			gYieldOk ? "read" : "NOT read", SLOTS_PROPERTY, gSlotsOk ? "read" : "NOT read");
	}
}
