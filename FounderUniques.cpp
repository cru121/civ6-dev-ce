// SPDX-License-Identifier: AGPL-3.0-only
#include "FounderUniques.h"
#include "Runtime.h"
#include "DevBridge.h"
#include <Windows.h>
#include <cstring>
#include <vector>

namespace FounderUniques {
	// Executable memory within rel32 reach of target (as LuaHooks does it).
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

	typedef void* (__fastcall* EditPlayerFn)(int player);
	typedef uint32_t(__fastcall* MakeHashFn)(const char*);
	typedef const uint8_t* (__fastcall* VariantGetFn)(void* map, uint32_t key);

	static EditPlayerFn EditPlayer;
	static uint32_t gKey = 0;
	static bool gOk = false;

	struct Check { const char* name; uintptr_t rva; uint8_t bytes[16]; size_t n; };
	static const Check kChecks[] = {
		// CanTrain: mov edi, edx; mov r14, rcx (unit, queue)
		{ "CanTrain: unit in edi, queue in r14", CAN_TRAIN_OFFSET + 0x2d, { 0x8b, 0xfa, 0x4c, 0x8b, 0xf1 }, 5 },
		// mov rax,[rax+0x6c8]; ... mov rcx,[rax+0x128]; ... cmp [r9+rcx], edi; jne fail
		{ "CanTrain: player units +0x6c8", 0xf873e, { 0x48, 0x8b, 0x80, 0xc8, 0x06, 0x00, 0x00 }, 7 },
		{ "CanTrain: version table +0x128", 0xf874a, { 0x48, 0x8b, 0x88, 0x28, 0x01, 0x00, 0x00 }, 7 },
		{ "CanTrain: version test", CAN_TRAIN_VERSION_JNE - 4, { 0x41, 0x39, 0x3c, 0x09, 0x0f, 0x85, 0x37, 0x10, 0x00, 0x00 }, 10 },
		// inner CanConstruct: movsxd rbp, edx; mov r13, rcx (building, queue)
		{ "CanConstruct: building in ebp, queue in r13", CAN_CONSTRUCT_OFFSET + 0x23, { 0x48, 0x63, 0xea, 0x4c, 0x8b, 0xe9 }, 6 },
		{ "CanConstruct: player cities +0x6d0, version table +0x150", 0xf629e, { 0x48, 0x8b, 0x8f, 0xd0, 0x06, 0x00, 0x00, 0x4c, 0x63, 0xe5, 0x48, 0x8b, 0x81, 0x50, 0x01, 0x00 }, 16 },
		{ "CanConstruct: version test", CAN_CONSTRUCT_VERSION_JNE - 4, { 0x41, 0x39, 0x2c, 0x00, 0x0f, 0x85, 0x1f, 0x0e, 0x00, 0x00 }, 10 },
		{ "CanConstruct: after the test (IsCityProductionBanned)", CAN_CONSTRUCT_PASS, { 0x8b, 0xd5, 0xe8, 0xb3, 0xf9, 0x16, 0x00 }, 7 },
		// version tables: FAutoVariable at +0x118 / +0x140; edit() = +0x10 -> data +0x128 / +0x150, count +0x138 / +0x160
		{ "SetCanEverTrain: version FAutoVariable +0x118", 0x350dd9, { 0x4d, 0x8d, 0xa5, 0x18, 0x01, 0x00, 0x00 }, 7 },
		{ "SetCanEverConstruct: version table +0x150", 0x266774, { 0x49, 0x8b, 0x87, 0x50, 0x01, 0x00, 0x00 }, 7 },
		{ "Lua City:GetOriginalOwner: city +0xf0", 0x6c901b, { 0x66, 0x0f, 0x6e, 0x80, 0xf0, 0x00, 0x00, 0x00 }, 8 },
		{ "Lua City:GetProperty: properties +0x1a0", 0x6c929e, { 0x4c, 0x8d, 0x80, 0xa0, 0x01, 0x00, 0x00 }, 7 },
		{ "Globals::EditPlayer", EDIT_PLAYER_OFFSET, { 0x8b, 0xd1, 0x48, 0x8b, 0x0d }, 5 },
		{ "Utilities::MakeHash", MAKE_HASH_OFFSET, { 0x48, 0x8b, 0xd1, 0x41, 0x83, 0xc8, 0xff, 0x48 }, 8 },
	};

	// ---- the rule --------------------------------------------------------------------------------------------------------
	static int CityProperty(void* city, uint32_t key) {
		void* map = (char*)city + CITY_PROPERTIES;
		const uint8_t* v = ((VariantGetFn)(*(void***)map)[0x60 / 8])(map, key);
		if (v == nullptr) return 0;
		const uint16_t type = *(const uint16_t*)v;
		const uint32_t flags = *(const uint32_t*)(v + 0x14);
		const bool isNumber = ((flags >> 29) & 1) == 0 && type <= 0x15 && ((0x30002eu >> type) & 1) != 0;
		return isNumber ? *(const int*)(v + 8) : 0;
	}

	// The founder's version table maps the item to itself.
	static bool FounderHas(void* queue, int item, uintptr_t component, uintptr_t table, uintptr_t count) {
		if (queue == nullptr || item < 0) return false;
		char* city = *(char**)((char*)queue + QUEUE_CITY);
		if (city == nullptr) return false;
		const int owner = *(const int*)(city + CITY_OWNER);
		const int founder = *(const int*)(city + CITY_ORIGINAL_OWNER);
		if (founder < 0 || founder >= 64 || founder == owner) return false;
		if (CityProperty(city, gKey) < 1) return false;
		char* player = (char*)EditPlayer(founder);
		if (player == nullptr) return false;
		char* comp = *(char**)(player + component);
		if (comp == nullptr) return false;
		const int* versions = *(const int* const*)(comp + table);
		const unsigned n = *(const unsigned*)(comp + count);
		return versions != nullptr && (unsigned)item < n && versions[item] == item;
	}

