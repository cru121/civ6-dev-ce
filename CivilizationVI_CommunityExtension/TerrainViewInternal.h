// SPDX-License-Identifier: AGPL-3.0-only
// Part of Dev CE, a fork of the Civilization VI Community Extension by Wild-W (https://github.com/Wild-W/CivilizationVI_CommunityExtension).
// Copyright (C) 2026 cru121. Licensed under the GNU Affero General Public License v3.0 (see LICENSE.txt). Not endorsed by Wild-W, Firaxis or 2K.
#pragma once
// Shared between TerrainView.cpp (reader) and TerrainViewPatch.cpp (patcher). Not a public interface.
#include <windows.h>
#include <d3d11.h>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include "TerrainView.h"

namespace TerrainView {
namespace detail {

constexpr uint32_t kMaxHandle = 8192;
// cell record (944 bytes) fields
constexpr size_t kRecordBytes = 944, kIdCount = 0x390, kIdFirst = 0x394, kIdVb = 0x398, kIdIb = 0x38c, kVbCount = 0xd0, kVbFirst = 0xd4, kVbHandle = 0xd8, kIbCC = 0xcc, kTable = 0x18c;

struct Sig {                          // what identifies the data of a cell: buffer handles, runs and the 32-slot range table
	uint32_t idVb = 0, idFirst = 0, idCount = 0, vb = 0, vbFirst = 0, vbCount = 0, idIb = 0;
	uint32_t table[64] = {};
	bool operator==(const Sig& o) const { return memcmp(this, &o, sizeof(Sig)) == 0; }
};

struct Range { int kind; uint32_t handle; uint32_t off; uint32_t len; uint32_t at; };   // kind 0 = vertex pool, 1 = index pool

// Fixed addresses (RVAs) of the exe the DLL runs in: the DX11 and the DX12 exe of the Steam build share the cell code but not the addresses.
struct ExeAddrs {
	const char* name; bool dx12; uint32_t timestamp, imageSize;
	uintptr_t ctxGlobal, getRec, dims, getState, createVB, createIB, release, vbReleasePool, ibReleasePool, sampler, samplerSx, samplerSy;
	uintptr_t editVB, editIB;                                     // creators of the height-edit buffers (Terrain_CellEdit_VB / IB) that keep a CPU mirror
	uintptr_t vbPool, ibPool;                                     // DX11: ID3D11Buffer* tables by handle
	uintptr_t vbRec, ibRec;                                       // DX12: 0x28-byte record per handle (size at +0x1c)
};
extern const ExeAddrs* gA;                                        // the table of the running exe (null until the build check passed)
uintptr_t ExeBase();
void L(const char* fmt, ...);                                     // DevBridge.log, prefix [TerrainView]
bool SafeCopy(void* dst, const void* src, size_t n);
void* CellMgr();
bool Dims(void* mgr, int* w, int* h);
const uint8_t* Record(void* mgr, int x, int y);                   // live record (944 bytes), null on failure
ID3D11Buffer* PoolBuf(int kind, uint32_t h);
uint32_t BufBytes(ID3D11Buffer* b);
bool HandleLive(int kind, uint32_t h);                            // a buffer exists under this handle (DX11: pool table entry, DX12: kept copy or per-handle record)
uint32_t HandleBytes(int kind, uint32_t h);                       // its size in bytes, 0 if unknown
bool ReadSig(const uint8_t* r, Sig& s);
void IbRange(const Sig& s, uint32_t* lo, uint32_t* hi);
bool SigPlausible(const Sig& s);
// Blocking read-back of arbitrary ranges (render-thread job); assigns Range::at; `data` receives the bytes in item order.
// Served from the shadow copies kept at buffer creation when they cover every item, otherwise (or with forceGpu) read from the GPU.
bool ReadBackItems(std::vector<Range>& items, std::vector<uint8_t>& data, double* msOut, int timeoutMs, bool forceGpu = false);
void SetPreferSnapshot(bool on);                                  // reads of edit buffers use the snapshot of the generated world (first sight of a cell) instead of the live mirror
bool SectionBUnchangedSinceSnapshot(int cx, int cy, const uint32_t cur[6]);   // the snapshot holds this cell's second section data
size_t EditSnapshotTake();                                        // copy the edit-buffer mirrors (before the first terrain change of a world); returns the number of buffers copied
void EditMirrorCheck(int timeoutMs, const char* when);              // DX11: compare the CPU mirrors of the height-edit buffers with the GPU (diagnostic)
bool ShadowInstall();                                             // hook CreateVB / CreateIB (must run before the world is built)
void ShadowStats(unsigned long* captured, unsigned long* served, unsigned long* gpu);
// The patcher registers this: for a cell it has patched, the ORIGINAL signature (the live record points at patch buffers).
void SetOriginalSigProvider(std::function<bool(int, int, Sig&)> fn);
bool MarkerExists(const char* name);                              // file next to the DLL

// objects (TerrainViewObjects.cpp): re-placing features, resources and improvements with the game's builders on the game thread
struct PlotObjects { int feature = -1, resource = -1, count = 0, improvement = -1, owner = -1; bool pillaged = false, wonder = false; };
struct ObjStats { unsigned long ticks, plots, features, resources, improvements, pillaged, wonders, failed; bool hooked; };
void ObjectsCreate();                                             // hook on the game tick; re-placement is ON when the marker file TerrainView.objects exists
bool ObjectsEnabled();
void SetObjectsEnabled(bool on);
void RunOnGameThreadLater(std::function<void()> fn, int delayMs); // executed by the game tick hook
bool RunOnGameThread(std::function<void()> fn, int timeoutMs);    // blocking; false on timeout
bool ReadPlotObjects(int x, int y, PlotObjects& o);
bool ReplacePlotObjects(int x, int y, std::string& msg);          // game thread only
void ScheduleObjectRefresh(const std::vector<std::pair<int, int>>& plots, int delayMs);
ObjStats GetObjStats();
int ObjectsSelfTest();

// patcher (TerrainViewPatch.cpp)
void PatchCreate();                                               // hook on _SetTerrainType + worker thread (only when the marker file TerrainView.patch exists)
int PatchSelfTest();                                              // 0 = pass, >0 = failures, -1 = could not run

}  // namespace detail
}  // namespace TerrainView
