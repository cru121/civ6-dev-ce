// SPDX-License-Identifier: AGPL-3.0-only
#include "CityRadius.h"
#include "Runtime.h"
#include "DevBridge.h"
#include <Windows.h>
#include <cstring>
#include <cstdlib>
#include <vector>

namespace CityRadius {
	typedef void* (__fastcall* CapabilitiesGetFn)();
	typedef bool(__fastcall* HasCapabilityFn)(void* manager, uint32_t hash);
	typedef void* (__fastcall* MapFn)();
	typedef void* (__fastcall* MapPlotFn)(void* map, const int* coord);
	typedef uint32_t(__fastcall* MakeHashFn)(const char*);
	typedef const uint8_t* (__fastcall* VariantGetFn)(void* map, uint32_t key);
	typedef void* (__fastcall* PlotFromIndexFn)(void* city, int index);
	typedef int(__fastcall* IndexCoreFn)(const int* center, const int* plot, unsigned width, unsigned height, bool wrapX, bool wrapY);
	typedef int(__fastcall* IndexCityFn)(void* city, const void* plot);
	typedef void(__fastcall* RemoveWorkersFn)(void* citizens, bool send);

	static CapabilitiesGetFn CapabilitiesGet;
	static HasCapabilityFn HasCapability;
	static MapFn GetMap, EditMap;
	static PlotFromIndexFn orig_EditPlot, orig_GetPlot;
	static IndexCoreFn orig_IndexCore;
	static IndexCityFn orig_IndexCityPlot, orig_IndexCityCoord;
	static RemoveWorkersFn orig_RemoveWorkers;
	static uint32_t gCapability = 0, gProperty = 0;
	static bool gOk = false;

	// index -> (dq, dr) offsets and ring, and (dq, dr) -> index, out to radius 5 (the game's own order for rings 0..4)
	static int gDQ[MAX_PLOTS], gDR[MAX_PLOTS], gRing[MAX_PLOTS];
	static int gIndex[11 * 11];

