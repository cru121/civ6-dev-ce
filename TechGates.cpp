// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3.0 (see LICENSE.txt).
#include "TechGates.h"
#include "Runtime.h"
#include "DevBridge.h"
#include "HavokScript.h"
#include <Windows.h>
#include <climits>
#include <cstring>
#include <string>
#include <vector>

namespace TechGates {
	enum Tree { TECH = 0, CIVIC = 1 };

	typedef void(__fastcall* BuildCollectionFn)(void* builder, void* collection);
	typedef bool(__fastcall* CanFn)(void* owner, int item);
	typedef bool(__fastcall* ChangeProgressFn)(void* owner, int item, int* amount);
	typedef void(__fastcall* SetQueueFn)(void* owner, void* queue, int mode);
	typedef void(__fastcall* SetItemFn)(void* owner, int item, int mode);
	typedef int(__cdecl* LuaFn)(hks::lua_State* L);
	typedef void(__fastcall* TechEvaluateFn)(void* analysis);
	typedef void(__fastcall* CivicExecuteFn)(void* analysis, unsigned arg);
	typedef bool(__fastcall* HasFn)(void* owner, int item);
	typedef void* (__fastcall* GetGameFn)();
	typedef const int* const* (__fastcall* PrereqsFn)(void* game, int item);   // -> { begin, end }
	typedef int(__fastcall* CostFn)(void* game, int item, bool flag);
	typedef int(__fastcall* IndexFn)(int indexOrHash);
	typedef void* (__fastcall* EditPlayerFn)(int player);
	typedef int(__fastcall* EventSendFn)(uint64_t type, int player);
	typedef void* (__fastcall* LuaInstanceFn)(hks::lua_State* L, int index, bool check);
	typedef int(__fastcall* NumberOrHashFn)(hks::lua_State* L, int index);
	typedef void* const* (__fastcall* DefinitionsFn)();
	typedef void* (__fastcall* PropertyEditFn)(void* object);
	typedef const uint8_t* (__fastcall* VariantGetFn)(void* map, uint32_t key);
	typedef uint32_t(__fastcall* MakeHashFn)(const char* s);
	// Database::IDatabaseConnection1 / IDatabaseQuery1 (as CitySpecialists uses them)
	typedef void* (__fastcall* DbPrepareFn)(void* connection, const char* sql, int length);   // +0x18
	typedef void(__fastcall* DbReleaseFn)(void* query);                                        // +0x08
	typedef bool(__fastcall* DbStepFn)(void* query);                                           // +0x20
	typedef void(__fastcall* DbResetFn)(void* query);                                          // +0x38
	typedef const char* (__fastcall* DbTextFn)(void* query, int column);                       // +0xf0
	typedef int(__fastcall* DbIntFn)(void* query, int column);                                 // +0x110

	constexpr uintptr_t MAKE_HASH_OFFSET = 0x606270;
	constexpr int MAX_DEPTH = 256;
	constexpr int UNREACHABLE_VALUE = INT_MIN / 8;   // an AI target we marked unreachable sorts below every real one

	struct Opener { int tree; int item; int amount; };
	struct Gate { uint32_t key; std::vector<Opener> openers; };
	static std::vector<Gate> gGateKeys;                 // one per distinct property
	static std::vector<std::vector<int>> gItemGates[2];  // per tree, per item: indices into gGateKeys
	static volatile bool gAny = false;
	static bool gOk = false;

	static BuildCollectionFn orig_BuildTechPrereqs;
	static CanFn orig_CanResearch, orig_CanProgress;
	static ChangeProgressFn orig_ChangeResearch, orig_ChangeCulture;
	static SetQueueFn orig_SetResearchQueue, orig_SetCivicQueue;
	static SetItemFn orig_SetResearchTech, orig_SetCivic;
	static LuaFn orig_lGetResearchPath, orig_lGetCivicPath;
	static TechEvaluateFn orig_TechEvaluate;
	static CivicExecuteFn orig_CivicExecute;
	static HasFn HasTech, HasCivic;
	static GetGameFn GameTechs, GameCulture;
	static PrereqsFn TechPrereqs, CivicPrereqs;
	static CostFn TechCost, CivicCost;
	static IndexFn TechIndex, CivicIndex;
	static EditPlayerFn EditPlayer;
	static EventSendFn EventSend;
	static LuaInstanceFn TechsLuaInstance, CultureLuaInstance;
	static NumberOrHashFn NumberOrHash;
	static DefinitionsFn TechDefinitions, CivicDefinitions;
	static PropertyEditFn PropertyEdit;
	static MakeHashFn MakeHash;

