#include "Runtime.h"
#include "MinHook.h"
#include <iostream>
#include "HavokScript.h"
#pragma comment(lib, "libMinHook.x64.lib")

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

	// Call on dll exit
	void Destroy() {
		if (GameCore) {
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
