// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <string>
namespace BankTrace {
	// Installs the CreateFileW trace if "DevCE_BankTrace.on" exists in dllDir. Call after MinHook is initialised.
	void Install(const std::string& dllDir);
}
