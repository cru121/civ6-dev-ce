// SPDX-License-Identifier: AGPL-3.0-only
// Part of Dev CE, a fork of the Civilization VI Community Extension by Wild-W (https://github.com/Wild-W/CivilizationVI_CommunityExtension).
// Copyright (C) 2026 cru121. Licensed under the GNU Affero General Public License v3.0 (see LICENSE.txt). Not endorsed by Wild-W, Firaxis or 2K.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "MinHook.h"
#include "DevBridge.h"
#include "TerrainViewInternal.h"

// Terrain view sync, milestone 4: re-place what stands on a plot whose ground height changed (see dev-ce/TERRAIN_VIEW.md).
// Features, resources and improvements are placed once at the ground height of that moment. The game's own builders (the ones Lua's TerrainBuilder / ResourceBuilder /
// ImprovementBuilder call) remove and add them again, which places the view objects again at the CURRENT ground height (mesh + overlay). These calls change game state
// (events, signals), so they run on the game thread: a hook on the game's per-frame tick (Game::Autoplay::Manager::Update, static void, ~60 calls/s on the game thread)
// executes the queued jobs. Verified in game.
namespace TerrainView {
namespace detail {
namespace {

// ---- GameCore addresses (installed build 15038592) ------------------------------------------------------------------------------------------------------------------------
constexpr uintptr_t kTickRva = 0x42780;                        // static void Game::Autoplay::Manager::Update(void)
constexpr uintptr_t kEditMapRva = 0x44d40;                     // Map::Instance* Context::Globals::EditMap()
constexpr uintptr_t kGetTerrainBuilder = 0x53010, kGetResourceBuilder = 0x52f90, kGetImprovementBuilder = 0x52c30;     // Builder* Map::Instance::GetXBuilder()
constexpr uintptr_t kIsNaturalWonder = 0x60280;                // bool Plot::Instance::IsNaturalWonder() const
constexpr uintptr_t kSetFeatureType = 0x5dbfb0;                // bool Terrain::Builder::SetFeatureType(Plot*, FeatureParam = { int type, int direction } in one 64-bit register)
constexpr uintptr_t kSetResourceType = 0x5d2b20;               // bool Resource::Builder::SetResourceType(Plot*, ResourceTypes, int count)
constexpr uintptr_t kSetImprovementType = 0x25d430;            // void Improvement::Builder::SetImprovementType(Plot*, ImprovementTypes, PlayerTypes owner, DirectionTypes, bool)
constexpr uintptr_t kSetImprovementPillaged = 0x25cfa0;        // void Improvement::Builder::SetImprovementPillaged(Plot*, bool, PlayerTypes pillager)
// plot object fields (symbol-build decompile; x/y/terrain/resource/improvement checked against Lua on the installed build)
constexpr size_t kFeat = 0x3c, kRes = 0x40, kCnt = 0x42, kImp = 0x44, kOwner = 0x46, kFlags = 0x4d;

uintptr_t Gc() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(L"GameCore_XP2_FinalRelease.dll")); }

void* Map0(uintptr_t rva) {
	typedef void* (__fastcall* Fn)();
	if (!Gc()) return nullptr;
	__try { return reinterpret_cast<Fn>(Gc() + rva)(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
void* Builder(uintptr_t rva, void* map) {
	typedef void* (__fastcall* Fn)(void*);
	__try { return reinterpret_cast<Fn>(Gc() + rva)(map); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
void* PlotAt(int x, int y) {                                   // Map vtable +0x38 = GetPlot(PlotCoord*)
	typedef void* (__fastcall* Fn)(void*, int*);
	void* map = Map0(kEditMapRva); if (!map) return nullptr;
	int co[2] = { x, y };
	__try { void** vt = *reinterpret_cast<void***>(map); return reinterpret_cast<Fn>(vt[7])(map, co); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
bool IsWonder(void* plot) {
	typedef bool(__fastcall* Fn)(void*);
	__try { return reinterpret_cast<Fn>(Gc() + kIsNaturalWonder)(plot); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return true; }       // unknown: do not touch
}
bool CallSetFeature(void* tb, void* plot, int f) {
	typedef bool(__fastcall* Fn)(void*, void*, uint64_t);
	const uint64_t param = static_cast<uint32_t>(f) | (static_cast<uint64_t>(0xffffffffu) << 32);       // FeatureParam { type, NO_DIRECTION }
	__try { reinterpret_cast<Fn>(Gc() + kSetFeatureType)(tb, plot, param); return true; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallSetResource(void* rb, void* plot, int type, int count) {
	typedef bool(__fastcall* Fn)(void*, void*, int, int);
	__try { reinterpret_cast<Fn>(Gc() + kSetResourceType)(rb, plot, type, count); return true; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallSetImprovement(void* ib, void* plot, int type, int owner) {
	typedef void(__fastcall* Fn)(void*, void*, int, int, int, bool);
	__try { reinterpret_cast<Fn>(Gc() + kSetImprovementType)(ib, plot, type, owner, -1, false); return true; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallSetPillaged(void* ib, void* plot, bool pillaged) {
	typedef void(__fastcall* Fn)(void*, void*, bool, int);
	__try { reinterpret_cast<Fn>(Gc() + kSetImprovementPillaged)(ib, plot, pillaged, -1); return true; }   // pillager -1: no "pillaged" notification
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

std::atomic<bool> gEnabled{ false };
std::atomic<unsigned long> gPlots{ 0 }, gFeatures{ 0 }, gResources{ 0 }, gImprovements{ 0 }, gPillagedKept{ 0 }, gWonders{ 0 }, gFailed{ 0 };

// ---- game-thread job queue (tick hook) ---------------------------------------------------------------------------------------------------------------------------------------
struct GJob { std::chrono::steady_clock::time_point due; std::function<void()> fn; };
std::mutex gGMx;
std::vector<GJob> gGJobs;
std::atomic<bool> gGHas{ false };
std::atomic<bool> gTickHooked{ false };
std::atomic<unsigned long> gTicks{ 0 };
typedef void(__fastcall* TickFn)();
TickFn orig_Tick = nullptr;

void RunDue() {
	std::vector<GJob> due;
	{
		std::lock_guard<std::mutex> g(gGMx);
		const auto now = std::chrono::steady_clock::now();
		for (size_t i = 0; i < gGJobs.size();) { if (gGJobs[i].due <= now) { due.push_back(std::move(gGJobs[i])); gGJobs.erase(gGJobs.begin() + i); } else i++; }
		if (gGJobs.empty()) gGHas.store(false);
	}
	for (auto& j : due) { try { j.fn(); } catch (...) { gFailed++; L("objects: job threw an exception"); } }
}

void __fastcall Hook_Tick() {
	gTicks++;
	if (gGHas.load(std::memory_order_relaxed)) RunDue();
	orig_Tick();
}

}  // namespace

bool ObjectsEnabled() { return gEnabled.load(); }
void SetObjectsEnabled(bool on) { gEnabled.store(on); }

void RunOnGameThreadLater(std::function<void()> fn, int delayMs) {
	{
		std::lock_guard<std::mutex> g(gGMx);
		gGJobs.push_back({ std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs), std::move(fn) });
	}
	gGHas.store(true);
}

bool RunOnGameThread(std::function<void()> fn, int timeoutMs) {
	auto done = std::make_shared<std::promise<void>>();
	std::future<void> f = done->get_future();
	RunOnGameThreadLater([fn, done] { fn(); done->set_value(); }, 0);
	return f.wait_for(std::chrono::milliseconds(timeoutMs)) == std::future_status::ready;
}

bool ReadPlotObjects(int x, int y, PlotObjects& o) {
	void* p = PlotAt(x, y);
	uint8_t b[0x50];
	if (!p || !SafeCopy(b, p, sizeof(b))) return false;
	auto s16 = [&](size_t off) { int16_t v; memcpy(&v, b + off, 2); return static_cast<int>(v); };
	o.feature = s16(kFeat); o.resource = s16(kRes); o.count = s16(kCnt); o.improvement = s16(kImp);
	o.owner = static_cast<int8_t>(b[kOwner]); o.pillaged = (b[kFlags] & 1) != 0;
	o.wonder = IsWonder(p);
	return true;
}

// Game thread only. Removes and re-adds feature, resource and improvement of the plot with the game's builders (the same calls Lua's builders make). What the game dropped
// because it became invalid on the new terrain (a resource on the wrong terrain) is not there to be read and is not re-added.
bool ReplacePlotObjects(int x, int y, std::string& msg) {
	void* map = Map0(kEditMapRva);
	void* plot = PlotAt(x, y);
	PlotObjects o;
	if (!map || !plot || !ReadPlotObjects(x, y, o)) { msg = "plot not found"; return false; }
	char buf[200]; msg.clear();
	bool ok = true;
	if (o.feature >= 0) {
		if (o.wonder) { gWonders++; msg += " feature skipped (natural wonder);"; }
		else if (void* tb = Builder(kGetTerrainBuilder, map)) {
			if (CallSetFeature(tb, plot, -1) && CallSetFeature(tb, plot, o.feature)) { gFeatures++; snprintf(buf, sizeof(buf), " feature %d;", o.feature); msg += buf; }
			else { ok = false; msg += " feature FAILED;"; }
		} else { ok = false; msg += " no terrain builder;"; }
	}
	if (o.resource >= 0) {
		if (void* rb = Builder(kGetResourceBuilder, map)) {
			if (CallSetResource(rb, plot, -1, 0) && CallSetResource(rb, plot, o.resource, o.count < 1 ? 1 : o.count)) { gResources++; snprintf(buf, sizeof(buf), " resource %d x%d;", o.resource, o.count); msg += buf; }
			else { ok = false; msg += " resource FAILED;"; }
		} else { ok = false; msg += " no resource builder;"; }
	}
	if (o.improvement >= 0) {
		if (void* ib = Builder(kGetImprovementBuilder, map)) {
			if (CallSetImprovement(ib, plot, -1, -1) && CallSetImprovement(ib, plot, o.improvement, o.owner)) {
				gImprovements++;
				snprintf(buf, sizeof(buf), " improvement %d (owner %d);", o.improvement, o.owner); msg += buf;
				if (o.pillaged) { if (CallSetPillaged(ib, plot, true)) { gPillagedKept++; msg += " pillaged again;"; } else { ok = false; msg += " pillage FAILED;"; } }
			} else { ok = false; msg += " improvement FAILED;"; }
		} else { ok = false; msg += " no improvement builder;"; }
	}
	gPlots++;
	if (!ok) gFailed++;
	return ok;
}

void ScheduleObjectRefresh(const std::vector<std::pair<int, int>>& plots, int delayMs) {
	if (plots.empty() || !gEnabled.load() || !gTickHooked.load()) return;
	RunOnGameThreadLater([plots] {
		for (auto& p : plots) {
			std::string msg;
			PlotObjects o;
			if (!ReadPlotObjects(p.first, p.second, o) || (o.feature < 0 && o.resource < 0 && o.improvement < 0)) continue;
			const bool ok = ReplacePlotObjects(p.first, p.second, msg);
			L("objects: plot (%d,%d) re-placed:%s%s", p.first, p.second, msg.c_str(), ok ? "" : " (some calls failed)");
		}
	}, delayMs);
	L("objects: %zu plot(s) scheduled for re-placement in %d ms", plots.size(), delayMs);
}

ObjStats GetObjStats() { return { gTicks.load(), gPlots.load(), gFeatures.load(), gResources.load(), gImprovements.load(), gPillagedKept.load(), gWonders.load(), gFailed.load(), gTickHooked.load() }; }

void ObjectsCreate() {
	gEnabled.store(MarkerExists("TerrainView.objects"));
	if (gTickHooked.load()) return;
	LPVOID target = reinterpret_cast<LPVOID>(Gc() + kTickRva);
	MH_STATUS s1 = MH_CreateHook(target, reinterpret_cast<LPVOID>(&Hook_Tick), reinterpret_cast<LPVOID*>(&orig_Tick));
	MH_STATUS s2 = (s1 == MH_OK) ? MH_EnableHook(target) : s1;
	if (s1 != MH_OK || s2 != MH_OK) { L("objects: hook on the game tick failed: %s / %s (objects will not be re-placed)", MH_StatusToString(s1), MH_StatusToString(s2)); return; }
	gTickHooked.store(true);
	L("objects: hooked the game tick at %p; re-placement of features, resources and improvements is %s (marker file TerrainView.objects)", target, gEnabled.load() ? "ON" : "off");
}

// ---- self-test (mutates the game's own objects on a few plots: use in a throw-away game) ---------------------------------------------------------------------------------
int ObjectsSelfTest() {
	int fails = 0;
	auto check = [&](bool ok, const std::string& what) { if (!ok) fails++; L("objects self-test: %-86s %s", what.c_str(), ok ? "OK" : "FAILED"); };
	if (!gTickHooked.load()) { L("objects self-test: the game tick hook is not installed"); return -1; }
	void* map = Map0(kEditMapRva);
	int dims[2] = { 0, 0 };
	if (!map || !SafeCopy(dims, static_cast<uint8_t*>(map) + 0x28, 8) || dims[0] <= 0 || dims[1] <= 0) { L("objects self-test: no map"); return -1; }
	{ const unsigned long t0 = gTicks.load(); std::this_thread::sleep_for(std::chrono::milliseconds(300)); check(gTicks.load() > t0 + 3, "the game tick hook is called (game thread is running)"); }
	struct Found { int x, y; PlotObjects o; };
	std::vector<Found> all;
	int sane = 0, insane = 0;
	for (int y = 0; y < dims[1]; y++) for (int x = 0; x < dims[0]; x++) {
		PlotObjects o;
		if (!ReadPlotObjects(x, y, o)) continue;
		const bool ok = o.feature >= -1 && o.feature < 200 && o.resource >= -1 && o.resource < 200 && o.improvement >= -1 && o.improvement < 200 && o.owner >= -1 && o.owner < 64 && o.count >= 0 && o.count < 1000;
		if (ok) sane++; else insane++;
		if (o.feature >= 0 || o.resource >= 0 || o.improvement >= 0) all.push_back({ x, y, o });
	}
	L("objects self-test: map %dx%d, %zu plots carry objects", dims[0], dims[1], all.size());
	check(insane == 0 && sane > 0, "plot object fields (feature +0x3c, resource +0x40, count +0x42, improvement +0x44, owner +0x46) hold plausible values on every plot");
	auto pick = [&](const char* what, std::function<bool(const PlotObjects&)> pred) -> const Found* {
		for (const Found& f : all) if (pred(f.o)) { L("objects self-test: %-14s plot (%d,%d) feature %d resource %d x%d improvement %d owner %d pillaged %d wonder %d", what, f.x, f.y, f.o.feature, f.o.resource, f.o.count, f.o.improvement, f.o.owner, f.o.pillaged, f.o.wonder); return &f; }
		L("objects self-test: %-14s none in this game", what); return nullptr;
	};
	std::vector<const Found*> picks = {
		pick("feature", [](const PlotObjects& o) { return o.feature >= 0 && !o.wonder && o.resource < 0 && o.improvement < 0; }),
		pick("resource", [](const PlotObjects& o) { return o.resource >= 0 && o.feature < 0 && o.improvement < 0; }),
		pick("improvement", [](const PlotObjects& o) { return o.improvement >= 0 && !o.pillaged && o.feature < 0; }),
		pick("feat+res", [](const PlotObjects& o) { return o.feature >= 0 && !o.wonder && o.resource >= 0; }),
		pick("feat+imp", [](const PlotObjects& o) { return o.feature >= 0 && !o.wonder && o.improvement >= 0; }),
		pick("pillaged", [](const PlotObjects& o) { return o.improvement >= 0 && o.pillaged; }),
		pick("wonder", [](const PlotObjects& o) { return o.wonder; }),
	};
	int tested = 0;
	for (const Found* f : picks) {
		if (!f) continue;
		std::string msg; bool ok = false;
		const bool ran = RunOnGameThread([&] { ok = ReplacePlotObjects(f->x, f->y, msg); }, 5000);
		PlotObjects after; const bool rd = ReadPlotObjects(f->x, f->y, after);
		const PlotObjects& b = f->o;
		const bool same = rd && after.feature == b.feature && after.resource == b.resource && after.count == b.count && after.improvement == b.improvement && after.owner == b.owner && after.pillaged == b.pillaged;
		char tag[160]; snprintf(tag, sizeof(tag), "plot (%d,%d):%s", f->x, f->y, msg.c_str());
		check(ran && ok && same, std::string(tag) + " game state identical afterwards");
		tested++;
	}
	check(tested > 0, "at least one plot with objects was tested");
	const ObjStats st = GetObjStats();
	L("objects self-test: RESULT %s (%d failure(s)); re-placed: %lu plots, %lu features, %lu resources, %lu improvements, %lu pillaged flags, %lu wonders skipped", fails == 0 ? "PASS" : "FAIL", fails, st.plots, st.features, st.resources, st.improvements, st.pillaged, st.wonders);
	return fails;
}

}  // namespace detail
}  // namespace TerrainView
