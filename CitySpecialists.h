// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstdint>

namespace hks { struct lua_State; }

// City-centre specialists (alternate citizen mode). GlobalParameter DEVCE_CITY_CENTER_SPECIALISTS = 1 switches it on (default 0 = the
// game's normal specialists). Each city then has slots per specialist type (table DevCE_Specialists, yields in DevCE_Specialist_Yields);
// a citizen in a slot gives that type's yields. The game's own auto-assign fills the slots together with tiles.
//
// State, saved in city properties (<Type> = SpecialistType):
//   DEVCE_SPECIALIST_SLOTS_<Type>    slots, set by modifiers (EFFECT_ADJUST_CITY_PROPERTY); read only
//   DEVCE_SPECIALISTS_<Type>         working specialists, locked and auto (written by Dev CE in a reallocation only; yields read this)
//   DEVCE_SPECIALISTS_LOCKED_<Type>  the player's lock request; applied at the next reallocation (a raw write alone changes nothing)
//   DEVCE_SPECIALIST_YIELD_<Type>_<YieldType>  extra yield per specialist of that type, set by modifiers (base yields only)
// Great person points per specialist: table DevCE_Specialist_GreatPersonPoints, added to the city's points like District_CitizenGreatPersonPoints.
//
// Hooks (all do nothing in mode 0):
//   GlobalParameters::Initialize   after the game: read the mode and the two tables (the game's own database connection)
//   RemoveAllCitizenWorkers        before: room left by locked tiles; after: working count = locked request (trimmed), taken from the free citizens
//   knapsack call in TryAssignAllWorkers (a jump at 0x110f9a): add one candidate per open slot, apply the chosen specialists
//   TryAssignAllWorkers            before: cap the free citizens (growth skips RemoveAll); after: fill open slots when the game had no tile candidates
//   City::GetYieldFromTiles        after: + specialists x their yields
//   AdjustCityProperty Apply/Remove  after: a slot key that fell below the working count reallocates the city at once
//   GreatPeoplePoints::CalculatePointsPerTurn (a jump at 0x2e2a81): add the city's specialist points before the city's % modifier
//
// Gameplay Lua: CitySpecialists.SetLocked(playerId, cityId, specialistType, count) sets the lock request and reallocates the city
// at once (returns the working count, or -1); CitySpecialists.IsEnabled(). UI scripts read the city properties and send a request
// through UI.RequestPlayerOperation(EXECUTE_SCRIPT) to a gameplay script that calls SetLocked.
namespace CitySpecialists {
	constexpr uintptr_t GLOBAL_PARAMETERS_INITIALIZE_OFFSET = 0x1f02a0;   // GlobalParameters::Initialize(params, IDatabaseConnection1*)
	constexpr uintptr_t REMOVE_ALL_WORKERS_OFFSET = 0x110760;             // City::Citizens::RemoveAllCitizenWorkers(citizens, bool send)
	constexpr uintptr_t TRY_ASSIGN_WORKERS_OFFSET = 0x110ba0;             // City::Citizens::TryAssignAllWorkers(citizens, bool send)
	constexpr uintptr_t KNAPSACK_OFFSET = 0x10f050;                       // bounded_knapsack<SimpleScoring>(begin, end, capacity, foodNeeded, bool)
	constexpr uintptr_t KNAPSACK_PATCH = 0x110f9a;                        //   mov rcx,rsi; call knapsack (8 bytes); r15 = citizens
	constexpr uintptr_t KNAPSACK_RESUME = 0x110fa2;                       //   mov r13,rax
	constexpr uintptr_t GET_YIELD_FROM_TILES_OFFSET = 0x133780;           // City::Instance::GetYieldFromTiles(city, FixedPoint* out, YieldTypes) -> out
	constexpr uintptr_t MAKE_HASH_OFFSET = 0x606270;
	constexpr uintptr_t NAME_TO_YIELD_TYPE_OFFSET = 0x7d8d70;
	constexpr uintptr_t GET_CITY_OFFSET = 0x42e40;                        // City::Instance* GetCity(const CityID*)
	constexpr uintptr_t REALLOCATE_CITIZENS_OFFSET = 0x110650;            // City::Citizens::ReallocateCitizens(citizens)
	constexpr uintptr_t REPORT_EVENT_SEND_OFFSET = 0x5fb890;              // Reporting::Event::Send(type, const ComponentID*, int key): updates the UI cache
	constexpr uint32_t CITY_PROPERTY_CHANGED_EVENT = 0x30fbf8d2;          //   the type Lua City:SetProperty sends after a change
	constexpr uintptr_t REPORT_EVENT_SEND_PLOT_OFFSET = 0x5fb110;         // Reporting::Event::Send(type, const ComponentID*, const PlotCoord*)
	constexpr uint64_t CITY_CITIZENS_CHANGED_EVENT = 0x15d11e18c2f385c0ull;  //   what City::Commands::ManageCitizen::Start sends after a citizen change:
	                                                                      //   the UI then raises Events.CityWorkerChanged (PlotInfo, city panel redraw)
	constexpr uintptr_t ADJUST_CITY_PROPERTY_APPLY_OFFSET = 0x8b41d0;     // GameEffects::Effects::AdjustCityProperty::Apply(effect, City*) (vtable slot 12)
	constexpr uintptr_t ADJUST_CITY_PROPERTY_REMOVE_OFFSET = 0x8b4a30;    //   ::Remove (slot 13); effect +8 = key hash, +0xc = amount
	constexpr uintptr_t GPP_PATCH = 0x2e2a81;                             // Player::GreatPeoplePoints::CalculatePointsPerTurn, per city after the buildings:
	                                                                      //   mov eax,[r13+0x1500] (7 bytes); r13 = city, ebp = its points (FixedPoint<8>), r15d = class
	constexpr uintptr_t GPP_RESUME = 0x2e2a88;                            //   test ebp,ebp
	constexpr uintptr_t PROPERTY_EDIT_OFFSET = 0x72a920;                  // FAutoVariable<VariantMap>::edit(object) -> map
	constexpr uintptr_t YIELD_FOOD_INDEX = 0xb8f648;                      // yield index globals (set when the database loads)
	constexpr uintptr_t YIELD_GOLD_INDEX = 0xb8f650;

