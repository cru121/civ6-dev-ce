// SPDX-License-Identifier: AGPL-3.0-only
// Part of Dev CE, a fork of the Civilization VI Community Extension by Wild-W (https://github.com/Wild-W/CivilizationVI_CommunityExtension).
// Copyright (C) 2026 cru121. Licensed under the GNU Affero General Public License v3.0 (see LICENSE.txt). Not endorsed by Wild-W, Firaxis or 2K.
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

// Terrain view sync, milestone 1: lazy per-cell reader (see dev-ce/TERRAIN_VIEW.md).
// The terrain of the 3D map lives in IMMUTABLE D3D11 buffers that cannot be edited, so a patch starts from a copy of the cell's original data. This module reads that
// data back from the GPU on demand: per render cell (256x256 texels, about 2x2 hexes) the ID mesh (materials), the base mesh (heights) and the ID index range.
// Windows DX11 exe only, build 15038592 (CivilizationVI.exe, PE timestamp 0x667c6fc6, SizeOfImage 0x3704000); everything is disabled when that check fails.
namespace TerrainView {
	struct CellKey { int x, y; };

	// Baseline of one cell as it is in the live buffers when it is read.
	struct CellData {
		int x = 0, y = 0;
		uint32_t idVbHandle = 0, idFirst = 0, idCount = 0;     // ID vertices: 8 bytes each (x, y as int16, weights, material ids), run [idFirst, idFirst + idCount) of the shared VB
		uint32_t vbHandle = 0, vbFirst = 0, vbCount = 0;       // base mesh vertices: 8 bytes each (x, y as int16, u16 height, 2 normal bytes)
		uint32_t idIbHandle = 0, ibLo = 0, ibHi = 0;           // ID index range [ibLo, ibHi) in u16 elements of the shared IB
		uint32_t table[32][2] = {};                            // (triangle count, start) per material slot, start in the coordinates of the shared IB
		std::vector<uint8_t> idvb;                             // idCount * 8 bytes
		std::vector<uint8_t> vb;                               // vbCount * 8 bytes
		std::vector<uint16_t> ib;                              // ibHi - ibLo indices; slot k triangles start at (table[k][1] - ibLo)
	};

	struct Stats { unsigned long requests, cellsRequested, cacheHits, cellsRead, jobs, timeouts, failures; unsigned long long bytesRead; double msInJobs; };

	void Create();                    // from InitHooks: build check, optional self-test thread (marker file TerrainView.selftest next to the DLL)
	bool Available();                 // build check passed and the terrain cell manager is up
	bool GridSize(int* w, int* h);    // render cell grid (22x11 on a Small map, 53x26 on a Huge one)

	// Reads the cells (blocking, never call it on the render thread). A cache hit needs the live record to be unchanged since the cell was read.
	// timeoutMs: how long to wait for the render thread to run the copy (it needs a draw call). Returns false on failure/timeout; `out` has one entry per key in order.
	bool ReadCells(const std::vector<CellKey>& keys, std::vector<std::shared_ptr<const CellData>>& out, int timeoutMs = 500);
	void Invalidate();                // forget all cached cells (a new world view)
	Stats GetStats();
	int SelfTest(int timeoutMs = 10000);   // full read-back vs targeted reads, logged to DevBridge.log ("[TerrainView]"); returns the number of mismatching cells (-1 = could not run)
}
