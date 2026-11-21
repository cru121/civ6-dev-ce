// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstdint>

// District specialists (citizens working a district tile), driven by reserved district properties. No new effects: mods set
// the properties with the game's EFFECT_ADJUST_DISTRICT_PROPERTY (Key, Amount), so the values are saved with the district.
//   DEVCE_CITIZEN_<YieldType>  (e.g. DEVCE_CITIZEN_YIELD_SCIENCE): each citizen working the district gives that much more
//   DEVCE_CITIZEN_SLOTS                                                    : the district tile takes that many more citizens
//
//   - Rules::Yields::GetYield, district branch: District_CitizenYieldChanges + property (then Building_CitizenYieldChanges)
//   - City::Citizens::GetPlotWorkerCapacity: Districts.CitizenSlots + property (then the buildings' CitizenSlots)
//      Each is one jump written into the function at that point
namespace DistrictCitizens {
	constexpr uintptr_t MAKE_HASH_OFFSET = 0x606270;                      // Utilities::MakeHash(const char*)
	constexpr uintptr_t NAME_TO_YIELD_TYPE_OFFSET = 0x7d8d70;             // Definition::Utility::NameToYieldType(MakeHash(name)) -> index or -1 (indices set at DB load)
	constexpr uintptr_t DISTRICT_PROPERTY_APPLY_OFFSET = 0x8dbb70;        // AdjustDistrictProperty::Apply (checked: where the properties live, the number test)
	constexpr uintptr_t GET_CITY_OFFSET = 0x42e40;                        // City::Instance* GetCity(const CityID*)
	constexpr uintptr_t GET_DISTRICT_AT_OFFSET = 0x11e520;                // City::Districts::GetDistrictAt(districts, const PlotCoord*)
	constexpr uintptr_t GET_YIELD_OFFSET = 0x37ec60;                      // Rules::Yields::Instance::GetYield(rules, plot, yield, player, bool)
	constexpr uintptr_t GET_YIELD_PATCH = 0x37ee04;                       //   after the District_CitizenYieldChanges loop (esi = sum, r12d = yield, r14 = plot)
	constexpr uintptr_t GET_YIELD_RESUME = 0x37ee0a;
	constexpr uintptr_t WORKER_CAPACITY_OFFSET = 0x10fff0;                // City::Citizens::GetPlotWorkerCapacity(citizens, plot)
	constexpr uintptr_t WORKER_CAPACITY_PATCH = 0x110238;                 //   after ebp = Districts.CitizenSlots (rsi = plot, r15 = citizens)
	constexpr uintptr_t WORKER_CAPACITY_RESUME = 0x11023e;

	constexpr uintptr_t DISTRICT_PROPERTIES = 0x378;       // District::Instance: m_kProperties (FAutoVariable at +0x368, map at +0x10); vtable +0x60 = get(key)
	constexpr uintptr_t CITY_DISTRICTS = 0xb58;            // City::Instance: City::Districts

	constexpr const char* YIELD_PROPERTY_PREFIX = "DEVCE_CITIZEN_";        // + the YieldType name: DEVCE_CITIZEN_YIELD_SCIENCE
	constexpr const char* SLOTS_PROPERTY = "DEVCE_CITIZEN_SLOTS";

	void Create();
}
