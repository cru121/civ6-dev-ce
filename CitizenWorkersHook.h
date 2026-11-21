// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstdint>

// One shared hook on City::Citizens::RemoveAllCitizenWorkers. Two features need it (city radius lets go of locked workers beyond the
// radius; city-centre specialists rebuild the working counts around it), and MinHook allows one hook per function, so each of them
// used to see the other's patched prologue and refuse to install.
//   AddPre(fn)     fn(citizens) runs before the game's function, in the order added (CityRadius)
//   SetAround(fn)  fn(citizens, send) runs instead of a direct call and calls Base() itself (CitySpecialists); at most one
//   Base()         the game's own function
namespace CitizenWorkersHook {
	typedef void(__fastcall* Fn)(void* citizens, bool send);
	typedef void(__fastcall* PreFn)(void* citizens);

	constexpr uintptr_t REMOVE_ALL_WORKERS_OFFSET = 0x110760;   // void City::Citizens::RemoveAllCitizenWorkers(citizens, bool send)

	// Checks the function's first bytes and installs the hook (once). False: the function is not the expected one, or hooking failed.
	bool Ensure();
	void AddPre(PreFn fn);
	void SetAround(Fn fn);
	Fn Base();
}
