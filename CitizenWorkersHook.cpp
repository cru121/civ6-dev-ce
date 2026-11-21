// SPDX-License-Identifier: AGPL-3.0-only
#include "CitizenWorkersHook.h"
#include "Runtime.h"
#include "DevBridge.h"
#include <cstring>

namespace CitizenWorkersHook {
	static Fn gBase = nullptr;
	static Fn gAround = nullptr;
	static PreFn gPre[4];
	static int gPreCount = 0;
	static bool gState = false, gTried = false;

	static void __fastcall Dispatch(void* citizens, bool send) {
		for (int i = 0; i < gPreCount; ++i) gPre[i](citizens);
		if (gAround != nullptr) gAround(citizens, send);
		else gBase(citizens, send);
	}

	bool Ensure() {
		if (gTried) return gState;
		gTried = true;
		static const uint8_t kPrologue[16] = { 0x40, 0x53, 0x57, 0x41, 0x54, 0x41, 0x55, 0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x01, 0x48, 0x8b };
		const uintptr_t b = Runtime::GameCoreAddress;
		if (memcmp((void*)(b + REMOVE_ALL_WORKERS_OFFSET), kPrologue, sizeof(kPrologue)) != 0) {
			DevBridge::Log("[CitizenWorkersHook] RemoveAllCitizenWorkers has unexpected bytes at 0x%llx: the features that need it are not available", (unsigned long long)REMOVE_ALL_WORKERS_OFFSET);
			return false;
		}
		LPVOID target = (LPVOID)(b + REMOVE_ALL_WORKERS_OFFSET);
		if (MH_CreateHook(target, (LPVOID)&Dispatch, (LPVOID*)&gBase) != MH_OK || MH_EnableHook(target) != MH_OK) {
			DevBridge::Log("[CitizenWorkersHook] the hook could not be installed: the features that need it are not available");
			return false;
		}
		gState = true;
		return true;
	}

	void AddPre(PreFn fn) { if (gPreCount < 4) gPre[gPreCount++] = fn; }
	void SetAround(Fn fn) { gAround = fn; }
	Fn Base() { return gBase; }
}
