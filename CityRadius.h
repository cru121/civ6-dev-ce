// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstdint>

// City radius. Cities normally work tiles within 3 of the centre (the 37 "city plots" of CityHexspace). With the game
// capability CAPABILITY_DEVCE_CITY_RADIUS (a Types row of KIND_CAPABILITY and a GameCapabilities row) a city with the property
// DEVCE_CITY_EXTRA_RINGS works, buys and swaps tiles out to 3 + that many rings (capped at 5). Properties add up, so a civ
// trait giving +1 and a wonder giving +1 make 5; other cities building the wonder get 4. Culture border growth prefers tiles
// inside the radius. District and wonder placement stay at 3 (CanHaveDistrict / CanHaveWonder have their own distance check).
//
// Why a capability: every city's per-tile arrays (citizens, AI plot data) are sized from the plot count when the city is
// founded. With the capability that count is 91 (radius 5) for every city of the game; a save keeps its mods, so a game
// started without it never sees the bigger count.
//
// Hooks / patches (all pass through without the capability):
//   CityHexspace::ComputePlotsFromRadius(2)    every "plots in radius 3" loop (always called as R(2) + 18): 73, so 91 plots
//   CityHexspace::Get/EditPlotFromCityIndex    index -> plot: null beyond the city's radius; ring 5 (61..90) computed here
//                                              (the game's tables stop at ring 4); same ring order as the game
//   CityHexspace::GetCityIndexFromPlot         plot -> index: ring 5 added (core); the per-city overloads give -1 beyond
//                                              the city's radius
//   City::Citizens::RemoveAllCitizenWorkers    before it: locked workers beyond the radius are let go (the game reallocates
//                                              every turn, so a smaller radius takes effect at the next turn)
//   City::Gold::GetPlotPurchaseCost            "distance > CITY_MAX_BUY_PLOT_RANGE" jump: allowed up to the city's radius
//   City::Culture::CanSwapPlotFromCity         "distance > 3" jump: allowed up to the city's radius
//   City::Culture::GetBuyablePlotList          "distance <= 3" (border growth prefers these tiles): the city's radius
namespace CityRadius {
	constexpr uintptr_t PLOTS_FROM_RADIUS_OFFSET = 0x447d0;      // static unsigned ComputePlotsFromRadius(int)
	constexpr uintptr_t EDIT_PLOT_FROM_INDEX_OFFSET = 0x44980;   // static Plot* EditPlotFromCityIndex(City*, int)
	constexpr uintptr_t GET_PLOT_FROM_INDEX_OFFSET = 0x44d10;    // static const Plot* GetPlotFromCityIndex(const City*, int)
	constexpr uintptr_t INDEX_CORE_OFFSET = 0x44a90;             // static int GetCityIndexFromPlot(PlotCoord, PlotCoord, int w, int h, bool wrapX, bool wrapY)
	constexpr uintptr_t INDEX_CITY_PLOT_OFFSET = 0x449b0;        // static int GetCityIndexFromPlot(const City*, const Plot*)
	constexpr uintptr_t INDEX_CITY_COORD_OFFSET = 0x44a20;       // static int GetCityIndexFromPlot(const City*, const PlotCoord&)
	constexpr uintptr_t REMOVE_WORKERS_OFFSET = 0x110760;        // void Citizens::RemoveAllCitizenWorkers(bool)
	constexpr uintptr_t CAPABILITIES_GET_OFFSET = 0x48230;       // Capabilities::Manager* Capabilities::Get()
	constexpr uintptr_t HAS_CAPABILITY_OFFSET = 0x484e0;         // bool Capabilities::Manager::HasCapability(unsigned hash)
	constexpr uintptr_t GET_MAP_OFFSET = 0x526c0;                // const Map::Instance* Globals::GetMap()
	constexpr uintptr_t EDIT_MAP_OFFSET = 0x44d40;               // Map::Instance* Globals::EditMap()
	constexpr uintptr_t MAKE_HASH_OFFSET = 0x606270;
	constexpr uintptr_t GAME_DQ_TABLE = 0xb60df0;                // the game's index -> offset tables (61 entries, rings 0..4)
	constexpr uintptr_t GAME_DR_TABLE = 0xb60ef0;
	constexpr uintptr_t GAME_INDEX_TABLE = 0xb60ff0;             // offset -> index, 9 x 9

	constexpr uintptr_t BUY_JG = 0x1211ef;                       // GetPlotPurchaseCost: jg fail (distance in eax, Gold* in r14)
	constexpr uintptr_t BUY_PASS = 0x1211f5;
	constexpr uintptr_t BUY_FAIL = 0x1213c8;
	constexpr uintptr_t SWAP_JG = 0x11b6dc;                      // CanSwapPlotFromCity: jg fail (distance in eax, City* in rbp)
	constexpr uintptr_t SWAP_PASS = 0x11b6e2;
	constexpr uintptr_t SWAP_FAIL = 0x11b7c0;
	constexpr uintptr_t CULTURE_CMP = 0x11c2d6;                  // GetBuyablePlotList: cmp eax, 3; jle within (Culture* at [rsp + 0x110])
	constexpr uintptr_t CULTURE_WITHIN = 0x11c205;
	constexpr uintptr_t CULTURE_BEYOND = 0x11c2df;

	constexpr uintptr_t CITY_X = 0xc0;
	constexpr uintptr_t CITY_Y = 0xc4;
	constexpr uintptr_t CITY_PROPERTIES = 0x1a0;                 // Data::VariantMap (vtable +0x60 = get(key) -> Variant*)
	constexpr uintptr_t CITIZENS_FORCED = 0x20;                  // int* locked workers per city index
	constexpr uintptr_t CITIZENS_FORCED_COUNT = 0x30;
	constexpr uintptr_t MAP_WIDTH = 0x28;
	constexpr uintptr_t MAP_HEIGHT = 0x2c;
	constexpr uintptr_t MAP_WRAP_X = 0x40;
	constexpr uintptr_t MAP_WRAP_Y = 0x41;
	constexpr uintptr_t MAP_EDIT_PLOT_SLOT = 0x38;               // Map vtable: Plot* EditPlot(const PlotCoord&)
	constexpr uintptr_t MAP_GET_PLOT_SLOT = 0x50;                // Map vtable: const Plot* GetPlot(const PlotCoord&)

	constexpr int BASE_RADIUS = 3;
	constexpr int MAX_RADIUS = 5;
	constexpr int BASE_PLOTS = 37;      // radius 3
	constexpr int GAME_PLOTS = 61;      // radius 4: what the game's tables cover
	constexpr int MAX_PLOTS = 91;       // radius 5

	constexpr const char* CAPABILITY = "CAPABILITY_DEVCE_CITY_RADIUS";
	constexpr const char* PROPERTY = "DEVCE_CITY_EXTRA_RINGS";

	void Create();
}
