// SPDX-License-Identifier: AGPL-3.0-only
#include "Features.h"
#include "DevBridge.h"
#include <atomic>
#include <cstring>

namespace Features {
	typedef void* (__fastcall* CapabilitiesGetFn)();
	typedef bool(__fastcall* HasCapabilityFn)(void* manager, uint32_t hash);
	typedef uint32_t(__fastcall* MakeHashFn)(const char*);

	static CapabilitiesGetFn CapabilitiesGet = nullptr;
	static HasCapabilityFn HasCapability = nullptr;
	static bool gReady = false;

	struct Def { const char* capability; const char* name; uint32_t hash; };
	static Def kDefs[COUNT] = {
		{ "CAPABILITY_DEVCE_UPGRADE_RULES", "unit upgrade rules", 0 },
		{ "CAPABILITY_DEVCE_UPGRADE_FALLBACK", "upgrade fallback", 0 },
		{ "CAPABILITY_DEVCE_GOLD_PURCHASE", "gold purchase unlocks", 0 },
		{ "CAPABILITY_DEVCE_FOOD_PRODUCTION", "food to production", 0 },
		{ "CAPABILITY_DEVCE_DISTRICT_CITIZENS", "district citizens", 0 },
		{ "CAPABILITY_DEVCE_CITY_RADIUS", "city radius", 0 },
		{ "CAPABILITY_DEVCE_GW_ARCHIVE", "great works archive", 0 },
	};

	// -1 = not asked in this game yet, 0 = off, 1 = on
	static std::atomic<int> gState[COUNT];
	static std::atomic<int> gGeneration{ 0 };
	static std::atomic<int> gSeen[COUNT];   // the generation in which gState was set

	bool Init(uintptr_t b) {
		if (gReady) return true;
		static const struct { uintptr_t rva; uint8_t bytes[8]; } kChecks[] = {
			{ CAPABILITIES_GET_OFFSET, { 0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x0d, 0x00 } },
			{ HAS_CAPABILITY_OFFSET, { 0x89, 0x54, 0x24, 0x10, 0x48, 0x83, 0xec, 0x48 } },
			{ MAKE_HASH_OFFSET, { 0x48, 0x8b, 0xd1, 0x41, 0x83, 0xc8, 0xff, 0x48 } },
		};
		for (const auto& c : kChecks) {
			if (memcmp((void*)(b + c.rva), c.bytes, 7) != 0) {
				DevBridge::Log("[Features] unexpected bytes at 0x%llx: the feature switches are not available, every switched feature stays off", (unsigned long long)c.rva);
				return false;
			}
		}
		CapabilitiesGet = (CapabilitiesGetFn)(b + CAPABILITIES_GET_OFFSET);
		HasCapability = (HasCapabilityFn)(b + HAS_CAPABILITY_OFFSET);
		const MakeHashFn MakeHash = (MakeHashFn)(b + MAKE_HASH_OFFSET);
		for (int i = 0; i < COUNT; ++i) { kDefs[i].hash = MakeHash(kDefs[i].capability); gState[i] = -1; gSeen[i] = -1; }
		gReady = true;
		return true;
	}

	void NewGame() {
		++gGeneration;
	}

	bool On(Id id) {
		if (!gReady) return false;
		const int gen = gGeneration.load();
		int s = gState[id].load();
		if (s >= 0 && gSeen[id].load() == gen) return s != 0;
		void* manager = CapabilitiesGet();
		if (manager == nullptr) return false;   // no game yet: ask again next time
		s = HasCapability(manager, kDefs[id].hash) ? 1 : 0;
		gState[id] = s;
		gSeen[id] = gen;
		DevBridge::Log("[Features] %s: %s (%s)", kDefs[id].name, s ? "ON" : "off", kDefs[id].capability);
		return s != 0;
	}
}
