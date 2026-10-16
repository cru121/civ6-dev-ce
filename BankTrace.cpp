// SPDX-License-Identifier: AGPL-3.0-only
// Part of Dev CE. Diagnostic: logs (once per path) every .bnk / .ini / DevCE file the process opens, so we can see whether the game's audio
// system tries to load a mod's soundbank. Written to DevBridge.log. Off unless the file "DevCE_BankTrace.on" exists next to the DLL.
#include "BankTrace.h"
#include "DevBridge.h"
#include "MinHook.h"
#include <windows.h>
#include <mutex>
#include <set>
#include <string>
#include <cwctype>

namespace BankTrace {
	namespace {
		typedef HANDLE(WINAPI* CreateFileW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
		CreateFileW_t gOrig = nullptr;
		thread_local bool tInHook = false;
		std::mutex gLock;
		std::set<std::wstring> gSeen;

		bool Interesting(const std::wstring& p) {
			std::wstring l = p;
			for (auto& c : l) c = (wchar_t)towlower(c);
			return l.find(L".bnk") != std::wstring::npos || l.find(L"banks.ini") != std::wstring::npos || l.find(L"devce") != std::wstring::npos;
		}

		HANDLE WINAPI Hook(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
			HANDLE h = gOrig(name, access, share, sa, disp, flags, tmpl);
			if (!tInHook && name) {
				tInHook = true;
				std::wstring p(name);
				if (Interesting(p)) {
					const DWORD err = (h == INVALID_HANDLE_VALUE) ? GetLastError() : 0;
					std::lock_guard<std::mutex> g(gLock);
					if (gSeen.insert(p).second) {
						char buf[1024];
						WideCharToMultiByte(CP_UTF8, 0, p.c_str(), -1, buf, sizeof(buf), nullptr, nullptr);
						DevBridge::Log("[BankTrace] open %s -> %s (err %lu)", buf, h == INVALID_HANDLE_VALUE ? "FAILED" : "ok", err);
					}
				}
				tInHook = false;
			}
			return h;
		}
	}

	void Install(const std::string& dllDir) {
		const std::string flag = dllDir + "\DevCE_BankTrace.on";
		if (GetFileAttributesA(flag.c_str()) == INVALID_FILE_ATTRIBUTES) return;
		if (MH_CreateHook(reinterpret_cast<void*>(&CreateFileW), reinterpret_cast<void*>(&Hook), reinterpret_cast<void**>(&gOrig)) != MH_OK) { DevBridge::Log("[BankTrace] hook failed"); return; }
		if (MH_EnableHook(reinterpret_cast<void*>(&CreateFileW)) != MH_OK) { DevBridge::Log("[BankTrace] enable failed"); return; }
		DevBridge::Log("[BankTrace] tracing .bnk / Banks.ini / DevCE file opens");
	}
}
