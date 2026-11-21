#pragma once
#include "MinHook.h"
#include "ProxyTypes.h"
#include "asmjit/x86.h"
#include <iostream>
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

	extern void InitConsole();

	extern void CloseConsole();
};