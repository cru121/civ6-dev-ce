#include "NationalParks.h"
#include "Runtime.h"

namespace NationalParks {
	Types::DesignatePark DesignatePark;
	Types::Edit Edit;
	Types::FindPark FindPark;
	Types::RestoreVisualState RestoreVisualState;

	//void RemoveVisualState()

	//bool DesignateParkInPlots(NationalParks* nationalParks, int playerId, int* baseCoord, const std::vector<int>& plotIds) {
	//	DesignatePark(nationalParks, playerId, baseCoord);
	//	ParkData* park = FindPark(nationalParks, baseCoord);
	//
	//	if (park == NULL) {
	//		return false;
	//	}
	//
	//
	//
	//	return true;
	//}

	//static int lDesignateParkInPlots(hks::lua_State* L) {
	//	int playerId = hks::checkinteger(L, 1);
	//	int coord[2]{
	//		hks::checkinteger(L, 2),
	//		hks::checkinteger(L, 3)
	//	};
	//
	//	int arraySize = hks::objlen(L, 2);
	//	std::vector<int> plotIds;
	//	for (int i = 1; i <= arraySize; i++) {
	//		hks::pushinteger(L, i);
	//		hks::gettable(L, 2);
	//
	//		int plotId = hks::checkinteger(L, -1);
	//
	//		plotIds.push_back(plotId);
	//		hks::pop(L, 1);
	//	}
	//
	//	hks::pushboolean(L, DesignateParkInPlots(Edit(), playerId, coord, plotIds));
	//	return 1;
	//}

	// ---- Dev CE: custom-shaped parks -------------------------------------------------------------------------------------
	// Why this works (live-tested 2026-10-24, findings/topics/national-park-custom-shape.md):
	//  * DesignatePark always builds the 2x2 rhombus and ends with RestoreVisualState, which sends ADDED with the plot list read from the
	//    current game state. Fences and the green border are ADDITIVE per ADDED event, so editing the list after that event leaves the
	//    rhombus drawn (union). Therefore the list must be final BEFORE the single ADDED event.
	//  * Map::Player::Visibility::Manager byte +0x2b (m_bDisabledVisibilityUpdateMessages) makes RestoreVisualState/RestoreVisibility
	//    send nothing. We set it while DesignatePark creates the record (the Gossip/moment signal still fires), overwrite the
	//    record's plot indices in place (<= 4 plots, so no reallocation), restore the flag, then call RestoreVisualState once.
	// ParkData layout (0x30 bytes, vector at NationalParks+0xb8..+0xc0): +0 founder, +4 turn, +8 vector<int> {begin,end,cap}, +0x28 name.
	static constexpr size_t PARK_STRIDE = 0x30;
	static constexpr size_t PARKS_BEGIN = 0xb8, PARKS_END = 0xc0, PARKS_VAR = 0xa8;
	static constexpr int MAP_WIDTH_OFF = 0x28, MAP_COUNT_OFF = 0x30;

	static int ParkCount(char* np) {
		return (int)((*(char**)(np + PARKS_END) - *(char**)(np + PARKS_BEGIN)) / PARK_STRIDE);
	}

	static bool InAnyPark(NationalParks* np, int x, int y) {
		int coord[2] = { x, y };
		return FindPark(np, coord) != NULL;
	}

