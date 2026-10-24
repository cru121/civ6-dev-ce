#pragma once
#include "HavokScript.h"
#include "vector"

namespace NationalParks {
	typedef class NationalParks;
	typedef struct ParkData;
	namespace Types {
		typedef NationalParks* (*__cdecl Edit)(void);
		typedef void (*__thiscall DesignatePark)(NationalParks*, int playerId, int* coord);
		typedef bool (*__thiscall RestoreVisualState)(NationalParks*, ParkData*);
		typedef ParkData* (*__thiscall FindPark)(NationalParks*, int* coord);
	}

// #pragma pack(push, 1)
// 	struct ParkData {
// 		int id;
// 		int i1;
// 		eastl::vector* plotIds;
// 		void* p1;
// 		char padding[0x18];
// 		char* parkName;
// 	};
// #pragma pack(pop)

	//extern bool DesignateParkInPlots(NationalParks*, int playerId, int* baseCoord, std::vector<int>& plotIds);

	constexpr uintptr_t GET_NATIONAL_PARKS_OFFSET = 0x2363e0;
	extern Types::Edit Edit;

	constexpr uintptr_t DESIGNATE_PARK_OFFSET = 0x235830;
	extern Types::DesignatePark DesignatePark;

	constexpr uintptr_t RESTORE_VISUAL_STATE_OFFSET = 0x2389f0;
	extern Types::RestoreVisualState RestoreVisualState;

	constexpr uintptr_t FIND_PARK_OFFSET = 0x2362c0;
	extern Types::FindPark FindPark;

	// --- Dev CE additions: custom-shaped parks (see findings/topics/national-park-custom-shape.md) ---
	namespace Types {
		typedef void* (*EditMapFn)(void);                                  // Context::Globals::EditMap -> Map::IInstance*
		typedef void* (*MapEditInstanceFn)(void);                          // Map::EditInstance
		typedef void* (*EditPlayerVisibilityFn)(void* map);                // Map::Instance::EditPlayerVisibility
		typedef void* (*VariableEditFn)(void* autoVariable);               // FAutoVariable<VariantMap,Game::Instance>::edit
		typedef bool (*IsValidParkTileFn)(NationalParks*, void* plot, int playerId);
		typedef int (*GetDistrictTypeFn)(void* cityAttributes);            // Map::Plot::CityAttributes::GetDistrictType
		typedef bool (*IsNaturalWonderFn)(void* plot);                     // Map::Plot::Instance::IsNaturalWonder
	}
	constexpr uintptr_t EDIT_MAP_OFFSET = 0x44d40;
	constexpr uintptr_t MAP_EDIT_INSTANCE_OFFSET = 0x526c0;
	constexpr uintptr_t EDIT_PLAYER_VISIBILITY_OFFSET = 0x52ce0;
	constexpr uintptr_t VARIABLE_EDIT_OFFSET = 0x72a920;
	constexpr uintptr_t IS_VALID_PARK_TILE_OFFSET = 0x237860;
	constexpr uintptr_t GET_DISTRICT_TYPE_OFFSET = 0x5e720;
	constexpr uintptr_t IS_NATURAL_WONDER_OFFSET = 0x60280;

	// Creates a park on 1..4 arbitrary plots (plot indices). Returns 0 on success, otherwise an error code (see NationalParks.cpp).
	extern int DesignateCustom(int playerId, const int* plotIds, int count);

	extern int Register(hks::lua_State* L);
	extern int RegisterUI(hks::lua_State* L);
	extern void Create();
}