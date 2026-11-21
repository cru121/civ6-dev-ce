// SPDX-License-Identifier: AGPL-3.0-only
#include "UnitUpgradeFallback.h"
#include "Runtime.h"
#include "DevBridge.h"
#include <intrin.h>
#include <cstring>

#pragma intrinsic(_ReturnAddress)

namespace UnitUpgradeFallback {
	typedef bool(__fastcall* CanTrainFn)(void* queue, int unit, bool testVisible, void* results, int formation, bool ignoreRequirements, bool p6);
	typedef int(__fastcall* HasReplacementFn)(void* rules, int unit, void* player);
	typedef bool(__fastcall* IsMajorFn)(void* player);

	static CanTrainFn base_CanTrain;
	static HasReplacementFn base_HasReplacement;
	static IsMajorFn base_IsMajor;
	static uintptr_t gReplacementReturn = 0, gIsMajorReturn = 0;
	static bool gOk = false;

	struct Check { const char* name; uintptr_t rva; uint8_t bytes[12]; size_t n; };
	static const Check kChecks[] = {
		{ "City::BuildQueue::CanTrain", CAN_TRAIN_OFFSET, { 0x4c, 0x89, 0x4c, 0x24, 0x20, 0x44, 0x88, 0x44, 0x24, 0x18, 0x89, 0x54 }, 12 },
		{ "Rules::Units::HasAvailableReplacementForUnit", HAS_REPLACEMENT_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x48, 0x89, 0x7c, 0x24, 0x20, 0x41, 0x56 }, 12 },
		{ "Player::Instance::IsMajor", IS_MAJOR_OFFSET, { 0x48, 0x83, 0xec, 0x28, 0x8b, 0x91, 0xd8, 0x00, 0x00, 0x00, 0x48, 0x8b }, 12 },
		// CanTrain's upgrade loop: `mov edx, r12d; call HasAvailableReplacementForUnit` and `mov rcx, r13; call IsMajor; test al, al; je`
		{ "CanTrain upgrade loop (replacement call)", CAN_TRAIN_REPLACEMENT_RETURN - 8, { 0x41, 0x8b, 0xd4, 0xe8, 0x39, 0x58, 0x28, 0x00 }, 8 },
		{ "CanTrain upgrade loop (IsMajor call)", CAN_TRAIN_IS_MAJOR_RETURN - 8, { 0x49, 0x8b, 0xcd, 0xe8, 0xd6, 0x0e, 0x20, 0x00, 0x84, 0xc0, 0x74, 0x4f }, 12 },
	};

	// One frame per CanTrain call on this thread (CanTrain calls itself through us for the upgrade).
	constexpr int MAX_DEPTH = 8;
	struct Frame { void* queue; int unit; bool p6; int upgrade; };
	static thread_local Frame tFrames[MAX_DEPTH];
	static thread_local int tDepth = 0;

	static bool __fastcall Hook_CanTrain(void* queue, int unit, bool testVisible, void* results, int formation, bool ignoreRequirements, bool p6) {
		if (tDepth >= MAX_DEPTH) return base_CanTrain(queue, unit, testVisible, results, formation, ignoreRequirements, p6);
		tFrames[tDepth] = { queue, unit, p6, -1 };
		++tDepth;
		const bool r = base_CanTrain(queue, unit, testVisible, results, formation, ignoreRequirements, p6);
		--tDepth;
		return r;
	}

	// In CanTrain's upgrade loop: remember which unit this upgrade is for the player (a unique unit replaces the generic one).
	static int __fastcall Hook_HasReplacement(void* rules, int unit, void* player) {
		const int r = base_HasReplacement(rules, unit, player);
		if (tDepth > 0 && (uintptr_t)_ReturnAddress() == gReplacementReturn) tFrames[tDepth - 1].upgrade = r != -1 ? r : unit;
		return r;
	}

	static bool __fastcall Hook_IsMajor(void* player) {
		const bool major = base_IsMajor(player);
		if (!major || tDepth <= 0 || (uintptr_t)_ReturnAddress() != gIsMajorReturn) return major;
		// The upgrade is unlocked and the game is about to hide the unit: keep it unless this city can train the upgrade.
		const Frame f = tFrames[tDepth - 1];
		if (f.upgrade < 0) return major;
		for (int i = 0; i < tDepth; ++i) if (tFrames[i].unit == f.upgrade) return major;   // upgrade cycle: leave it to the game
		const bool upgradeTrainable = Hook_CanTrain(f.queue, f.upgrade, false, nullptr, 0, false, f.p6);
		return upgradeTrainable;   // false: the loop goes on as for a minor civ (this upgrade does not hide the unit)
	}

	static bool Verify(uintptr_t b) {
		for (const Check& c : kChecks) {
			if (memcmp((void*)(b + c.rva), c.bytes, c.n) != 0) {
				DevBridge::Log("[UnitUpgradeFallback] %s has unexpected bytes at 0x%llx: upgrade fallback is not available", c.name, (unsigned long long)c.rva);
				return false;
			}
		}
		return true;
	}

	void Create() {
		if (gOk) return;   // hooks stay installed when Dev CE starts again in the same process
		const uintptr_t b = Runtime::GameCoreAddress;
		if (!Verify(b)) return;
		gReplacementReturn = b + CAN_TRAIN_REPLACEMENT_RETURN;
		gIsMajorReturn = b + CAN_TRAIN_IS_MAJOR_RETURN;
		Runtime::CreateHook((LPVOID)(b + CAN_TRAIN_OFFSET), (LPVOID)&Hook_CanTrain, &base_CanTrain);
		Runtime::CreateHook((LPVOID)(b + HAS_REPLACEMENT_OFFSET), (LPVOID)&Hook_HasReplacement, &base_HasReplacement);
		Runtime::CreateHook((LPVOID)(b + IS_MAJOR_OFFSET), (LPVOID)&Hook_IsMajor, &base_IsMajor);
		gOk = true;
		DevBridge::Log("[UnitUpgradeFallback] hooks installed: a unit stays buildable while its city cannot train its upgrade");
	}
}
