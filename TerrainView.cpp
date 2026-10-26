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
#include <string>
#include <unordered_map>
#include <vector>
#include "MinHook.h"
#include "DevBridge.h"
#include "TerrainView.h"
#include "TerrainViewInternal.h"

// Terrain view sync, milestone 1: lazy per-cell reader. Overview: dev-ce/TERRAIN_VIEW.md.
namespace TerrainView {
namespace detail {

// ---- fixed addresses of the Windows exes, build 15038592 (the game is no longer developed, so no signature scan) -------------------------------------------------------
// DX11 (CivilizationVI.exe) verified in the game; DX12 (CivilizationVI_DX12.exe) found by matching the functions of the two exes (the cell record and buffer creation functions are the same code in both).
// ctxGlobal: TerrainContext global; *(this + 8) = impl; impl + 0x5a0 = Terrain::Cell::Manager. getRec: GetRecord(mgr, uint64 key = x | y << 32). dims: GetDimensions(mgr, int[2]).
const ExeAddrs kDx11 = { "DX11", false, 0x667c6fc6, 0x3704000,
	0x2305e20, 0xc5fee0, 0xc5ff20, 0xc5ff00, 0x948cb0, 0x944fe0, 0x956600, 0x2b91200, 0x2b0a740, 0xab58c0, 0xffa404, 0x10732ec,
	0x614120, 0x613fb0,
	0x2b99740, 0x2b12c80, 0, 0 };
const ExeAddrs kDx12 = { "DX12", true, 0x667c7363, 0xfe55000,
	0x233d4a0, 0xc6e8d0, 0xc6e910, 0xc6e8f0, 0x944ee0, 0x943e90, 0x94d670, 0xf2b50c0, 0x6c72040, 0xac3930, 0x1009824, 0x108406c,
	0x615320, 0x6151b0,
	0, 0, 0xf2bd4c0, 0x6c7a440 };
const ExeAddrs* gA = nullptr;
constexpr uint32_t kExeTimestamp = 0x667c6fc6, kExeImageSize = 0x3704000;   // (DX11 values, kept for the log only)

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
	if (!gExe || !SafeCopy(&impl, reinterpret_cast<const void*>(gExe + gA->ctxGlobal + 8), sizeof(impl)) || !impl) return nullptr;
	return reinterpret_cast<void*>(impl + 0x5a0);
}

bool Dims(void* mgr, int* w, int* h) {
	typedef void* (__fastcall* Fn)(void*, int*);
	int d[2] = { 0, 0 };
	__try { reinterpret_cast<Fn>(gExe + gA->dims)(mgr, d); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	*w = d[0]; *h = d[1];
	return d[0] > 0 && d[1] > 0 && d[0] * d[1] < 100000;
}

const uint8_t* Record(void* mgr, int x, int y) {
	typedef void* (__fastcall* Fn)(void*, uint64_t);
	const uint64_t key = static_cast<uint32_t>(x) | (static_cast<uint64_t>(static_cast<uint32_t>(y)) << 32);
	__try { return static_cast<const uint8_t*>(reinterpret_cast<Fn>(gExe + gA->getRec)(mgr, key)); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

ID3D11Buffer* PoolBuf(int kind, uint32_t h) {
	if (h == 0 || h >= kMaxHandle || !gA || gA->dx12) return nullptr;          // DX12 has no buffer object per handle
	ID3D11Buffer* p = nullptr;
	if (!SafeCopy(&p, reinterpret_cast<const void*>(gExe + (kind == 0 ? gA->vbPool : gA->ibPool) + static_cast<uintptr_t>(h) * 8), sizeof(p))) return nullptr;
	return p;
}

uint32_t BufBytes(ID3D11Buffer* b) {
	if (!b) return 0;
	D3D11_BUFFER_DESC d = {};
	__try { b->GetDesc(&d); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
	return d.ByteWidth;
}

bool ShadowSize(int kind, uint32_t h, uint32_t* bytes);          // defined with the shadow copies
bool HandleLive(int kind, uint32_t h) {
	if (h == 0 || h >= kMaxHandle || !gA) return false;
	if (!gA->dx12) return PoolBuf(kind, h) != nullptr;
	uint32_t b = 0;
	return ShadowSize(kind, h, &b) || HandleBytes(kind, h) != 0;
}
uint32_t RecordBytes(int kind, uint32_t h) {                       // DX12: the byte size in the exe's own per-handle record (independent of the kept copies)
	uint32_t b = 0;
	if (!gA || !gA->dx12 || h == 0 || h >= kMaxHandle) return 0;
	if (!SafeCopy(&b, reinterpret_cast<const void*>(gExe + (kind == 0 ? gA->vbRec : gA->ibRec) + static_cast<uintptr_t>(h) * 0x28 + 0x1c), 4)) return 0;
	return b;
}
uint32_t HandleBytes(int kind, uint32_t h) {
	if (h == 0 || h >= kMaxHandle || !gA) return 0;
	if (!gA->dx12) return BufBytes(PoolBuf(kind, h));
	uint32_t b = 0;
	if (ShadowSize(kind, h, &b)) return b;
	b = RecordBytes(kind, h);
	return b < 0x10000000u ? b : 0;                               // the byte size of the allocation (record layout from the exe analysis)
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
	void* t20 = vt[20]; void* t21 = vt[21];
	MH_STATUS s1 = MH_CreateHook(t20, reinterpret_cast<LPVOID>(&Hook_DrawIndexedInstanced), reinterpret_cast<LPVOID*>(&orig_DII));
	MH_STATUS s2 = MH_CreateHook(t21, reinterpret_cast<LPVOID>(&Hook_DrawInstanced), reinterpret_cast<LPVOID*>(&orig_DI));
	MH_STATUS e1 = MH_UNKNOWN, e2 = MH_UNKNOWN;
	if (s1 == MH_OK && s2 == MH_OK) { e1 = MH_EnableHook(t20); e2 = MH_EnableHook(t21); }
	if (s1 == MH_OK && s2 == MH_OK && e1 == MH_OK && e2 == MH_OK) {
		gHooked.store(true);
		L("draw hooks installed (inline): immediate context %p, DrawIndexedInstanced %p, DrawInstanced %p", static_cast<void*>(ctx), t20, t21);
		return true;
	}
	L("inline draw hooks failed (create %s / %s, enable %s / %s, targets %p / %p); trying the vtable fallback", MH_StatusToString(s1), MH_StatusToString(s2), MH_StatusToString(e1), MH_StatusToString(e2), t20, t21);
	if (s1 == MH_OK) { MH_DisableHook(t20); MH_RemoveHook(t20); }                 // start clean, also for the next attempt
	if (s2 == MH_OK) { MH_DisableHook(t21); MH_RemoveHook(t21); }
	// fallback: replace the two entries of the context's vtable (works next to overlays that patch the function bodies); the originals are called through
	orig_DII = reinterpret_cast<DrawIndexedInstancedFn>(t20); orig_DI = reinterpret_cast<DrawInstancedFn>(t21);
	DWORD oldProt = 0;
	if (!VirtualProtect(&vt[20], 2 * sizeof(void*), PAGE_READWRITE, &oldProt)) { L("vtable fallback: VirtualProtect failed (error %lu)", GetLastError()); return false; }
	vt[20] = reinterpret_cast<void*>(&Hook_DrawIndexedInstanced);
	vt[21] = reinterpret_cast<void*>(&Hook_DrawInstanced);
	DWORD dummy = 0; VirtualProtect(&vt[20], 2 * sizeof(void*), oldProt, &dummy);
	gHooked.store(true);
	L("draw hooks installed (vtable): immediate context %p, DrawIndexedInstanced %p, DrawInstanced %p", static_cast<void*>(ctx), t20, t21);
	return true;
}

// ---- shadow copies ----------------------------------------------------------------------------------------------------------------------------------------------------
// The terrain buffers are immutable, but the data they were created from is known at creation: a hook on the game's CreateVB / CreateIB keeps a copy of every Terrain_Cell* buffer
// (handle -> bytes). Baseline reads are served from it, so no GPU read-back (and no hook on the draw calls) is needed; the GPU path stays as the fallback.
typedef uint16_t(__fastcall* CreateBufFn)(uint32_t, uint32_t, uint32_t, const void*, uint32_t, uint32_t, const char*);
CreateBufFn orig_CreateVB = nullptr, orig_CreateIB = nullptr;
std::mutex gShadowMx;
std::unordered_map<uint32_t, std::vector<uint8_t>> gShadow[2];
std::atomic<bool> gShadowOn{ false };
std::atomic<bool> gCompare{ false };
std::atomic<unsigned long> gShadowCaptured{ 0 }, gShadowServed{ 0 }, gGpuServed{ 0 };

bool IsTerrainBufName(const char* name) {
	char n[13] = { 0 };
	return name && SafeCopy(n, name, 12) && memcmp(n, "Terrain_Cell", 12) == 0;
}

void ShadowStore(int kind, uint16_t h, uint32_t count, uint32_t stride, const void* data, const char* name) {
	std::lock_guard<std::mutex> g(gShadowMx);
	if (gCompare.load()) {                                        // diagnostics (marker TerrainView.compare): every distinct buffer name the game creates (first 80), with the handle and size
		static std::unordered_map<std::string, int> seen;
		char nm[48] = { 0 }; if (name) SafeCopy(nm, name, 40);
		std::string key = std::string(kind ? "IB " : "VB ") + nm;
		if (seen.size() < 80 && seen.emplace(key, 1).second) L("create %s: handle %u, %u elements%s", key.c_str(), h, count, IsTerrainBufName(name) ? " (kept)" : "");
	}
	gShadow[kind].erase(h);                                       // a reused handle never keeps stale bytes
	if (!h || h >= kMaxHandle || !data || !count || count > 4000000 || !IsTerrainBufName(name)) return;
	const uint32_t bytes = count * stride;
	if (!gA->dx12 && BufBytes(PoolBuf(kind, h)) != bytes) return;  // not the layout we expect: leave it to the GPU path (DX12 has no buffer object to compare with)
	std::vector<uint8_t> v(bytes);
	if (!SafeCopy(v.data(), data, bytes)) return;
	gShadow[kind][h] = std::move(v);
	gShadowCaptured++;
}
uint16_t __fastcall Hook_CreateVB(uint32_t a, uint32_t b, uint32_t count, const void* data, uint32_t e, uint32_t f, const char* name) {
	const uint16_t h = orig_CreateVB(a, b, count, data, e, f, name);
	ShadowStore(0, h, count, 8, data, name);
	return h;
}
uint16_t __fastcall Hook_CreateIB(uint32_t a, uint32_t b, uint32_t count, const void* data, uint32_t e, uint32_t f, const char* name) {
	const uint16_t h = orig_CreateIB(a, b, count, data, e, f, name);
	ShadowStore(1, h, count, 2, data, name);
	return h;
}
// The height-edit buffers (Terrain_CellEdit_VB / IB: the second base mesh section of edited cells lives there) are created without initial data and filled at run time,
// but the game keeps a CPU mirror of each (WorldView_TerrainContext_HeightEdit.cpp: a 0x80000-byte block per buffer, entries of 0x38 bytes {elements u32, ?, mirror ptr at +8, handle u16 at +0x10}
// in a vector {begin, capacity, count}). The creators are hooked only to learn where that vector is.
typedef int(__fastcall* EditCreateFn)(void*, uint32_t);
EditCreateFn orig_EditVB = nullptr, orig_EditIB = nullptr;
std::atomic<uintptr_t> gEditVecVB{ 0 }, gEditVecIB{ 0 };
int __fastcall Hook_EditVB(void* self, uint32_t a) { const int r = orig_EditVB(self, a); gEditVecVB.store(reinterpret_cast<uintptr_t>(self)); return r; }
int __fastcall Hook_EditIB(void* self, uint32_t a) { const int r = orig_EditIB(self, a); gEditVecIB.store(reinterpret_cast<uintptr_t>(self) + 0x18); return r; }
// the mirror pointer of an edit buffer, 0 if there is none; *bytes receives its size
uintptr_t EditMirror(int kind, uint32_t handle, uint32_t* bytes) {
	const uintptr_t vec = (kind ? gEditVecIB : gEditVecVB).load();
	if (!vec) return 0;
	uintptr_t begin = 0; uint32_t count = 0;
	if (!SafeCopy(&begin, reinterpret_cast<const void*>(vec), 8) || !SafeCopy(&count, reinterpret_cast<const void*>(vec + 0x10), 4) || !begin || count > 4096) return 0;
	for (uint32_t i = 0; i < count; i++) {
		const uintptr_t e = begin + static_cast<uintptr_t>(i) * 0x38;
		uint32_t elems = 0; uintptr_t mir = 0; uint16_t hd = 0;
		if (!SafeCopy(&elems, reinterpret_cast<const void*>(e), 4) || !SafeCopy(&mir, reinterpret_cast<const void*>(e + 8), 8) || !SafeCopy(&hd, reinterpret_cast<const void*>(e + 0x10), 2)) continue;
		if (hd == handle && mir && elems) { *bytes = elems * (kind ? 2u : 8u); return mir; }
	}
	return 0;
}
bool ShadowInstall() {
	MH_STATUS s1 = MH_CreateHook(reinterpret_cast<LPVOID>(gExe + gA->createVB), reinterpret_cast<LPVOID>(&Hook_CreateVB), reinterpret_cast<LPVOID*>(&orig_CreateVB));
	MH_STATUS s2 = MH_CreateHook(reinterpret_cast<LPVOID>(gExe + gA->createIB), reinterpret_cast<LPVOID>(&Hook_CreateIB), reinterpret_cast<LPVOID*>(&orig_CreateIB));
	MH_STATUS e1 = MH_UNKNOWN, e2 = MH_UNKNOWN;
	if (s1 == MH_OK && s2 == MH_OK) { e1 = MH_EnableHook(reinterpret_cast<LPVOID>(gExe + gA->createVB)); e2 = MH_EnableHook(reinterpret_cast<LPVOID>(gExe + gA->createIB)); }
	if (s1 == MH_OK && s2 == MH_OK && e1 == MH_OK && e2 == MH_OK) {
		gShadowOn.store(true);
		MH_STATUS m1 = MH_CreateHook(reinterpret_cast<LPVOID>(gExe + gA->editVB), reinterpret_cast<LPVOID>(&Hook_EditVB), reinterpret_cast<LPVOID*>(&orig_EditVB));
		MH_STATUS m2 = MH_CreateHook(reinterpret_cast<LPVOID>(gExe + gA->editIB), reinterpret_cast<LPVOID>(&Hook_EditIB), reinterpret_cast<LPVOID*>(&orig_EditIB));
		if (m1 == MH_OK && m2 == MH_OK) { m1 = MH_EnableHook(reinterpret_cast<LPVOID>(gExe + gA->editVB)); m2 = MH_EnableHook(reinterpret_cast<LPVOID>(gExe + gA->editIB)); }
		L("shadow copies: hooked CreateVB / CreateIB, baseline reads need no GPU read-back; edit-buffer mirrors: %s / %s", MH_StatusToString(m1), MH_StatusToString(m2));
		return true;
	}
	L("shadow copies unavailable (create %s / %s, enable %s / %s): baseline reads use the GPU path", MH_StatusToString(s1), MH_StatusToString(s2), MH_StatusToString(e1), MH_StatusToString(e2));
	if (s1 == MH_OK) { MH_DisableHook(reinterpret_cast<LPVOID>(gExe + gA->createVB)); MH_RemoveHook(reinterpret_cast<LPVOID>(gExe + gA->createVB)); }
	if (s2 == MH_OK) { MH_DisableHook(reinterpret_cast<LPVOID>(gExe + gA->createIB)); MH_RemoveHook(reinterpret_cast<LPVOID>(gExe + gA->createIB)); }
	return false;
}
bool ShadowSize(int kind, uint32_t h, uint32_t* bytes) {
	std::lock_guard<std::mutex> g(gShadowMx);
	auto f = gShadow[kind].find(h);
	if (f == gShadow[kind].end()) return false;
	*bytes = static_cast<uint32_t>(f->second.size()); return true;
}
void ShadowStats(unsigned long* captured, unsigned long* served, unsigned long* gpu) { *captured = gShadowCaptured; *served = gShadowServed; *gpu = gGpuServed; }
// The mirror is written by the game the moment a terrain change makes it re-bake a cell, while the GPU copy follows later: a baseline read from the live mirror can return the NEW data
// (seen as a wildly over-subdivided, corrupted tile). The baseline is the data of the generated world, so a snapshot of the mirrors is taken once the world view is ready.
std::unordered_map<uint32_t, std::vector<uint8_t>> gEditSnap[2];
std::atomic<bool> gPreferSnapshot{ false };
void SetPreferSnapshot(bool on) { gPreferSnapshot.store(on); }
size_t EditSnapshotTake() {
	std::lock_guard<std::mutex> g(gShadowMx);
	size_t n = 0, bytes = 0;
	for (int kind = 0; kind < 2; kind++) {
		gEditSnap[kind].clear();
		const uintptr_t vec = (kind ? gEditVecIB : gEditVecVB).load();
		uintptr_t begin = 0; uint32_t count = 0;
		if (!vec || !SafeCopy(&begin, reinterpret_cast<const void*>(vec), 8) || !SafeCopy(&count, reinterpret_cast<const void*>(vec + 0x10), 4) || !begin || count > 4096) continue;
		for (uint32_t i = 0; i < count; i++) {
			uint16_t hd = 0; SafeCopy(&hd, reinterpret_cast<const void*>(begin + static_cast<uintptr_t>(i) * 0x38 + 0x10), 2);
			uint32_t mb = 0; const uintptr_t m = EditMirror(kind, hd, &mb);
			if (!m || !mb) continue;
			std::vector<uint8_t> v(mb);
			if (!SafeCopy(v.data(), reinterpret_cast<const void*>(m), mb)) continue;
			bytes += mb; n++; gEditSnap[kind][hd] = std::move(v);
		}
	}
	L("edit-buffer mirrors: snapshot of %zu buffer(s), %.1f KiB (baseline for the second base mesh section)", n, bytes / 1024.0);
	return n;
}
void EditMirrorCheck(int timeoutMs, const char* when) {              // DX11 only: the CPU mirrors of the height-edit buffers against the GPU (a real GPU read)
	if (!gA || gA->dx12) return;
	for (int kind = 0; kind < 2; kind++) {
		const uintptr_t vec = (kind ? gEditVecIB : gEditVecVB).load();
		uintptr_t begin = 0; uint32_t count = 0;
		if (!vec || !SafeCopy(&begin, reinterpret_cast<const void*>(vec), 8) || !SafeCopy(&count, reinterpret_cast<const void*>(vec + 0x10), 4)) { L("edit mirror check (%s): no height-edit %s vector known", when, kind ? "index" : "vertex"); continue; }
		for (uint32_t i = 0; i < count && i < 8; i++) {
			uint16_t hd = 0; SafeCopy(&hd, reinterpret_cast<const void*>(begin + static_cast<uintptr_t>(i) * 0x38 + 0x10), 2);
			uint32_t mb = 0; const uintptr_t m = EditMirror(kind, hd, &mb);
			std::vector<Range> one{ { kind, hd, 0, mb, 0 } }; std::vector<uint8_t> gpu; double ms2 = 0;
			if (!m || !mb || !ReadBackItems(one, gpu, &ms2, timeoutMs, true)) { L("edit mirror check (%s): %s buffer %u: mirror %p, GPU read failed", when, kind ? "index" : "vertex", hd, reinterpret_cast<void*>(m)); continue; }
			size_t diff = 0, nz = 0; std::vector<uint8_t> mir(mb); SafeCopy(mir.data(), reinterpret_cast<const void*>(m), mb);
			for (size_t b = 0; b < mb; b++) { if (mir[b] != gpu[b]) diff++; if (gpu[b]) nz++; }
			L("edit mirror check (%s): %s buffer %u: CPU mirror vs GPU: %zu of %u bytes differ (%zu non-zero bytes on the GPU)", when, kind ? "index" : "vertex", hd, diff, mb, nz);
		}
	}
}

// all items from the shadow copies? then fills `data` (at assigned) and returns true
bool ServeFromShadow(std::vector<Range>& items, std::vector<uint8_t>& data) {
	if (!gShadowOn.load()) return false;
	std::lock_guard<std::mutex> g(gShadowMx);
	uint32_t total = 0;
	std::vector<uintptr_t> mirror(items.size(), 0);                  // != 0: this item comes from the CPU mirror of an edit buffer
	for (size_t k = 0; k < items.size(); k++) {
		Range& it = items[k];
		auto f = gShadow[it.kind].find(it.handle);
		bool ok = f != gShadow[it.kind].end() && static_cast<uint64_t>(it.off) + it.len <= f->second.size();
		if (ok && !gA->dx12 && BufBytes(PoolBuf(it.kind, it.handle)) != f->second.size()) ok = false;      // the handle now holds another buffer (created by a path we did not see)
		if (!ok) {
			uint32_t mb = 0; const uintptr_t m = EditMirror(it.kind, it.handle, &mb);
			auto sn = gEditSnap[it.kind].find(it.handle);
			if (gPreferSnapshot.load() && sn != gEditSnap[it.kind].end() && static_cast<uint64_t>(it.off) + it.len <= sn->second.size()) mirror[k] = reinterpret_cast<uintptr_t>(sn->second.data());      // snapshot taken when the world view was ready
			else if (m && static_cast<uint64_t>(it.off) + it.len <= mb) mirror[k] = m;
			else {
				static int said = 0;
				if (said++ < 12) L("no kept copy for %s handle %u (range %u + %u): %s, exe record says %u bytes", it.kind ? "index" : "vertex", it.handle, it.off, it.len, f == gShadow[it.kind].end() ? "none kept, no edit mirror" : "copy too small", RecordBytes(it.kind, it.handle));
				return false;
			}
		}
		it.at = total; total += it.len;
	}
	data.assign(total, 0);
	for (size_t k = 0; k < items.size(); k++) {
		const Range& it = items[k];
		if (mirror[k]) { if (!SafeCopy(data.data() + it.at, reinterpret_cast<const void*>(mirror[k] + it.off), it.len)) return false; }
		else memcpy(data.data() + it.at, gShadow[it.kind][it.handle].data() + it.off, it.len);
	}
	return true;
}

// Blocking: queue the job and wait for the render thread. Assigns `at` and fills `data` (total bytes).
bool ReadBackItems(std::vector<Range>& items, std::vector<uint8_t>& data, double* msOut, int timeoutMs, bool forceGpu) {
	if (items.empty()) { data.clear(); if (msOut) *msOut = 0; return true; }
	if (!forceGpu && ServeFromShadow(items, data)) {
		gShadowServed++; if (msOut) *msOut = 0;
		if (!gA->dx12 && gCompare.load()) {                          // diagnostic (marker TerrainView.compare, DX11): the same ranges from the GPU
			std::vector<Range> it2 = items; std::vector<uint8_t> gpu;
			if (ReadBackItems(it2, gpu, nullptr, timeoutMs, true)) {
				for (size_t k = 0; k < items.size(); k++) {
					size_t diff = 0, first = 0;
					for (uint32_t b = 0; b < items[k].len; b++) if (data[items[k].at + b] != gpu[it2[k].at + b]) { if (!diff) first = b; diff++; }
					if (diff) L("COMPARE: %s handle %u range %u + %u: kept copy differs from the GPU in %zu bytes (first at +%zu), edit mirror %s", items[k].kind ? "index" : "vertex", items[k].handle, items[k].off, items[k].len, diff, first, gShadow[items[k].kind].count(items[k].handle) ? "not used" : "used");
				}
			}
		}
		return true;
	}
	gGpuServed++;
	if (gA->dx12) { static bool said = false; if (!said) { said = true; L("no kept copy for a terrain buffer and the GPU read-back is not available on DX12: the change is not applied"); } return false; }
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
			const uint32_t bytes = HandleBytes(b.kind, b.h);
			if (!bytes) continue;
			fullAt[k] = 0; full.push_back({ b.kind, b.h, 0, bytes, 0 });
		}
	}
	std::vector<uint8_t> fullData; double fullMs = 0;
	const auto t0 = std::chrono::steady_clock::now();
	if (!ReadBackItems(full, fullData, &fullMs, timeoutMs, !gA->dx12)) { L("self-test: full read-back failed"); return -1; }      // always the GPU: it is the reference for the shadow copies
	const double fullWall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	EditMirrorCheck(timeoutMs, "start");
	if (gA->dx12) {                                                      // no GPU reference on DX12: check the kept copies against the exe's own size records
		int bad = 0; for (auto& r : full) { const uint32_t rb = RecordBytes(r.kind, r.handle); if (rb != r.len) { bad++; if (bad <= 5) L("self-test: handle %u (kind %d): kept copy %u bytes, exe record %u bytes", r.handle, r.kind, r.len, rb); } }
		L("self-test (DX12): %zu buffers, kept-copy size equals the exe's size record: %zu of %zu", full.size(), full.size() - bad, full.size());
	}
	for (auto& r : full) fullAt[(static_cast<uint64_t>(r.kind) << 32) | r.handle] = r.at;
	{ size_t sv = 0, si = 0; { std::lock_guard<std::mutex> g(gShadowMx); sv = gShadow[0].size(); si = gShadow[1].size(); } L("self-test: shadow copies %s: %zu vertex + %zu index buffers kept, of %zu buffers read from the GPU", gShadowOn.load() ? "ON" : "OFF", sv, si, full.size()); }
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
	const bool isDx12Name = strstr(exeName, "CivilizationVI_DX12.exe") != nullptr;
	const bool isDx11Name = !isDx12Name && strstr(exeName, "CivilizationVI.exe") != nullptr;
	const ExeAddrs* want = isDx12Name ? &kDx12 : isDx11Name ? &kDx11 : nullptr;
	gBuildOk = want && nt->FileHeader.TimeDateStamp == want->timestamp && nt->OptionalHeader.SizeOfImage == want->imageSize;
	if (gBuildOk) gA = want;
	L("build check: %s (exe %s, timestamp 0x%08x, image 0x%x)%s", gBuildOk ? "OK" : "FAILED: terrain view disabled", exeName, nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage,
		want ? (gBuildOk ? (want->dx12 ? " (DX12 exe)" : " (DX11 exe)") : " (not the build this was made for)") : " (not a Civilization VI exe)");
	if (!gBuildOk) return;
	// self-test: only when the marker file TerrainView.selftest exists next to this DLL
	HMODULE self = nullptr;
	char dllPath[MAX_PATH] = { 0 };
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&Create), &self);
	GetModuleFileNameA(self, dllPath, MAX_PATH);
	char* slash = strrchr(dllPath, '\\');
	if (slash) *(slash + 1) = 0;
	strcpy_s(gDllDir, dllPath);
	gCompare.store(MarkerExists("TerrainView.compare"));
	if (!MarkerExists("TerrainView.noshadow") && (MarkerExists("TerrainView.patch") || MarkerExists("TerrainView.selftest"))) ShadowInstall();
	strcat_s(dllPath, MAX_PATH, "TerrainView.selftest");
	if (GetFileAttributesA(dllPath) != INVALID_FILE_ATTRIBUTES) {
		L("self-test enabled (marker file %s): runs once per world view", dllPath);
		HANDLE t = CreateThread(nullptr, 0, &SelfTestThread, nullptr, 0, nullptr);
		if (t) CloseHandle(t);
	}
	if (MarkerExists("TerrainView.patch")) PatchCreate();       // M2: material patches on terrain changes (marker file next to the DLL)
}

}  // namespace TerrainView
