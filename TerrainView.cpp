// SPDX-License-Identifier: AGPL-3.0-only
// Part of Dev CE, a fork of the Civilization VI Community Extension by Wild-W (https://github.com/Wild-W/CivilizationVI_CommunityExtension).
// Copyright (C) 2026 cru121. Licensed under the GNU Affero General Public License v3.0 (see LICENSE.txt). Not endorsed by Wild-W, Firaxis or 2K.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <unordered_map>
#include <vector>
#include "MinHook.h"
#include "DevBridge.h"
#include "TerrainView.h"
#include "TerrainViewInternal.h"

// Terrain view sync, milestone 1: lazy per-cell reader. Overview: dev-ce/TERRAIN_VIEW.md.
namespace TerrainView {
namespace detail {

// ---- fixed addresses of the Windows DX11 exe, build 15038592 (the game is no longer developed, so no signature scan) -----------------------------------------------
constexpr uint32_t kExeTimestamp = 0x667c6fc6;       // IMAGE_FILE_HEADER.TimeDateStamp of CivilizationVI.exe
constexpr uint32_t kExeImageSize = 0x3704000;        // IMAGE_OPTIONAL_HEADER.SizeOfImage
constexpr uintptr_t kCtxGlobal = 0x2305e20;          // TerrainContext global; *(this + 8) = impl; impl + 0x5a0 = Terrain::Cell::Manager
constexpr uintptr_t kGetRec = 0xc5fee0;              // pointer Cell::Manager::GetRecord(mgr, uint64 key = x | y << 32)
constexpr uintptr_t kDims = 0xc5ff20;                // Cell::Manager::GetDimensions(mgr, int[2])
constexpr uintptr_t kVbPool = 0x2b99740;             // ID3D11Buffer* table indexed by vertex buffer handle (8 bytes per entry)
constexpr uintptr_t kIbPool = 0x2b12c80;             // ... by index buffer handle

uintptr_t gExe = 0;
uintptr_t ExeBase() { return gExe; }
char gDllDir[MAX_PATH] = { 0 };
std::function<bool(int, int, Sig&)> gOrigProvider;
void SetOriginalSigProvider(std::function<bool(int, int, Sig&)> fn) { gOrigProvider = std::move(fn); }
bool MarkerExists(const char* name) { char p[MAX_PATH]; strcpy_s(p, gDllDir); strcat_s(p, name); return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }
bool gBuildOk = false;
std::atomic<bool> gCreated{ false };

void L(const char* fmt, ...) {
	char buf[900];
	va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
	DevBridge::Log("[TerrainView] %s", buf);
}

// ---- guarded raw memory access (no C++ objects in functions with __try) -----------------------------------------------------------------------------------------------
bool SafeCopy(void* dst, const void* src, size_t n) {
	__try { memcpy(dst, src, n); return true; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void* CellMgr() {
	uintptr_t impl = 0;
	if (!gExe || !SafeCopy(&impl, reinterpret_cast<const void*>(gExe + kCtxGlobal + 8), sizeof(impl)) || !impl) return nullptr;
	return reinterpret_cast<void*>(impl + 0x5a0);
}

bool Dims(void* mgr, int* w, int* h) {
	typedef void* (__fastcall* Fn)(void*, int*);
	int d[2] = { 0, 0 };
	__try { reinterpret_cast<Fn>(gExe + kDims)(mgr, d); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	*w = d[0]; *h = d[1];
	return d[0] > 0 && d[1] > 0 && d[0] * d[1] < 100000;
}

const uint8_t* Record(void* mgr, int x, int y) {
	typedef void* (__fastcall* Fn)(void*, uint64_t);
	const uint64_t key = static_cast<uint32_t>(x) | (static_cast<uint64_t>(static_cast<uint32_t>(y)) << 32);
	__try { return static_cast<const uint8_t*>(reinterpret_cast<Fn>(gExe + kGetRec)(mgr, key)); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

ID3D11Buffer* PoolBuf(int kind, uint32_t h) {
	if (h == 0 || h >= kMaxHandle) return nullptr;
	ID3D11Buffer* p = nullptr;
	if (!SafeCopy(&p, reinterpret_cast<const void*>(gExe + (kind == 0 ? kVbPool : kIbPool) + static_cast<uintptr_t>(h) * 8), sizeof(p))) return nullptr;
	return p;
}

uint32_t BufBytes(ID3D11Buffer* b) {
	if (!b) return 0;
	D3D11_BUFFER_DESC d = {};
	__try { b->GetDesc(&d); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
	return d.ByteWidth;
}

// ---- cell record signature --------------------------------------------------------------------------------------------------------------------------------------------
bool ReadSig(const uint8_t* r, Sig& s) {
	if (!r) return false;
	uint8_t raw[944];
	if (!SafeCopy(raw, r, sizeof(raw))) return false;
	auto u32 = [&](size_t off) { uint32_t v; memcpy(&v, raw + off, 4); return v; };
	s = Sig();
	s.idVb = u32(kIdVb) & 0xffff; s.idFirst = u32(kIdFirst); s.idCount = u32(kIdCount);
	s.vb = u32(kVbHandle) & 0xffff; s.vbFirst = u32(kVbFirst); s.vbCount = u32(kVbCount);
	s.idIb = u32(kIdIb) & 0xffff;
	for (int i = 0; i < 64; i++) s.table[i] = u32(kTable + 4 * i);
	return true;
}

void IbRange(const Sig& s, uint32_t* lo, uint32_t* hi) {
	uint32_t l = 0xffffffffu, h = 0;
	for (int i = 0; i < 32; i++) {
		const uint32_t cnt = s.table[2 * i], st = s.table[2 * i + 1];
		if (cnt) { l = (std::min)(l, st); h = (std::max)(h, st + cnt * 3); }
	}
	if (h == 0) l = 0;
	*lo = l; *hi = h;
}

// ---- render-thread job queue ------------------------------------------------------------------------------------------------------------------------------------------
struct Job {
	std::vector<Range> items;
	uint32_t total = 0;
	std::vector<uint8_t> data;
	int err = 0;                     // 0 ok, otherwise a code (see ExecuteRaw)
	bool done = false;
	double ms = 0;
	std::mutex m;
	std::condition_variable cv;
};

std::mutex gSerialize;               // one request at a time
std::mutex gQueueMx;
std::shared_ptr<Job> gPendingJob;
std::atomic<bool> gHasPending{ false };
std::atomic<bool> gHooked{ false };
ID3D11DeviceContext* gImmCtx = nullptr;

typedef void (STDMETHODCALLTYPE* DrawIndexedInstancedFn)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
typedef void (STDMETHODCALLTYPE* DrawInstancedFn)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
DrawIndexedInstancedFn orig_DII = nullptr;
DrawInstancedFn orig_DI = nullptr;

// Runs on the render thread inside a draw call of the immediate context. Raw pointers only (SEH guard); returns 0 on success.
int ExecuteRaw(ID3D11DeviceContext* ctx, const Range* items, size_t n, uint32_t total, uint8_t* out) {
	int code = 0;
	ID3D11Device* dev = nullptr;
	ID3D11Buffer* st = nullptr;
	__try {
		ctx->GetDevice(&dev);
		D3D11_BUFFER_DESC d = {};
		d.ByteWidth = total; d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (FAILED(dev->CreateBuffer(&d, nullptr, &st))) { code = 1; }
		for (size_t i = 0; code == 0 && i < n; i++) {
			ID3D11Buffer* src = PoolBuf(items[i].kind, items[i].handle);
			if (!src) { code = 2; break; }
			D3D11_BUFFER_DESC sd = {}; src->GetDesc(&sd);
			if (static_cast<uint64_t>(items[i].off) + items[i].len > sd.ByteWidth) { code = 3; break; }
			D3D11_BOX box = { items[i].off, 0, 0, items[i].off + items[i].len, 1, 1 };
			ctx->CopySubresourceRegion(st, 0, items[i].at, 0, 0, src, 0, &box);
		}
		if (code == 0) {
			D3D11_MAPPED_SUBRESOURCE m = {};
			if (FAILED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) code = 4;
			else { memcpy(out, m.pData, total); ctx->Unmap(st, 0); }
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { code = 5; }
	if (st) st->Release();
	if (dev) dev->Release();
	return code;
}

void RunPending(ID3D11DeviceContext* self) {
	if (self != gImmCtx) return;                                  // only the immediate context may be used from here
	if (!gHasPending.exchange(false)) return;
	std::shared_ptr<Job> job;
	{ std::lock_guard<std::mutex> g(gQueueMx); job = gPendingJob; gPendingJob.reset(); }
	if (!job) return;
	const auto t0 = std::chrono::steady_clock::now();
	job->err = ExecuteRaw(self, job->items.data(), job->items.size(), job->total, job->data.data());
	job->ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	{ std::lock_guard<std::mutex> g(job->m); job->done = true; }
	job->cv.notify_all();
}

void STDMETHODCALLTYPE Hook_DrawIndexedInstanced(ID3D11DeviceContext* self, UINT a, UINT b, UINT c, INT d, UINT e) {
	if (gHasPending.load(std::memory_order_relaxed)) RunPending(self);
	orig_DII(self, a, b, c, d, e);
}
void STDMETHODCALLTYPE Hook_DrawInstanced(ID3D11DeviceContext* self, UINT a, UINT b, UINT c, UINT d) {
	if (gHasPending.load(std::memory_order_relaxed)) RunPending(self);
	orig_DI(self, a, b, c, d);
}

// The immediate context is found through any terrain buffer: buffer -> device -> immediate context. The game uses DrawIndexedInstanced (vtable slot 20) and DrawInstanced (21).
bool InstallHook(ID3D11Buffer* anyBuffer) {
	if (gHooked.load()) return true;
	if (!anyBuffer) return false;
	ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
	anyBuffer->GetDevice(&dev);
	if (!dev) return false;
	dev->GetImmediateContext(&ctx);
	dev->Release();
	if (!ctx) return false;
	void** vt = *reinterpret_cast<void***>(ctx);
	gImmCtx = ctx;                                                // keep the reference for the life of the process
	MH_STATUS s1 = MH_CreateHook(vt[20], reinterpret_cast<LPVOID>(&Hook_DrawIndexedInstanced), reinterpret_cast<LPVOID*>(&orig_DII));
	MH_STATUS s2 = MH_CreateHook(vt[21], reinterpret_cast<LPVOID>(&Hook_DrawInstanced), reinterpret_cast<LPVOID*>(&orig_DI));
	if (s1 != MH_OK || s2 != MH_OK) { L("CreateHook failed: %s / %s", MH_StatusToString(s1), MH_StatusToString(s2)); return false; }
	if (MH_EnableHook(vt[20]) != MH_OK || MH_EnableHook(vt[21]) != MH_OK) { L("EnableHook failed"); return false; }
	gHooked.store(true);
	L("draw hooks installed: immediate context %p, DrawIndexedInstanced %p, DrawInstanced %p", static_cast<void*>(ctx), vt[20], vt[21]);
	return true;
}

// Blocking: queue the job and wait for the render thread. Assigns `at` and fills `data` (total bytes).
bool ReadBackItems(std::vector<Range>& items, std::vector<uint8_t>& data, double* msOut, int timeoutMs) {
	if (items.empty()) { data.clear(); if (msOut) *msOut = 0; return true; }
	std::lock_guard<std::mutex> serial(gSerialize);
	if (!InstallHook(PoolBuf(items[0].kind, items[0].handle))) return false;
	uint32_t total = 0;
	for (auto& it : items) { it.at = total; total += it.len; }
	auto job = std::make_shared<Job>();
	job->items = items; job->total = total; job->data.assign(total, 0);
	{ std::lock_guard<std::mutex> g(gQueueMx); gPendingJob = job; }
	gHasPending.store(true);
	std::unique_lock<std::mutex> lk(job->m);
	if (!job->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return job->done; })) {
		lk.unlock();
		{ std::lock_guard<std::mutex> g(gQueueMx); if (gPendingJob == job) { gPendingJob.reset(); gHasPending.store(false); } }
		lk.lock();
		// if the render thread already took the job it is running: wait for it
		if (!job->cv.wait_for(lk, std::chrono::milliseconds(3000), [&] { return job->done; })) { L("job timed out (no draw call within %d ms)", timeoutMs); return false; }
		if (job->err == 0 && job->done && gPendingJob != job) { /* completed just in time */ }
	}
	if (job->err != 0) { L("render-thread job failed with code %d", job->err); return false; }
	data = std::move(job->data);
	if (msOut) *msOut = job->ms;
	return true;
}

// ---- cache ------------------------------------------------------------------------------------------------------------------------------------------------------------
struct CacheEntry { Sig sig; std::shared_ptr<const CellData> data; };
std::mutex gCacheMx;
std::unordered_map<uint64_t, CacheEntry> gCache;
Stats gStats = {};
inline uint64_t KeyOf(int x, int y) { return static_cast<uint32_t>(x) | (static_cast<uint64_t>(static_cast<uint32_t>(y)) << 32); }

bool SigPlausible(const Sig& s) {
	if (s.idCount > 0x20000 || s.vbCount > 0x20000) return false;
	if (s.idCount && (s.idVb == 0 || s.idVb >= kMaxHandle)) return false;       // a handle only has to be valid when the cell uses that buffer
	if (s.vbCount && (s.vb == 0 || s.vb >= kMaxHandle)) return false;
	return true;
}

// the ranges one cell needs (up to three); returns false if the record does not describe valid ranges
bool CellRanges(const Sig& s, std::vector<Range>& out, uint32_t* lo, uint32_t* hi) {
	if (!SigPlausible(s)) return false;
	IbRange(s, lo, hi);
	if (s.idCount) out.push_back({ 0, s.idVb, s.idFirst * 8u, s.idCount * 8u, 0 });
	if (s.vbCount) out.push_back({ 0, s.vb, s.vbFirst * 8u, s.vbCount * 8u, 0 });
	if (*hi > *lo) {
		if (s.idIb == 0 || s.idIb >= kMaxHandle) return false;
		out.push_back({ 1, s.idIb, *lo * 2u, (*hi - *lo) * 2u, 0 });
	}
	return true;
}

}  // namespace detail
using namespace detail;

bool Available() {
	if (!gBuildOk) return false;
	void* mgr = CellMgr(); int w, h;
	return mgr && Dims(mgr, &w, &h);
}

bool GridSize(int* w, int* h) {
	void* mgr = gBuildOk ? CellMgr() : nullptr; int a = 0, b = 0;
	if (!mgr || !Dims(mgr, &a, &b)) return false;
	if (w) *w = a; if (h) *h = b;
	return true;
}

void Invalidate() { std::lock_guard<std::mutex> g(gCacheMx); gCache.clear(); }
Stats GetStats() { std::lock_guard<std::mutex> g(gCacheMx); return gStats; }

bool ReadCells(const std::vector<CellKey>& keys, std::vector<std::shared_ptr<const CellData>>& out, int timeoutMs) {
	out.assign(keys.size(), nullptr);
	if (!gBuildOk) return false;
	void* mgr = CellMgr(); int gw = 0, gh = 0;
	if (!mgr || !Dims(mgr, &gw, &gh)) return false;
	std::lock_guard<std::mutex> cache(gCacheMx);
	gStats.requests++; gStats.cellsRequested += static_cast<unsigned long>(keys.size());

	struct Need { size_t first; int x, y; Sig sig; uint32_t lo, hi; size_t itemStart, itemCount; };
	std::vector<Need> need; std::vector<Range> items;
	std::unordered_map<uint64_t, size_t> needIdx;
	for (size_t i = 0; i < keys.size(); i++) {
		const int x = keys[i].x, y = keys[i].y;
		if (x < 0 || y < 0 || x >= gw || y >= gh) { gStats.failures++; return false; }
		Sig s;
		const bool patched = gOrigProvider && gOrigProvider(x, y, s);          // a cell patched by us: its baseline is in the ORIGINAL buffers
		if (!patched && !ReadSig(Record(mgr, x, y), s)) { gStats.failures++; return false; }
		auto it = gCache.find(KeyOf(x, y));
		if (it != gCache.end() && it->second.sig == s) { out[i] = it->second.data; gStats.cacheHits++; continue; }
		if (needIdx.count(KeyOf(x, y))) continue;
		Need n{ i, x, y, s, 0, 0, items.size(), 0 };
		if (!CellRanges(s, items, &n.lo, &n.hi)) { gStats.failures++; return false; }
		n.itemCount = items.size() - n.itemStart;
		needIdx[KeyOf(x, y)] = need.size(); need.push_back(n);
	}
	if (!need.empty()) {
		std::vector<uint8_t> data; double ms = 0;
		if (!ReadBackItems(items, data, &ms, timeoutMs)) { gStats.failures++; gStats.timeouts++; return false; }
		gStats.jobs++; gStats.msInJobs += ms; gStats.bytesRead += data.size(); gStats.cellsRead += static_cast<unsigned long>(need.size());
		for (auto& n : need) {
			auto cd = std::make_shared<CellData>();
			cd->x = n.x; cd->y = n.y;
			cd->idVbHandle = n.sig.idVb; cd->idFirst = n.sig.idFirst; cd->idCount = n.sig.idCount;
			cd->vbHandle = n.sig.vb; cd->vbFirst = n.sig.vbFirst; cd->vbCount = n.sig.vbCount;
			cd->idIbHandle = n.sig.idIb; cd->ibLo = n.lo; cd->ibHi = n.hi;
			for (int k = 0; k < 32; k++) { cd->table[k][0] = n.sig.table[2 * k]; cd->table[k][1] = n.sig.table[2 * k + 1]; }
			size_t p = n.itemStart;
			if (n.sig.idCount) { const Range& r = items[p++]; cd->idvb.assign(data.begin() + r.at, data.begin() + r.at + r.len); }
			if (n.sig.vbCount) { const Range& r = items[p++]; cd->vb.assign(data.begin() + r.at, data.begin() + r.at + r.len); }
			if (n.hi > n.lo) { const Range& r = items[p++]; cd->ib.resize((n.hi - n.lo)); memcpy(cd->ib.data(), data.data() + r.at, r.len); }
			gCache[KeyOf(n.x, n.y)] = CacheEntry{ n.sig, cd };
			out[n.first] = cd;
		}
		// duplicates in `keys` and keys that were already needed
		for (size_t i = 0; i < keys.size(); i++) if (!out[i]) out[i] = gCache[KeyOf(keys[i].x, keys[i].y)].data;
	}
	return true;
}

// ---- self-test ---------------------------------------------------------------------------------------------------------------------------------------------------------
int SelfTest(int timeoutMs) {
	if (!gBuildOk) { L("self-test: build check failed"); return -1; }
	void* mgr = CellMgr(); int gw = 0, gh = 0;
	if (!mgr || !Dims(mgr, &gw, &gh)) { L("self-test: no cell manager"); return -1; }
	const int ncell = gw * gh;
	std::vector<Sig> sigs(ncell);
	for (int y = 0; y < gh; y++) for (int x = 0; x < gw; x++) if (!ReadSig(Record(mgr, x, y), sigs[y * gw + x])) { L("self-test: cannot read record %d,%d", x, y); return -1; }

	// 1. full read-back of every buffer the cells use (the reference)
	std::vector<Range> full; std::unordered_map<uint64_t, uint32_t> fullAt;      // (kind << 32 | handle) -> offset in the full data
	for (const Sig& s : sigs) {
		const struct { int kind; uint32_t h; } bs[3] = { {0, s.idVb}, {0, s.vb}, {1, s.idIb} };
		for (auto& b : bs) {
			if (b.h == 0 || b.h >= kMaxHandle) continue;
			const uint64_t k = (static_cast<uint64_t>(b.kind) << 32) | b.h;
			if (fullAt.count(k)) continue;
			const uint32_t bytes = BufBytes(PoolBuf(b.kind, b.h));
			if (!bytes) continue;
			fullAt[k] = 0; full.push_back({ b.kind, b.h, 0, bytes, 0 });
		}
	}
	std::vector<uint8_t> fullData; double fullMs = 0;
	const auto t0 = std::chrono::steady_clock::now();
	if (!ReadBackItems(full, fullData, &fullMs, timeoutMs)) { L("self-test: full read-back failed"); return -1; }
	const double fullWall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	for (auto& r : full) fullAt[(static_cast<uint64_t>(r.kind) << 32) | r.handle] = r.at;
	L("self-test: grid %dx%d = %d cells; full read-back: %zu buffers, %.1f MiB, render-thread %.1f ms (wall %.1f ms)", gw, gh, ncell, full.size(), fullData.size() / 1048576.0, fullMs, fullWall);

	auto expectedEqual = [&](const CellData& c, const Sig& s) -> bool {
		uint32_t lo, hi; IbRange(s, &lo, &hi);
		auto at = [&](int kind, uint32_t h) { return fullAt.at((static_cast<uint64_t>(kind) << 32) | h); };
		bool ok = c.idvb.size() == s.idCount * 8u && c.vb.size() == s.vbCount * 8u && c.ib.size() == hi - lo && c.ibLo == lo && c.ibHi == hi;
		if (ok && s.idCount) ok = memcmp(c.idvb.data(), fullData.data() + at(0, s.idVb) + s.idFirst * 8u, c.idvb.size()) == 0;
		if (ok && s.vbCount) ok = memcmp(c.vb.data(), fullData.data() + at(0, s.vb) + s.vbFirst * 8u, c.vb.size()) == 0;
		if (ok && hi > lo) ok = memcmp(c.ib.data(), fullData.data() + at(1, s.idIb) + lo * 2u, c.ib.size() * 2) == 0;
		if (ok) for (int k = 0; k < 32 && ok; k++) ok = c.table[k][0] == s.table[2 * k] && c.table[k][1] == s.table[2 * k + 1];
		return ok;
	};

	// 2. targeted reads, compared with slices of the full data
	int mismatches = 0;
	auto run = [&](const char* label, std::vector<CellKey> keys) {
		Invalidate();
		const Stats before = GetStats();
		std::vector<std::shared_ptr<const CellData>> out;
		const auto a = std::chrono::steady_clock::now();
		const bool ok = ReadCells(keys, out, timeoutMs);
		const double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
		int bad = 0; unsigned long long bytes = 0;
		if (!ok) bad = static_cast<int>(keys.size());
		else for (size_t i = 0; i < keys.size(); i++) {
			const CellData& c = *out[i];
			bytes += c.idvb.size() + c.vb.size() + c.ib.size() * 2;
			if (!expectedEqual(c, sigs[keys[i].y * gw + keys[i].x])) bad++;
		}
		const Stats after = GetStats();
		L("self-test: %-22s %4zu cell(s): %s, %.0f KB, %.1f ms wall (%.1f ms in the render-thread job, %lu job(s)), identical to the full read-back: %d of %zu", label, keys.size(),
			ok ? "read" : "FAILED", bytes / 1024.0, wall, after.msInJobs - before.msInJobs, after.jobs - before.jobs, static_cast<int>(keys.size()) - bad, keys.size());
		mismatches += bad;
	};
	std::mt19937 rng(5);
	std::vector<CellKey> all; for (int y = 0; y < gh; y++) for (int x = 0; x < gw; x++) all.push_back({ x, y });
	std::vector<CellKey> shuffled = all; std::shuffle(shuffled.begin(), shuffled.end(), rng);
	// the biggest cell
	size_t big = 0, bigBytes = 0;
	for (int i = 0; i < ncell; i++) { const size_t b = (static_cast<size_t>(sigs[i].idCount) + sigs[i].vbCount) * 8; if (b > bigBytes) { bigBytes = b; big = i; } }
	run("one cell (biggest)", { all[big] });
	run("one cell (first)", { all[0] });
	run("one cell (last)", { all.back() });
	run("13 cells (one plot)", std::vector<CellKey>(shuffled.begin(), shuffled.begin() + (std::min)(13, ncell)));
	run("100 cells", std::vector<CellKey>(shuffled.begin(), shuffled.begin() + (std::min)(100, ncell)));
	run("all cells, one job", all);

	// 3. the cache: the same 13 cells again must be served without a job
	{
		Invalidate();
		std::vector<CellKey> k13(shuffled.begin(), shuffled.begin() + (std::min)(13, ncell));
		std::vector<std::shared_ptr<const CellData>> o1, o2;
		ReadCells(k13, o1, timeoutMs);
		const Stats b = GetStats();
		const bool ok = ReadCells(k13, o2, timeoutMs);
		const Stats a = GetStats();
		const bool hits = ok && a.jobs == b.jobs && a.cacheHits - b.cacheHits == k13.size();
		L("self-test: cache: second read of 13 cells made %lu job(s), %lu cache hit(s): %s", a.jobs - b.jobs, a.cacheHits - b.cacheHits, hits ? "OK" : "FAILED");
		if (!hits) mismatches++;
	}
	// 4. out-of-range keys fail cleanly
	{
		std::vector<std::shared_ptr<const CellData>> o;
		const bool r1 = ReadCells({ {gw, 0} }, o, timeoutMs), r2 = ReadCells({ {0, -1} }, o, timeoutMs);
		L("self-test: out-of-range cells rejected: %s", (!r1 && !r2) ? "OK" : "FAILED");
		if (r1 || r2) mismatches++;
	}
	Invalidate();
	const Stats st = GetStats();
	L("self-test: RESULT %s (%d mismatch(es)); totals: %lu requests, %lu cells read, %lu cache hits, %lu jobs, %.1f MiB, %lu failure(s)", mismatches == 0 ? "PASS" : "FAIL", mismatches, st.requests, st.cellsRead, st.cacheHits, st.jobs,
		st.bytesRead / 1048576.0, st.failures);
	return mismatches;
}

// ---- startup -----------------------------------------------------------------------------------------------------------------------------------------------------------
namespace {

uint64_t WorldSig() {
	void* mgr = CellMgr(); int w = 0, h = 0;
	if (!mgr || !Dims(mgr, &w, &h)) return 0;
	uint64_t hsh = 1469598103934665603ull;
	auto mix = [&](uint32_t v) { hsh ^= v; hsh *= 1099511628211ull; };
	mix(w); mix(h);
	const int pts[5][2] = { {0, 0}, {w - 1, h - 1}, {w / 2, h / 2}, {1, 1}, {w / 3, h / 3} };
	for (auto& p : pts) {
		Sig s;
		if (!ReadSig(Record(mgr, p[0], p[1]), s) || !SigPlausible(s)) return 0;
		mix(s.idVb); mix(s.idFirst); mix(s.idCount); mix(s.vb); mix(s.vbFirst); mix(s.vbCount); mix(s.idIb); mix(s.table[1]);
	}
	return hsh;
}

DWORD WINAPI SelfTestThread(LPVOID) {
	uint64_t lastSeen = 0, lastRun = 0;
	for (;;) {
		Sleep(1000);
		const uint64_t sig = WorldSig();
		if (sig == 0) { lastSeen = 0; continue; }
		if (sig == lastSeen && sig != lastRun) {              // stable for two polls and not tested yet: the world view is built
			lastRun = sig;
			Sleep(3000);                                      // let the first frames of the new world draw
			L("self-test: world view detected (signature %016llx), running", static_cast<unsigned long long>(sig));
			SelfTest(15000);
			if (MarkerExists("TerrainView.patchtest")) PatchSelfTest();
		}
		lastSeen = sig;
	}
}

}  // namespace

void Create() {
	if (gCreated.exchange(true)) return;
	gExe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
	const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(gExe);
	const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(gExe + dos->e_lfanew);
	char exeName[MAX_PATH] = { 0 };
	GetModuleFileNameA(nullptr, exeName, MAX_PATH);
	const bool dx11 = strstr(exeName, "CivilizationVI.exe") != nullptr && strstr(exeName, "DX12") == nullptr;
	gBuildOk = dx11 && nt->FileHeader.TimeDateStamp == kExeTimestamp && nt->OptionalHeader.SizeOfImage == kExeImageSize;
	L("build check: %s (exe %s, timestamp 0x%08x, image 0x%x)%s", gBuildOk ? "OK" : "FAILED: terrain view disabled", exeName, nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage,
		dx11 ? "" : " (not the DX11 exe)");
	if (!gBuildOk) return;
	// self-test: only when the marker file TerrainView.selftest exists next to this DLL
	HMODULE self = nullptr;
	char dllPath[MAX_PATH] = { 0 };
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&Create), &self);
	GetModuleFileNameA(self, dllPath, MAX_PATH);
	char* slash = strrchr(dllPath, '\\');
	if (slash) *(slash + 1) = 0;
	strcpy_s(gDllDir, dllPath);
	strcat_s(dllPath, MAX_PATH, "TerrainView.selftest");
	if (GetFileAttributesA(dllPath) != INVALID_FILE_ATTRIBUTES) {
		L("self-test enabled (marker file %s): runs once per world view", dllPath);
		HANDLE t = CreateThread(nullptr, 0, &SelfTestThread, nullptr, 0, nullptr);
		if (t) CloseHandle(t);
	}
	if (MarkerExists("TerrainView.patch")) PatchCreate();       // M2: material patches on terrain changes (marker file next to the DLL)
}

}  // namespace TerrainView