	constexpr uintptr_t CITY_PROPERTIES = 0x190;      // City::Instance: properties FAutoVariable (map at +0x10)
	constexpr uintptr_t CITY_ID = 0xa8;
	constexpr uintptr_t CITY_PLOT = 0xc0;             // PlotCoord (int x, int y) of the city centre
	constexpr uintptr_t CITY_OWNER = 0xd8;
	constexpr uintptr_t CITY_POPULATION = 0x268;
	constexpr uintptr_t CITY_CITIZENS = 0xc28;        // City::Citizens (its +0 is the city)
	constexpr uintptr_t CITIZENS_LOCKED = 0x20;       // int* locked workers per tile index
	constexpr uintptr_t CITIZENS_LOCKED_COUNT = 0x30;
	constexpr uintptr_t CITIZENS_FAVORED = 0x38;      // char* per yield index
	constexpr uintptr_t CITIZENS_DISFAVORED = 0x50;   // char* per yield index
	constexpr uintptr_t CITIZENS_FREE = 0x68;         // citizens not placed yet
	constexpr uintptr_t CITIZENS_ASSIGNED = 0x6c;     // auto-placed citizens (locked ones not counted)

	constexpr const char* MODE_PARAMETER = "DEVCE_CITY_CENTER_SPECIALISTS";
	constexpr const char* SLOTS_PREFIX = "DEVCE_SPECIALIST_SLOTS_";
	constexpr const char* COUNT_PREFIX = "DEVCE_SPECIALISTS_";
	constexpr const char* LOCKED_PREFIX = "DEVCE_SPECIALISTS_LOCKED_";
	constexpr const char* YIELD_PREFIX = "DEVCE_SPECIALIST_YIELD_";   // + <Type> + "_" + <YieldType>

	void Create();
	int Register(hks::lua_State* L);   // gameplay state: the CitySpecialists table
}