	int DesignateCustom(int playerId, const int* plotIds, int count) {
		using namespace Runtime;
		if (count < 1 || count > 4 || plotIds == NULL) return 1;
		auto editMap = GetGameCoreGlobalAt<Types::EditMapFn>(EDIT_MAP_OFFSET);
		auto mapEditInstance = GetGameCoreGlobalAt<Types::MapEditInstanceFn>(MAP_EDIT_INSTANCE_OFFSET);
		auto editVis = GetGameCoreGlobalAt<Types::EditPlayerVisibilityFn>(EDIT_PLAYER_VISIBILITY_OFFSET);
		auto varEdit = GetGameCoreGlobalAt<Types::VariableEditFn>(VARIABLE_EDIT_OFFSET);

		char* map = (char*)editMap();
		if (map == NULL) return 6;
		const int width = *(int*)(map + MAP_WIDTH_OFF);
		const int plotTotal = *(int*)(map + MAP_COUNT_OFF);
		if (width <= 0 || plotTotal <= 0) return 6;

		for (int i = 0; i < count; i++) {
			if (plotIds[i] < 0 || plotIds[i] >= plotTotal) return 2;
			for (int j = 0; j < i; j++) if (plotIds[j] == plotIds[i]) return 3;
		}
		NationalParks* np = Edit();
		for (int i = 0; i < count; i++) {
			if (InAnyPark(np, plotIds[i] % width, plotIds[i] / width)) return 4;
		}

		char* vis = (char*)editVis(mapEditInstance());
		if (vis == NULL) return 6;
		char* npc = (char*)np;
		const int before = ParkCount(npc);
		const char oldFlag = vis[0x2b];
		vis[0x2b] = 1;                               // suppress ADDED / VISIBILITY events of the temporary rhombus record

		// DesignatePark silently does nothing when the rhombus around the base plot leaves the map: try each plot as the base.
		// It also names the park after the city that owns the base plot and CRASHES (null deref at DesignatePark+0x259, seen live
		// 2026-10-24) when the base plot has no owning city and the rhombus holds no natural wonder -> only plots owned by a city may be the base.
		auto getPlotByIndex = (void* (*)(void*, int))(*(void***)map)[0x60 / 8];
		auto ownerNamesCity = [&](int plotIndex) -> bool {
			char* plot = (char*)getPlotByIndex(map, plotIndex);
			if (plot == NULL) return false;
			auto getOwnerData = (char* (*)(void*))(*(void***)plot)[0x30 / 8];
			char* owner = getOwnerData(plot);
			return owner != NULL && *(signed char*)(owner + 0x1c) >= 0 && *(int*)(owner + 0xc) != -1;
		};
		bool created = false;
		bool anyOwned = false;
		for (int i = 0; i < count && !created; i++) {
			if (!ownerNamesCity(plotIds[i])) continue;
			anyOwned = true;
			int base[2] = { plotIds[i] % width, plotIds[i] / width };
			DesignatePark(np, playerId, base);
			created = ParkCount(npc) == before + 1;
		}
		vis[0x2b] = oldFlag;
		if (!anyOwned) return 7;
		if (!created) return 5;

		char* park = *(char**)(npc + PARKS_END) - PARK_STRIDE;
		int** vec = (int**)(park + 8);               // {begin, end, cap}
		int* begin = vec[0];
		if (begin == NULL || vec[1] - begin != 4 || vec[2] - begin < 4) return 6;   // the rhombus always has 4 plots
		varEdit(npc + PARKS_VAR);                    // mark the parks variable as edited (sync / change tracking)
		for (int i = 0; i < count; i++) begin[i] = plotIds[i];
		vec[1] = begin + count;                      // shrink in place when fewer than 4 plots were requested
		RestoreVisualState(np, (ParkData*)park);     // ONE ADDED event with the final list + per-plot visibility events
		return 0;
	}

	static int lDesignateCustom(hks::lua_State* L) {
		int playerId = hks::checkinteger(L, 1);
		int n = (int)hks::objlen(L, 2);
		if (n < 1 || n > 4) {
			hks::pushinteger(L, 1);
			return 1;
		}
		int plots[4];
		for (int i = 1; i <= n; i++) {
			hks::rawgeti(L, 2, i);
			plots[i - 1] = hks::checkinteger(L, -1);
			hks::pop(L, 1);
		}
		hks::pushinteger(L, DesignateCustom(playerId, plots, n));
		return 1;
	}

	// NationalParks.IsValidParkTile(playerId, plotIndex) -> boolean (the game's own rule: owner, no improvement, no district, terrain/wonder/appeal, not in a park)
	static int lIsValidParkTile(hks::lua_State* L) {
		using namespace Runtime;
		int playerId = hks::checkinteger(L, 1);
		int plotIndex = hks::checkinteger(L, 2);
		char* map = (char*)GetGameCoreGlobalAt<Types::EditMapFn>(EDIT_MAP_OFFSET)();
		bool result = false;
		if (map != NULL && plotIndex >= 0 && plotIndex < *(int*)(map + MAP_COUNT_OFF)) {
			void** vtable = *(void***)map;
			auto getPlotByIndex = (void* (*)(void*, int))vtable[0x60 / 8];
			void* plot = getPlotByIndex(map, plotIndex);
			if (plot != NULL) {
				result = GetGameCoreGlobalAt<Types::IsValidParkTileFn>(IS_VALID_PARK_TILE_OFFSET)(Edit(), plot, playerId);
			}
		}
		hks::pushboolean(L, result);
		return 1;
	}