	static bool __fastcall AllowUnit(void* queue, int unit) {
		return FounderHas(queue, unit, PLAYER_UNITS, UNITS_VERSION, UNITS_VERSION_COUNT);
	}

	static bool __fastcall AllowBuilding(void* queue, int building) {
		return FounderHas(queue, building, PLAYER_CITIES, CITIES_VERSION, CITIES_VERSION_COUNT);
	}

	// ---- the stubs -------------------------------------------------------------------------------------------------------
	// Entered from the redirected jne (owner's version[item] != item). Saves the volatile registers, calls allow(queue, item);
	// true -> continue after the jne, false -> the original fail target. Stack: aligned at the site, 7 pushes + 0x28 keeps it so.
	static void Emit(std::vector<uint8_t>& b, std::initializer_list<uint8_t> l) { b.insert(b.end(), l.begin(), l.end()); }
	static void Emit64(std::vector<uint8_t>& b, uint64_t v) { for (int i = 0; i < 8; i++) b.push_back((uint8_t)(v >> (8 * i))); }

	static std::vector<uint8_t> Stub(std::initializer_list<uint8_t> argMoves, void* allow, uintptr_t pass, uintptr_t fail) {
		std::vector<uint8_t> b;
		Emit(b, { 0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53 });   // push rax rcx rdx r8 r9 r10 r11
		Emit(b, { 0x48, 0x83, 0xec, 0x28 });                                             // sub rsp, 0x28
		Emit(b, argMoves);                                                               // rcx = queue, edx = item
		Emit(b, { 0x48, 0xb8 }); Emit64(b, (uint64_t)allow);                             // mov rax, allow
		Emit(b, { 0xff, 0xd0 });                                                         // call rax
		Emit(b, { 0x48, 0x83, 0xc4, 0x28 });                                             // add rsp, 0x28
		Emit(b, { 0x84, 0xc0 });                                                         // test al, al
		Emit(b, { 0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5a, 0x59, 0x58 });   // pop r11 r10 r9 r8 rdx rcx rax (flags kept)
		Emit(b, { 0x75, 0x0e });                                                         // jnz pass (skip the next 14 bytes)
		Emit(b, { 0xff, 0x25, 0, 0, 0, 0 }); Emit64(b, fail);                            // jmp [rip] fail
		Emit(b, { 0xff, 0x25, 0, 0, 0, 0 }); Emit64(b, pass);                            // jmp [rip] pass
		return b;
	}

	// Points the 6-byte jne (0f 85 rel32) at site to stub.
	static bool Redirect(uintptr_t site, uintptr_t stub) {
		const intptr_t rel = (intptr_t)stub - (intptr_t)(site + 6);
		if (rel > INT32_MAX || rel < INT32_MIN) return false;
		DWORD old = 0;
		if (!VirtualProtect((void*)site, 6, PAGE_EXECUTE_READWRITE, &old)) return false;
		const int32_t r = (int32_t)rel;
		memcpy((void*)(site + 2), &r, 4);
		VirtualProtect((void*)site, 6, old, &old);
		FlushInstructionCache(GetCurrentProcess(), (void*)site, 6);
		return true;
	}

	void Create() {
		if (gOk) return;   // the patches stay in place when Dev CE starts again in the same process
		const uintptr_t b = Runtime::GameCoreAddress;
		for (const Check& c : kChecks) {
			if (memcmp((void*)(b + c.rva), c.bytes, c.n) != 0) {
				DevBridge::Log("[FounderUniques] %s has unexpected bytes at 0x%llx: founder uniques not available", c.name, (unsigned long long)c.rva);
				return;
			}
		}
		EditPlayer = (EditPlayerFn)(b + EDIT_PLAYER_OFFSET);
		gKey = ((MakeHashFn)(b + MAKE_HASH_OFFSET))(PROPERTY);

		const std::vector<uint8_t> unitStub = Stub({ 0x49, 0x8b, 0xce, 0x8b, 0xd7 },          // mov rcx, r14; mov edx, edi
			(void*)&AllowUnit, b + CAN_TRAIN_PASS, b + CAN_TRAIN_FAIL);
		const std::vector<uint8_t> buildingStub = Stub({ 0x49, 0x8b, 0xcd, 0x8b, 0xd5 },      // mov rcx, r13; mov edx, ebp
			(void*)&AllowBuilding, b + CAN_CONSTRUCT_PASS, b + CAN_CONSTRUCT_FAIL);
		uint8_t* mem = (uint8_t*)AllocNear(b + CAN_TRAIN_VERSION_JNE, 0x1000);
		if (mem == nullptr) {
			DevBridge::Log("[FounderUniques] no memory near GameCore: founder uniques not available");
			return;
		}
		memcpy(mem, unitStub.data(), unitStub.size());
		memcpy(mem + 0x80, buildingStub.data(), buildingStub.size());
		FlushInstructionCache(GetCurrentProcess(), mem, 0x1000);
		const bool ok = Redirect(b + CAN_TRAIN_VERSION_JNE, (uintptr_t)mem)
			&& Redirect(b + CAN_CONSTRUCT_VERSION_JNE, (uintptr_t)(mem + 0x80));
		gOk = ok;
		DevBridge::Log("[FounderUniques] %s: city property %s >= 1 lets a city build its founder's units and buildings",
			ok ? "installed" : "FAILED", PROPERTY);
	}
}