	struct Check { const char* name; uintptr_t rva; uint8_t bytes[16]; size_t n; };
	static const Check kChecks[] = {
		{ "BuildCollection<TechnologyPrereq>", BUILD_TECH_PREREQS_OFFSET, { 0x40, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x89, 0x5c, 0x24, 0x30, 0x41, 0x83, 0xc8, 0xff, 0x48 }, 16 },
		{ "Techs::CanResearch", CAN_RESEARCH_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x56, 0x48, 0x83, 0xec, 0x20, 0x8b, 0xda, 0x48, 0x8b, 0xf1, 0x83 }, 16 },
		{ "Culture::CanProgress", CAN_PROGRESS_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x56, 0x48, 0x83, 0xec, 0x20, 0x8b, 0xda, 0x48, 0x8b, 0xf1, 0x83 }, 16 },
		{ "Techs::ChangeResearchProgress", CHANGE_RESEARCH_PROGRESS_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57 }, 16 },
		{ "Culture::ChangeCulturalProgress", CHANGE_CULTURAL_PROGRESS_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57 }, 16 },
		{ "Techs::SetResearchingTech(queue)", SET_RESEARCHING_QUEUE_OFFSET, { 0x48, 0x8b, 0xc4, 0x57, 0x41, 0x55, 0x48, 0x81, 0xec, 0x88, 0x00, 0x00, 0x00, 0x48, 0x89, 0x58 }, 16 },
		{ "Techs::SetResearchingTech(tech)", SET_RESEARCHING_TECH_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xf9 }, 16 },
		{ "Culture::SetProgressingCivic(queue)", SET_PROGRESSING_QUEUE_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57 }, 16 },
		{ "Culture::SetProgressingCivic(civic)", SET_PROGRESSING_CIVIC_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xf9 }, 16 },
		{ "IPlayerTechs::lGetResearchPath", L_GET_RESEARCH_PATH_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57 }, 16 },
		{ "IPlayerCulture::lGetCivicPath", L_GET_CIVIC_PATH_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57 }, 16 },
		{ "AI::TechTreeAnalysis::Evaluate", TECH_ANALYSIS_EVALUATE_OFFSET, { 0x48, 0x89, 0x4c, 0x24, 0x08, 0x55, 0x53, 0x41, 0x55, 0x41, 0x57, 0x48, 0x8d, 0x6c, 0x24, 0xf8 }, 16 },
		{ "AI::CivicAnalysis::Execute", CIVIC_ANALYSIS_EXECUTE_OFFSET, { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x48, 0x08, 0x55, 0x56, 0x41, 0x56, 0x48, 0x8d, 0x68, 0xb8, 0x48 }, 16 },
		{ "HasTech", HAS_TECH_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x63, 0xda, 0x48, 0x8b, 0xf9 }, 16 },
		{ "HasCivic", HAS_CIVIC_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x8b, 0xda, 0x48, 0x8b, 0xf9, 0x83 }, 16 },
		{ "Game::Techs::Get", GAME_TECHS_OFFSET, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x8b, 0x1d }, 8 },
		{ "Game::Culture::Get", GAME_CULTURE_OFFSET, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x8b, 0x1d }, 8 },
		{ "Game::Techs prereqs", TECH_PREREQS_OFFSET, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0xe8 }, 10 },
		{ "Game::Culture::GetCivicPrerequisites", CIVIC_PREREQS_OFFSET, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0xe8 }, 10 },
		{ "Game::Techs::GetTechCost", TECH_COST_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57 }, 16 },
		{ "Game::Culture::GetCivicCost", CIVIC_COST_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57 }, 16 },
		{ "tech index", TECH_INDEX_OFFSET, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x8b, 0xd9, 0xe8 }, 9 },
		{ "civic index", CIVIC_INDEX_OFFSET, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x8b, 0xd9, 0xe8 }, 9 },
		{ "Globals::EditPlayer", EDIT_PLAYER_OFFSET, { 0x8b, 0xd1, 0x48, 0x8b, 0x0d }, 5 },
		{ "Reporting::Event::Send(type, player)", EVENT_SEND_OFFSET, { 0x48, 0x89, 0x4c, 0x24, 0x08, 0x53, 0x48, 0x81, 0xec, 0x80, 0x00, 0x00, 0x00, 0x80, 0x3d }, 15 },
		{ "IPlayerTechs GetInstance", TECHS_LUA_INSTANCE_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48 }, 16 },
		{ "IPlayerCulture GetInstance", CULTURE_LUA_INSTANCE_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48 }, 16 },
		{ "Lua::Utility::GetNumberOrHash", NUMBER_OR_HASH_OFFSET, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x8b, 0xfa, 0x83 }, 16 },
		{ "GetTechnologyDefinitions", TECH_DEFINITIONS_OFFSET, { 0x48, 0x83, 0xec, 0x28, 0xe8 }, 5 },
		{ "GetCivicDefinitions", CIVIC_DEFINITIONS_OFFSET, { 0x48, 0x83, 0xec, 0x28, 0xe8 }, 5 },
		{ "FAutoVariable<VariantMap>::edit", PROPERTY_EDIT_OFFSET, { 0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0x41, 0x08, 0x48, 0x8b, 0xd9, 0x48, 0x85, 0xc0 }, 16 },
		{ "Utilities::MakeHash", MAKE_HASH_OFFSET, { 0x48, 0x8b, 0xd1, 0x41, 0x83, 0xc8, 0xff, 0x48 }, 8 },
		// layout: queue count / data (ChangeCurrent*Progress), player properties (IPlayer::lSetProperty), player techs / culture (cache CanResearch / CanProgress)
		{ "ChangeCurrentResearchProgress: queue count +0xf0", 0x333624, { 0x83, 0xb9, 0xf0, 0x00, 0x00, 0x00, 0x00 }, 7 },
		{ "ChangeCurrentResearchProgress: queue data +0xe0", 0x333645, { 0x48, 0x8b, 0x81, 0xe0, 0x00, 0x00, 0x00 }, 7 },
		{ "ChangeCurrentCulturalProgress: queue count +0x418", 0x27c074, { 0x83, 0xb9, 0x18, 0x04, 0x00, 0x00, 0x00 }, 7 },
		{ "ChangeCurrentCulturalProgress: queue data +0x408", 0x27c095, { 0x48, 0x8b, 0x81, 0x08, 0x04, 0x00, 0x00 }, 7 },
		{ "IPlayer::lSetProperty: player +0x518", 0x6ee405, { 0x48, 0x8d, 0x88, 0x18, 0x05, 0x00, 0x00 }, 7 },
		{ "cache CanResearch: player techs +0x718", 0x0c3774, { 0x48, 0x8b, 0x88, 0x18, 0x07, 0x00, 0x00 }, 7 },
		{ "cache CanProgress: player culture +0x6f0", 0x0b2499, { 0x48, 0x8b, 0x88, 0xf0, 0x06, 0x00, 0x00 }, 7 },
		{ "SetResearchingTech: owner +0x90, queue event", 0x336554, { 0x48, 0x8b, 0x87, 0x90, 0x00, 0x00, 0x00, 0x48, 0xc7, 0x44, 0x24, 0x20, 0x05, 0xa3, 0x78, 0x43 }, 16 },
		{ "SetProgressingCivic: owner +0x90, queue event", 0x2858dc, { 0x48, 0x8b, 0x87, 0x90, 0x00, 0x00, 0x00, 0xc7, 0x44, 0x24, 0x20, 0xa3, 0x69, 0x27, 0xac }, 15 },
	};

	// ---- players and trees ------------------------------------------------------------------------------------------------
	static int ReadPlayerProperty(void* player, uint32_t key) {
		void* map = (char*)player + PLAYER_PROPERTIES + 0x10;
		const uint8_t* v = ((VariantGetFn)(*(void***)map)[0x60 / 8])(map, key);
		if (v == nullptr) return 0;
		const uint16_t type = *(const uint16_t*)v;
		const uint32_t flags = *(const uint32_t*)(v + 0x14);
		const bool isNumber = ((flags >> 29) & 1) == 0 && type <= 0x15 && ((0x30002eu >> type) & 1) != 0;
		return isNumber ? *(const int*)(v + 8) : 0;
	}

	static void* PlayerOf(void* owner) { return *(void**)((char*)owner + OWNER_PLAYER); }
	static void* OwnerOf(void* player, int tree) { return *(void**)((char*)player + (tree == TECH ? PLAYER_TECHS : PLAYER_CULTURE)); }
	static int PlayerId(void* player) { return *(int*)((char*)player + PLAYER_ID); }

	static int Count(int tree) {
		void* const* defs = tree == TECH ? TechDefinitions() : CivicDefinitions();
		return (int)(((uintptr_t)defs[1] - (uintptr_t)defs[0]) / 8);
	}
	static bool Has(void* player, int tree, int item) {
		return tree == TECH ? HasTech(OwnerOf(player, TECH), item) : HasCivic(OwnerOf(player, CIVIC), item);
	}
	static int Cost(int tree, int item) { return tree == TECH ? TechCost(GameTechs(), item, false) : CivicCost(GameCulture(), item, false); }
	static const int* const* Prereqs(int tree, int item) { return tree == TECH ? TechPrereqs(GameTechs(), item) : CivicPrereqs(GameCulture(), item); }

	static const std::vector<int>* GatesOf(int tree, int item) {
		const auto& v = gItemGates[tree];
		if (item < 0 || item >= (int)v.size() || v[item].empty()) return nullptr;
		return &v[item];
	}

	// True when every gate of the item is open now (property >= 1).
	static bool GatesOpen(void* player, int tree, int item) {
		const std::vector<int>* gates = GatesOf(tree, item);
		if (gates == nullptr) return true;
		for (int g : *gates) if (ReadPlayerProperty(player, gGateKeys[g].key) < 1) return false;
		return true;
	}

	// ---- paths ------------------------------------------------------------------------------------------------------------
	// Items to take, in order, to reach a target; `taken` = items already in the plan (each opener there adds its Amount).
	struct Plan {
		void* player; int tree;
		std::vector<uint8_t> taken, visiting;
		std::vector<int> order;
		long long cost = 0;
	};

	static Plan NewPlan(void* player, int tree) {
		Plan p; p.player = player; p.tree = tree;
		const int n = Count(tree);
		p.taken.assign(n, 0); p.visiting.assign(n, 0);
		return p;
	}

	static bool GateOpenIn(const Plan& p, int g) {
		long long sum = ReadPlayerProperty(p.player, gGateKeys[g].key);
		for (const Opener& o : gGateKeys[g].openers)
			if (o.tree == p.tree && o.item >= 0 && o.item < (int)p.taken.size() && p.taken[o.item]) sum += o.amount;
		return sum >= 1;
	}

	static bool Visit(Plan& p, int item, int depth) {
		if (item < 0 || item >= (int)p.taken.size() || depth > MAX_DEPTH) return false;
		if (p.taken[item] || Has(p.player, p.tree, item)) return true;
		if (p.visiting[item]) return false;   // a loop in the data
		p.visiting[item] = 1;
		const int* const* pre = Prereqs(p.tree, item);
		if (pre != nullptr) for (const int* it = pre[0]; it != pre[1]; ++it) if (*it >= 0 && !Visit(p, *it, depth + 1)) return false;
		if (const std::vector<int>* gates = GatesOf(p.tree, item)) {
			for (int g : *gates) {
				if (GateOpenIn(p, g)) continue;
				// The cheapest opener in this tree (by the cost of what it adds to the plan).
				bool found = false; Plan best;
				for (const Opener& o : gGateKeys[g].openers) {
					if (o.tree != p.tree || o.amount <= 0) continue;
					Plan trial = p;
					if (!Visit(trial, o.item, depth + 1) || !GateOpenIn(trial, g)) continue;
					if (!found || trial.cost < best.cost) { best = std::move(trial); found = true; }
				}
				if (!found) return false;   // only civic / policy openers (or none): no way through in this tree
				p = std::move(best);
			}
		}
		p.visiting[item] = 0;
		p.taken[item] = 1;
		p.order.push_back(item);
		p.cost += Cost(p.tree, item);
		return true;
	}

	// The path to `target` (prereqs first, without items the player has), or false when there is none.
	static bool FindPath(void* player, int tree, int target, std::vector<int>& out) {
		Plan p = NewPlan(player, tree);
		if (!Visit(p, target, 0)) return false;
		out = std::move(p.order);
		return true;
	}

	// ---- queues -----------------------------------------------------------------------------------------------------------
	static void* QueueValue(void* owner, int tree) { return (char*)owner + (tree == TECH ? TECHS_QUEUE : CULTURE_QUEUE) + 0x10; }
	static unsigned QueueCount(const void* queue) { return *(const unsigned*)((const char*)queue + QUEUE_COUNT); }
	static const int* QueueData(const void* queue) { return *(const int* const*)((const char*)queue + QUEUE_DATA); }

	// Clears the queue as SetResearchingTech / SetProgressingCivic do, with their "queue changed" report (the UI and the
	// choose-research / choose-civic prompt follow it).
	static void ClearQueue(void* owner, int tree) {
		void* value = PropertyEdit((char*)owner + (tree == TECH ? TECHS_QUEUE : CULTURE_QUEUE));
		if (value == nullptr) return;
		*(unsigned*)((char*)value + QUEUE_COUNT) = 0;
		EventSend(tree == TECH ? RESEARCH_QUEUE_CHANGED_EVENT : CIVIC_QUEUE_CHANGED_EVENT, PlayerId(PlayerOf(owner)));
	}

	// How many leading items of `items` can stay: each must have its gates open now or opened by an item before it
	// (`before` = items ahead of them, e.g. the current queue for an append).
	static unsigned KeepCount(void* player, int tree, const int* before, unsigned numBefore, const int* items, unsigned n) {
		Plan p = NewPlan(player, tree);
		for (unsigned i = 0; i < numBefore; i++) if (before[i] >= 0 && before[i] < (int)p.taken.size()) p.taken[before[i]] = 1;
		for (unsigned i = 0; i < n; i++) {
			const int item = items[i];
			if (const std::vector<int>* gates = GatesOf(tree, item))
				for (int g : *gates) if (!GateOpenIn(p, g)) return i;
			if (item >= 0 && item < (int)p.taken.size()) p.taken[item] = 1;
		}
		return n;
	}

	static void SetQueue(int tree, SetQueueFn orig, void* owner, void* queue, int mode) {
		const unsigned n = queue != nullptr ? QueueCount(queue) : 0;
		if (!gAny || n == 0) { orig(owner, queue, mode); return; }
		const void* current = QueueValue(owner, tree);
		const bool append = mode == 2;
		const unsigned keep = KeepCount(PlayerOf(owner), tree, append ? QueueData(current) : nullptr, append ? QueueCount(current) : 0, QueueData(queue), n);
		if (keep == 0) return;   // nothing can be taken: keep the current research
		*(unsigned*)((char*)queue + QUEUE_COUNT) = keep;   // the caller's temporary queue; put back after
		orig(owner, queue, mode);
		*(unsigned*)((char*)queue + QUEUE_COUNT) = n;
	}

	static void SetItem(int tree, SetItemFn orig, void* owner, int arg, int mode) {
		const int item = !gAny ? -1 : tree == TECH ? TechIndex(arg) : CivicIndex(arg);   // the game takes an index or a hash
		if (gAny && GatesOf(tree, item) != nullptr) {
			const void* current = QueueValue(owner, tree);
			const bool append = mode == 2;
			if (KeepCount(PlayerOf(owner), tree, append ? QueueData(current) : nullptr, append ? QueueCount(current) : 0, &item, 1) == 0) return;
		}
		orig(owner, arg, mode);
	}

	// Progress for the head of the queue whose gate shut since it was queued (a policy unslotted): clear the queue, so the
	// amount goes to overflow as with an empty queue and the player chooses again. Progress already made stays on the item.
	static bool ChangeProgress(int tree, ChangeProgressFn orig, void* owner, int item, int* amount) {
		if (gAny && item >= 0) {
			const void* queue = QueueValue(owner, tree);
			if (QueueCount(queue) > 0 && QueueData(queue)[0] == item && !GatesOpen(PlayerOf(owner), tree, item)) {
				ClearQueue(owner, tree);
				return orig(owner, -1, amount);
			}
		}
		return orig(owner, item, amount);
	}

	static bool __fastcall Hook_CanResearch(void* techs, int tech) {
		const bool r = orig_CanResearch(techs, tech);
		return r && (!gAny || GatesOpen(PlayerOf(techs), TECH, tech));
	}
	static bool __fastcall Hook_CanProgress(void* culture, int civic) {
		const bool r = orig_CanProgress(culture, civic);
		return r && (!gAny || GatesOpen(PlayerOf(culture), CIVIC, civic));
	}
	static bool __fastcall Hook_ChangeResearch(void* techs, int tech, int* amount) { return ChangeProgress(TECH, orig_ChangeResearch, techs, tech, amount); }
	static bool __fastcall Hook_ChangeCulture(void* culture, int civic, int* amount) { return ChangeProgress(CIVIC, orig_ChangeCulture, culture, civic, amount); }
	static void __fastcall Hook_SetResearchQueue(void* techs, void* queue, int mode) { SetQueue(TECH, orig_SetResearchQueue, techs, queue, mode); }
	static void __fastcall Hook_SetCivicQueue(void* culture, void* queue, int mode) { SetQueue(CIVIC, orig_SetCivicQueue, culture, queue, mode); }
	static void __fastcall Hook_SetResearchTech(void* techs, int tech, int mode) { SetItem(TECH, orig_SetResearchTech, techs, tech, mode); }
	static void __fastcall Hook_SetCivic(void* culture, int civic, int mode) { SetItem(CIVIC, orig_SetCivic, culture, civic, mode); }

	// ---- Lua: GetResearchPath / GetCivicPath ------------------------------------------------------------------------------
	// As the game's: (self, tech index or hash) -> { [1] = first item, ... } or nothing. Ours follows gates through openers.
	static int LuaPath(hks::lua_State* L, int tree) {
		void* cache = tree == TECH ? TechsLuaInstance(L, 1, true) : CultureLuaInstance(L, 1, true);
		if (cache == nullptr || hks::gettop(L) < 2) return 0;
		const int arg = NumberOrHash(L, 2);
		const int target = tree == TECH ? TechIndex(arg) : CivicIndex(arg);
		const int playerId = **(int**)cache;
		void* player = EditPlayer(playerId);
		if (player == nullptr || target < 0) return 0;
		std::vector<int> path;
		if (!FindPath(player, tree, target, path) || path.empty()) return 0;
		hks::createtable(L, (int)path.size(), 0);
		for (size_t i = 0; i < path.size(); i++) {
			hks::pushinteger(L, (int)i + 1);
			hks::pushinteger(L, path[i]);
			hks::settable(L, -3);
		}
		return 1;
	}
	static int __cdecl Hook_lGetResearchPath(hks::lua_State* L) { return gAny ? LuaPath(L, TECH) : orig_lGetResearchPath(L); }
	static int __cdecl Hook_lGetCivicPath(hks::lua_State* L) { return gAny ? LuaPath(L, CIVIC) : orig_lGetCivicPath(L); }

	// ---- AI ---------------------------------------------------------------------------------------------------------------
	// The analysis looks up to 3 prereq levels back from a target and picks a next step; for a gated target that step can be
	// one the player cannot take, and QueueNextTech / QueueNextCivic then research a random item. Give it the first step of our
	// path instead, or mark the target unreachable (turns -1, lowest value: the AI's best-target choice puts it last).
	static void FixAnalysis(void* player, int tree, int target, int* step, int* turns, int* value, int* valueCopy) {
		if (player == nullptr || *turns < 0) return;
		void* owner = OwnerOf(player, tree);
		const bool can = tree == TECH ? Hook_CanResearch(owner, *step) : Hook_CanProgress(owner, *step);
		if (can) return;
		std::vector<int> path;
		if (FindPath(player, tree, target, path) && !path.empty()) {
			const bool first = tree == TECH ? Hook_CanResearch(owner, path[0]) : Hook_CanProgress(owner, path[0]);
			if (first) { *step = path[0]; return; }
		}
		*turns = -1;
		*value = UNREACHABLE_VALUE;
		*valueCopy = UNREACHABLE_VALUE;
	}

	// TechTreeAnalysis: +0 target, +4 next step, +8 Player*, +0x10 value, +0x14 value used to rank, +0x18 researched, +0x1c turns (-1 = unreachable)
	static void __fastcall Hook_TechEvaluate(void* a) {
		orig_TechEvaluate(a);
		if (!gAny) return;
		char* p = (char*)a;
		if (p[0x18] != 0) return;
		FixAnalysis(*(void**)(p + 8), TECH, *(int*)p, (int*)(p + 4), (int*)(p + 0x1c), (int*)(p + 0x10), (int*)(p + 0x14));
	}

	// CivicAnalysis: +0 Player*, +8 target, +0xc completed, +0x10 value, +0x14 value used to rank, +0x18 turns (-1 = unreachable), +0x1c next step
	static void __fastcall Hook_CivicExecute(void* a, unsigned arg) {
		orig_CivicExecute(a, arg);
		if (!gAny) return;
		char* p = (char*)a;
		if (*(int*)(p + 8) == -1 || *(void**)p == nullptr || p[0xc] != 0) return;
		FixAnalysis(*(void**)p, CIVIC, *(int*)(p + 8), (int*)(p + 0x1c), (int*)(p + 0x18), (int*)(p + 0x10), (int*)(p + 0x14));
	}

	// ---- database ---------------------------------------------------------------------------------------------------------
	template <typename T> static T Slot(void* object, size_t offset) { return (T)(*(void***)object)[offset / 8]; }

	static int GateIndex(uint32_t key) {
		for (size_t i = 0; i < gGateKeys.size(); i++) if (gGateKeys[i].key == key) return (int)i;
		return -1;
	}

	// Rows of (item index, property name[, amount]) from a query; item index = position in the table by rowid, as the game numbers them.
	template <typename F> static int Rows(void* connection, const char* sql, F each) {
		void* q = Slot<DbPrepareFn>(connection, 0x18)(connection, sql, -1);
		if (q == nullptr) return -1;   // a table is missing
		int n = 0;
		while (Slot<DbStepFn>(q, 0x20)(q)) {
			const int item = Slot<DbIntFn>(q, 0x110)(q, 0);
			const char* name = Slot<DbTextFn>(q, 0xf0)(q, 1);
			const int amount = Slot<DbIntFn>(q, 0x110)(q, 2);
			if (name == nullptr || name[0] == '\0') continue;
			each(item, MakeHash(name), amount);
			n++;
		}
		Slot<DbResetFn>(q, 0x38)(q);
		Slot<DbReleaseFn>(q, 0x08)(q);
		return n;
	}

	static void LoadTables(void* connection) {
		gAny = false;
		gGateKeys.clear();
		for (auto& v : gItemGates) v.clear();
		if (connection == nullptr) return;
		static const char* const kGateSql[2] = {
			"SELECT (SELECT count(*) FROM Technologies x WHERE x.rowid < t.rowid), g.Property, 0 "
			"FROM DevCE_TechnologyGates g JOIN Technologies t ON t.TechnologyType = g.TechnologyType",
			"SELECT (SELECT count(*) FROM Civics x WHERE x.rowid < c.rowid), g.Property, 0 "
			"FROM DevCE_CivicGates g JOIN Civics c ON c.CivicType = g.CivicType",
		};
		static const char* const kCountSql[2] = { "SELECT count(*) FROM Technologies", "SELECT count(*) FROM Civics" };
		int gates[2] = { 0, 0 };
		for (int tree = 0; tree < 2; tree++) {
			void* q = Slot<DbPrepareFn>(connection, 0x18)(connection, kCountSql[tree], -1);
			if (q == nullptr) continue;
			const int n = Slot<DbStepFn>(q, 0x20)(q) ? Slot<DbIntFn>(q, 0x110)(q, 0) : 0;
			Slot<DbResetFn>(q, 0x38)(q);
			Slot<DbReleaseFn>(q, 0x08)(q);
			gItemGates[tree].assign(n > 0 ? n : 0, {});
			gates[tree] = Rows(connection, kGateSql[tree], [&](int item, uint32_t key, int) {
				if (item < 0 || item >= (int)gItemGates[tree].size()) return;
				int g = GateIndex(key);
				if (g < 0) { gGateKeys.push_back({ key, {} }); g = (int)gGateKeys.size() - 1; }
				gItemGates[tree][item].push_back(g);
			});
		}
		if (gGateKeys.empty()) {
			DevBridge::Log("[TechGates] no gate rows (DevCE_TechnologyGates %s, DevCE_CivicGates %s): tech and civic gates off",
				gates[TECH] < 0 ? "missing" : "empty", gates[CIVIC] < 0 ? "missing" : "empty");
			return;
		}
		// Openers: TechnologyModifiers / CivicModifiers rows that add to a gate's property on the owner.
		static const char* const kOpenerSql[2] = {
			"SELECT (SELECT count(*) FROM Technologies x WHERE x.rowid < t.rowid), k.Value, CAST(IFNULL(a.Value, 0) AS INTEGER) "
			"FROM TechnologyModifiers s JOIN Technologies t ON t.TechnologyType = s.TechnologyType "
			"JOIN Modifiers m ON m.ModifierId = s.ModifierId JOIN DynamicModifiers d ON d.ModifierType = m.ModifierType "
			"JOIN ModifierArguments k ON k.ModifierId = m.ModifierId AND k.Name = 'Key' "
			"LEFT JOIN ModifierArguments a ON a.ModifierId = m.ModifierId AND a.Name = 'Amount' "
			"WHERE d.EffectType = 'EFFECT_ADJUST_PLAYER_PROPERTY' AND d.CollectionType = 'COLLECTION_OWNER'",
			"SELECT (SELECT count(*) FROM Civics x WHERE x.rowid < c.rowid), k.Value, CAST(IFNULL(a.Value, 0) AS INTEGER) "
			"FROM CivicModifiers s JOIN Civics c ON c.CivicType = s.CivicType "
			"JOIN Modifiers m ON m.ModifierId = s.ModifierId JOIN DynamicModifiers d ON d.ModifierType = m.ModifierType "
			"JOIN ModifierArguments k ON k.ModifierId = m.ModifierId AND k.Name = 'Key' "
			"LEFT JOIN ModifierArguments a ON a.ModifierId = m.ModifierId AND a.Name = 'Amount' "
			"WHERE d.EffectType = 'EFFECT_ADJUST_PLAYER_PROPERTY' AND d.CollectionType = 'COLLECTION_OWNER'",
		};
		int openers[2] = { 0, 0 };
		for (int tree = 0; tree < 2; tree++) {
			Rows(connection, kOpenerSql[tree], [&](int item, uint32_t key, int amount) {
				const int g = GateIndex(key);
				if (g < 0 || amount <= 0) return;
				gGateKeys[g].openers.push_back({ tree, item, amount });
				openers[tree]++;
			});
		}
		gAny = true;
		DevBridge::Log("[TechGates] ON: %d tech gate rows, %d civic gate rows, %d properties; openers: %d techs, %d civics",
			gates[TECH], gates[CIVIC], (int)gGateKeys.size(), openers[TECH], openers[CIVIC]);
	}

	static void SafeLoadTables(void* connection) {
		__try { LoadTables(connection); }
		__except (EXCEPTION_EXECUTE_HANDLER) { gAny = false; DevBridge::Log("[TechGates] reading the tables faulted: tech and civic gates off"); }
	}

	static void __fastcall Hook_BuildTechPrereqs(void* builder, void* collection) {
		orig_BuildTechPrereqs(builder, collection);
		SafeLoadTables(builder != nullptr ? *(void**)builder : nullptr);
	}

	// ---- setup ------------------------------------------------------------------------------------------------------------
	template <typename T> static bool Hook(uintptr_t rva, void* detour, T* original) {
		LPVOID target = (LPVOID)(Runtime::GameCoreAddress + rva);
		if (MH_CreateHook(target, detour, (LPVOID*)original) != MH_OK) return false;
		return MH_EnableHook(target) == MH_OK;
	}

	void Create() {
		if (gOk) return;   // the hooks stay installed when Dev CE starts again in the same process
		const uintptr_t b = Runtime::GameCoreAddress;
		for (const Check& c : kChecks) {
			if (memcmp((void*)(b + c.rva), c.bytes, c.n) != 0) {
				DevBridge::Log("[TechGates] %s has unexpected bytes at 0x%llx: tech and civic gates not available", c.name, (unsigned long long)c.rva);
				return;
			}
		}
		HasTech = (HasFn)(b + HAS_TECH_OFFSET);
		HasCivic = (HasFn)(b + HAS_CIVIC_OFFSET);
		GameTechs = (GetGameFn)(b + GAME_TECHS_OFFSET);
		GameCulture = (GetGameFn)(b + GAME_CULTURE_OFFSET);
		TechPrereqs = (PrereqsFn)(b + TECH_PREREQS_OFFSET);
		CivicPrereqs = (PrereqsFn)(b + CIVIC_PREREQS_OFFSET);
		TechCost = (CostFn)(b + TECH_COST_OFFSET);
		CivicCost = (CostFn)(b + CIVIC_COST_OFFSET);
		TechIndex = (IndexFn)(b + TECH_INDEX_OFFSET);
		CivicIndex = (IndexFn)(b + CIVIC_INDEX_OFFSET);
		EditPlayer = (EditPlayerFn)(b + EDIT_PLAYER_OFFSET);
		EventSend = (EventSendFn)(b + EVENT_SEND_OFFSET);
		TechsLuaInstance = (LuaInstanceFn)(b + TECHS_LUA_INSTANCE_OFFSET);
		CultureLuaInstance = (LuaInstanceFn)(b + CULTURE_LUA_INSTANCE_OFFSET);
		NumberOrHash = (NumberOrHashFn)(b + NUMBER_OR_HASH_OFFSET);
		TechDefinitions = (DefinitionsFn)(b + TECH_DEFINITIONS_OFFSET);
		CivicDefinitions = (DefinitionsFn)(b + CIVIC_DEFINITIONS_OFFSET);
		PropertyEdit = (PropertyEditFn)(b + PROPERTY_EDIT_OFFSET);
		MakeHash = (MakeHashFn)(b + MAKE_HASH_OFFSET);
		gOk = true;
		const bool ok = Hook(BUILD_TECH_PREREQS_OFFSET, (void*)&Hook_BuildTechPrereqs, &orig_BuildTechPrereqs)
			&& Hook(CAN_RESEARCH_OFFSET, (void*)&Hook_CanResearch, &orig_CanResearch)
			&& Hook(CAN_PROGRESS_OFFSET, (void*)&Hook_CanProgress, &orig_CanProgress)
			&& Hook(CHANGE_RESEARCH_PROGRESS_OFFSET, (void*)&Hook_ChangeResearch, &orig_ChangeResearch)
			&& Hook(CHANGE_CULTURAL_PROGRESS_OFFSET, (void*)&Hook_ChangeCulture, &orig_ChangeCulture)
			&& Hook(SET_RESEARCHING_QUEUE_OFFSET, (void*)&Hook_SetResearchQueue, &orig_SetResearchQueue)
			&& Hook(SET_RESEARCHING_TECH_OFFSET, (void*)&Hook_SetResearchTech, &orig_SetResearchTech)
			&& Hook(SET_PROGRESSING_QUEUE_OFFSET, (void*)&Hook_SetCivicQueue, &orig_SetCivicQueue)
			&& Hook(SET_PROGRESSING_CIVIC_OFFSET, (void*)&Hook_SetCivic, &orig_SetCivic)
			&& Hook(L_GET_RESEARCH_PATH_OFFSET, (void*)&Hook_lGetResearchPath, &orig_lGetResearchPath)
			&& Hook(L_GET_CIVIC_PATH_OFFSET, (void*)&Hook_lGetCivicPath, &orig_lGetCivicPath)
			&& Hook(TECH_ANALYSIS_EVALUATE_OFFSET, (void*)&Hook_TechEvaluate, &orig_TechEvaluate)
			&& Hook(CIVIC_ANALYSIS_EXECUTE_OFFSET, (void*)&Hook_CivicExecute, &orig_CivicExecute);
		DevBridge::Log("[TechGates] hooks %s (gates are read when the database loads)", ok ? "installed" : "FAILED");
	}
}
