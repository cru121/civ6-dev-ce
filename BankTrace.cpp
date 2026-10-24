// SPDX-License-Identifier: AGPL-3.0-only
// Part of Dev CE. Diagnostic: logs (once per path) every .bnk / Banks.ini / DevCE file the process opens, so we can see whether the game's audio
// system tries to load a mod's soundbank. Written to BankTrace.log next to the DLL (own file: DevBridge.log is reset when Dev CE starts).
// Off unless the file "DevCE_BankTrace.on" exists next to the DLL.
#include "BankTrace.h"
#include "MinHook.h"
#include <windows.h>
#include <share.h>
#include <cstdio>
#include <cstdarg>
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
		std::string gLogPath;

		void Log(const char* fmt, ...) {   // caller holds no lock needed beyond gLock
			FILE* f = _fsopen(gLogPath.c_str(), "a", _SH_DENYNO);
			if (!f) return;
			SYSTEMTIME t; GetLocalTime(&t);
			fprintf(f, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
			va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
			fputc('\n', f);
			fclose(f);
		}

		bool Interesting(const std::wstring& p) {
			std::wstring l = p;
			for (auto& c : l) c = (wchar_t)towlower(c);
			return l.find(L".bnk") != std::wstring::npos || l.find(L"banks.ini") != std::wstring::npos || l.find(L"devce") != std::wstring::npos;
		}

		HANDLE WINAPI Hook(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
			HANDLE h = gOrig(name, access, share, sa, disp, flags, tmpl);
			const DWORD err = (h == INVALID_HANDLE_VALUE) ? GetLastError() : 0;
			if (!tInHook && name) {
				tInHook = true;
				std::wstring p(name);
				if (Interesting(p)) {
					std::lock_guard<std::mutex> g(gLock);
					if (gSeen.insert(p).second) {
						char buf[1024];
						WideCharToMultiByte(CP_UTF8, 0, p.c_str(), -1, buf, sizeof(buf), nullptr, nullptr);
						Log("open %s -> %s (err %lu)", buf, h == INVALID_HANDLE_VALUE ? "FAILED" : "ok", err);
					}
				}
				tInHook = false;
			}
			if (h == INVALID_HANDLE_VALUE) SetLastError(err);
			return h;
		}
	}

	void Install(const std::string& dllDir) {
		const std::string flag = dllDir + "\\DevCE_BankTrace.on";
		if (GetFileAttributesA(flag.c_str()) == INVALID_FILE_ATTRIBUTES) return;
		gLogPath = dllDir + "\\BankTrace.log";
		// hook KernelBase's CreateFileW (kernel32's is only a forwarding stub, and api-ms-win-core-file imports resolve straight to KernelBase)
		HMODULE kb = GetModuleHandleA("kernelbase.dll");
		void* target = kb ? reinterpret_cast<void*>(GetProcAddress(kb, "CreateFileW")) : nullptr;
		if (!target) target = reinterpret_cast<void*>(&CreateFileW);
		if (MH_CreateHook(target, reinterpret_cast<void*>(&Hook), reinterpret_cast<void**>(&gOrig)) != MH_OK) { Log("hook failed"); return; }
		if (MH_EnableHook(target) != MH_OK) { Log("enable failed"); return; }
		Log("tracing .bnk / Banks.ini / DevCE file opens");
	}
}
