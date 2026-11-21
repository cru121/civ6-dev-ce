#include "Runtime.h"
#include "MinHook.h"
#include <iostream>
#include "HavokScript.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#pragma comment(lib, "libMinHook.x64.lib")
#pragma comment(lib, "bcrypt.lib")

namespace Runtime {
	HMODULE GameCore;
	uintptr_t GameCoreAddress;
	asmjit::JitRuntime Jit;
	HMODULE CivVIModHandle;
	Log::Logger* MainLogger;

	typedef size_t(__fastcall* EXP_GetLogPath_t)(wchar_t* pszOutBuffer, const wchar_t* pszSubPath);

	// Should only be called once
	void Create() {
		CivVIModHandle = GetModuleHandle(NULL);

		// Log directory: <game log dir>/CE/ (the exe resolves it); fall back to the working directory
		std::filesystem::path logPath = ".";
		auto getLogPath = reinterpret_cast<EXP_GetLogPath_t>(GetProcAddress(CivVIModHandle, "EXP_GetLogPath"));
		if (getLogPath) {
			std::wstring buffer(MAX_PATH, L'\0');
			size_t charsWritten = getLogPath(buffer.data(), L"CE/");
			if (charsWritten > 0 && charsWritten <= buffer.size()) {
				buffer.resize(charsWritten);
				logPath = buffer;
			}
		}
		std::error_code ec;
		std::filesystem::create_directories(logPath, ec);
		Log::Init(logPath);
		MainLogger = new Log::Logger("CE", "Community_Extension.log");

		GameCore = LoadLibrary(TEXT("../../../DLC/Expansion2/Binaries/Win64/GameCore_XP2_FinalRelease.dll"));
		if (!GameCore) {
			MainLogger->Error("Original GameCore failed to load!");
			return;
		}
		GameCoreAddress = reinterpret_cast<uintptr_t>(GameCore);
	}

	void InitMinHook() {
		if (MH_Initialize() != MH_OK) {
			MainLogger->Error("MH failed to init");
			return;
		}
		MainLogger->Info("MH initialized successfully.");
	}

	bool InstallHookGroup(std::span<const HookSpec> hooks) {
		size_t created = 0;
		auto rollback = [&] {
			for (const auto& hook : hooks.first(created)) {
				// RemoveHook disables a hook if ApplyQueued enabled it before
				// reporting failure. Keep trampolines valid on cleanup failure.
				if (auto status = MH_RemoveHook(hook.target); status != MH_OK) {
					MainLogger->Error("Failed to remove {}: {}", hook.name, MH_StatusToString(status));
				}
				else {
					*hook.original = nullptr;
				}
			}
		};

		for (const auto& hook : hooks) {
			if (auto status = MH_CreateHook(hook.target, hook.detour, hook.original); status != MH_OK) {
				rollback();
				MainLogger->Error("Failed to create {}: {}", hook.name, MH_StatusToString(status));
				return false;
			}
			++created;
		}
		for (const auto& hook : hooks) {
			if (auto status = MH_QueueEnableHook(hook.target); status != MH_OK) {
				rollback();
				MainLogger->Error("Failed to queue {}: {}", hook.name, MH_StatusToString(status));
				return false;
			}
		}
		if (auto status = MH_ApplyQueued(); status != MH_OK) {
			rollback();
			MainLogger->Error("Failed to enable hook group: {}", MH_StatusToString(status));
			return false;
		}
		return true;
	}


namespace {
	struct InstructionCall {
		unsigned char* site{};
		void* relay{};
		DWORD originalProtection{};
		std::array<unsigned char, 5> original{};
		std::array<unsigned char, 5> replacement{};
	};

	// The adapter is installed once before the first game context is created.
	// Its target module is pinned. Context changes retire policy, not code.
	InstructionCall instructionCall;

	void* AllocateCallRelay(uintptr_t returnAddress, void (*target)()) {
		using AllocateNear = void* (WINAPI*)(HANDLE, void*, SIZE_T, ULONG, ULONG,
			MEM_EXTENDED_PARAMETER*, ULONG);
		const auto allocate = reinterpret_cast<AllocateNear>(
			GetProcAddress(GetModuleHandleW(L"KernelBase.dll"), "VirtualAlloc2"));
		if (!allocate)
			return nullptr;

		SYSTEM_INFO info{};
		GetSystemInfo(&info);
		const auto alignment = static_cast<uintptr_t>(info.dwAllocationGranularity);
		const auto center = returnAddress & ~(alignment - 1);
		// Stay inside the signed CALL range, allowing for the page and alignment.
		constexpr uintptr_t reach = 0x7fff0000;
		MEM_ADDRESS_REQUIREMENTS range{};
		range.LowestStartingAddress = reinterpret_cast<void*>((std::max)(
			center > reach ? center - reach : 0, reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress)));
		range.HighestEndingAddress = reinterpret_cast<void*>((std::min)(
			center + reach - 1, reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress)));
		MEM_EXTENDED_PARAMETER parameter{};
		parameter.Type = MemExtendedParameterAddressRequirements;
		parameter.Pointer = &range;
		auto* relay = static_cast<unsigned char*>(allocate(GetCurrentProcess(), nullptr,
			info.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, &parameter, 1));
		if (!relay)
			return nullptr;

