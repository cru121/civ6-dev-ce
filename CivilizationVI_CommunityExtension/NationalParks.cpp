#include "NationalParks.h"
#include "Runtime.h"
#include "EventSystems.h"
#include "GameProcessor.h"
#include "DevBridge.h"
#include <variant>

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

	// ---- Park rules (0.5.0) ---------------------------------------------------------------------------------------------------
	// A gameplay script pushes the rules with NationalParks.SetRules (read from the NationalParkTypes tables of mod DevCE_ParkRules). While
	// active, the game's own IsValidParkTile (whose only direct caller is GetPossibleParks: UI highlight, the vanilla Naturalist action and the
	// AI all go through it) is replaced by RuleValid below. IsValidParkTile is also called from the UI thread, which must never enter the
	// gameplay Lua state, so the rules are evaluated natively; the Lua processor "NationalParkTileValid" is only consulted on the simulation
	// thread while it holds the GameCore lock (AI and gameplay), so the UI highlight cannot see what only the processor decides.
	struct Rules {
		volatile bool active = false;
		int minTiles = 4, maxTiles = 4;
		int minAppeal = 2, maxAppeal = 1000000;
		int flags = 2 | 4;                      // 1 improvements allowed, 2 mountains always fit, 4 natural wonders always fit
		int districtCount = 0;
		int districts[64] = {};                 // district type indices that may be part of a park (-1 / no district is always fine)
	};
	static Rules gRules;
	static constexpr int RULE_IMPROVEMENTS = 1, RULE_MOUNTAINS = 2, RULE_NATURAL_WONDERS = 4;

	void ResetRules() { gRules.active = false; }

	static bool DistrictAllowed(int districtType) {
		for (int i = 0; i < gRules.districtCount; i++) if (gRules.districts[i] == districtType) return true;
		return false;
	}

	// Native rule set, same structure as the game's rule: owner, improvement, district, terrain/appeal, not in a park.
	static bool RuleValid(NationalParks* np, char* plot, int playerId, int width) {
		using namespace Runtime;
		if (plot == NULL) return false;
		auto getOwnerData = (char* (*)(void*))(*(void***)plot)[0x30 / 8];
		char* owner = getOwnerData(plot);
		if (owner == NULL || (int)*(signed char*)(owner + 0x1c) != playerId) return false;
		if (!(gRules.flags & RULE_IMPROVEMENTS) && *(short*)(plot + 0x44) != -1) return false;
		const int district = GetGameCoreGlobalAt<Types::GetDistrictTypeFn>(GET_DISTRICT_TYPE_OFFSET)(owner);
		if (district != -1) {
			if (!DistrictAllowed(district)) return false;       // an allowed district is an explicit permission: no terrain/appeal check
		} else {
			const unsigned short terrain = *(unsigned short*)(plot + 0x2c);
			bool ok = (gRules.flags & RULE_MOUNTAINS) && terrain <= 0xe && ((0x4924u >> (terrain & 0x1f)) & 1u);
			if (!ok && (gRules.flags & RULE_NATURAL_WONDERS)) ok = GetGameCoreGlobalAt<Types::IsNaturalWonderFn>(IS_NATURAL_WONDER_OFFSET)(plot);
			if (!ok) {
				auto appeal = (int (*)(void*))(*(void***)plot)[0x28 / 8];
				const int a = appeal(plot);
				ok = a >= gRules.minAppeal && a <= gRules.maxAppeal;
			}
			if (!ok) return false;
		}
		int coord[2] = { (int)*(short*)(plot + 0x28), (int)*(short*)(plot + 0x2a) };
		return FindPark(np, coord) == NULL;
	}

	static Types::IsValidParkTileFn base_IsValidParkTile;
	static thread_local bool inProcessor = false;

	static bool Hook_IsValidParkTile(NationalParks* np, void* plotPtr, int playerId) {
		using namespace Runtime;
		if (!gRules.active) return base_IsValidParkTile(np, plotPtr, playerId);
		char* map = (char*)GetGameCoreGlobalAt<Types::EditMapFn>(EDIT_MAP_OFFSET)();
		if (map == NULL || plotPtr == NULL) return false;
		const int width = *(int*)(map + MAP_WIDTH_OFF);
		char* plot = (char*)plotPtr;
		bool valid = RuleValid(np, plot, playerId, width);

		// Optional Lua processor: RegisterProcessor("NationalParkTileValid", function(map) ... end) with map.PlotIndex, map.PlayerId, map.Valid; set
		// map.Valid (0/1) and map.Handled = 1 to override. Only for tiles owned by the player (cheap pre-filter), on the simulation thread
		// holding the GameCore lock, and not from inside the processor itself (re-entrancy guard: the processor gets the native answer back).
		if (!inProcessor && EventSystems::DoesProcessorExist("NationalParkTileValid") && GameProcessor::HasGameCoreLock()) {
			auto getOwnerData = (char* (*)(void*))(*(void***)plot)[0x30 / 8];
			char* owner = getOwnerData(plot);
			if (owner != NULL && (int)*(signed char*)(owner + 0x1c) == playerId) {
				Data::LuaVariantMap variantMap;
				variantMap.emplace("PlotIndex", Data::LuaVariant((int)*(short*)(plot + 0x2a) * width + (int)*(short*)(plot + 0x28)));
				variantMap.emplace("PlayerId", Data::LuaVariant(playerId));
				variantMap.emplace("Valid", Data::LuaVariant(valid ? 1 : 0));
				variantMap.emplace("Handled", Data::LuaVariant(0));
				inProcessor = true;
				const bool handled = GameProcessor::Call("NationalParkTileValid", variantMap);
				inProcessor = false;
				if (handled && std::get<int>(variantMap.at("Handled")) != 0) {
					int coord[2] = { (int)*(short*)(plot + 0x28), (int)*(short*)(plot + 0x2a) };
					// a tile that is already in a park can never be granted
					valid = std::get<int>(variantMap.at("Valid")) != 0 && FindPark(np, coord) == NULL;
				}
			}
		}
		return valid;
	}

	// NationalParks.SetRules(minTiles, maxTiles, minAppeal, maxAppeal, flags, {districtIndex, ...})
	static int lSetRules(hks::lua_State* L) {
		Rules next;
		next.minTiles = hks::checkinteger(L, 1);
		next.maxTiles = hks::checkinteger(L, 2);
		next.minAppeal = hks::checkinteger(L, 3);
		next.maxAppeal = hks::checkinteger(L, 4);
		next.flags = hks::checkinteger(L, 5);
		int n = (int)hks::objlen(L, 6);
		if (n > 64) n = 64;
		for (int i = 1; i <= n; i++) {
			hks::rawgeti(L, 6, i);
			next.districts[next.districtCount++] = hks::checkinteger(L, -1);
			hks::pop(L, 1);
		}
		if (next.minTiles < 1) next.minTiles = 1;
		if (next.maxTiles > MAX_CUSTOM_PLOTS) next.maxTiles = MAX_CUSTOM_PLOTS;
		if (next.maxTiles < next.minTiles) next.maxTiles = next.minTiles;
		gRules.active = false;                     // never let another thread see a half-written set
		gRules.minTiles = next.minTiles; gRules.maxTiles = next.maxTiles;
		gRules.minAppeal = next.minAppeal; gRules.maxAppeal = next.maxAppeal; gRules.flags = next.flags;
		gRules.districtCount = next.districtCount;
		for (int i = 0; i < next.districtCount; i++) gRules.districts[i] = next.districts[i];
		gRules.active = true;
		DevBridge::Log("[DevCEPark] rules set: tiles %d..%d appeal %d..%d flags %d districts %d", gRules.minTiles, gRules.maxTiles, gRules.minAppeal, gRules.maxAppeal, gRules.flags, gRules.districtCount);
		return 0;
	}

	static int lClearRules(hks::lua_State*) { ResetRules(); return 0; }

	// NationalParks.GetRules() -> active, minTiles, maxTiles, minAppeal, maxAppeal, flags   (vanilla values when not active)
	static int lGetRules(hks::lua_State* L) {
		const bool active = gRules.active;
		hks::pushboolean(L, active);
		hks::pushinteger(L, active ? gRules.minTiles : 4);
		hks::pushinteger(L, active ? gRules.maxTiles : 4);
		hks::pushinteger(L, active ? gRules.minAppeal : 2);
		hks::pushinteger(L, active ? gRules.maxAppeal : 1000000);
		hks::pushinteger(L, active ? gRules.flags : (RULE_MOUNTAINS | RULE_NATURAL_WONDERS));
		return 6;
	}

	// Error codes: 0 ok, 1 bad count (1..32), 2 plot outside the map, 3 duplicate, 4 plot already in a park, 5 the game could not create the
	// temporary rhombus record (map edge), 6 internal, 7 the first plot is not inside a city's borders (that city owns the park: tourism,
	// amenities and removal with the city all follow plot #1).
	int DesignateCustom(int playerId, const int* plotIds, int count) {
		using namespace Runtime;
		if (count < 1 || count > MAX_CUSTOM_PLOTS || plotIds == NULL) return 1;
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
		// city id (player byte, city int) of the owner of a plot, or false when no city owns it
		auto cityOf = [&](int plotIndex, int* player, int* city) -> bool {
			char* plot = (char*)getPlotByIndex(map, plotIndex);
			if (plot == NULL) return false;
			auto getOwnerData = (char* (*)(void*))(*(void***)plot)[0x30 / 8];
			char* owner = getOwnerData(plot);
			if (owner == NULL || *(signed char*)(owner + 0x1c) < 0 || *(int*)(owner + 0xc) == -1) return false;
			*player = *(signed char*)(owner + 0x1c);
			*city = *(int*)(owner + 0xc);
			return true;
		};
		int ownerPlayer = 0, ownerCity = 0;
		if (!cityOf(plotIds[0], &ownerPlayer, &ownerCity)) { vis[0x2b] = oldFlag; return 7; }

		// The temporary base must lie in the same city as plot #1 (the park is named after the base plot's city) and have a complete rhombus on the map.
		bool created = false;
		for (int i = 0; i < count && !created; i++) {
			int p = 0, c = 0;
			if (!cityOf(plotIds[i], &p, &c) || p != ownerPlayer || c != ownerCity) continue;
			int base[2] = { plotIds[i] % width, plotIds[i] / width };
			DesignatePark(np, playerId, base);
			created = ParkCount(npc) == before + 1;
		}
		vis[0x2b] = oldFlag;
		if (!created) return 5;

		char* park = *(char**)(npc + PARKS_END) - PARK_STRIDE;
		int** vec = (int**)(park + 8);               // {begin, end, cap}
		int* begin = vec[0];
		if (begin == NULL || vec[1] - begin != 4 || vec[2] - begin < 4) return 6;   // the rhombus always has 4 plots
		varEdit(npc + PARKS_VAR);                    // mark the parks variable as edited (sync / change tracking)
		const int inPlace = count < 4 ? count : 4;
		for (int i = 0; i < inPlace; i++) begin[i] = plotIds[i];
		vec[1] = begin + inPlace;                    // shrink in place when fewer than 4 plots were requested
		if (count > 4) {
			// grow with the game's own vector code (its allocator frees the buffer later); it updates begin/end/cap itself
			auto insert = GetGameCoreGlobalAt<void (*)(void* vec, int* position, const int* value)>(VECTOR_INT_DO_INSERT_VALUE_OFFSET);
			for (int i = 4; i < count; i++) {
				// The game only calls DoInsertValue when the vector is FULL (it then reallocates); with spare room it appends inline.
				// DoInsertValue with position == end and spare room does a memmove of size -4 (heap corruption, seen live 2026-10-25).
				if (vec[1] < vec[2]) { *vec[1] = plotIds[i]; vec[1]++; }
				else insert(vec, vec[1], &plotIds[i]);
			}
		}
		RestoreVisualState(np, (ParkData*)park);     // ONE ADDED event with the final list + per-plot visibility events
		return 0;
	}

	static int lDesignateCustom(hks::lua_State* L) {
		int playerId = hks::checkinteger(L, 1);
		int n = (int)hks::objlen(L, 2);
		if (n < 1 || n > MAX_CUSTOM_PLOTS) {
			hks::pushinteger(L, 1);
			return 1;
		}
		int plots[MAX_CUSTOM_PLOTS];
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
		PushLuaMethod(L, lSetRules, "lSetRules", -2, "SetRules");
		PushLuaMethod(L, lClearRules, "lClearRules", -2, "ClearRules");
		PushLuaMethod(L, lGetRules, "lGetRules", -2, "GetRules");

		hks::pushlightuserdata(L, Edit());
		hks::setfield(L, -2, "__instance");

		hks::setfield(L, hks::LUA_GLOBAL, "NationalParks");
		return 0;
	}

	// UI-cache Lua state: read-only subset (the UI picks tiles with the game's own validity rule). Mutating calls stay gameplay-only.
	int RegisterUI(hks::lua_State* L) {
		hks::createtable(L, 0, 3);
		PushLuaMethod(L, lIsValidParkTile, "lIsValidParkTile", -2, "IsValidParkTile");
		PushLuaMethod(L, lIsValidParkTileEx, "lIsValidParkTileEx", -2, "IsValidParkTileEx");
		PushLuaMethod(L, lGetRules, "lGetRules", -2, "GetRules");
		hks::setfield(L, hks::LUA_GLOBAL, "NationalParks");
		return 0;
	}

	void Create() {
		using namespace Runtime;

		DesignatePark = GetGameCoreGlobalAt<Types::DesignatePark>(DESIGNATE_PARK_OFFSET);
		Edit = GetGameCoreGlobalAt<Types::Edit>(GET_NATIONAL_PARKS_OFFSET);
		RestoreVisualState = GetGameCoreGlobalAt<Types::RestoreVisualState>(RESTORE_VISUAL_STATE_OFFSET);
		FindPark = GetGameCoreGlobalAt<Types::FindPark>(FIND_PARK_OFFSET);

		CreateHook(GetGameCoreGlobalAt<Types::IsValidParkTileFn>(IS_VALID_PARK_TILE_OFFSET), &Hook_IsValidParkTile, &base_IsValidParkTile);
	}
}