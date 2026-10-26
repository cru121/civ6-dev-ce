// SPDX-License-Identifier: AGPL-3.0-only
// Part of Dev CE, a fork of the Civilization VI Community Extension by Wild-W (https://github.com/Wild-W/CivilizationVI_CommunityExtension).
// Copyright (C) 2026 cru121. Licensed under the GNU Affero General Public License v3.0 (see LICENSE.txt). Not endorsed by Wild-W, Firaxis or 2K.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "MinHook.h"
#include "DevBridge.h"
#include "TerrainViewInternal.h"

// Terrain view sync, milestone 2: material-only patch (see dev-ce/TERRAIN_VIEW.md).
// When a script changes a plot's terrain, the ground MATERIALS of the render cells around the plot change to the new terrain's material pair. Heights are not touched
// (flat -> hills dome = milestone 3; hills -> flat is done by the Lua mod FlattenHillsView).
namespace TerrainView {
namespace detail {
namespace {

// ---- fixed addresses (Windows DX11 build 15038592) ----------------------------------------------------------------------------------------------------------------
constexpr uint32_t kGcTimestamp = 0x667c6f5b, kGcImage = 0xc60000;            // GameCore_XP2_FinalRelease.dll
constexpr uintptr_t kSetTerrainTypeRva = 0x5dd0f0;                            // GameCore: bool Terrain::Builder::_SetTerrainType(Builder*, Plot*, TerrainTypes): every SetTerrainType path ends here
constexpr uint8_t kSetTerrainTypePrologue[8] = { 0x44, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x4c };
constexpr uintptr_t kEditMapRva = 0x44d40;                                    // GameCore: Map::Instance* Context::Globals::EditMap()
constexpr double kKY = 1.1547;                                                // vertical stretch of the hex layout in the distance metric
constexpr int kOY = 220;                                                      // plot (px, py) centre = (128 px + 64 (py odd), 96 py + 220) in cell texels (220 on a Small and a Huge map)
constexpr int kMaxCand = 16;
// milestone 3: dome (flat -> hills) and ground-height overlay (values of terrain_patch.py)
constexpr double kHillAmp = 1300.0;                                           // dome height in u16 base-mesh height units
constexpr double kR0 = 12.0, kR1 = 76.0;                                      // full height out to R0 texels from the plot centre, smoothstep to 0 at R1
constexpr float kAlpha = 0.006f;                                              // sampler world units per base-mesh height unit of a dome (tuned by eye: trees stand on it)
constexpr double kRes = 2.0;                                                  // overlay raster cell size in sampler units
std::atomic<bool> gOverlayOn{ true };                                         // diagnostics (tv overlay on|off): switch the ground-height overlay without removing it
std::unordered_set<uint64_t> gFrozen;                                         // cells kept at their generated state for an experiment (tv freeze X Y / tv unfreeze)
bool gSplitShore = false;                                                     // tv splitshore on|off: also split edges that have an ocean endpoint (off: shoreline triangles keep their generated shape)
bool gSeamFix = false;                                                         // seam vertices without a partner in the neighbour cell follow the neighbour's edge (tv seamfix on|off)
double gShoreFade = 24.0;                                                    // tv shorefade W: the dome fades to zero over W texels towards the nearest ocean vertex (0 = off), so the land edge does not stand up as a wall
double gAmpScale = 1.0;                                                      // diagnostics (tv ampscale V): scales every dome (mesh and overlay)

// ground material pair (A, B) per terrain index (GameInfo.Terrains order of the base game); -1 = not supported (mountains, coast, ocean)
struct Pair { int a, b; };
const Pair kPair[17] = { {2, 3}, {4, 5}, {-1, -1}, {6, 7}, {8, 9}, {-1, -1}, {10, 10}, {12, 12}, {-1, -1}, {13, 15}, {13, 15}, {-1, -1}, {16, 16}, {17, 17}, {-1, -1}, {-1, -1}, {-1, -1} };
bool PairOf(int t, Pair* p) { if (t < 0 || t > 16 || kPair[t].a < 0) return false; *p = kPair[t]; return true; }
inline bool HillsT(int t) { return t == 1 || t == 4 || t == 7 || t == 10 || t == 13; }
inline bool FlatT(int t) { Pair p; return PairOf(t, &p) && !HillsT(t); }

inline uint64_t KeyOf(int x, int y) { return static_cast<uint32_t>(x) | (static_cast<uint64_t>(static_cast<uint32_t>(y)) << 32); }

// ---- state ------------------------------------------------------------------------------------------------------------------------------------------------------------
struct Saved {                                // the record values of a cell before our first patch (undo) and the buffers our patch created
	uint32_t o398 = 0, o394 = 0, oCC = 0, o38c = 0;
	uint8_t table[256] = {};
	Sig sig;
	uint32_t oD8 = 0, oD4 = 0;                // base mesh VB handle word and first vertex
	uint32_t newVb = 0, newIb = 0, newBase = 0, newBaseIb = 0;
	uint32_t oC8 = 0, oC4 = 0, oD0 = 0;
	uint32_t oChunkA[8] = {}, oBoxA[5] = {};  // the base mesh chunk table of the record ((start, count) in triangles for the 4 quadrant chunks) and the max-z floats of its boxes, as the game wrote them
	uint32_t oChunkB[8] = {}, oBoxB[5] = {};  // the same for the second section       // base mesh index start, triangle count and vertex count of the record before our first patch
	uint32_t oB[6] = {};                      // the game's second base mesh section (record +0x174..+0x188) as the game wrote it, when the cell has one
	std::vector<uint8_t> bVb, bIb;            // the second section's vertices and triangle indices as read when oB was captured (the game rewrites the edit buffers when it bakes a cell, so they are read once)
	bool hasB = false; uint32_t ourB = 0;     // ourB: handle of the patched copy of that section's vertices (never released: the game may release the record's handle itself)
};
struct Work {                                 // the CPU working copy of a patched cell
	Sig orig;
	std::vector<uint8_t> idvb;                // current ID vertices (8 bytes each)
	std::vector<std::array<uint16_t, 3>> tris;   // unique triangles of the cell (vertex indices into idvb), in table order
	std::vector<uint16_t> block;              // the ORIGINAL index buffer block (the whole shared buffer: it also holds the base mesh triangles); read when the ID mesh first changes
	std::vector<uint8_t> vb;                  // current base mesh vertices (8 bytes each)
	std::vector<uint8_t> baseIdvb, baseVb;    // the baseline runs, to see which part of the cell differs
	std::vector<uint16_t> baseBib, bib;       // the ORIGINAL base mesh triangle indices of the cell, and the refined ones (when `refined`)
	uint32_t chunkCnt[4] = {};                // triangles per chunk of the refined base mesh
	bool refined = false;                     // the base mesh was subdivided inside the domes: its own index buffer and vertex count
};
std::recursive_mutex gMx;                     // patch state
std::unordered_map<uint64_t, Saved> gSaved;
std::unordered_map<uint64_t, Work> gWork;
struct PlotOp { int x, y, baseT, curT; };
std::vector<PlotOp> gOps;                     // net change of every plot against the terrain the mesh was generated with, in order of last modification
std::unordered_map<uint64_t, int> gBaseT;     // plot -> terrain when first seen (what the generated mesh shows)
std::mutex gSigMx;                            // leaf lock: original signatures, read by the reader's provider
std::unordered_map<uint64_t, Sig> gOrigSig;

struct Change { int x, y, oldT, newT; };
std::mutex gQueueMx;
std::condition_variable gQueueCv;
std::vector<Change> gQueue;
std::atomic<bool> gReady{ false };            // the world view exists and is stable: changes are recorded
std::atomic<bool> gHooked{ false };
std::atomic<unsigned long> gSeen{ 0 }, gDropped{ 0 };
unsigned long gPatchedPlots = 0, gPatchedCells = 0, gUnsupported = 0, gErrors = 0;
uint64_t gWorldId = 0;

// ---- engine access ------------------------------------------------------------------------------------------------------------------------------------------------
uintptr_t GameCoreBase() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(L"GameCore_XP2_FinalRelease.dll")); }

void* EditMap() {
	typedef void* (__fastcall* Fn)();
	const uintptr_t gc = GameCoreBase();
	if (!gc) return nullptr;
	__try { return reinterpret_cast<Fn>(gc + kEditMapRva)(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

bool PlotGrid(int* w, int* h) {                // width at +0x28 (seen in IMapPlot::GetPlotIndex), height assumed at +0x2c and cross-checked with the cell grid
	void* map = EditMap(); int v[2] = { 0, 0 };
	if (!map || !SafeCopy(v, static_cast<uint8_t*>(map) + 0x28, 8)) return false;
	*w = v[0]; *h = v[1];
	return v[0] > 0 && v[1] > 0 && v[0] < 1000 && v[1] < 1000;
}

bool PlotTerrain(int x, int y, int* t) {       // Map vtable +0x38 = GetPlot(PlotCoord*); plot: x +0x28, y +0x2a, terrain +0x2c (int16)
	typedef void* (__fastcall* Fn)(void*, int*);
	void* map = EditMap(); if (!map) return false;
	int co[2] = { x, y }; void* plot = nullptr;
	__try { void** vt = *reinterpret_cast<void***>(map); plot = reinterpret_cast<Fn>(vt[7])(map, co); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	int16_t v = 0;
	if (!plot || !SafeCopy(&v, static_cast<uint8_t*>(plot) + 0x2c, 2)) return false;
	*t = v; return true;
}

uint16_t CallCreate(uintptr_t fn, uint32_t a, uint32_t b, uint32_t count, const void* data, const char* name) {
	typedef uint16_t(__fastcall* Fn)(uint32_t, uint32_t, uint32_t, const void*, uint32_t, uint32_t, const char*);
	__try { return reinterpret_cast<Fn>(fn)(a, b, count, data, 2, 0, name); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
void CallRelease(uintptr_t pool, uint32_t h) {
	typedef int(__fastcall* Fn)(void*, uint16_t);
	__try { reinterpret_cast<Fn>(ExeBase() + gA->release)(reinterpret_cast<void*>(ExeBase() + pool), static_cast<uint16_t>(h)); }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}
void MarkDirty(void* mgr, int x, int y) {
	typedef uint8_t* (__fastcall* Fn)(void*, uint64_t);
	__try { uint8_t* p = reinterpret_cast<Fn>(ExeBase() + gA->getState)(mgr, KeyOf(x, y)); if (p) *p |= 2; }
	__except (EXCEPTION_EXECUTE_HANDLER) {}
}
bool WriteU32(uint8_t* rec, size_t off, uint32_t v) { return SafeCopy(rec + off, &v, 4); }
uint32_t ReadU32(const uint8_t* rec, size_t off) { uint32_t v = 0; SafeCopy(&v, rec + off, 4); return v; }

// ---- plot / cell geometry ------------------------------------------------------------------------------------------------------------------------------------------
inline void Centre(int px, int py, double* x, double* y) { *x = 128.0 * px + ((py & 1) ? 64.0 : 0.0); *y = 96.0 * py + kOY; }
inline void VertexWorld(int cx, int cy, const uint8_t* v, double* X, double* Y) {
	int16_t lx, ly; memcpy(&lx, v, 2); memcpy(&ly, v + 2, 2);
	*X = cx * 256.0 + lx; *Y = cy * 256.0 + ly + std::floor(lx / 2.0);
}

// cells that can contain vertices within 100 texels of the plot centre (a superset: the caller filters by the real vertices)
std::vector<CellKey> Candidates(int px, int py, int gw, int gh) {
	double cxp, cyp; Centre(px, py, &cxp, &cyp);
	const int x0 = static_cast<int>(std::floor((cxp - 100) / 256)), x1 = static_cast<int>(std::floor((cxp + 100) / 256));
	const int y0 = static_cast<int>(std::floor((cyp - 100 - 384) / 256)), y1 = static_cast<int>(std::floor((cyp + 100) / 256));
	std::vector<CellKey> out;
	for (int y = (std::max)(0, y0); y <= (std::min)(gh - 1, y1); y++) for (int x = (std::max)(0, x0); x <= (std::min)(gw - 1, x1); x++) out.push_back({ x, y });
	return out;
}

// port of terrain_patch.World.apply_change, ID part: returns true when a vertex material changed
bool ApplyMaterials(std::vector<uint8_t>& vb, int cx, int cy, int px, int py, int plotW, int plotH, const Pair& po, const Pair& pn) {
	struct P { double x, y; };
	std::vector<P> cents;
	P self{ 0, 0 };
	for (int dx = -2; dx <= 2; dx++) for (int dy = -2; dy <= 2; dy++) {
		const int qx = px + dx, qy = py + dy;
		if (qx < 0 || qy < 0 || qx >= plotW || qy >= plotH) continue;
		P c; Centre(qx, qy, &c.x, &c.y);
		if (dx == 0 && dy == 0) self = c;
		cents.push_back(c);
	}
	bool changed = false;
	for (size_t i = 0; i + 8 <= vb.size(); i += 8) {
		uint8_t* v = &vb[i];
		double X, Y; VertexWorld(cx, cy, v, &X, &Y);
		auto dist = [&](const P& c) { const double dx = X - c.x, dy = (Y - c.y) * kKY; return std::sqrt(dx * dx + dy * dy); };
		const double d0 = dist(self);
		double mind = 1e300;
		for (auto& c : cents) mind = (std::min)(mind, dist(c));
		const bool own = d0 <= mind + 1e-9;
		const bool border = (d0 < 88) && !own;
		const int ma = v[6], mb = v[7], wb = v[5];
		const bool inA = (ma == po.a) || (ma == po.b), inB = (mb == po.a) || (mb == po.b);
		const bool selA = (own && inA) || (border && inA && wb > 0);
		const bool selB = (own || border) && inB && wb > 0;
		if (selA) { v[6] = static_cast<uint8_t>(ma == po.a ? pn.a : pn.b); changed = true; }
		if (selB) { v[7] = static_cast<uint8_t>(mb == po.a ? pn.a : pn.b); changed = true; }
	}
	return changed;
}

// unique triangles of a cell in table order (as terrain_patch.World.cell builds them)
void CellTriangles(const CellData& c, std::vector<std::array<uint16_t, 3>>& tris) {
	std::unordered_set<uint64_t> seen;
	for (int k = 0; k < 32; k++) {
		const uint32_t cnt = c.table[k][0], st = c.table[k][1];
		if (!cnt || st < c.ibLo) continue;
		for (uint32_t t = 0; t < cnt; t++) {
			const size_t o = static_cast<size_t>(st - c.ibLo) + 3 * t;
			if (o + 3 > c.ib.size()) break;
			const uint64_t key = static_cast<uint64_t>(c.ib[o]) | (static_cast<uint64_t>(c.ib[o + 1]) << 16) | (static_cast<uint64_t>(c.ib[o + 2]) << 32);
			if (seen.insert(key).second) tris.push_back({ c.ib[o], c.ib[o + 1], c.ib[o + 2] });
		}
	}
}

// port of terrain_patch.World.build_files: regroup the triangles per material slot; new index buffer = original block + the regrouped run
void BuildBuffers(const Work& w, std::vector<uint16_t>& ib, uint32_t table[64]) {
	std::vector<std::vector<std::array<uint16_t, 3>>> groups(32);
	const size_t nv = w.idvb.size() / 8;
	for (const auto& t : w.tris) {
		int ms[6]; int n = 0;
		for (int k = 0; k < 3; k++) {
			if (t[k] >= nv) continue;
			const uint8_t* v = &w.idvb[static_cast<size_t>(t[k]) * 8];
			if (v[4] > 0) ms[n++] = v[6];
			if (v[5] > 0) ms[n++] = v[7];
		}
		std::sort(ms, ms + n);
		int prev = -1;
		for (int i = 0; i < n; i++) { if (ms[i] == prev) continue; prev = ms[i]; if (ms[i] < 32) groups[ms[i]].push_back(t); }
	}
	ib = w.block;
	uint32_t pos = static_cast<uint32_t>(w.block.size());
	for (int k = 0; k < 32; k++) {
		table[2 * k] = static_cast<uint32_t>(groups[k].size()); table[2 * k + 1] = pos;
		for (auto& t : groups[k]) { ib.push_back(t[0]); ib.push_back(t[1]); ib.push_back(t[2]); }
		pos += 3 * static_cast<uint32_t>(groups[k].size());
	}
}

// ---- ground-height overlay (port of cmds/hov.js: the native detour with a float raster) ---------------------------------------------------------------------------------
// Units, trees, clutter and landmarks ask Terrain::Sampler::GetWorldHeight for the ground height; the patch only changes the mesh, so the answer is corrected here: the detour adds
// the value of a float raster (nearest cell, sampler units, cell size kRes) to the original answer. The raster holds alpha * dome height; it is rebuilt from the net plot changes
// and published as an immutable object through an atomic pointer (the hook reads it from any thread, without locks).
struct SamplerGeom { float offx, offy, sx, sy; int gw, gh; double X0, Y0; int NX, NY; };
struct Grid { float X0, Y0, inv; uint32_t NX, NY; std::vector<float> v; };
std::atomic<const Grid*> gGrid{ nullptr };
std::vector<std::unique_ptr<Grid>> gGrids;     // every published grid, oldest first; older ones are freed after 16 newer ones
typedef float(__fastcall* SamplerFn)(void*, uint64_t, uint32_t);
SamplerFn orig_Sampler = nullptr;
std::atomic<bool> gSamplerHooked{ false };

float __fastcall Hook_Sampler(void* self, uint64_t xy, uint32_t type) {
	float r = orig_Sampler(self, xy, type);
	const Grid* g = gGrid.load(std::memory_order_acquire);
	if (g && gOverlayOn.load(std::memory_order_relaxed)) {
		const uint32_t lo = static_cast<uint32_t>(xy), hi = static_cast<uint32_t>(xy >> 32);
		float x, y; memcpy(&x, &lo, 4); memcpy(&y, &hi, 4);
		const int ix = static_cast<int>((x - g->X0) * g->inv), iy = static_cast<int>((y - g->Y0) * g->inv);
		if (static_cast<uint32_t>(ix) < g->NX && static_cast<uint32_t>(iy) < g->NY) r += g->v[static_cast<size_t>(iy) * g->NX + ix];
	}
	return r;
}

bool SamplerGeometry(SamplerGeom& g, void** samplerOut = nullptr) {
	uintptr_t impl = 0;
	if (!SafeCopy(&impl, reinterpret_cast<const void*>(ExeBase() + gA->ctxGlobal + 8), sizeof(impl)) || !impl) return false;
	uint8_t* sm = reinterpret_cast<uint8_t*>(impl) + 0x180;
	float a = 0, b = 0;
	if (!SafeCopy(&a, sm + 0x60, 4) || !SafeCopy(&b, sm + 0x64, 4) || !SafeCopy(&g.gw, sm + 0xc4, 4) || !SafeCopy(&g.gh, sm + 0xc8, 4)) return false;
	if (!SafeCopy(&g.sx, reinterpret_cast<const void*>(ExeBase() + gA->samplerSx), 4) || !SafeCopy(&g.sy, reinterpret_cast<const void*>(ExeBase() + gA->samplerSy), 4)) return false;
	g.offx = -a; g.offy = -b;
	if (g.gw <= 0 || g.gh <= 0 || g.sx <= 0 || g.sy <= 0 || g.gw > 100000 || g.gh > 100000) return false;
	g.X0 = -g.offx / g.sx - 64; g.Y0 = (-2 - g.offy) / g.sy;
	g.NX = static_cast<int>(std::ceil((g.gw / g.sx + 128) / kRes)); g.NY = static_cast<int>(std::ceil(((g.gh + 4) / g.sy) / kRes));
	if (samplerOut) *samplerOut = sm;
	return true;
}

// texel position of the cells (world texels of the cell layout) <-> sampler world units: texel = 32 * (off + x * s)
inline double TexelX(const SamplerGeom& g, double x) { return 32.0 * (g.offx + x * g.sx); }
inline double TexelY(const SamplerGeom& g, double y) { return 32.0 * (g.offy + y * g.sy); }
inline double SamplerX(const SamplerGeom& g, double X) { return (X / 32.0 - g.offx) / g.sx; }
inline double SamplerY(const SamplerGeom& g, double Y) { return (Y / 32.0 - g.offy) / g.sy; }

std::vector<std::pair<int, int>> DomePlots() {
	std::vector<std::pair<int, int>> d;
	for (const PlotOp& op : gOps) if (FlatT(op.baseT) && HillsT(op.curT)) d.push_back({ op.x, op.y });
	return d;
}

void RebuildOverlayLocked() {                   // call with gMx held
	const auto domes = DomePlots();
	if (domes.empty()) { gGrid.store(nullptr, std::memory_order_release); return; }
	SamplerGeom G;
	if (!SamplerGeometry(G)) { L("overlay: sampler geometry not available"); gGrid.store(nullptr, std::memory_order_release); return; }
	struct R { int ix0, ix1, iy0, iy1; double cx, cy; };
	std::vector<R> rs; int ix0 = INT_MAX, ix1 = INT_MIN, iy0 = INT_MAX, iy1 = INT_MIN;
	for (auto& d : domes) {
		double cx, cy; Centre(d.first, d.second, &cx, &cy);
		R r;
		r.ix0 = (std::max)(0, static_cast<int>((SamplerX(G, cx - kR1 - 8) - G.X0) / kRes)); r.ix1 = (std::min)(G.NX, static_cast<int>((SamplerX(G, cx + kR1 + 8) - G.X0) / kRes) + 2);
		r.iy0 = (std::max)(0, static_cast<int>((SamplerY(G, cy - kR1 - 8) - G.Y0) / kRes)); r.iy1 = (std::min)(G.NY, static_cast<int>((SamplerY(G, cy + kR1 + 8) - G.Y0) / kRes) + 2);
		r.cx = cx; r.cy = cy;
		if (r.ix1 <= r.ix0 || r.iy1 <= r.iy0) continue;
		ix0 = (std::min)(ix0, r.ix0); ix1 = (std::max)(ix1, r.ix1); iy0 = (std::min)(iy0, r.iy0); iy1 = (std::max)(iy1, r.iy1);
		rs.push_back(r);
	}
	if (rs.empty()) { gGrid.store(nullptr, std::memory_order_release); return; }
	auto g = std::make_unique<Grid>();
	g->NX = static_cast<uint32_t>(ix1 - ix0); g->NY = static_cast<uint32_t>(iy1 - iy0); g->inv = static_cast<float>(1.0 / kRes);
	g->X0 = static_cast<float>(G.X0 + ix0 * kRes); g->Y0 = static_cast<float>(G.Y0 + iy0 * kRes);
	g->v.assign(static_cast<size_t>(g->NX) * g->NY, 0.0f);
	for (const R& r : rs) {
		for (int iy = r.iy0; iy < r.iy1; iy++) {
			const double Yt = TexelY(G, G.Y0 + iy * kRes);
			for (int ix = r.ix0; ix < r.ix1; ix++) {
				const double Xt = TexelX(G, G.X0 + ix * kRes);
				const double dx = Xt - r.cx, dy = (Yt - r.cy) * kKY, d = std::sqrt(dx * dx + dy * dy);
				const double t = (std::min)(1.0, (std::max)(0.0, (d - kR0) / (kR1 - kR0))), sf = 1 - t * t * (3 - 2 * t);
				g->v[static_cast<size_t>(iy - iy0) * g->NX + (ix - ix0)] += static_cast<float>(kAlpha * kHillAmp * gAmpScale * sf);
			}
		}
	}
	gGrid.store(g.get(), std::memory_order_release);
	gGrids.push_back(std::move(g));
	if (gGrids.size() > 16) gGrids.erase(gGrids.begin());
	L("overlay: %zu dome(s), raster %d x %d (%.1f KB)", domes.size(), ix1 - ix0, iy1 - iy0, (ix1 - ix0) * static_cast<double>(iy1 - iy0) * 4 / 1024.0);
}

// ---- dome in the base mesh (port of terrain_patch.World.recompute_base for dome plots: k = 1, so the baseline heights cancel out) -------------------------------------------------
struct DomeP { double x, y; };
inline double DomeDh(const std::vector<DomeP>& domes, double X, double Y) {
	double dh = 0;
	for (const DomeP& q : domes) {
		const double dx = X - q.x, dy = Y - q.y, d = std::sqrt(dx * dx + (dy * kKY) * (dy * kKY));
		if (d >= kR1) continue;
		const double t = (std::min)(1.0, (std::max)(0.0, (d - kR0) / (kR1 - kR0)));
		dh += kHillAmp * gAmpScale * (1 - t * t * (3 - 2 * t));
	}
	return dh;
}

// A vertex on a cell seam that the neighbouring cell does not have (the meshes of two cells are not built against each other: a sparse neighbour has long edges) must rise exactly
// as the neighbour's edge does, linearly between the neighbour's flanking edge vertices, otherwise a crack opens when only one side is lifted. Returns false when the vertex is
// not on a seam, has a partner, or no flank was found.
bool SeamRise(const std::vector<DomeP>& domes, int cx, int cy, int lx, int ly, double X, double Y, double* dhOut) {
	const int bx = lx == 0 ? 0 : lx == 256 ? 1 : -1, by = ly == 0 ? 0 : ly == 256 ? 1 : -1;
	if ((bx < 0) == (by < 0)) return false;                                // interior, or a corner (skipped)
	struct NC { int x, y; };
	NC nc[2]; int n = 0; bool alongX;                                       // alongX: the seam runs along world X (parameter X), else along world Y
	int partnerLx = 0, partnerLy = 0;
	if (by >= 0) { nc[n++] = { cx, cy + (by ? 1 : -1) }; alongX = true; partnerLy = by ? 0 : 256; partnerLx = -1; }
	else if (bx == 0) { nc[n++] = { cx - 1, cy - 1 }; nc[n++] = { cx - 1, cy }; alongX = false; partnerLx = 256; partnerLy = -1; }
	else { nc[n++] = { cx + 1, cy }; nc[n++] = { cx + 1, cy + 1 }; alongX = false; partnerLx = 0; partnerLy = -1; }
	double lo = -1e300, hi = 1e300, dLo = 0, dHi = 0; bool haveLo = false, haveHi = false;
	const double p = alongX ? X : Y;
	for (int k = 0; k < n; k++) {
		std::vector<std::shared_ptr<const CellData>> o;
		if (nc[k].x < 0 || nc[k].y < 0 || !ReadCells({ { nc[k].x, nc[k].y } }, o, 3000) || o.empty() || !o[0]) continue;
		const std::vector<uint8_t>& nb = o[0]->vb;
		for (size_t i = 0; i + 8 <= nb.size(); i += 8) {
			const uint8_t* w = &nb[i];
			int16_t wx, wy; memcpy(&wx, w, 2); memcpy(&wy, w + 2, 2);
			if (partnerLx >= 0 && wx != partnerLx) continue;
			if (partnerLy >= 0 && wy != partnerLy) continue;
			double WX, WY; VertexWorld(nc[k].x, nc[k].y, w, &WX, &WY);
			if (std::fabs(WX - X) < 0.5 && std::fabs(WY - Y) < 0.5) return false;   // has a partner: same position, same rise
			const double q = alongX ? WX : WY;
			uint16_t wh; memcpy(&wh, w + 4, 2);
			const double d = wh >= 30000 ? 0.0 : DomeDh(domes, WX, WY);
			if (q <= p && q > lo) { lo = q; dLo = d; haveLo = true; }
			if (q >= p && q < hi) { hi = q; dHi = d; haveHi = true; }
		}
	}
	if (haveLo && haveHi) *dhOut = hi > lo ? dLo + (dHi - dLo) * (p - lo) / (hi - lo) : dLo;
	else if (haveLo) *dhOut = dLo;
	else if (haveHi) *dhOut = dHi;
	else return false;
	return true;
}

// ---- subdivision of the base mesh inside the domes ---------------------------------------------------------------------------------------------------------------------
// The generated base mesh is adaptive: flat ground has triangles with edges of 100..300 texels, so a dome (76 texels wide) finds no vertices to lift. Inside the dome zone every
// edge longer than kRefineL is split at its midpoint (all triangles that share an edge split it together, so the mesh stays watertight), repeated until the edges are short.
constexpr double kRefineL = 8.0;
constexpr size_t kBaseStart = 0xc8, kBaseTris = 0xc4;                          // record: base mesh index start (u16 elements) and triangle count
struct V8 { int16_t x, y; uint16_t h; uint8_t n6, n7; };
inline void V8World(int cx, int cy, const V8& a, double* X, double* Y) { *X = cx * 256.0 + a.x; *Y = cy * 256.0 + a.y + std::floor(a.x / 2.0); }

bool RefineMesh(std::vector<uint8_t>& vbBytes, const std::vector<uint16_t>& ib0, const uint32_t chIn[4], std::vector<uint16_t>& ibOut, uint32_t chOut[4], int cx, int cy, const std::vector<DomeP>& domes) {
	if (domes.empty() || ib0.size() < 3 || vbBytes.size() < 8) return false;
	if (static_cast<uint64_t>(chIn[0]) + chIn[1] + chIn[2] + chIn[3] != ib0.size() / 3 || ib0.size() % 3) return false;   // the chunk table must describe the whole index list
	std::vector<V8> v(vbBytes.size() / 8); memcpy(v.data(), vbBytes.data(), v.size() * 8);
	std::vector<uint16_t> ib = ib0;
	std::vector<uint8_t> cid(ib.size() / 3);
	{ size_t t = 0; for (int k = 0; k < 4; k++) for (uint32_t i = 0; i < chIn[k]; i++) cid[t++] = static_cast<uint8_t>(k); }
	for (uint16_t i : ib) if (i >= v.size()) return false;                // indices outside the run: do not touch
	auto segDist = [&](const V8& a, const V8& b) {
		double ax, ay, bx, by; V8World(cx, cy, a, &ax, &ay); V8World(cx, cy, b, &bx, &by);
		ay *= kKY; by *= kKY;
		double best = 1e300;
		for (const DomeP& q : domes) {
			const double qx = q.x, qy = q.y * kKY, dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
			double t = l2 > 0 ? ((qx - ax) * dx + (qy - ay) * dy) / l2 : 0; t = (std::min)(1.0, (std::max)(0.0, t));
			best = (std::min)(best, std::hypot(ax + t * dx - qx, ay + t * dy - qy));
		}
		return best;
	};
	auto len = [&](uint16_t a, uint16_t b) { double ax, ay, bx, by; V8World(cx, cy, v[a], &ax, &ay); V8World(cx, cy, v[b], &bx, &by); return std::hypot(bx - ax, (by - ay) * kKY); };
	bool any = false;
	for (int pass = 0; pass < 9; pass++) {
		std::unordered_map<uint32_t, int> mid;
		auto split = [&](uint16_t a, uint16_t b) -> int {
			const uint32_t key = (static_cast<uint32_t>((std::min)(a, b)) << 16) | (std::max)(a, b);
			auto it = mid.find(key); if (it != mid.end()) return it->second;
			int r = -1;
			if (!gSplitShore && (v[a].h >= 30000 || v[b].h >= 30000)) { mid[key] = -1; return -1; }       // an ocean vertex (negative height stored as u16): leave this edge alone
			if (v.size() < 60000 && len(a, b) > kRefineL && segDist(v[a], v[b]) < kR1 + 1.0) {
				V8 m; m.x = static_cast<int16_t>((v[a].x + v[b].x) >> 1); m.y = static_cast<int16_t>((v[a].y + v[b].y) >> 1);
				m.h = static_cast<uint16_t>(static_cast<int16_t>((static_cast<int>(static_cast<int16_t>(v[a].h)) + static_cast<int>(static_cast<int16_t>(v[b].h))) >> 1));
				m.n6 = static_cast<uint8_t>((v[a].n6 + v[b].n6 + 1) >> 1); m.n7 = static_cast<uint8_t>((v[a].n7 + v[b].n7 + 1) >> 1);
				r = static_cast<int>(v.size()); v.push_back(m);
			}
			mid[key] = r; return r;
		};
		std::vector<uint16_t> out; out.reserve(ib.size() * 2);
		std::vector<uint8_t> outC; outC.reserve(cid.size() * 2);
		bool marked = false;
		uint8_t curC = 0;
		auto tri = [&](int a, int b, int c) { out.push_back(static_cast<uint16_t>(a)); out.push_back(static_cast<uint16_t>(b)); out.push_back(static_cast<uint16_t>(c)); outC.push_back(curC); };
		auto shorter = [&](int a, int b, int c, int d) { return len(static_cast<uint16_t>(a), static_cast<uint16_t>(b)) <= len(static_cast<uint16_t>(c), static_cast<uint16_t>(d)); };
		for (size_t t = 0; t < ib.size(); t += 3) {
			curC = cid[t / 3];
			const int v0 = ib[t], v1 = ib[t + 1], v2 = ib[t + 2];
			const int m0 = split(static_cast<uint16_t>(v0), static_cast<uint16_t>(v1)), m1 = split(static_cast<uint16_t>(v1), static_cast<uint16_t>(v2)), m2 = split(static_cast<uint16_t>(v2), static_cast<uint16_t>(v0));
			const int mask = (m0 >= 0 ? 1 : 0) | (m1 >= 0 ? 2 : 0) | (m2 >= 0 ? 4 : 0);
			if (mask) marked = true;
			switch (mask) {
			case 0: tri(v0, v1, v2); break;
			case 1: tri(v0, m0, v2); tri(m0, v1, v2); break;
			case 2: tri(v1, m1, v0); tri(m1, v2, v0); break;
			case 4: tri(v2, m2, v1); tri(m2, v0, v1); break;
			case 3: tri(m0, v1, m1); if (shorter(v0, m1, m0, v2)) { tri(v0, m0, m1); tri(v0, m1, v2); } else { tri(v0, m0, v2); tri(m0, m1, v2); } break;
			case 6: tri(m1, v2, m2); if (shorter(v0, m1, v1, m2)) { tri(v0, v1, m1); tri(v0, m1, m2); } else { tri(v0, v1, m2); tri(v1, m1, m2); } break;
			case 5: tri(m2, v0, m0); if (shorter(m2, v1, m0, v2)) { tri(m2, m0, v1); tri(m2, v1, v2); } else { tri(m2, m0, v2); tri(m0, v1, v2); } break;
			default: tri(v0, m0, m2); tri(m0, v1, m1); tri(m2, m1, v2); tri(m0, m1, m2); break;
			}
		}
		if (!marked) break;
		ib.swap(out); cid.swap(outC); any = true;
	}
	if (!any) return false;
	vbBytes.resize(v.size() * 8); memcpy(vbBytes.data(), v.data(), v.size() * 8);
	std::vector<uint16_t> parts[4];
	for (size_t t = 0; t < cid.size(); t++) parts[cid[t]].insert(parts[cid[t]].end(), ib.begin() + t * 3, ib.begin() + t * 3 + 3);
	ibOut.clear();
	for (int k = 0; k < 4; k++) { chOut[k] = static_cast<uint32_t>(parts[k].size() / 3); ibOut.insert(ibOut.end(), parts[k].begin(), parts[k].end()); }
	return true;
}

// refines a base mesh (vertex bytes in place, triangles from `ib0`) for the current domes; false when no dome reaches it or it needs no subdivision
bool RefineDomes(std::vector<uint8_t>& vb, const std::vector<uint16_t>& ib0, const uint32_t chIn[4], std::vector<uint16_t>& ibOut, uint32_t chOut[4], int cx, int cy) {
	std::vector<DomeP> domes;
	for (auto& d : DomePlots()) { DomeP c; Centre(d.first, d.second, &c.x, &c.y); domes.push_back(c); }
	return RefineMesh(vb, ib0, chIn, ibOut, chOut, cx, cy, domes);
}

// record helpers: the chunk table ((start, count) in triangles per quadrant chunk; the first section at +0x5c + 0x20 k, the second one 0xb0 later) and the max-z floats of the boxes
// (header box at +0x40, chunk boxes at +0x58 + 0x20 k). z is about 0.0045 per height unit.
constexpr double kZPerUnit = 0.0045;
void ReadChunkWords(const uint8_t* rec, size_t off, uint32_t ch[8], uint32_t bx[5]) {
	for (int k = 0; k < 4; k++) { ch[2 * k] = ReadU32(rec, 0x5c + 0x20 * k + off); ch[2 * k + 1] = ReadU32(rec, 0x60 + 0x20 * k + off); }
	bx[0] = ReadU32(rec, 0x40 + off); for (int k = 0; k < 4; k++) bx[1 + k] = ReadU32(rec, 0x58 + 0x20 * k + off);
}
void WriteChunkWords(uint8_t* rec, size_t off, const uint32_t ch[8], const uint32_t bx[5]) {
	for (int k = 0; k < 4; k++) { WriteU32(rec, 0x5c + 0x20 * k + off, ch[2 * k]); WriteU32(rec, 0x60 + 0x20 * k + off, ch[2 * k + 1]); }
	WriteU32(rec, 0x40 + off, bx[0]); for (int k = 0; k < 4; k++) WriteU32(rec, 0x58 + 0x20 * k + off, bx[1 + k]);
}
void WriteChunkCounts(uint8_t* rec, size_t off, const uint32_t cnt[4]) { uint32_t st = 0; for (int k = 0; k < 4; k++) { WriteU32(rec, 0x5c + 0x20 * k + off, st); WriteU32(rec, 0x60 + 0x20 * k + off, cnt[k]); st += cnt[k]; } }
void WriteBoxesRaised(uint8_t* rec, size_t off, const uint32_t bx[5], float dz) {
	static const size_t at[5] = { 0x40, 0x58, 0x78, 0x98, 0xb8 };
	for (int i = 0; i < 5; i++) { float f; memcpy(&f, &bx[i], 4); f += dz; uint32_t w; memcpy(&w, &f, 4); WriteU32(rec, at[i] + off, w); }
}
inline float DomeDz() { return gAmpScale > 0 ? static_cast<float>(kHillAmp * gAmpScale * kZPerUnit + 0.3) : 0.0f; }
inline void ChunkCountsOf(const uint32_t ch[8], uint32_t cnt[4]) { for (int k = 0; k < 4; k++) cnt[k] = ch[2 * k + 1]; }

// the ORIGINAL base mesh triangle indices of a cell (relative to its first vertex): from the saved record values when the cell is patched, else from the live record
bool ReadChunksOriginal(int cx, int cy, uint32_t cnt[4]) {
	void* mgr = CellMgr(); const uint8_t* rec = Record(mgr, cx, cy);
	if (!rec) return false;
	auto it = gSaved.find(KeyOf(cx, cy));
	uint32_t ch[8], bx[5];
	if (it != gSaved.end()) { memcpy(ch, it->second.oChunkA, sizeof(ch)); } else ReadChunkWords(rec, 0, ch, bx);
	ChunkCountsOf(ch, cnt);
	return true;
}

bool ReadBaseIbOriginal(int cx, int cy, std::vector<uint16_t>& out) {
	void* mgr = CellMgr(); const uint8_t* rec = Record(mgr, cx, cy);
	if (!rec) return false;
	uint32_t ccw, st, cnt;
	auto it = gSaved.find(KeyOf(cx, cy));
	if (it != gSaved.end()) { ccw = it->second.oCC; st = it->second.oC8; cnt = it->second.oC4; }
	else { ccw = ReadU32(rec, kIbCC); st = ReadU32(rec, kBaseStart); cnt = ReadU32(rec, kBaseTris); }
	const uint16_t h = static_cast<uint16_t>(ccw & 0xffff);
	if (!cnt || cnt > 200000 || !h || h >= kMaxHandle || !HandleLive(1, h)) return false;
	std::vector<Range> rs{ { 1, h, st * 2u, cnt * 6u, 0 } }; std::vector<uint8_t> b;
	if (!ReadBackItems(rs, b, nullptr, 3000) || b.size() != cnt * 6u) return false;
	out.resize(cnt * 3); memcpy(out.data(), b.data(), b.size());
	return true;
}

bool ApplyDomes(std::vector<uint8_t>& bv, int cx, int cy) {
	std::vector<DomeP> domes;
	for (auto& d : DomePlots()) { DomeP c; Centre(d.first, d.second, &c.x, &c.y); domes.push_back(c); }
	if (domes.empty()) return false;
	bool changed = false;
	// ocean vertices near the domes (negative heights stored as u16 >= 30000), in a grid of gShoreFade-sized buckets, for the distance to the shore
	const double W = gShoreFade;
	std::unordered_map<int64_t, std::vector<std::pair<double, double>>> sea;
	auto bucket = [&](double X, double Yk) { return (static_cast<int64_t>(static_cast<int32_t>(std::floor(X / W))) << 32) ^ static_cast<uint32_t>(static_cast<int32_t>(std::floor(Yk / W))); };
	if (W > 0) {
		for (size_t i = 0; i + 8 <= bv.size(); i += 8) {
			uint16_t h; memcpy(&h, &bv[i + 4], 2);
			if (h < 30000) continue;
			double X, Y; VertexWorld(cx, cy, &bv[i], &X, &Y); const double Yk = Y * kKY;
			bool near_ = false;
			for (const DomeP& q : domes) if (std::fabs(X - q.x) < kR1 + W + 16 && std::fabs(Yk - q.y * kKY) < kR1 + W + 16) { near_ = true; break; }
			if (near_) sea[bucket(X, Yk)].push_back({ X, Yk });
		}
	}
	for (size_t i = 0; i + 8 <= bv.size(); i += 8) {
		uint8_t* v = &bv[i];
		uint16_t h0; memcpy(&h0, v + 4, 2);
		if (h0 >= 30000) continue;                                            // water / not land
		double X, Y; VertexWorld(cx, cy, v, &X, &Y);
		double dh = 0, gX = 0, gY = 0; bool any = false;
		for (const DomeP& q : domes) {
			const double dx = X - q.x, dy = Y - q.y, d = std::sqrt(dx * dx + (dy * kKY) * (dy * kKY));
			if (d >= kR1) continue;
			const double t = (std::min)(1.0, (std::max)(0.0, (d - kR0) / (kR1 - kR0))), s = 1 - t * t * (3 - 2 * t);
			const double dsdd = -6 * t * (1 - t) / (kR1 - kR0), dd = (std::max)(d, 1e-6);
			dh += kHillAmp * gAmpScale * s; gX += kHillAmp * gAmpScale * dsdd * dx / dd; gY += kHillAmp * gAmpScale * dsdd * (kKY * kKY * dy) / dd;
			any = true;
		}
		if (any && W > 0 && !sea.empty()) {                                   // fade towards the ocean
			const double Yk = Y * kKY; double best = 1e300;
			const int gx = static_cast<int>(std::floor(X / W)), gy = static_cast<int>(std::floor(Yk / W));
			for (int ox = -1; ox <= 1; ox++) for (int oy = -1; oy <= 1; oy++) {
				auto it = sea.find((static_cast<int64_t>(gx + ox) << 32) ^ static_cast<uint32_t>(gy + oy));
				if (it == sea.end()) continue;
				for (const auto& pt : it->second) best = (std::min)(best, std::hypot(pt.first - X, pt.second - Yk));
			}
			if (best < W) { const double t = best / W, m = t * t * (3 - 2 * t); dh *= m; gX *= m; gY *= m; }
		}
		bool closeBy = any;
		if (!closeBy) for (const DomeP& q : domes) if (std::fabs(X - q.x) < kR1 + 300 && std::fabs(Y - q.y) < kR1 + 300) { closeBy = true; break; }
		if (closeBy && gSeamFix) {
			int16_t lx, ly; memcpy(&lx, v, 2); memcpy(&ly, v + 2, 2);
			double sd;
			if (SeamRise(domes, cx, cy, lx, ly, X, Y, &sd)) { dh = sd; any = true; }
		}
		if (!any) continue;
		const uint16_t nh = static_cast<uint16_t>((std::min)(65535.0, (std::max)(0.0, std::nearbyint(h0 + dh))));
		const uint8_t n6 = static_cast<uint8_t>((std::min)(255.0, (std::max)(0.0, std::nearbyint(128 + (v[6] - 128) - 0.53 * gX))));
		const uint8_t n7 = static_cast<uint8_t>((std::min)(255.0, (std::max)(0.0, std::nearbyint(128 + (v[7] - 128) - 0.45 * gY))));
		if (nh != h0 || n6 != v[6] || n7 != v[7]) changed = true;
		memcpy(v + 4, &nh, 2); v[6] = n6; v[7] = n7;
	}
	return changed;
}

// ---- apply / undo ------------------------------------------------------------------------------------------------------------------------------------------------------
bool EnsureBlock(Work& w) {                    // the ORIGINAL index block, read lazily from the original ID index buffer
	if (!w.block.empty()) return true;
	const uint32_t bytes = HandleBytes(1, w.orig.idIb);
	if (!bytes) return false;
	std::vector<Range> r{ { 1, w.orig.idIb, 0, bytes, 0 } };
	std::vector<uint8_t> b;
	if (!ReadBackItems(r, b, nullptr, 3000)) return false;
	w.block.resize(b.size() / 2); memcpy(w.block.data(), b.data(), b.size());
	return true;
}

constexpr size_t kBTri = 0x174, kBIb = 0x17c, kBCnt = 0x180, kBFirst = 0x184, kBVb = 0x188;   // second base mesh section: triangles, index handle, vertex count, first vertex, vertex handle

void RestoreRecord(uint8_t* rec, const Saved& S, void* mgr, int cx, int cy) {
	if (S.hasB && S.ourB && (ReadU32(rec, kBVb) & 0xffff) == S.ourB) { for (int i = 0; i < 6; i++) WriteU32(rec, kBTri + 4 * i, S.oB[i]); WriteChunkWords(rec, 0xb0, S.oChunkB, S.oBoxB); }
	WriteU32(rec, kIdVb, S.o398); WriteU32(rec, kIdFirst, S.o394); WriteU32(rec, kIbCC, S.oCC); WriteU32(rec, kIdIb, S.o38c);
	WriteU32(rec, kVbHandle, S.oD8); WriteU32(rec, kVbFirst, S.oD4); WriteU32(rec, kVbCount, S.oD0);
	WriteU32(rec, kBaseStart, S.oC8); WriteU32(rec, kBaseTris, S.oC4);
	WriteChunkWords(rec, 0, S.oChunkA, S.oBoxA);
	SafeCopy(rec + kTable, S.table, 256);
	MarkDirty(mgr, cx, cy);
}
void ReleasePatchBuffers(const Saved& S) {
	if (S.newVb) CallRelease(gA->vbReleasePool, S.newVb);
	if (S.newIb) CallRelease(gA->ibReleasePool, S.newIb);
	if (S.newBase) CallRelease(gA->vbReleasePool, S.newBase);
	if (S.newBaseIb) CallRelease(gA->ibReleasePool, S.newBaseIb);
}

// Some cells (coastal ones in practice) carry a SECOND base mesh section that the game draws INSTEAD of the first one, so the dome has to go in there as well. The game writes it when it bakes
// the cell (also after our MarkDirty), so it is checked again from the worker thread. Its vertices have the same 8-byte format as the first section's.
bool RepatchB(int cx, int cy, Saved& S, uint8_t* rec) {
	const uint32_t cnt = S.oB[3];
	if (!cnt || cnt > 400000) return false;
	std::vector<uint8_t> b = S.bVb;
	if (b.size() != cnt * 8u) { L("section B %d,%d: vertex data missing", cx, cy); return false; }
	std::vector<uint16_t> ib0, ib1; bool refined = false; uint32_t chIn[4], chOut[4] = {};
	ChunkCountsOf(S.oChunkB, chIn);
	{	// its own triangles: indices relative to its first vertex
		const uint32_t tris = S.oB[0];
		if (tris && tris < 200000 && S.bIb.size() == tris * 6u) { ib0.resize(tris * 3); memcpy(ib0.data(), S.bIb.data(), S.bIb.size()); refined = RefineDomes(b, ib0, chIn, ib1, chOut, cx, cy); }
	}
	const bool lifted = ApplyDomes(b, cx, cy);
	if (!refined && !lifted) {                                              // no dome reaches this section: leave the game's own
		if (S.ourB && (ReadU32(rec, kBVb) & 0xffff) == S.ourB) { for (int i = 0; i < 6; i++) WriteU32(rec, kBTri + 4 * i, S.oB[i]); WriteChunkWords(rec, 0xb0, S.oChunkB, S.oBoxB); }
		S.ourB = 0; return false;
	}
	const uint32_t ncnt = static_cast<uint32_t>(b.size() / 8);
	const uint16_t hb = CallCreate(ExeBase() + gA->createVB, 0, 0x15, ncnt, b.data(), "Terrain_Cell_VB");
	if (hb == 0 || hb >= kMaxHandle || !HandleLive(0, hb)) { L("section B %d,%d: CreateVB failed (handle %u)", cx, cy, hb); return false; }
	uint16_t hbi = 0;
	if (refined) {
		hbi = CallCreate(ExeBase() + gA->createIB, 0, 0, static_cast<uint32_t>(ib1.size()), ib1.data(), "Terrain_Cell_IB");
		if (hbi == 0 || hbi >= kMaxHandle || !HandleLive(1, hbi)) { L("section B %d,%d: CreateIB failed (handle %u)", cx, cy, hbi); return false; }
	}
	WriteU32(rec, kBVb, (S.oB[5] & 0xffff0000u) | hb); WriteU32(rec, kBFirst, 0); WriteU32(rec, kBCnt, ncnt);
	if (refined) { WriteU32(rec, kBIb, (S.oB[2] & 0xffff0000u) | hbi); WriteU32(rec, kBTri + 4, 0); WriteU32(rec, kBTri, static_cast<uint32_t>(ib1.size() / 3)); WriteChunkCounts(rec, 0xb0, chOut); }
	else WriteChunkWords(rec, 0xb0, S.oChunkB, S.oBoxB);
	WriteBoxesRaised(rec, 0xb0, S.oBoxB, DomeDz());
	S.ourB = hb;
	L("section B %d,%d: patched (%u -> %u vertices%s, new handle %u)", cx, cy, cnt, ncnt, refined ? ", subdivided" : "", hb);
	return true;
}
bool PatchSectionB(int cx, int cy, Saved& S, uint8_t* rec) {
	uint32_t cur[6]; for (int i = 0; i < 6; i++) cur[i] = ReadU32(rec, kBTri + 4 * i);
	if (!cur[0] || !(cur[2] & 0xffff) || !cur[3]) return false;           // no second section
	if (S.hasB && S.ourB && (cur[5] & 0xffff) == S.ourB) return false;     // ours already
	const bool firstSight = !S.hasB;
	memcpy(S.oB, cur, sizeof(cur)); ReadChunkWords(rec, 0xb0, S.oChunkB, S.oBoxB); S.hasB = true; S.ourB = 0;             // written by the game (new or regenerated): its fields are the original
	{	// read its data once: the generated world's data (snapshot) the first time, afterwards what the game has just baked (live)
		const uint16_t h = static_cast<uint16_t>(S.oB[5] & 0xffff); const uint32_t first = S.oB[4], cnt = S.oB[3];
		S.bVb.clear(); S.bIb.clear();
		const bool fromSnap = firstSight && SectionBUnchangedSinceSnapshot(cx, cy, cur);
		L("section B %d,%d: baseline from the %s (%s)", cx, cy, fromSnap ? "snapshot" : "live mirror", firstSight ? (fromSnap ? "generated range unchanged" : "the game has baked the cell again") : "regenerated by the game");
		SetPreferSnapshot(fromSnap);
		bool ok = h && h < kMaxHandle && HandleLive(0, h) && cnt && cnt <= 400000;
		if (ok) { std::vector<Range> r{ { 0, h, first * 8, cnt * 8, 0 } }; ok = ReadBackItems(r, S.bVb, nullptr, 3000) && S.bVb.size() == cnt * 8u; }
		const uint32_t tris = S.oB[0], start = S.oB[1]; const uint16_t ih = static_cast<uint16_t>(S.oB[2] & 0xffff);
		if (ok && tris && tris < 200000 && ih && ih < kMaxHandle && HandleLive(1, ih)) {
			std::vector<Range> ri{ { 1, ih, start * 2u, tris * 6u, 0 } };
			if (!ReadBackItems(ri, S.bIb, nullptr, 3000) || S.bIb.size() != tris * 6u) S.bIb.clear();
		}
		SetPreferSnapshot(false);
		if (!ok) { L("section B %d,%d: vertex read failed", cx, cy); S.hasB = firstSight ? false : S.hasB; return false; }
	}
	return RepatchB(cx, cy, S, rec);
}
void MaintainSectionB() {
	std::unique_lock<std::recursive_mutex> g(gMx, std::try_to_lock);
	if (!g.owns_lock() || gSaved.empty()) return;
	void* mgr = CellMgr(); if (!mgr) return;
	for (auto& kv : gSaved) {
		const int cx = static_cast<int>(kv.first & 0xffffffffu), cy = static_cast<int>(kv.first >> 32);
		uint8_t* rec = const_cast<uint8_t*>(Record(mgr, cx, cy));
		if (rec) PatchSectionB(cx, cy, kv.second, rec);
	}
}

// Replaces the buffers of the parts of the cell that differ from the baseline (ID mesh: new VB + new IB with the regrouped triangles; base mesh: new VB) and restores the original
// record values of the parts that do not.
bool ApplyCell(int cx, int cy, Work& w) {
	void* mgr = CellMgr();
	uint8_t* rec = const_cast<uint8_t*>(Record(mgr, cx, cy));
	if (!rec) return false;
	const uint64_t key = KeyOf(cx, cy);
	std::lock_guard<std::recursive_mutex> g(gMx);
	auto it = gSaved.find(key);
	if (it == gSaved.end()) {
		Saved s;
		s.o398 = ReadU32(rec, kIdVb); s.o394 = ReadU32(rec, kIdFirst); s.oCC = ReadU32(rec, kIbCC); s.o38c = ReadU32(rec, kIdIb);
		s.oD8 = ReadU32(rec, kVbHandle); s.oD4 = ReadU32(rec, kVbFirst); s.oD0 = ReadU32(rec, kVbCount); s.oC8 = ReadU32(rec, kBaseStart); s.oC4 = ReadU32(rec, kBaseTris); ReadChunkWords(rec, 0, s.oChunkA, s.oBoxA);
		SafeCopy(s.table, rec + kTable, 256);
		ReadSig(rec, s.sig);
		it = gSaved.emplace(key, s).first;
		{ std::lock_guard<std::mutex> sg(gSigMx); gOrigSig[key] = s.sig; }
	}
	Saved& S = it->second;
	const bool idCh = w.idvb != w.baseIdvb, bCh = w.vb != w.baseVb;
	if (idCh && ReadU32(rec, kIdCount) * 8u != w.idvb.size()) { L("apply %d,%d: ID vertex count mismatch (%u vs %zu)", cx, cy, ReadU32(rec, kIdCount), w.idvb.size() / 8); return false; }
	if (bCh && !w.refined && S.oD0 * 8u != w.vb.size()) { L("apply %d,%d: base vertex count mismatch (%u vs %zu)", cx, cy, S.oD0, w.vb.size() / 8); return false; }
	static const char kVbName[] = "Terrain_Cell_Material_VB", kIbName[] = "Terrain_Cell_IB", kBaseName[] = "Terrain_Cell_VB";
	uint16_t hv = 0, hi = 0, hb = 0, hbi = 0; uint32_t table[64] = {};
	auto bad = [](uint16_t h, int kind) { return h == 0 || h >= kMaxHandle || !HandleLive(kind, h); };
	auto dropNew = [&]() {
		if (hv && !bad(hv, 0)) CallRelease(gA->vbReleasePool, hv);
		if (hi && !bad(hi, 1)) CallRelease(gA->ibReleasePool, hi);
		if (hb && !bad(hb, 0)) CallRelease(gA->vbReleasePool, hb);
		if (hbi && !bad(hbi, 1)) CallRelease(gA->ibReleasePool, hbi);
	};
	if (idCh) {
		if (!EnsureBlock(w)) { L("apply %d,%d: original index block not readable", cx, cy); return false; }
		std::vector<uint16_t> ib;
		BuildBuffers(w, ib, table);
		hv = CallCreate(ExeBase() + gA->createVB, 0, 0x24, static_cast<uint32_t>(w.idvb.size() / 8), w.idvb.data(), kVbName);
		hi = CallCreate(ExeBase() + gA->createIB, 0, 0, static_cast<uint32_t>(ib.size()), ib.data(), kIbName);
		if (bad(hv, 0) || bad(hi, 1)) { L("apply %d,%d: CreateVB/CreateIB failed (handles %u / %u)", cx, cy, hv, hi); dropNew(); return false; }
	}
	if (bCh) {
		hb = CallCreate(ExeBase() + gA->createVB, 0, 0x15, static_cast<uint32_t>(w.vb.size() / 8), w.vb.data(), kBaseName);
		if (bad(hb, 0)) { L("apply %d,%d: CreateVB(base) failed (handle %u)", cx, cy, hb); dropNew(); return false; }
		if (w.refined) {
			hbi = CallCreate(ExeBase() + gA->createIB, 0, 0, static_cast<uint32_t>(w.bib.size()), w.bib.data(), kIbName);
			if (bad(hbi, 1)) { L("apply %d,%d: CreateIB(base) failed (handle %u)", cx, cy, hbi); dropNew(); return false; }
		}
	}
	const Saved prev = S;
	if (idCh) {
		WriteU32(rec, kIdVb, (S.o398 & 0xffff0000u) | hv);
		WriteU32(rec, kIdFirst, 0);
		if ((S.oCC & 0xffff) == (S.o38c & 0xffff)) WriteU32(rec, kIbCC, (S.oCC & 0xffff0000u) | hi);   // base IB = ID IB (same shared buffer): the new block keeps the base triangles
		WriteU32(rec, kIdIb, (S.o38c & 0xffff0000u) | hi);
		SafeCopy(rec + kTable, table, 256);
	} else {
		WriteU32(rec, kIdVb, S.o398); WriteU32(rec, kIdFirst, S.o394); WriteU32(rec, kIbCC, S.oCC); WriteU32(rec, kIdIb, S.o38c);
		SafeCopy(rec + kTable, S.table, 256);
	}
	if (bCh) { WriteU32(rec, kVbHandle, (S.oD8 & 0xffff0000u) | hb); WriteU32(rec, kVbFirst, 0); WriteU32(rec, kVbCount, static_cast<uint32_t>(w.vb.size() / 8)); }
	else { WriteU32(rec, kVbHandle, S.oD8); WriteU32(rec, kVbFirst, S.oD4); WriteU32(rec, kVbCount, S.oD0); }
	if (w.refined && hbi) { WriteU32(rec, kIbCC, (S.oCC & 0xffff0000u) | hbi); WriteU32(rec, kBaseStart, 0); WriteU32(rec, kBaseTris, static_cast<uint32_t>(w.bib.size() / 3)); WriteChunkCounts(rec, 0, w.chunkCnt); }
	else {
		uint32_t cc = S.oCC; if (idCh && (S.oCC & 0xffff) == (S.o38c & 0xffff)) cc = (S.oCC & 0xffff0000u) | hi;
		WriteU32(rec, kIbCC, cc); WriteU32(rec, kBaseStart, S.oC8); WriteU32(rec, kBaseTris, S.oC4);
		WriteChunkWords(rec, 0, S.oChunkA, S.oBoxA);
	}
	if (bCh) WriteBoxesRaised(rec, 0, S.oBoxA, DomeDz());
	MarkDirty(mgr, cx, cy);
	S.newVb = hv; S.newIb = hi; S.newBase = hb; S.newBaseIb = hbi;
	ReleasePatchBuffers(prev);
	if (S.hasB && S.ourB && (ReadU32(rec, kBVb) & 0xffff) == S.ourB) RepatchB(cx, cy, S, rec); else PatchSectionB(cx, cy, S, rec);
	return true;
}

int UndoAllLocked() {
	void* mgr = CellMgr();
	int n = 0;
	for (auto& kv : gSaved) {
		const int cx = static_cast<int>(kv.first & 0xffffffffu), cy = static_cast<int>(kv.first >> 32);
		uint8_t* rec = const_cast<uint8_t*>(Record(mgr, cx, cy));
		if (rec) { RestoreRecord(rec, kv.second, mgr, cx, cy); ReleasePatchBuffers(kv.second); n++; }
	}
	gSaved.clear(); gWork.clear(); gOps.clear(); gBaseT.clear();
	{ std::lock_guard<std::mutex> sg(gSigMx); gOrigSig.clear(); }
	RebuildOverlayLocked();
	return n;
}

// puts one cell back to its generated state (original record values, patch buffers freed) and forgets its working copy
bool UndoCellLocked(int cx, int cy) {
	const uint64_t key = KeyOf(cx, cy);
	auto it = gSaved.find(key);
	if (it == gSaved.end()) return false;
	void* mgr = CellMgr();
	uint8_t* rec = const_cast<uint8_t*>(Record(mgr, cx, cy));
	if (rec) { RestoreRecord(rec, it->second, mgr, cx, cy); ReleasePatchBuffers(it->second); }
	gSaved.erase(it); gWork.erase(key);
	{ std::lock_guard<std::mutex> sg(gSigMx); gOrigSig.erase(key); }
	return true;
}

// ---- patching one batch of changes ---------------------------------------------------------------------------------------------------------------------------------
// State based (the Python tool patched incrementally, which cannot be undone exactly): every plot keeps its NET change against the terrain the mesh was generated with (gOps, in
// order of last modification); a cell's vertices are always recomputed from the baseline by replaying those changes, so putting a plot back restores the baseline exactly and a
// cell that is back at its baseline gets its original record back. Materials: every supported change. Heights: a plot that was FLAT when the mesh was generated and is HILLS now gets
// a dome in the base mesh and in the ground-height overlay (plots that were hills at generation are flattened by the Lua mod FlattenHillsView through the engine).
// View only: never changes the game data. Returns the cells whose buffers were replaced.
bool PatchPlotsLocked(const std::vector<Change>& changes, std::vector<CellKey>* touchedOut, bool recomputeAll = false) {
	int gw = 0, gh = 0, plotW = 0, plotH = 0;
	void* mgr = CellMgr();
	if (!mgr || !Dims(mgr, &gw, &gh) || !PlotGrid(&plotW, &plotH)) { L("patch: no cell manager or plot grid"); return false; }
	if (plotW != 2 * gw) { L("patch: plot grid width %d does not match the cell grid %d x 2: disabled for safety", plotW, gw); return false; }
	std::vector<uint64_t> recompute; std::unordered_set<uint64_t> inRecompute;
	const std::vector<PlotOp> opsBefore = gOps;                                   // restored if the baseline cannot be read, so the net state never gets ahead of the buffers
	const unsigned long patchedBefore = gPatchedPlots;
	std::vector<std::pair<int, int>> objFast, objSlow;    // plots whose ground height changes: their features/resources/improvements are re-placed afterwards
	for (const Change& c : changes) {
		if (c.x < 0 || c.y < 0 || c.x >= plotW || c.y >= plotH) continue;
		const uint64_t pk = KeyOf(c.x, c.y);
		const int baseT = gBaseT.emplace(pk, c.oldT).first->second;
		auto op = std::find_if(gOps.begin(), gOps.end(), [&](const PlotOp& o) { return o.x == c.x && o.y == c.y; });
		const int curT = op != gOps.end() ? op->curT : baseT;
		if (curT == c.newT) continue;
		Pair pb, pn;
		if (!PairOf(baseT, &pb) || !PairOf(c.newT, &pn)) { gUnsupported++; L("plot (%d,%d): terrain %d -> %d needs a full redraw (mountains, coast and ocean are not supported): not patched", c.x, c.y, curT, c.newT); continue; }
		if (op != gOps.end()) gOps.erase(op);
		if (c.newT != baseT) gOps.push_back({ c.x, c.y, baseT, c.newT });
		if (HillsT(curT) != HillsT(c.newT)) (FlatT(baseT) ? objFast : objSlow).push_back({ c.x, c.y });   // dome: the overlay is immediate; a plot that was a hill is flattened by the engine (Lua mod), which takes about a second
		gPatchedPlots++;
		L("plot (%d,%d): terrain %d -> %d (generated as %d), materials (%d,%d) -> (%d,%d)%s", c.x, c.y, curT, c.newT, baseT, pb.a, pb.b, pn.a, pn.b, (FlatT(baseT) && HillsT(c.newT)) ? ", dome" : "");
		for (const CellKey& ck : Candidates(c.x, c.y, gw, gh)) if (inRecompute.insert(KeyOf(ck.x, ck.y)).second) recompute.push_back(KeyOf(ck.x, ck.y));
	}
	if (recomputeAll) for (const PlotOp& op : gOps) for (const CellKey& ck : Candidates(op.x, op.y, gw, gh)) if (inRecompute.insert(KeyOf(ck.x, ck.y)).second) recompute.push_back(KeyOf(ck.x, ck.y));
	if (recompute.empty()) return true;
	std::vector<CellKey> keys; for (uint64_t k : recompute) keys.push_back({ static_cast<int>(k & 0xffffffffu), static_cast<int>(k >> 32) });
	std::vector<std::shared_ptr<const CellData>> base;
	if (!ReadCells(keys, base, 3000)) { L("patch: baseline read failed, change not applied"); gErrors++; gOps = opsBefore; gPatchedPlots = patchedBefore; return false; }
	std::vector<uint64_t> toApply; size_t restored = 0;
	for (size_t i = 0; i < keys.size(); i++) {
		const CellData& bc = *base[i];
		if (bc.idvb.empty() && bc.vb.empty()) continue;
		const int cx = keys[i].x, cy = keys[i].y; const uint64_t ck = KeyOf(cx, cy);
			if (gFrozen.count(ck)) continue;
		double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
		for (const std::vector<uint8_t>* run : { &bc.idvb, &bc.vb })
			for (size_t o = 0; o + 8 <= run->size(); o += 8) { double X, Y; VertexWorld(cx, cy, &(*run)[o], &X, &Y); x0 = (std::min)(x0, X); x1 = (std::max)(x1, X); y0 = (std::min)(y0, Y); y1 = (std::max)(y1, Y); }
		std::vector<uint8_t> vb = bc.idvb, bvb = bc.vb;
		for (const PlotOp& op : gOps) {                                       // replay the net changes on the baseline
			double pcx, pcy; Centre(op.x, op.y, &pcx, &pcy);
			if (pcx < x0 - 100 || pcx > x1 + 100 || pcy < y0 - 100 || pcy > y1 + 100) continue;
			Pair pb, pn; PairOf(op.baseT, &pb); PairOf(op.curT, &pn);
			ApplyMaterials(vb, cx, cy, op.x, op.y, plotW, plotH, pb, pn);
		}
		std::vector<uint16_t> bib, orig; bool refined = false; uint32_t chNew[4] = {};
		{
			auto w0 = gWork.find(ck);
			if (w0 != gWork.end() && !w0->second.baseBib.empty()) orig = w0->second.baseBib; else ReadBaseIbOriginal(cx, cy, orig);
			uint32_t chOrig[4] = {}; if (!orig.empty() && ReadChunksOriginal(cx, cy, chOrig)) refined = RefineDomes(bvb, orig, chOrig, bib, chNew, cx, cy);
		}
		ApplyDomes(bvb, cx, cy);
		auto wk = gWork.find(ck);
		const std::vector<uint8_t>& curId = wk != gWork.end() ? wk->second.idvb : bc.idvb;
		const std::vector<uint8_t>& curBase = wk != gWork.end() ? wk->second.vb : bc.vb;
		if (vb == curId && bvb == curBase) continue;
		if (vb == bc.idvb && bvb == bc.vb) { if (UndoCellLocked(cx, cy)) restored++; continue; }   // back at the generated state: original record, patch buffers freed
		if (wk == gWork.end()) {                                              // first change of this cell: keep the baseline, the original signature and the triangle list
			Work w; ReadSig(Record(mgr, cx, cy), w.orig);
			{ std::lock_guard<std::mutex> sg(gSigMx); auto o = gOrigSig.find(ck); if (o != gOrigSig.end()) w.orig = o->second; }
			CellTriangles(bc, w.tris);
			w.baseIdvb = bc.idvb; w.baseVb = bc.vb; w.baseBib = orig;
			wk = gWork.emplace(ck, std::move(w)).first;
		}
		wk->second.idvb = std::move(vb); wk->second.vb = std::move(bvb); wk->second.bib = std::move(bib); wk->second.refined = refined; memcpy(wk->second.chunkCnt, chNew, sizeof(chNew)); if (wk->second.baseBib.empty()) wk->second.baseBib = orig;
		toApply.push_back(ck);
	}
	bool ok = true;
	for (uint64_t k : toApply) {
		const int cx = static_cast<int>(k & 0xffffffffu), cy = static_cast<int>(k >> 32);
		if (ApplyCell(cx, cy, gWork[k])) { gPatchedCells++; if (touchedOut) touchedOut->push_back({ cx, cy }); }
		else { ok = false; gErrors++; }
	}
	RebuildOverlayLocked();
	ScheduleObjectRefresh(objFast, 150);
	ScheduleObjectRefresh(objSlow, 1500);
	L("patch: %zu cell(s) recomputed, %zu replaced, %zu restored to the generated state, %zu net plot change(s) in force", keys.size(), toApply.size(), restored, gOps.size());
	if (MarkerExists("TerrainView.selftest")) EditMirrorCheck(3000, "after a patch");
	return ok;
}

// ---- hook and worker thread -------------------------------------------------------------------------------------------------------------------------------------------
typedef bool(__fastcall* SetTerrainFn)(void*, void*, int);
SetTerrainFn orig_SetTerrain = nullptr;
std::atomic<bool> gSnapTaken{ false };

bool __fastcall Hook_SetTerrain(void* builder, void* plot, int type) {
	int16_t pos[2] = { -1, -1 }, oldT = -1;
	if (plot) { SafeCopy(pos, static_cast<uint8_t*>(plot) + 0x28, 4); SafeCopy(&oldT, static_cast<uint8_t*>(plot) + 0x2c, 2); }
	if (gReady.load(std::memory_order_relaxed) && !gSnapTaken.load()) { if (EditSnapshotTake() > 0) gSnapTaken.store(true); }     // before the game touches anything: the mirrors still hold the generated world
	const bool r = orig_SetTerrain(builder, plot, type);
	if (r && plot) {
		gSeen++;
		int16_t newT = -1; SafeCopy(&newT, static_cast<uint8_t*>(plot) + 0x2c, 2);
		if (gReady.load(std::memory_order_relaxed) && newT != oldT) {
			{ std::lock_guard<std::mutex> g(gQueueMx); gQueue.push_back({ pos[0], pos[1], oldT, newT }); }
			gQueueCv.notify_one();
		}
		else gDropped++;
	}
	return r;
}

uint64_t WorldId() {                           // same idea as the reader's world signature
	void* mgr = CellMgr(); int w = 0, h = 0;
	if (!mgr || !Dims(mgr, &w, &h)) return 0;
	uint64_t hsh = 1469598103934665603ull;
	auto mix = [&](uint32_t v) { hsh ^= v; hsh *= 1099511628211ull; };
	mix(w); mix(h);
	const int pts[4][2] = { {0, 0}, {w - 1, h - 1}, {w / 2, h / 2}, {1, 1} };
	for (auto& p : pts) {
		Sig s;
		if (!ReadSig(Record(mgr, p[0], p[1]), s) || !SigPlausible(s)) return 0;
		mix(s.idVb); mix(s.idFirst); mix(s.idCount); mix(s.vb); mix(s.vbFirst); mix(s.vbCount); mix(s.idIb); mix(s.table[1]);
	}
	return hsh;
}

DWORD WINAPI WorkerThread(LPVOID) {
	uint64_t last = 0;
	for (;;) {
		std::vector<Change> batch;
		{
			std::unique_lock<std::mutex> lk(gQueueMx);
			gQueueCv.wait_for(lk, std::chrono::milliseconds(150), [] { return !gQueue.empty(); });
			if (!gQueue.empty()) { lk.unlock(); Sleep(80); lk.lock(); batch.swap(gQueue); }     // debounce a burst of changes
		}
		const uint64_t id = WorldId();
		if (id != gWorldId) {                                                   // the world view changed (new load/restart) or is being built
			gReady.store(false);
			if (gWorldId != 0) {                                                // its buffers are gone with the old world: forget everything without touching them
				std::lock_guard<std::recursive_mutex> g(gMx);
				gSaved.clear(); gWork.clear(); gOps.clear(); gBaseT.clear(); gGrid.store(nullptr, std::memory_order_release);
				{ std::lock_guard<std::mutex> sg(gSigMx); gOrigSig.clear(); }
				Invalidate();
				L("world view changed: patch state dropped");
			}
			{ std::lock_guard<std::mutex> g(gQueueMx); gQueue.clear(); }
			if (id != 0 && id == last) { gWorldId = id; gSnapTaken.store(false); gReady.store(true); L("world view ready (signature %016llx): terrain changes are patched", static_cast<unsigned long long>(id)); }
			else gWorldId = (id == 0) ? 0 : gWorldId;
			last = id;
			continue;
		}
		last = id;
		if (gReady.load()) MaintainSectionB();
		if (!gReady.load() || batch.empty()) continue;
		// coalesce: first old terrain and last new terrain per plot, in order of first appearance
		std::vector<Change> co; std::unordered_map<uint64_t, size_t> at;
		for (const Change& c : batch) {
			auto it = at.find(KeyOf(c.x, c.y));
			if (it == at.end()) { at[KeyOf(c.x, c.y)] = co.size(); co.push_back(c); } else co[it->second].newT = c.newT;
		}
		std::lock_guard<std::recursive_mutex> g(gMx);
		const auto t0 = std::chrono::steady_clock::now();
		PatchPlotsLocked(co, nullptr);
		L("batch of %zu plot change(s) handled in %.1f ms", co.size(), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
	}
}

}  // namespace

std::string CellInfoImpl(int x, int y);                  // defined after ReadLive
std::string HeightProbeImpl(int px, int py);
std::string HeightProfileImpl(int px, int py, bool holes = false);

// ---- commands (exported for tools: Frida calls TerrainView_Command) -------------------------------------------------------------------------------------------------------
std::string Command(const std::string& line) {
	char buf[512];
	if (line == "status") {
		std::lock_guard<std::recursive_mutex> g(gMx);
		snprintf(buf, sizeof(buf), "hooked=%d ready=%d seen=%lu dropped=%lu patchedPlots=%lu patchedCells=%lu unsupported=%lu errors=%lu savedCells=%zu netPlotChanges=%zu domes=%zu overlay=%d samplerHook=%d",
			gHooked.load() ? 1 : 0, gReady.load() ? 1 : 0, gSeen.load(), gDropped.load(), gPatchedPlots, gPatchedCells, gUnsupported, gErrors, gSaved.size(), gOps.size(), DomePlots().size(), gGrid.load() ? 1 : 0, gSamplerHooked.load() ? 1 : 0);
		const ObjStats o = GetObjStats();
		char buf2[300];
		snprintf(buf2, sizeof(buf2), " | objects: %s tick=%d plots=%lu features=%lu resources=%lu improvements=%lu pillagedKept=%lu wondersSkipped=%lu failed=%lu", ObjectsEnabled() ? "ON" : "off", o.hooked ? 1 : 0, o.plots, o.features, o.resources, o.improvements, o.pillaged, o.wonders, o.failed);
		char buf3[120]; snprintf(buf3, sizeof(buf3), " | overlay %s, dome scale %.2f", gOverlayOn.load() ? "ON" : "OFF", gAmpScale);
		return std::string(buf) + buf2 + buf3;
	}
	if (line == "undo") { std::lock_guard<std::recursive_mutex> g(gMx); const int n = UndoAllLocked(); snprintf(buf, sizeof(buf), "restored %d cell(s)", n); return buf; }
	int x, y, o, n;
	if (sscanf_s(line.c_str(), "patch %d %d %d %d", &x, &y, &o, &n) == 4) {          // view-only manual patch: terrain o -> n on plot x,y (does not change the game data)
		std::lock_guard<std::recursive_mutex> g(gMx);
		std::vector<CellKey> t;
		const bool ok = PatchPlotsLocked({ { x, y, o, n } }, &t);
		snprintf(buf, sizeof(buf), "%s: %zu cell(s) applied", ok ? "ok" : "FAILED", t.size());
		return buf;
	}
	if (line == "overlay on" || line == "overlay off") { gOverlayOn.store(line == "overlay on"); return std::string("ground-height overlay ") + (gOverlayOn.load() ? "ON" : "OFF"); }
	if (line == "objects on" || line == "objects off") { SetObjectsEnabled(line == "objects on"); return std::string("object re-placement ") + (ObjectsEnabled() ? "ON" : "OFF"); }
	if (sscanf_s(line.c_str(), "freeze %d %d", &x, &y) == 2) {                       // experiment: put one cell back to its generated state and keep it there
		std::lock_guard<std::recursive_mutex> g(gMx);
		gFrozen.insert(KeyOf(x, y)); const bool r = UndoCellLocked(x, y);
		snprintf(buf, sizeof(buf), "cell %d,%d frozen at the generated state (%s)", x, y, r ? "restored" : "it was not patched");
		return buf;
	}
	if (line == "unfreeze") {
		std::lock_guard<std::recursive_mutex> g(gMx);
		gFrozen.clear();
		std::vector<CellKey> t; const bool ok = PatchPlotsLocked({}, &t, true);
		snprintf(buf, sizeof(buf), "unfrozen: %s, %zu cell(s) replaced", ok ? "ok" : "FAILED", t.size());
		return buf;
	}
	if (line == "seamfix on" || line == "seamfix off") {                              // diagnostics: seam vertices follow the neighbour's edge, then recompute the patched cells
		std::lock_guard<std::recursive_mutex> g(gMx);
		gSeamFix = line == "seamfix on";
		std::vector<CellKey> t;
		const bool ok = PatchPlotsLocked({}, &t, true);
		snprintf(buf, sizeof(buf), "seam fix %s: %s, %zu cell(s) replaced", gSeamFix ? "ON" : "OFF", ok ? "ok" : "FAILED", t.size());
		return buf;
	}
	if (line == "splitshore on" || line == "splitshore off") {                      // recompute every patched cell with / without splitting shoreline edges
		std::lock_guard<std::recursive_mutex> g(gMx);
		gSplitShore = line == "splitshore on";
		std::vector<CellKey> t;
		const bool ok = PatchPlotsLocked({}, &t, true);
		snprintf(buf, sizeof(buf), "split shoreline edges %s: %s, %zu cell(s) replaced", gSplitShore ? "ON" : "OFF", ok ? "ok" : "FAILED", t.size());
		return buf;
	}
	double scale;
	if (sscanf_s(line.c_str(), "shorefade %lf", &scale) == 1) {                      // fade width in texels (0 = off), then recompute the patched cells
		std::lock_guard<std::recursive_mutex> g(gMx);
		gShoreFade = (std::max)(0.0, (std::min)(200.0, scale));
		std::vector<CellKey> t;
		const bool ok = PatchPlotsLocked({}, &t, true);
		snprintf(buf, sizeof(buf), "shore fade %.0f texels: %s, %zu cell(s) replaced", gShoreFade, ok ? "ok" : "FAILED", t.size());
		return buf;
	}
	if (sscanf_s(line.c_str(), "ampscale %lf", &scale) == 1) {                        // scale every dome and recompute the patched cells
		std::lock_guard<std::recursive_mutex> g(gMx);
		gAmpScale = (std::max)(0.0, (std::min)(2.0, scale));
		std::vector<CellKey> t;
		const bool ok = PatchPlotsLocked({}, &t, true);
		RebuildOverlayLocked();
		snprintf(buf, sizeof(buf), "dome scale %.2f: %s, %zu cell(s) replaced", gAmpScale, ok ? "ok" : "FAILED", t.size());
		return buf;
	}
	if (sscanf_s(line.c_str(), "cellinfo %d %d", &x, &y) == 2) return CellInfoImpl(x, y);
	if (sscanf_s(line.c_str(), "height %d %d", &x, &y) == 2) return HeightProbeImpl(x, y);
	if (sscanf_s(line.c_str(), "hprofile %d %d", &x, &y) == 2) return HeightProfileImpl(x, y);
	if (sscanf_s(line.c_str(), "holes %d %d", &x, &y) == 2) return HeightProfileImpl(x, y, true);
	if (sscanf_s(line.c_str(), "replace %d %d", &x, &y) == 2) {                         // re-place the objects of a plot now (game thread)
		std::string msg; bool ok = false;
		const bool ran = RunOnGameThread([&] { ok = ReplacePlotObjects(x, y, msg); }, 5000);
		return std::string(ran ? (ok ? "ok:" : "FAILED:") : "timed out (no game tick):") + msg;
	}
	return "unknown command (status | undo | patch X Y OLD NEW | replace X Y | cellinfo X Y | overlay on|off | objects on|off | ampscale V | seamfix on|off | splitshore on|off | shorefade W | freeze X Y | unfreeze)";
}

void PatchCreate() {
	const uintptr_t gc = GameCoreBase();
	if (!gc) { L("patch: GameCore module not found"); return; }
	const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(gc);
	const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(gc + dos->e_lfanew);
	if (nt->FileHeader.TimeDateStamp != kGcTimestamp || nt->OptionalHeader.SizeOfImage != kGcImage) { L("patch: GameCore build check FAILED (timestamp 0x%08x): disabled", nt->FileHeader.TimeDateStamp); return; }
	if (memcmp(reinterpret_cast<const void*>(gc + kSetTerrainTypeRva), kSetTerrainTypePrologue, 8) != 0) { L("patch: _SetTerrainType prologue mismatch: disabled"); return; }
	SetOriginalSigProvider([](int x, int y, Sig& s) {
		std::lock_guard<std::mutex> g(gSigMx);
		auto it = gOrigSig.find(KeyOf(x, y));
		if (it == gOrigSig.end()) return false;
		s = it->second; return true;
	});
	LPVOID target = reinterpret_cast<LPVOID>(gc + kSetTerrainTypeRva);
	MH_STATUS s1 = MH_CreateHook(target, reinterpret_cast<LPVOID>(&Hook_SetTerrain), reinterpret_cast<LPVOID*>(&orig_SetTerrain));
	MH_STATUS s2 = (s1 == MH_OK) ? MH_EnableHook(target) : s1;
	if (s1 != MH_OK || s2 != MH_OK) { L("patch: hook on _SetTerrainType failed: %s / %s", MH_StatusToString(s1), MH_StatusToString(s2)); return; }
	{   // the ground-height detour (exe), for the overlay of the domes
		LPVOID st = reinterpret_cast<LPVOID>(ExeBase() + gA->sampler);
		MH_STATUS a1 = MH_CreateHook(st, reinterpret_cast<LPVOID>(&Hook_Sampler), reinterpret_cast<LPVOID*>(&orig_Sampler));
		MH_STATUS a2 = (a1 == MH_OK) ? MH_EnableHook(st) : a1;
		if (a1 != MH_OK || a2 != MH_OK) L("patch: hook on the ground-height sampler failed: %s / %s (domes will have no overlay)", MH_StatusToString(a1), MH_StatusToString(a2));
		else { gSamplerHooked.store(true); L("patch: hooked the ground-height sampler at %p", st); }
	}
	ObjectsCreate();
	gHooked.store(true);
	HANDLE t = CreateThread(nullptr, 0, &WorkerThread, nullptr, 0, nullptr);
	if (t) CloseHandle(t);
	L("patch: hooked _SetTerrainType at %p; terrain changes are patched once the world view is ready", target);
}

// ---- self-test -------------------------------------------------------------------------------------------------------------------------------------------------------------
namespace {

bool ReadLive(int x, int y, CellData& cd, std::vector<uint16_t>* fullIb) {       // the cell as it is in the live buffers now (bypasses cache and saved originals)
	void* mgr = CellMgr(); Sig s;
	if (!ReadSig(Record(mgr, x, y), s) || !SigPlausible(s)) return false;
	std::vector<Range> items; uint32_t lo, hi; IbRange(s, &lo, &hi);
	if (s.idCount) items.push_back({ 0, s.idVb, s.idFirst * 8u, s.idCount * 8u, 0 });
	if (s.vbCount) items.push_back({ 0, s.vb, s.vbFirst * 8u, s.vbCount * 8u, 0 });
	if (hi > lo) items.push_back({ 1, s.idIb, lo * 2u, (hi - lo) * 2u, 0 });
	if (fullIb) items.push_back({ 1, s.idIb, 0, HandleBytes(1, s.idIb), 0 });
	std::vector<uint8_t> data;
	if (!ReadBackItems(items, data, nullptr, 5000)) return false;
	cd = CellData(); cd.x = x; cd.y = y; cd.idCount = s.idCount; cd.vbCount = s.vbCount; cd.ibLo = lo; cd.ibHi = hi;
	for (int k = 0; k < 32; k++) { cd.table[k][0] = s.table[2 * k]; cd.table[k][1] = s.table[2 * k + 1]; }
	size_t p = 0;
	if (s.idCount) { cd.idvb.assign(data.begin() + items[p].at, data.begin() + items[p].at + items[p].len); p++; }
	if (s.vbCount) { cd.vb.assign(data.begin() + items[p].at, data.begin() + items[p].at + items[p].len); p++; }
	if (hi > lo) { cd.ib.resize(hi - lo); memcpy(cd.ib.data(), data.data() + items[p].at, items[p].len); p++; }
	if (fullIb) { fullIb->resize(items[p].len / 2); memcpy(fullIb->data(), data.data() + items[p].at, items[p].len); }
	return true;
}

}  // namespace


// diagnostics: every triangle of the live base meshes (first and second section of the cells around the plot) that contains the plot centre, with its vertex heights
std::string HeightProbeImpl(int px, int py) {
	std::lock_guard<std::recursive_mutex> g(gMx);
	void* mgr = CellMgr(); int gw = 0, gh = 0;
	if (!mgr || !Dims(mgr, &gw, &gh)) return "no world";
	double X, Y; Centre(px, py, &X, &Y);
	const int c0x = static_cast<int>(std::floor(X / 256)), c0y = static_cast<int>(std::floor(Y / 256));
	std::string out; char buf[400]; int found = 0;
	for (int cx = c0x - 1; cx <= c0x + 1; cx++) for (int cy = c0y - 2; cy <= c0y + 1; cy++) {
		if (cx < 0 || cy < 0 || cx >= gw || cy >= gh) continue;
		const uint8_t* rec = Record(mgr, cx, cy); if (!rec) continue;
		for (int sec = 0; sec < 2; sec++) {
			uint32_t vbh, first, cnt, ibh, start, tris;
			if (sec == 0) { vbh = ReadU32(rec, kVbHandle) & 0xffff; first = ReadU32(rec, kVbFirst); cnt = ReadU32(rec, kVbCount); ibh = ReadU32(rec, kIbCC) & 0xffff; start = ReadU32(rec, kBaseStart); tris = ReadU32(rec, kBaseTris); }
			else { vbh = ReadU32(rec, kBVb) & 0xffff; first = ReadU32(rec, kBFirst); cnt = ReadU32(rec, kBCnt); ibh = ReadU32(rec, kBIb) & 0xffff; start = ReadU32(rec, kBTri + 4); tris = ReadU32(rec, kBTri); }
			if (!tris || !cnt || !vbh || !ibh || vbh >= kMaxHandle || ibh >= kMaxHandle || !HandleLive(0, vbh) || !HandleLive(1, ibh) || tris > 400000 || cnt > 400000) continue;
			std::vector<Range> rs{ { 0, vbh, first * 8, cnt * 8, 0 }, { 1, ibh, start * 2, tris * 6, 0 } }; std::vector<uint8_t> data;
			if (!ReadBackItems(rs, data, nullptr, 3000)) { snprintf(buf, sizeof(buf), "cell %d,%d sec %c: read failed\n", cx, cy, 'A' + sec); out += buf; continue; }
			const uint8_t* vb = data.data() + rs[0].at; const uint16_t* ib = reinterpret_cast<const uint16_t*>(data.data() + rs[1].at);
			int hit = 0;
			for (uint32_t t = 0; t < tris; t++) {
				double wx[3], wy[3]; uint16_t hh[3]; bool ok = true;
				for (int k = 0; k < 3; k++) { const uint32_t i = ib[t * 3 + k]; if (i >= cnt) { ok = false; break; } VertexWorld(cx, cy, vb + i * 8, &wx[k], &wy[k]); memcpy(&hh[k], vb + i * 8 + 4, 2); }
				if (!ok) continue;
				const double d = (wy[1] - wy[2]) * (wx[0] - wx[2]) + (wx[2] - wx[1]) * (wy[0] - wy[2]);
				if (std::fabs(d) < 1e-9) continue;
				const double l1 = ((wy[1] - wy[2]) * (X - wx[2]) + (wx[2] - wx[1]) * (Y - wy[2])) / d, l2 = ((wy[2] - wy[0]) * (X - wx[2]) + (wx[0] - wx[2]) * (Y - wy[2])) / d, l3 = 1 - l1 - l2;
				if (l1 < -1e-6 || l2 < -1e-6 || l3 < -1e-6) continue;
				hit++;
				if (hit <= 3) { snprintf(buf, sizeof(buf), "cell %d,%d sec %c: triangle %u contains the centre: vertex heights %u %u %u, interpolated %.0f\n", cx, cy, 'A' + sec, t, hh[0], hh[1], hh[2], l1 * hh[0] + l2 * hh[1] + l3 * hh[2]); out += buf; }
			}
			found += hit;
			if (hit > 3) { snprintf(buf, sizeof(buf), "cell %d,%d sec %c: %d triangles in total contain the centre\n", cx, cy, 'A' + sec, hit); out += buf; }
		}
	}
	snprintf(buf, sizeof(buf), "plot %d,%d centre (%.0f, %.0f): %d triangle(s) contain it\n", px, py, X, Y, found);
	return buf + out;
}


// diagnostics: live height profile through a plot centre (north-south and east-west lines, step 16 texels): the highest surface of every live base mesh section that covers the point
std::string HeightProfileImpl(int px, int py, bool holes) {
	std::lock_guard<std::recursive_mutex> g(gMx);
	void* mgr = CellMgr(); int gw = 0, gh = 0;
	if (!mgr || !Dims(mgr, &gw, &gh)) return "no world";
	double X0, Y0; Centre(px, py, &X0, &Y0);
	struct Sec { int cx, cy, sec; std::vector<uint8_t> vb; std::vector<uint16_t> ib; uint32_t cnt = 0, tris = 0; };
	std::vector<Sec> secs;
	const int c0x = static_cast<int>(std::floor(X0 / 256)), c0y = static_cast<int>(std::floor(Y0 / 256));
	for (int cx = c0x - 1; cx <= c0x + 1; cx++) for (int cy = c0y - 2; cy <= c0y + 1; cy++) {
		if (cx < 0 || cy < 0 || cx >= gw || cy >= gh) continue;
		const uint8_t* rec = Record(mgr, cx, cy); if (!rec) continue;
		for (int sec = 0; sec < 2; sec++) {
			uint32_t vbh, first, cnt, ibh, start, tris;
			if (sec == 0) { vbh = ReadU32(rec, kVbHandle) & 0xffff; first = ReadU32(rec, kVbFirst); cnt = ReadU32(rec, kVbCount); ibh = ReadU32(rec, kIbCC) & 0xffff; start = ReadU32(rec, kBaseStart); tris = ReadU32(rec, kBaseTris); }
			else { vbh = ReadU32(rec, kBVb) & 0xffff; first = ReadU32(rec, kBFirst); cnt = ReadU32(rec, kBCnt); ibh = ReadU32(rec, kBIb) & 0xffff; start = ReadU32(rec, kBTri + 4); tris = ReadU32(rec, kBTri); }
			if (!tris || !cnt || !vbh || !ibh || vbh >= kMaxHandle || ibh >= kMaxHandle || !HandleLive(0, vbh) || !HandleLive(1, ibh) || tris > 400000 || cnt > 400000) continue;
			std::vector<Range> rs{ { 0, vbh, first * 8, cnt * 8, 0 }, { 1, ibh, start * 2, tris * 6, 0 } }; std::vector<uint8_t> data;
			if (!ReadBackItems(rs, data, nullptr, 3000)) continue;
			Sec S; S.cx = cx; S.cy = cy; S.sec = sec; S.cnt = cnt; S.tris = tris;
			S.vb.assign(data.begin() + rs[0].at, data.begin() + rs[0].at + cnt * 8); S.ib.resize(tris * 3); memcpy(S.ib.data(), data.data() + rs[1].at, tris * 6);
			secs.push_back(std::move(S));
		}
	}
	auto heightAt = [&](double X, double Y, int* nHit) -> double {
		double best = -1e9; *nHit = 0;
		for (const Sec& S : secs) for (uint32_t t = 0; t < S.tris; t++) {
			double wx[3], wy[3], hh[3]; bool ok = true;
			for (int k = 0; k < 3; k++) { const uint32_t i = S.ib[t * 3 + k]; if (i >= S.cnt) { ok = false; break; } VertexWorld(S.cx, S.cy, &S.vb[i * 8], &wx[k], &wy[k]); uint16_t h; memcpy(&h, &S.vb[i * 8 + 4], 2); hh[k] = static_cast<int16_t>(h); }
			if (!ok) continue;
			if (X < (std::min)({ wx[0], wx[1], wx[2] }) - 1e-6 || X > (std::max)({ wx[0], wx[1], wx[2] }) + 1e-6 || Y < (std::min)({ wy[0], wy[1], wy[2] }) - 1e-6 || Y > (std::max)({ wy[0], wy[1], wy[2] }) + 1e-6) continue;
			const double d = (wy[1] - wy[2]) * (wx[0] - wx[2]) + (wx[2] - wx[1]) * (wy[0] - wy[2]);
			if (std::fabs(d) < 1e-9) continue;
			const double l1 = ((wy[1] - wy[2]) * (X - wx[2]) + (wx[2] - wx[1]) * (Y - wy[2])) / d, l2 = ((wy[2] - wy[0]) * (X - wx[2]) + (wx[0] - wx[2]) * (Y - wy[2])) / d, l3 = 1 - l1 - l2;
			if (l1 < -1e-6 || l2 < -1e-6 || l3 < -1e-6) continue;
			(*nHit)++; best = (std::max)(best, l1 * hh[0] + l2 * hh[1] + l3 * hh[2]);
		}
		return best;
	};
	if (holes) {                                                            // grid scan: points no triangle covers, and the largest height difference between the covering layers
		int total = 0, nHoles = 0, maxLayers = 0; std::string list; char b2[120];
		for (int dy = -100; dy <= 100; dy += 4) for (int dx = -100; dx <= 100; dx += 4) {
			const double X = X0 + dx, Y = Y0 + dy / kKY;
			if (std::hypot(static_cast<double>(dx), static_cast<double>(dy)) > 100) continue;
			int n; heightAt(X, Y, &n); total++; maxLayers = (std::max)(maxLayers, n);
			if (n == 0) { nHoles++; if (nHoles <= 24) { snprintf(b2, sizeof(b2), " (%+d,%+d)", dx, dy); list += b2; } }
		}
		char b3[300]; snprintf(b3, sizeof(b3), "plot %d,%d: %d sample points in a radius of 100, %d not covered by any live triangle (max layers %d)%s%s\n", px, py, total, nHoles, maxLayers, nHoles ? ":" : "", list.c_str());
		return b3;
	}
	std::string out; char buf[200];
	snprintf(buf, sizeof(buf), "plot %d,%d centre (%.0f, %.0f), %zu section(s) read\nnorth-south (world Y offset: height [layers]):", px, py, X0, Y0, secs.size()); out += buf;
	for (int d = -80; d <= 80; d += 16) { int n; const double h = heightAt(X0, Y0 + d / kKY, &n); snprintf(buf, sizeof(buf), " %+d:%.0f[%d]", d, h, n); out += buf; }
	out += "\neast-west (world X offset):";
	for (int d = -80; d <= 80; d += 16) { int n; const double h = heightAt(X0 + d, Y0, &n); snprintf(buf, sizeof(buf), " %+d:%.0f[%d]", d, h, n); out += buf; }
	return out + "\n";
}

// diagnostics: what is really in the live buffers of a cell compared with what the model intends (baseline + net plot changes)
std::string CellInfoImpl(int cx, int cy) {
	std::lock_guard<std::recursive_mutex> g(gMx);
	void* mgr = CellMgr(); int gw = 0, gh = 0, plotW = 0, plotH = 0;
	if (!mgr || !Dims(mgr, &gw, &gh) || !PlotGrid(&plotW, &plotH)) return "no world";
	if (cx < 0 || cy < 0 || cx >= gw || cy >= gh) return "cell out of range";
	std::vector<std::shared_ptr<const CellData>> base;
	if (!ReadCells({ { cx, cy } }, base, 3000) || !base[0]) return "baseline read failed";
	CellData live;
	if (!ReadLive(cx, cy, live, nullptr)) return "live read failed";
	const CellData& b = *base[0];
	std::vector<uint8_t> expBase = b.vb, expId = b.idvb;
	{ auto wk = gWork.find(KeyOf(cx, cy)); if (wk != gWork.end()) expBase = wk->second.vb; else ApplyDomes(expBase, cx, cy); }
	for (const PlotOp& op : gOps) {
		double pcx, pcy; Centre(op.x, op.y, &pcx, &pcy);
		Pair pb, pn; if (!PairOf(op.baseT, &pb) || !PairOf(op.curT, &pn)) continue;
		ApplyMaterials(expId, cx, cy, op.x, op.y, plotW, plotH, pb, pn);
	}
	char buf[600];
	size_t bChanged = 0, eChanged = 0, bad = 0; int maxLive = 0, maxExp = 0; double worstX = 0, worstY = 0; int worstL = 0, worstE = 0, worstH0 = 0, worstD = 0;
	const size_t n = (std::min)((std::min)(live.vb.size(), b.vb.size()), expBase.size()) / 8;
	for (size_t i = 0; i < n; i++) {
		uint16_t h0, hl, he; memcpy(&h0, &b.vb[i * 8 + 4], 2); memcpy(&hl, &live.vb[i * 8 + 4], 2); memcpy(&he, &expBase[i * 8 + 4], 2);
		if (memcmp(&b.vb[i * 8], &live.vb[i * 8], 8) != 0) bChanged++;
		if (memcmp(&b.vb[i * 8], &expBase[i * 8], 8) != 0) eChanged++;
		maxLive = (std::max)(maxLive, static_cast<int>(hl) - static_cast<int>(h0)); maxExp = (std::max)(maxExp, static_cast<int>(he) - static_cast<int>(h0));
		if (memcmp(&live.vb[i * 8], &expBase[i * 8], 8) != 0) {
			bad++;
			const int d = std::abs(static_cast<int>(hl) - static_cast<int>(he));
			if (d >= worstD) { worstD = d; worstL = hl; worstE = he; worstH0 = h0; VertexWorld(cx, cy, &b.vb[i * 8], &worstX, &worstY); }
		}
	}
	size_t idBad = 0, idChangedLive = 0;
	for (size_t i = 0; i + 8 <= (std::min)(live.idvb.size(), expId.size()); i += 8) { if (memcmp(&live.idvb[i], &expId[i], 8) != 0) idBad++; if (memcmp(&live.idvb[i], &b.idvb[i], 8) != 0) idChangedLive++; }
	const uint8_t* rec = Record(mgr, cx, cy);
	snprintf(buf, sizeof(buf), "cell %d,%d: saved=%d | base vertices %zu: changed live %zu, intended %zu, live != intended %zu (largest: h0 %d live %d intended %d at X %.0f Y %.0f); max rise live %d intended %d | ID vertices %zu: changed live %zu, live != intended %zu | record: idFirst %u vbFirst %u idVb %u vb %u | net plot changes %zu",
		cx, cy, gSaved.count(KeyOf(cx, cy)) ? 1 : 0, n, bChanged, eChanged, bad, worstH0, worstL, worstE, worstX, worstY, maxLive, maxExp, b.idvb.size() / 8, idChangedLive, idBad,
		rec ? ReadU32(rec, kIdFirst) : 0u, rec ? ReadU32(rec, kVbFirst) : 0u, rec ? (ReadU32(rec, kIdVb) & 0xffff) : 0u, rec ? (ReadU32(rec, kVbHandle) & 0xffff) : 0u, gOps.size());
	return buf;
}

namespace {

// milestone 3: dome (flat -> hills) in the base mesh and in the ground-height overlay; returns the number of failed checks
int DomeSelfTest(int tx, int ty, int tt, const std::vector<CellKey>& cand, const std::vector<std::shared_ptr<const CellData>>& base) {
	int fails = 0;
	auto check = [&](bool ok, const std::string& what) { if (!ok) fails++; L("dome self-test: %-80s %s", what.c_str(), ok ? "OK" : "FAILED"); };
	const int ht = (tt == 0) ? 1 : 4;
	Pair pn, po; PairOf(ht, &pn); PairOf(tt, &po);
	double pcx, pcy; Centre(tx, ty, &pcx, &pcy);
	SamplerGeom G; void* smp = nullptr;
	if (!SamplerGeometry(G, &smp)) { check(false, "sampler geometry available"); return fails; }
	L("dome self-test: plot (%d,%d) terrain %d -> %d (generated flat); sampler grid %d x %d, offsets (%.3f, %.3f), raster %d x %d", tx, ty, tt, ht, G.gw, G.gh, G.offx, G.offy, G.NX, G.NY);
	auto sampleAt = [&](double X, double Y) {
		const float x = static_cast<float>(SamplerX(G, X)), y = static_cast<float>(SamplerY(G, Y));
		uint32_t lx, ly; memcpy(&lx, &x, 4); memcpy(&ly, &y, 4);
		return reinterpret_cast<SamplerFn>(ExeBase() + gA->sampler)(smp, static_cast<uint64_t>(lx) | (static_cast<uint64_t>(ly) << 32), 1);
	};
	const float c0 = sampleAt(pcx, pcy), f0 = sampleAt(pcx + 300, pcy);
	std::vector<CellKey> touched;
	{
		std::lock_guard<std::recursive_mutex> g(gMx);
		check(PatchPlotsLocked({ { tx, ty, tt, ht } }, &touched) && !touched.empty(), "dome patch applied");
		check(gGrid.load() != nullptr, "overlay raster published");
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	const float c1 = sampleAt(pcx, pcy), f1 = sampleAt(pcx + 300, pcy);
	char buf[200];
	snprintf(buf, sizeof(buf), "sampler at the plot centre rose by %.3f (expected %.3f = alpha x dome height)", c1 - c0, kAlpha * kHillAmp);
	check(std::fabs((c1 - c0) - kAlpha * kHillAmp) < 0.05, buf);
	snprintf(buf, sizeof(buf), "sampler 300 texels away unchanged (diff %.5f)", f1 - f0);
	check(std::fabs(f1 - f0) < 1e-4, buf);
	int maxDh = 0;
	for (const CellKey& ck : touched) {
		size_t bi = 0; while (bi < cand.size() && !(cand[bi].x == ck.x && cand[bi].y == ck.y)) bi++;
		const CellData& b0 = *base[bi];
		CellData live; char tag[64]; snprintf(tag, sizeof(tag), "cell %d,%d", ck.x, ck.y);
		if (!ReadLive(ck.x, ck.y, live, nullptr)) { check(false, std::string(tag) + ": live read"); continue; }
		// the base mesh may have been subdivided: the ORIGINAL vertices are the first b0.vb.size() bytes (new ones are appended), the live buffer must equal the working copy
		const bool grew = live.vb.size() >= b0.vb.size();
		bool pos = grew, up = grew, farSame = grew, water = grew, model = false, newOk = grew; int cellMax = 0; size_t changed = 0;
		{
			std::lock_guard<std::recursive_mutex> g(gMx);
			auto wk = gWork.find(KeyOf(ck.x, ck.y));
			model = wk != gWork.end() && wk->second.vb == live.vb;
		}
		for (size_t o = 0; grew && o + 8 <= b0.vb.size(); o += 8) {
			const uint8_t* a = &b0.vb[o]; const uint8_t* b = &live.vb[o];
			if (memcmp(a, b, 4) != 0) pos = false;
			uint16_t h0, h1; memcpy(&h0, a + 4, 2); memcpy(&h1, b + 4, 2);
			if (h0 >= 30000 && memcmp(a, b, 8) != 0) water = false;
			if (h1 < h0 || h1 - h0 > kHillAmp + 1) up = false;
			cellMax = (std::max)(cellMax, static_cast<int>(h1) - static_cast<int>(h0));
			if (memcmp(a, b, 8) != 0) {
				changed++;
				double X, Y; VertexWorld(ck.x, ck.y, a, &X, &Y);
				const double d = std::sqrt((X - pcx) * (X - pcx) + ((Y - pcy) * kKY) * ((Y - pcy) * kKY));
				if (d >= kR1 + 1) farSame = false;
			}
		}
		for (size_t o = b0.vb.size(); grew && o + 8 <= live.vb.size(); o += 8) {      // new vertices lie in the zone and never rise above the highest original land vertex + dome
			uint16_t h1; memcpy(&h1, &live.vb[o + 4], 2);
			if (h1 < 30000) { int hmax = 0; for (size_t q = 0; q + 8 <= b0.vb.size(); q += 8) { uint16_t h; memcpy(&h, &b0.vb[q + 4], 2); if (h < 30000) hmax = (std::max)(hmax, static_cast<int>(h)); } if (h1 > hmax + kHillAmp + 1) newOk = false; }
		}
		maxDh = (std::max)(maxDh, cellMax);
		check(grew && pos, std::string(tag) + ": original base vertices keep their positions (new ones are appended)");
		check(up && water && newOk, std::string(tag) + ": heights only rise, by at most the dome height; ocean vertices untouched; new vertices plausible");
		check(farSame, std::string(tag) + ": original base vertices beyond the dome radius unchanged");
		check(model, std::string(tag) + ": live base vertices equal the patcher's working copy");
		bool idOk = live.idvb.size() == b0.idvb.size();
		for (size_t o = 0; idOk && o + 8 <= b0.idvb.size(); o += 8) {
			const uint8_t* a = &b0.idvb[o]; const uint8_t* b = &live.idvb[o];
			if (memcmp(a, b, 6) != 0) idOk = false;
			if ((a[6] != b[6] && b[6] != pn.a && b[6] != pn.b) || (a[7] != b[7] && b[7] != pn.a && b[7] != pn.b)) idOk = false;
		}
		check(idOk, std::string(tag) + ": materials changed only to the hills pair");
		Invalidate();
		std::vector<std::shared_ptr<const CellData>> again;
		check(ReadCells({ ck }, again, 5000) && again[0] && again[0]->vb == b0.vb && again[0]->idvb == b0.idvb && again[0]->ib == b0.ib, std::string(tag) + ": baseline read still returns the original base and ID data");
		(void)changed;
	}
	snprintf(buf, sizeof(buf), "no original vertex rose by more than the dome height (largest rise %d, dome height %.0f)", maxDh, kHillAmp);
	check(maxDh <= kHillAmp + 1, buf);
	{
		std::lock_guard<std::recursive_mutex> g(gMx);
		std::vector<CellKey> t2;
		check(PatchPlotsLocked({ { tx, ty, ht, tt } }, &t2), "dome removed (terrain back to the generated one)");
		check(gSaved.empty() && gOps.empty() && gGrid.load() == nullptr, "no patched cell, no net change and no overlay left");
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	const float c2 = sampleAt(pcx, pcy);
	snprintf(buf, sizeof(buf), "sampler at the plot centre back to its original value (diff %.5f)", c2 - c0);
	check(std::fabs(c2 - c0) < 1e-4, buf);
	for (const CellKey& ck : touched) {
		size_t bi = 0; while (bi < cand.size() && !(cand[bi].x == ck.x && cand[bi].y == ck.y)) bi++;
		CellData live; char tag[64]; snprintf(tag, sizeof(tag), "cell %d,%d after the dome is removed", ck.x, ck.y);
		check(ReadLive(ck.x, ck.y, live, nullptr) && live.vb == base[bi]->vb && live.idvb == base[bi]->idvb && live.ib == base[bi]->ib, std::string(tag) + ": live data identical to the original");
	}
	return fails;
}

}  // namespace

int PatchSelfTestImpl() {
	int fails = 0;
	auto check = [&](bool ok, const char* what) { if (!ok) fails++; L("patch self-test: %-72s %s", what, ok ? "OK" : "FAILED"); };
	int gw = 0, gh = 0, plotW = 0, plotH = 0;
	void* mgr = CellMgr();
	if (!mgr || !Dims(mgr, &gw, &gh) || !PlotGrid(&plotW, &plotH)) { L("patch self-test: no world"); return -1; }
	L("patch self-test: cell grid %dx%d, plot grid %dx%d (width = 2 x cell width: %s)", gw, gh, plotW, plotH, plotW == 2 * gw ? "yes" : "NO");
	check(plotW == 2 * gw, "plot grid width matches the cell grid");
	{ std::lock_guard<std::recursive_mutex> g(gMx); if (!gSaved.empty()) UndoAllLocked(); }
	// a test plot: a flat grass/plains plot whose six neighbours are flat grass/plains too
	int tx = -1, ty = -1, tt = -1;
	for (int y = 4; y < plotH - 4 && tx < 0; y++) for (int x = 4; x < plotW - 4 && tx < 0; x++) {
		int t; if (!PlotTerrain(x, y, &t) || (t != 0 && t != 3)) continue;
		bool all = true;
		for (int dx = -1; dx <= 1 && all; dx++) for (int dy = -1; dy <= 1 && all; dy++) { int u; if (!PlotTerrain(x + dx, y + dy, &u) || (u != 0 && u != 3)) all = false; }
		if (all) { tx = x; ty = y; tt = t; }
	}
	if (tx < 0) { L("patch self-test: no flat grass/plains area found: cannot run"); return -1; }
	const int nt = (tt == 0) ? 3 : 0;
	Pair po, pn; PairOf(tt, &po); PairOf(nt, &pn);
	L("patch self-test: plot (%d,%d) terrain %d -> %d (view only), materials (%d,%d) -> (%d,%d)", tx, ty, tt, nt, po.a, po.b, pn.a, pn.b);
	std::vector<CellKey> cand = Candidates(tx, ty, gw, gh);
	std::vector<std::shared_ptr<const CellData>> base;
	check(ReadCells(cand, base, 5000), "baseline of the candidate cells read");
	if (base.size() != cand.size() || !base[0]) return fails ? fails : -1;
	std::vector<CellKey> touched;
	{
		std::lock_guard<std::recursive_mutex> g(gMx);
		check(PatchPlotsLocked({ { tx, ty, tt, nt } }, &touched), "patch applied");
	}
	check(!touched.empty(), "at least one cell touched");
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	double pcx, pcy; Centre(tx, ty, &pcx, &pcy);
	for (const CellKey& ck : touched) {
		size_t bi = 0; while (bi < cand.size() && !(cand[bi].x == ck.x && cand[bi].y == ck.y)) bi++;
		const CellData& b0 = *base[bi];
		CellData live; std::vector<uint16_t> liveBlock;
		char tag[96];
		if (!ReadLive(ck.x, ck.y, live, &liveBlock)) { check(false, "live read of a patched cell"); continue; }
		snprintf(tag, sizeof(tag), "cell %d,%d", ck.x, ck.y);
		// 1. same vertices and weights, materials changed only close to the plot and only to the new pair
		bool sameShape = live.idvb.size() == b0.idvb.size(), geomSame = sameShape, matOk = sameShape, nearOk = sameShape; size_t changedV = 0;
		for (size_t o = 0; sameShape && o + 8 <= b0.idvb.size(); o += 8) {
			const uint8_t* a = &b0.idvb[o]; const uint8_t* b = &live.idvb[o];
			if (memcmp(a, b, 6) != 0) geomSame = false;
			if (a[6] != b[6] || a[7] != b[7]) {
				changedV++;
				double X, Y; VertexWorld(ck.x, ck.y, a, &X, &Y);
				const double d = std::sqrt((X - pcx) * (X - pcx) + ((Y - pcy) * kKY) * ((Y - pcy) * kKY));
				if (d > 140) nearOk = false;
				if (!((b[6] == a[6] || b[6] == pn.a || b[6] == pn.b) && (b[7] == a[7] || b[7] == pn.a || b[7] == pn.b))) matOk = false;
			}
		}
		check(sameShape && geomSame, (std::string(tag) + ": vertex count, positions and weights unchanged").c_str());
		check(matOk && nearOk, (std::string(tag) + ": materials changed only to the new pair, only near the plot").c_str());
		check(changedV > 0, (std::string(tag) + ": at least one vertex changed").c_str());
		// 2. triangle slots: every slot holds exactly the triangles whose weighted vertices use that material
		std::vector<std::array<uint16_t, 3>> tris; CellTriangles(b0, tris);
		bool slotsOk = true; size_t total = 0;
		for (int k = 0; k < 32 && slotsOk; k++) {
			std::unordered_set<uint64_t> got;
			const uint32_t cnt = live.table[k][0], st = live.table[k][1];
			for (uint32_t t = 0; t < cnt; t++) { const size_t o = static_cast<size_t>(st) + 3 * t; if (o + 3 > liveBlock.size()) { slotsOk = false; break; } got.insert(static_cast<uint64_t>(liveBlock[o]) | (static_cast<uint64_t>(liveBlock[o + 1]) << 16) | (static_cast<uint64_t>(liveBlock[o + 2]) << 32)); }
			std::unordered_set<uint64_t> want;
			for (auto& t : tris) {
				bool has = false;
				for (int v = 0; v < 3; v++) { const uint8_t* vv = &live.idvb[static_cast<size_t>(t[v]) * 8]; if ((vv[4] > 0 && vv[6] == k) || (vv[5] > 0 && vv[7] == k)) has = true; }
				if (has) want.insert(static_cast<uint64_t>(t[0]) | (static_cast<uint64_t>(t[1]) << 16) | (static_cast<uint64_t>(t[2]) << 32));
			}
			if (got != want) slotsOk = false;
			total += got.size();
		}
		check(slotsOk, (std::string(tag) + ": every material slot holds exactly its triangles").c_str());
		// 3. the original block part of the new index buffer is identical to the original buffer (base triangles survive)
		std::vector<Range> r{ { 1, b0.idIbHandle, 0, HandleBytes(1, b0.idIbHandle), 0 } };
		std::vector<uint8_t> orig; bool blockOk = ReadBackItems(r, orig, nullptr, 5000) && orig.size() <= liveBlock.size() * 2 && memcmp(orig.data(), liveBlock.data(), orig.size()) == 0;
		check(blockOk, (std::string(tag) + ": original index block preserved in the new buffer").c_str());
		// 4. baseline reads of a patched cell still return the ORIGINAL data
		Invalidate();
		std::vector<std::shared_ptr<const CellData>> again;
		const bool rd = ReadCells({ ck }, again, 5000) && again[0] && again[0]->idvb == b0.idvb && again[0]->vb == b0.vb && again[0]->ib == b0.ib;
		check(rd, (std::string(tag) + ": baseline read of the patched cell returns the original data").c_str());
	}
	// 5. patching back to the original terrain restores the baseline exactly and gives the cell its original record back
	{
		std::lock_guard<std::recursive_mutex> g(gMx);
		std::vector<CellKey> t2;
		check(PatchPlotsLocked({ { tx, ty, nt, tt } }, &t2), "patch back to the original terrain applied");
		check(gSaved.empty() && gOps.empty(), "no patched cell and no net change left after patching back");
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	for (const CellKey& ck : touched) {
		size_t bi = 0; while (bi < cand.size() && !(cand[bi].x == ck.x && cand[bi].y == ck.y)) bi++;
		CellData live; char tag[96]; snprintf(tag, sizeof(tag), "cell %d,%d after patching back", ck.x, ck.y);
		check(ReadLive(ck.x, ck.y, live, nullptr) && live.idvb == base[bi]->idvb && live.ib == base[bi]->ib && live.vb == base[bi]->vb, (std::string(tag) + ": live data identical to the original").c_str());
	}
	// 6. patch again, then undo: every patched cell is restored
	std::vector<CellKey> touched2;
	{
		std::lock_guard<std::recursive_mutex> g(gMx);
		check(PatchPlotsLocked({ { tx, ty, tt, nt } }, &touched2) && !touched2.empty(), "second patch applied");
		const int n = UndoAllLocked();
		check(n == static_cast<int>(touched2.size()), "undo restored every patched cell");
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	for (const CellKey& ck : touched2) {
		size_t bi = 0; while (bi < cand.size() && !(cand[bi].x == ck.x && cand[bi].y == ck.y)) bi++;
		CellData live; char tag[96]; snprintf(tag, sizeof(tag), "cell %d,%d after undo", ck.x, ck.y);
		check(ReadLive(ck.x, ck.y, live, nullptr) && live.idvb == base[bi]->idvb && live.ib == base[bi]->ib, (std::string(tag) + ": live data identical to the original").c_str());
	}
	// 7. two plots next to each other: patch B, then A, then put A back: the cells equal "only B patched"
	{
		int t2t = -1; PlotTerrain(tx + 1, ty, &t2t);
		Pair pp;
		if (PairOf(t2t, &pp) && t2t != nt) {
			std::vector<CellKey> a1, a2, a3;
			std::lock_guard<std::recursive_mutex> g(gMx);
			PatchPlotsLocked({ { tx + 1, ty, t2t, nt } }, &a1);                // only the neighbour patched: reference state
			std::vector<std::vector<uint8_t>> refs; std::vector<CellKey> refKeys;
			for (const CellKey& ck : cand) { CellData lv; if (ReadLive(ck.x, ck.y, lv, nullptr)) { refs.push_back(lv.idvb); refKeys.push_back(ck); } }
			PatchPlotsLocked({ { tx, ty, tt, nt } }, &a2);                     // both patched
			PatchPlotsLocked({ { tx, ty, nt, tt } }, &a3);                     // first put back
			bool same = true;
			for (size_t i = 0; i < refKeys.size(); i++) { CellData lv; if (!ReadLive(refKeys[i].x, refKeys[i].y, lv, nullptr) || lv.idvb != refs[i]) same = false; }
			check(same, "order independence: patch B, patch A, put A back == only B patched");
			UndoAllLocked();
		}
	}
	fails += DomeSelfTest(tx, ty, tt, cand, base);
	L("patch self-test: RESULT %s (%d failure(s))", fails == 0 ? "PASS" : "FAIL", fails);
	return fails;
}

int PatchSelfTest() {
	const bool objWas = ObjectsEnabled();
	SetObjectsEnabled(false);                                                  // the patch tests change plots that may carry objects: do not re-place them
	const int r = PatchSelfTestImpl();
	SetObjectsEnabled(objWas);
	if (MarkerExists("TerrainView.objtest")) { const int o = ObjectsSelfTest(); return r < 0 ? r : r + (o > 0 ? o : 0); }
	return r;
}

}  // namespace detail
}  // namespace TerrainView

extern "C" __declspec(dllexport) int TerrainView_Command(const char* cmd, char* out, int outLen) {
	const std::string r = TerrainView::detail::Command(cmd ? cmd : "");
	if (out && outLen > 0) { strncpy_s(out, static_cast<size_t>(outLen), r.c_str(), _TRUNCATE); }
	return static_cast<int>(r.size());
}