		// JMP [RIP+0] leaves every register and the caller's return address intact.
		constexpr unsigned char jump[] = {0xff, 0x25, 0, 0, 0, 0};
		std::memcpy(relay, jump, sizeof jump);
		std::memcpy(relay + sizeof jump, &target, sizeof target);
		DWORD previous{};
		if (!VirtualProtect(relay, info.dwPageSize, PAGE_EXECUTE_READ, &previous) ||
			!FlushInstructionCache(GetCurrentProcess(), relay, sizeof jump + sizeof target)) {
			VirtualFree(relay, 0, MEM_RELEASE);
			return nullptr;
		}
		return relay;
	}

	void RestoreInstructionCall() {
		auto& patch = instructionCall;
		if (!patch.relay)
			return;
		DWORD discarded{};
		if (std::memcmp(patch.site, patch.replacement.data(), patch.replacement.size()) == 0 &&
			VirtualProtect(patch.site, patch.original.size(), PAGE_EXECUTE_READWRITE, &discarded))
			std::memcpy(patch.site, patch.original.data(), patch.original.size());
		if (std::memcmp(patch.site, patch.original.data(), patch.original.size()) == 0) {
			const bool restored = VirtualProtect(patch.site, patch.original.size(),
				patch.originalProtection, &discarded);
			const bool flushed = FlushInstructionCache(GetCurrentProcess(), patch.site, patch.original.size());
			if (restored && flushed) {
				VirtualFree(patch.relay, 0, MEM_RELEASE);
				patch = {};
				return;
			}
		}
		MainLogger->Error("Could not restore the verified GameCore instruction call");
	}
}

	bool InstallInstructionCall(uintptr_t rva, std::span<const unsigned char, 5> expected,
		void (*target)()) {
		auto* site = GetGameCoreGlobalAt<unsigned char*>(rva);
		if (instructionCall.relay || !target || std::memcmp(site, expected.data(), expected.size()))
			return false;
		const auto returnAddress = reinterpret_cast<uintptr_t>(site + expected.size());
		void* relay = AllocateCallRelay(returnAddress, target);
		if (!relay)
			return false;
		const auto displacement = reinterpret_cast<intptr_t>(relay) - static_cast<intptr_t>(returnAddress);
		DWORD previous{};
		HMODULE targetModule{};
		// A failed rollback must never leave a CALL to an unloaded module.
		// Once installed, the adapter and its target are process-owned.
		constexpr DWORD pinTarget = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN;
		if (displacement < INT32_MIN || displacement > INT32_MAX ||
			!GetModuleHandleExW(pinTarget, reinterpret_cast<LPCWSTR>(target), &targetModule) ||
			!VirtualProtect(site, expected.size(), PAGE_EXECUTE_READWRITE, &previous)) {
			VirtualFree(relay, 0, MEM_RELEASE);
			return false;
		}

		auto& patch = instructionCall;
		patch.site = site;
		patch.relay = relay;
		patch.originalProtection = previous;
		std::copy(expected.begin(), expected.end(), patch.original.begin());
		patch.replacement[0] = 0xe8; // CALL rel32
		const auto relative = static_cast<int32_t>(displacement);
		std::memcpy(patch.replacement.data() + 1, &relative, sizeof relative);
		std::memcpy(site, patch.replacement.data(), patch.replacement.size());
		DWORD discarded{};
		if (FlushInstructionCache(GetCurrentProcess(), site, patch.replacement.size()) &&
			VirtualProtect(site, patch.replacement.size(), previous, &discarded))
			return true;

		// Retain the relay and ownership record unless both code and page
		// protection are restored. An unpublished target passes the score through.
		RestoreInstructionCall();
		return false;
	}

	// Call on dll exit
	void Destroy() {
		if (GameCore) {
			RestoreInstructionCall();
			MH_DisableHook(MH_ALL_HOOKS);
			MH_Uninitialize();
			FreeLibrary(GameCore);
			GameCore = NULL;
			GameCoreAddress = 0x0;
		}
	}

	void InitConsole() {
		if (!AllocConsole()) {
			MessageBoxW(NULL, L"Failed to create the console!", L"Error", MB_ICONERROR);
			return;
		}

		// Redirect standard input/output streams to the console
		FILE* fDummy;
		freopen_s(&fDummy, "CONIN$", "r", stdin);
		freopen_s(&fDummy, "CONOUT$", "w", stderr);
		freopen_s(&fDummy, "CONOUT$", "w", stdout);

		std::cout << "Console initialized successfully." << std::endl;
	}

	void CloseConsole() {
		if (!FreeConsole()) {
			MessageBoxW(NULL, L"Failed to close the console!", L"Error", MB_ICONERROR);
		}
		else {
			std::cout << "Console closed successfully." << std::endl;
		}

		// Reset standard input/output streams
		freopen_s((FILE**)stdin, "NUL:", "r", stdin);
		freopen_s((FILE**)stdout, "NUL:", "w", stdout);
		freopen_s((FILE**)stderr, "NUL:", "w", stderr);

		// Clear the error state for each of the C++ standard streams after redirect.
		std::cin.clear();
		std::cout.clear();
		std::cerr.clear();
	}
};
