#pragma once
#include "MinHook.h"
#include "ProxyTypes.h"
#include "asmjit/x86.h"
#include <iostream>
#include <span>
#include "Log.h"

namespace Runtime {
	extern HMODULE GameCore;
	extern uintptr_t GameCoreAddress;
	extern asmjit::JitRuntime Jit;

	// The game executable (process module) and the shared CE logger (Logs/CE/Community_Extension.log, also echoed to the console when it is enabled).
	// Valid after Create().
	extern HMODULE CivVIModHandle;
	extern Log::Logger* MainLogger;

	// Should only be called once
	extern void Create();

	// Call on dll exit
	extern void Destroy();

	void InitMinHook();

	template <typename T>
	T GetGameCoreGlobalAt(uintptr_t address) {
		return reinterpret_cast<T>(GameCoreAddress + address);
	}

	template <typename T>
	void CreateHook(LPVOID pTarget, LPVOID pDetour, T** ppOriginal) {
		MH_STATUS status;
		if ((status = MH_CreateHook(pTarget, pDetour, reinterpret_cast<LPVOID*>(ppOriginal))) != MH_OK) {
			MainLogger->Error("Failed to load hook! {}", MH_StatusToString(status));
		}
		if ((status = MH_EnableHook(pTarget)) != MH_OK) {
			MainLogger->Error("Failed to enable hook! {}", MH_StatusToString(status));
		}
	}

	// Use for related hooks that must have valid trampolines before any detour
	// can execute. Install during serialized Runtime initialization, with no
	// other queued MinHook operations. Failure rolls back this group's hooks.
	struct HookSpec {
		const char* name;
		LPVOID target;
		LPVOID detour;
		LPVOID* original;
	};

	template <typename Function>
	HookSpec DescribeHook(const char* name, Function* target, Function* detour, Function** original) {
		return {name, reinterpret_cast<LPVOID>(target), reinterpret_cast<LPVOID>(detour),
			reinterpret_cast<LPVOID*>(original)};
	}

	bool InstallHookGroup(std::span<const HookSpec> hooks);

	// Replace one verified five-byte instruction with a near CALL. The target
	// must replay the instruction and obey the site's ABI. Install during
	// serialized initialization. Runtime pins the adapter's target module and
	// owns the sole relay for the process. A game-context reset retires policy.
	bool InstallInstructionCall(uintptr_t rva, std::span<const unsigned char, 5> expected,
		void (*target)());

	extern void InitConsole();

	extern void CloseConsole();
};