	struct Check { const char* name; uintptr_t rva; uint8_t bytes[16]; size_t n; };
	static const Check kChecks[] = {
		{ "ComputePlotsFromRadius", PLOTS_FROM_RADIUS_OFFSET, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x8b, 0xd9, 0x85, 0xc9, 0x75, 0x09, 0x8d, 0x41, 0x01, 0x48 }, 16 },
		// both: city +0xc0/+0xc4 = centre, then Edit/GetPlotFromCenterLocation
		{ "EditPlotFromCityIndex", EDIT_PLOT_FROM_INDEX_OFFSET, { 0x48, 0x83, 0xec, 0x38, 0x8b, 0x81, 0xc0, 0x00, 0x00, 0x00, 0x89, 0x44, 0x24, 0x20, 0x8b, 0x81 }, 16 },
		{ "GetPlotFromCityIndex", GET_PLOT_FROM_INDEX_OFFSET, { 0x48, 0x83, 0xec, 0x38, 0x8b, 0x81, 0xc0, 0x00, 0x00, 0x00, 0x89, 0x44, 0x24, 0x20, 0x8b, 0x81 }, 16 },
		{ "EditPlotFromCenterLocation: EditMap vtable +0x38", 0x44943, { 0xff, 0x50, 0x38 }, 3 },
		{ "GetPlotFromCenterLocation: GetMap vtable +0x50", 0x44cd3, { 0xff, 0x50, 0x50 }, 3 },
		{ "GetCityIndexFromPlot (core)", INDEX_CORE_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x44, 0x8b, 0x1a, 0x41, 0x8b, 0xd8, 0x44, 0x8b, 0x11, 0x45, 0x2b }, 16 },
		{ "GetCityIndexFromPlot (city, plot)", INDEX_CITY_PLOT_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x50, 0x48, 0x8b, 0xda, 0x48, 0x8b, 0xf9 }, 16 },
		{ "GetCityIndexFromPlot (city, coord)", INDEX_CITY_COORD_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x50, 0x48, 0x8b, 0xda, 0x48, 0x8b, 0xf9 }, 16 },
		{ "Citizens::RemoveAllCitizenWorkers", REMOVE_WORKERS_OFFSET, { 0x40, 0x53, 0x57, 0x41, 0x54, 0x41, 0x55, 0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x01, 0x48, 0x8b }, 16 },
		{ "Capabilities::Get", CAPABILITIES_GET_OFFSET, { 0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x0d }, 7 },
		{ "Capabilities::Manager::HasCapability", HAS_CAPABILITY_OFFSET, { 0x89, 0x54, 0x24, 0x10, 0x48, 0x83, 0xec, 0x48, 0x4c, 0x8b, 0x41, 0x08 }, 12 },
		{ "Globals::GetMap -> EditMap", GET_MAP_OFFSET, { 0xe9, 0x7b, 0x26, 0xff, 0xff }, 5 },
		{ "Globals::EditMap", EDIT_MAP_OFFSET, { 0x48, 0x8b, 0x05 }, 3 },
		{ "Utilities::MakeHash", MAKE_HASH_OFFSET, { 0x48, 0x8b, 0xd1, 0x41, 0x83, 0xc8, 0xff, 0x48 }, 8 },
		{ "Lua City:GetProperty: properties +0x1a0", 0x6c929e, { 0x4c, 0x8d, 0x80, 0xa0, 0x01, 0x00, 0x00 }, 7 },
		// mov rcx,[rip+params]; cmp eax,[rcx+0x124] (CITY_MAX_BUY_PLOT_RANGE); jg fail
		{ "GetPlotPurchaseCost: buy range test", BUY_JG - 13, { 0x48, 0x8b, 0x0d, 0x87, 0x98, 0xa6, 0x00, 0x3b, 0x81, 0x24, 0x01, 0x00, 0x00, 0x0f, 0x8f, 0xd3 }, 16 },
		{ "GetPlotPurchaseCost: Gold* in r14", 0x121189, { 0x4c, 0x8b, 0xf1 }, 3 },
		// mov rbp,[r14] (city) ... cmp eax,3; jg fail
		{ "CanSwapPlotFromCity: city in rbp", 0x11b6b7, { 0x49, 0x8b, 0x2e }, 3 },
		{ "CanSwapPlotFromCity: distance test", SWAP_JG - 3, { 0x83, 0xf8, 0x03, 0x0f, 0x8f, 0xde, 0x00, 0x00, 0x00 }, 9 },
		// cmp eax,3; jle within; Culture* spilled to [rsp+0x110] by the prologue (0xd0 frame + 7 pushes)
		{ "GetBuyablePlotList: distance test", CULTURE_CMP, { 0x83, 0xf8, 0x03, 0x0f, 0x8e, 0x26, 0xff, 0xff, 0xff }, 9 },
		{ "GetBuyablePlotList: prologue", 0x11bf25, { 0x48, 0x89, 0x4c, 0x24, 0x08, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 }, 16 },
		{ "GetBuyablePlotList: frame 0xd0", 0x11bf35, { 0x48, 0x81, 0xec, 0xd0, 0x00, 0x00, 0x00 }, 7 },
		{ "GetBuyablePlotList: Culture* at [rsp+0x110]", 0x11c381, { 0x48, 0x8b, 0x84, 0x24, 0x10, 0x01, 0x00, 0x00, 0x48, 0x8b, 0x08 }, 11 },
	};

	// ---- the rule --------------------------------------------------------------------------------------------------------
	static bool Enabled() {
		void* manager = CapabilitiesGet();
		return manager != nullptr && HasCapability(manager, gCapability);
	}

	static int CityProperty(void* city, uint32_t key) {
		void* map = (char*)city + CITY_PROPERTIES;
		const uint8_t* v = ((VariantGetFn)(*(void***)map)[0x60 / 8])(map, key);
		if (v == nullptr) return 0;
		const uint16_t type = *(const uint16_t*)v;
		const uint32_t flags = *(const uint32_t*)(v + 0x14);
		const bool isNumber = ((flags >> 29) & 1) == 0 && type <= 0x15 && ((0x30002eu >> type) & 1) != 0;
		return isNumber ? *(const int*)(v + 8) : 0;
	}

	static int ReadRadius(void* city) {
		if (city == nullptr) return BASE_RADIUS;
		int extra = CityProperty(city, gProperty);
		if (extra < 0) extra = 0;
		if (extra > MAX_RADIUS - BASE_RADIUS) extra = MAX_RADIUS - BASE_RADIUS;
		return BASE_RADIUS + extra;
	}

	// The game walks a city's plots by ascending index; the radius is read once per walk (a new city, or an index that is not
	// above the last one, starts a new walk).
	struct RadiusCache { void* city; int last; int radius; };
	static thread_local RadiusCache tCache = { nullptr, 0, BASE_RADIUS };

	static int WalkRadius(void* city, int index) {
		if (city != tCache.city || index <= tCache.last) {
			tCache.city = city;
			tCache.radius = ReadRadius(city);
		}
		tCache.last = index;
		return tCache.radius;
	}

	// ---- index -> plot ---------------------------------------------------------------------------------------------------
	static void* PlotFromIndex(void* city, int index, bool edit) {
		PlotFromIndexFn orig = edit ? orig_EditPlot : orig_GetPlot;
		if (index < BASE_PLOTS || city == nullptr || !Enabled()) return orig(city, index);
		if (index >= MAX_PLOTS || gRing[index] > WalkRadius(city, index)) return nullptr;
		if (index < GAME_PLOTS) return orig(city, index);   // the game's tables cover ring 4 (its bound is now 91)
		// ring 5, as Edit/GetPlotFromCenterLocation compute it
		const int cx = *(const int*)((char*)city + CITY_X);
		const int cy = *(const int*)((char*)city + CITY_Y);
		if (cx <= -9999 || cy <= -9999) return nullptr;
		char* map = (char*)(edit ? EditMap() : GetMap());
		if (map == nullptr) return nullptr;
		const int width = *(const int*)(map + MAP_WIDTH);
		const int height = *(const int*)(map + MAP_HEIGHT);
		int y = cy + gDR[index];
		int x = (cx - (cy >> 1)) + gDQ[index] + (y >> 1);   // >> on int is floor division by 2
		if (map[MAP_WRAP_X] && width > 0) x = ((x % width) + width) % width;
		if (map[MAP_WRAP_Y] && height > 0) y = ((y % height) + height) % height;
		if (x < 0 || x >= width || y < 0 || y >= height) return nullptr;
		const int coord[2] = { x, y };
		return ((MapPlotFn)(*(void***)map)[(edit ? MAP_EDIT_PLOT_SLOT : MAP_GET_PLOT_SLOT) / 8])(map, coord);
	}

	static void* __fastcall Hook_EditPlotFromCityIndex(void* city, int index) { return PlotFromIndex(city, index, true); }
	static void* __fastcall Hook_GetPlotFromCityIndex(void* city, int index) { return PlotFromIndex(city, index, false); }

	// Every "plots in radius 3" loop is ComputePlotsFromRadius(2) + 18.
	static unsigned __fastcall Hook_PlotsFromRadius(int radius) {
		if (radius == 2 && Enabled()) return MAX_PLOTS - 18;
		return radius < 0 ? 0u : (unsigned)(1 + 3 * radius * (radius + 1));
	}

	// ---- plot -> index ---------------------------------------------------------------------------------------------------
	static int Wrap(int d, unsigned size, bool wrap) {
		if (!wrap) return d;
		const int half = (int)(size >> 1);
		if (d > half) return d - (int)size;
		if (d < -half) return d + (int)size;
		return d;
	}

	static int __fastcall Hook_IndexCore(const int* center, const int* plot, unsigned width, unsigned height, bool wrapX, bool wrapY) {
		const int r = orig_IndexCore(center, plot, width, height, wrapX, wrapY);
		if (r != -1 || !Enabled()) return r;
		// the game's arithmetic, out to distance 5
		const int dx = Wrap(plot[0] - center[0], width, wrapX);
		const int dy = Wrap(plot[1] - center[1], height, wrapY);
		const int dq = Wrap(dx + (center[1] >> 1) - ((center[1] + dy) >> 1), width, wrapX);
		const int aq = abs(dq), ar = abs(dy);
		const int distance = ((dq < 0) != (dy < 0)) ? (aq > ar ? aq : ar) : aq + ar;
		if (distance != MAX_RADIUS) return -1;
		return gIndex[(dq + MAX_RADIUS) * 11 + (dy + MAX_RADIUS)];
	}

	static int Clamp(void* city, int r) {
		if (r < BASE_PLOTS || city == nullptr || !Enabled()) return r;
		if (r >= MAX_PLOTS || gRing[r] > ReadRadius(city)) return -1;
		return r;
	}

	static int __fastcall Hook_IndexCityPlot(void* city, const void* plot) { return Clamp(city, orig_IndexCityPlot(city, plot)); }
	static int __fastcall Hook_IndexCityCoord(void* city, const void* coord) { return Clamp(city, orig_IndexCityCoord(city, coord)); }

	// ---- citizens --------------------------------------------------------------------------------------------------------
	// The game recounts unassigned citizens from the locked ones right after this; locked workers on tiles beyond the radius
	// (the lookups return no plot there) would otherwise stay locked for good.
	static void __fastcall Hook_RemoveWorkers(void* citizens, bool send) {
		if (citizens != nullptr && Enabled()) {
			void* city = *(void**)citizens;
			int* forced = *(int**)((char*)citizens + CITIZENS_FORCED);
			const unsigned count = *(const unsigned*)((char*)citizens + CITIZENS_FORCED_COUNT);
			if (city != nullptr && forced != nullptr) {
				const int radius = ReadRadius(city);
				for (unsigned i = BASE_PLOTS; i < count && i < (unsigned)MAX_PLOTS; i++)
					if (gRing[i] > radius && forced[i] > 0) forced[i] = 0;
			}
		}
		orig_RemoveWorkers(citizens, send);
	}

	// ---- distance tests --------------------------------------------------------------------------------------------------
	// Reached only when the distance is above the game's limit (buy range / 3).
	static bool __fastcall AllowBeyond(void* city, int distance) {
		return city != nullptr && Enabled() && distance <= ReadRadius(city);
	}

	static bool __fastcall Within(void* city, int distance) {
		return distance <= BASE_RADIUS || AllowBeyond(city, distance);
	}

	// ---- stubs (as FounderUniques) ---------------------------------------------------------------------------------------
	static void* AllocNear(uintptr_t target, size_t size) {
		SYSTEM_INFO si; GetSystemInfo(&si);
		const uintptr_t gran = si.dwAllocationGranularity;
		const uintptr_t limit = 0x70000000;
		for (uintptr_t off = gran; off < limit; off += gran) {
			for (int dir = 0; dir < 2; dir++) {
				const uintptr_t cand = dir == 0 ? (target & ~(gran - 1)) - off : (target & ~(gran - 1)) + off;
				if (dir == 0 && cand > target) continue;   // wrapped around
				if (cand < 0x10000) continue;
				void* p = VirtualAlloc((void*)cand, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
				if (!p) continue;
				const intptr_t dist = (intptr_t)(uintptr_t)p - (intptr_t)target;
				if (dist > 0x7FF00000 || dist < -0x7FF00000) { VirtualFree(p, 0, MEM_RELEASE); continue; }
				return p;
			}
		}
		return nullptr;
	}

	static void Emit(std::vector<uint8_t>& b, std::initializer_list<uint8_t> l) { b.insert(b.end(), l.begin(), l.end()); }
	static void Emit64(std::vector<uint8_t>& b, uint64_t v) { for (int i = 0; i < 8; i++) b.push_back((uint8_t)(v >> (8 * i))); }

	// Saves the volatile registers, calls test(rcx, edx) set up by argMoves; true -> pass, false -> fail.
	// Stack: aligned at each site, 7 pushes + 0x28 keeps it so.
	static std::vector<uint8_t> Stub(std::initializer_list<uint8_t> argMoves, void* test, uintptr_t pass, uintptr_t fail) {
		std::vector<uint8_t> b;
		Emit(b, { 0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53 });   // push rax rcx rdx r8 r9 r10 r11
		Emit(b, { 0x48, 0x83, 0xec, 0x28 });                                             // sub rsp, 0x28
		Emit(b, argMoves);
		Emit(b, { 0x48, 0xb8 }); Emit64(b, (uint64_t)test);                              // mov rax, test
		Emit(b, { 0xff, 0xd0 });                                                         // call rax
		Emit(b, { 0x48, 0x83, 0xc4, 0x28 });                                             // add rsp, 0x28
		Emit(b, { 0x84, 0xc0 });                                                         // test al, al
		Emit(b, { 0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5a, 0x59, 0x58 });   // pop r11 r10 r9 r8 rdx rcx rax (flags kept)
		Emit(b, { 0x75, 0x0e });                                                         // jnz pass (skip the next 14 bytes)
		Emit(b, { 0xff, 0x25, 0, 0, 0, 0 }); Emit64(b, fail);                            // jmp [rip] fail
		Emit(b, { 0xff, 0x25, 0, 0, 0, 0 }); Emit64(b, pass);                            // jmp [rip] pass
		return b;
	}

	static bool Write(uintptr_t site, const void* bytes, size_t n) {
		DWORD old = 0;
		if (!VirtualProtect((void*)site, n, PAGE_EXECUTE_READWRITE, &old)) return false;
		memcpy((void*)site, bytes, n);
		VirtualProtect((void*)site, n, old, &old);
		FlushInstructionCache(GetCurrentProcess(), (void*)site, n);
		return true;
	}

	// Points the 6-byte jcc (0f 8x rel32) at site to stub.
	static bool Redirect(uintptr_t site, uintptr_t stub) {
		const intptr_t rel = (intptr_t)stub - (intptr_t)(site + 6);
		if (rel > INT32_MAX || rel < INT32_MIN) return false;
		const int32_t r = (int32_t)rel;
		return Write(site + 2, &r, 4);
	}

	// Replaces the 9 bytes "cmp eax, 3; jle rel32" at site with "jmp stub" + 4 nops.
	static bool JumpTo(uintptr_t site, uintptr_t stub) {
		const intptr_t rel = (intptr_t)stub - (intptr_t)(site + 5);
		if (rel > INT32_MAX || rel < INT32_MIN) return false;
		uint8_t b[9] = { 0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90 };
		const int32_t r = (int32_t)rel;
		memcpy(b + 1, &r, 4);
		return Write(site, b, sizeof(b));
	}

	// ---- setup ------------------------------------------------------------------------------------------------------------
	// Ring k starts at (0, k) and walks the six sides; this is the game's order (checked against its tables below).
	static void BuildTables() {
		static const int dirs[6][2] = { { 1, -1 }, { 0, -1 }, { -1, 0 }, { -1, 1 }, { 0, 1 }, { 1, 0 } };
		for (int& i : gIndex) i = -1;
		int n = 0;
		gDQ[n] = 0; gDR[n] = 0; gRing[n] = 0; n++;
		for (int k = 1; k <= MAX_RADIUS; k++) {
			int q = 0, r = k;
			for (const auto& d : dirs) {
				for (int s = 0; s < k; s++) {
					gDQ[n] = q; gDR[n] = r; gRing[n] = k; n++;
					q += d[0]; r += d[1];
				}
			}
		}
		for (int i = 0; i < MAX_PLOTS; i++) gIndex[(gDQ[i] + MAX_RADIUS) * 11 + (gDR[i] + MAX_RADIUS)] = i;
	}

	static bool TablesMatchGame(uintptr_t b) {
		const int* dq = (const int*)(b + GAME_DQ_TABLE);
		const int* dr = (const int*)(b + GAME_DR_TABLE);
		const int* index = (const int*)(b + GAME_INDEX_TABLE);
		for (int i = 0; i < GAME_PLOTS; i++)
			if (dq[i] != gDQ[i] || dr[i] != gDR[i]) return false;
		// the game's 9 x 9 box has -1 beyond ring 4 (some of those offsets are ring 5 here)
		for (int q = -4; q <= 4; q++) {
			for (int r = -4; r <= 4; r++) {
				const int mine = gIndex[(q + MAX_RADIUS) * 11 + (r + MAX_RADIUS)];
				const int expected = (mine >= 0 && gRing[mine] <= 4) ? mine : -1;
				if (index[(q + 4) * 9 + (r + 4)] != expected) return false;
			}
		}
		return true;
	}

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
				DevBridge::Log("[CityRadius] %s has unexpected bytes at 0x%llx: city radius not available", c.name, (unsigned long long)c.rva);
				return;
			}
		}
		BuildTables();
		if (!TablesMatchGame(b)) {
			DevBridge::Log("[CityRadius] the game's city plot order is not the expected one: city radius not available");
			return;
		}
		CapabilitiesGet = (CapabilitiesGetFn)(b + CAPABILITIES_GET_OFFSET);
		HasCapability = (HasCapabilityFn)(b + HAS_CAPABILITY_OFFSET);
		GetMap = (MapFn)(b + GET_MAP_OFFSET);
		EditMap = (MapFn)(b + EDIT_MAP_OFFSET);
		const MakeHashFn MakeHash = (MakeHashFn)(b + MAKE_HASH_OFFSET);
		gCapability = MakeHash(CAPABILITY);
		gProperty = MakeHash(PROPERTY);

		uint8_t* mem = (uint8_t*)AllocNear(b + BUY_JG, 0x1000);
		if (mem == nullptr) {
			DevBridge::Log("[CityRadius] no memory near GameCore: city radius not available");
			return;
		}
		const std::vector<uint8_t> buy = Stub({ 0x49, 0x8b, 0x0e, 0x8b, 0xd0 },                       // mov rcx, [r14] (city); mov edx, eax
			(void*)&AllowBeyond, b + BUY_PASS, b + BUY_FAIL);
		const std::vector<uint8_t> swap = Stub({ 0x48, 0x8b, 0xcd, 0x8b, 0xd0 },                      // mov rcx, rbp (city); mov edx, eax
			(void*)&AllowBeyond, b + SWAP_PASS, b + SWAP_FAIL);
		const std::vector<uint8_t> culture = Stub({ 0x48, 0x8b, 0x8c, 0x24, 0x70, 0x01, 0x00, 0x00,  // mov rcx, [rsp + 0x110 + 0x38 + 0x28] (Culture*)
			0x48, 0x8b, 0x09, 0x8b, 0xd0 },                                                            // mov rcx, [rcx] (city); mov edx, eax
			(void*)&Within, b + CULTURE_WITHIN, b + CULTURE_BEYOND);
		memcpy(mem, buy.data(), buy.size());
		memcpy(mem + 0x80, swap.data(), swap.size());
		memcpy(mem + 0x100, culture.data(), culture.size());
		FlushInstructionCache(GetCurrentProcess(), mem, 0x1000);

		gOk = true;
		const bool hooks = Hook(PLOTS_FROM_RADIUS_OFFSET, (void*)&Hook_PlotsFromRadius, (void**)nullptr)
			&& Hook(EDIT_PLOT_FROM_INDEX_OFFSET, (void*)&Hook_EditPlotFromCityIndex, &orig_EditPlot)
			&& Hook(GET_PLOT_FROM_INDEX_OFFSET, (void*)&Hook_GetPlotFromCityIndex, &orig_GetPlot)
			&& Hook(INDEX_CORE_OFFSET, (void*)&Hook_IndexCore, &orig_IndexCore)
			&& Hook(INDEX_CITY_PLOT_OFFSET, (void*)&Hook_IndexCityPlot, &orig_IndexCityPlot)
			&& Hook(INDEX_CITY_COORD_OFFSET, (void*)&Hook_IndexCityCoord, &orig_IndexCityCoord)
			&& Hook(REMOVE_WORKERS_OFFSET, (void*)&Hook_RemoveWorkers, &orig_RemoveWorkers);
		const bool patches = hooks
			&& Redirect(b + BUY_JG, (uintptr_t)mem)
			&& Redirect(b + SWAP_JG, (uintptr_t)(mem + 0x80))
			&& JumpTo(b + CULTURE_CMP, (uintptr_t)(mem + 0x100));
		DevBridge::Log("[CityRadius] %s: with %s, property %s adds rings to a city's radius (max %d)",
			patches ? "installed" : "FAILED", CAPABILITY, PROPERTY, MAX_RADIUS);
	}
}