	// NationalParks.IsValidParkTileEx(playerId, plotIndex, flags) -> boolean. Same rule as the game's IsValidParkTile (re-implemented from its
	// decompile, old 0x2381b0) with two switches: flags & 1 = ignore "the plot has a district" (world wonders, Preserve...), flags & 2 = ignore the
	// terrain / natural wonder / appeal requirement. Lua decides WHICH districts or wonders are acceptable; the owner, improvement and
	// already-in-a-park checks always apply.
	static bool IsValidParkTileEx(int playerId, int plotIndex, int flags) {
		using namespace Runtime;
		char* map = (char*)GetGameCoreGlobalAt<Types::EditMapFn>(EDIT_MAP_OFFSET)();
		if (map == NULL || plotIndex < 0 || plotIndex >= *(int*)(map + MAP_COUNT_OFF)) return false;
		const int width = *(int*)(map + MAP_WIDTH_OFF);
		auto getPlotByIndex = (void* (*)(void*, int))(*(void***)map)[0x60 / 8];
		char* plot = (char*)getPlotByIndex(map, plotIndex);
		if (plot == NULL) return false;
		auto getOwnerData = (char* (*)(void*))(*(void***)plot)[0x30 / 8];
		char* owner = getOwnerData(plot);
		if (owner == NULL || (int)*(signed char*)(owner + 0x1c) != playerId) return false;
		if (*(short*)(plot + 0x44) != -1) return false;                               // improvement
		if (!(flags & 1)) {
			auto getDistrictType = GetGameCoreGlobalAt<Types::GetDistrictTypeFn>(GET_DISTRICT_TYPE_OFFSET);
			if (getDistrictType(owner) != -1) return false;
		}
		if (!(flags & 2)) {
			const unsigned short terrain = *(unsigned short*)(plot + 0x2c);
			const bool mountain = terrain <= 0xe && ((0x4924u >> (terrain & 0x1f)) & 1u);
			bool ok = mountain;
			if (!ok) ok = GetGameCoreGlobalAt<Types::IsNaturalWonderFn>(IS_NATURAL_WONDER_OFFSET)(plot);
			if (!ok) {
				auto appeal = (int (*)(void*))(*(void***)plot)[0x28 / 8];
				ok = appeal(plot) >= 2;
			}
			if (!ok) return false;
		}
		int coord[2] = { plotIndex % width, plotIndex / width };
		return FindPark(Edit(), coord) == NULL;
	}

	static int lIsValidParkTileEx(hks::lua_State* L) {
		int playerId = hks::checkinteger(L, 1);
		int plotIndex = hks::checkinteger(L, 2);
		int flags = hks::checkinteger(L, 3);
		hks::pushboolean(L, IsValidParkTileEx(playerId, plotIndex, flags));
		return 1;
	}

	static int lDesignatePark(hks::lua_State* L) {
		int playerId = hks::checkinteger(L, 1);
		// x, y coordinate
		int coord[2] {
			hks::checkinteger(L, 2),
			hks::checkinteger(L, 3)
		};

		DesignatePark(Edit(), playerId, coord);
		return 0;
	}

	static int lFindPark(hks::lua_State* L) {
		int coord[2]{
			hks::checkinteger(L, 1),
			hks::checkinteger(L, 2)
		};

		ParkData* park = FindPark(Edit(), coord);
		if (park != NULL) {
			hks::pushlightuserdata(L, park);
			return 1;
		}
		return 0;
	}

	static int lRestoreVisualState(hks::lua_State* L) {
		if (!hks::isuserdata(L, 1)) {
			hks::error(L, "Type mismatch: Expected userdata pointer to ParkData object!\n");
			return 0;
		}
		auto park = (ParkData*)hks::touserdata(L, 1);

		hks::pushboolean(L, RestoreVisualState(Edit(), park));
		return 1;
	}

	int Register(hks::lua_State* L) {
		std::cout << "Registering NationalParks!\n";
		hks::createtable(L, 0, 6);

		PushLuaMethod(L, lDesignatePark, "lDesignatePark", -2, "DesignatePark");
		PushLuaMethod(L, lDesignateCustom, "lDesignateCustom", -2, "DesignateCustom");
		PushLuaMethod(L, lIsValidParkTile, "lIsValidParkTile", -2, "IsValidParkTile");
		PushLuaMethod(L, lIsValidParkTileEx, "lIsValidParkTileEx", -2, "IsValidParkTileEx");
		PushLuaMethod(L, lFindPark, "lFindPark", -2, "FindPark");
		PushLuaMethod(L, lRestoreVisualState, "lRestoreVisualState", -2, "RestoreVisualState");

		hks::pushlightuserdata(L, Edit());
		hks::setfield(L, -2, "__instance");

		hks::setfield(L, hks::LUA_GLOBAL, "NationalParks");
		return 0;
	}

	// UI-cache Lua state: read-only subset (the UI picks tiles with the game's own validity rule). Mutating calls stay gameplay-only.
	int RegisterUI(hks::lua_State* L) {
		hks::createtable(L, 0, 2);
		PushLuaMethod(L, lIsValidParkTile, "lIsValidParkTile", -2, "IsValidParkTile");
		PushLuaMethod(L, lIsValidParkTileEx, "lIsValidParkTileEx", -2, "IsValidParkTileEx");
		hks::setfield(L, hks::LUA_GLOBAL, "NationalParks");
		return 0;
	}

	void Create() {
		using namespace Runtime;

		DesignatePark = GetGameCoreGlobalAt<Types::DesignatePark>(DESIGNATE_PARK_OFFSET);
		Edit = GetGameCoreGlobalAt<Types::Edit>(GET_NATIONAL_PARKS_OFFSET);
		RestoreVisualState = GetGameCoreGlobalAt<Types::RestoreVisualState>(RESTORE_VISUAL_STATE_OFFSET);
		FindPark = GetGameCoreGlobalAt<Types::FindPark>(FIND_PARK_OFFSET);
	}
}