# Dev CE Lua debugger over a socket (plan)

Status: IMPLEMENTED in 0.3.0 (`LuaDebugger.cpp`, D0-D4); this document is the plan, the "D0 .. D4 RESULTS" sections below record what was built and measured. Written 2026-10-15 on top of the working native hooks (`LuaHooks.cpp`, branch `feature/lua-hooks`, `DESIGN_LUA_HOOKS.md`, self-test 24/24). Reference implementation of the behaviour: the Frida debugger `frida/live/cmds/luadbg.js` + `frida/live/dap_adapter.py` (breakpoints, step in/over/out, stack, locals, value inspector, `debug.c_breakpoint`, DAP front end).

## Goal
Attach an editor / small CLI to a running game with the Dev CE DLL loaded and no Frida: set breakpoints by file and line, stop, look at the stack, locals, upvalues and tables, evaluate expressions, step, continue. Opt-in, loopback only.

## What already exists and is reused
- Native hook engine: per-state mask, line / call / return / count events, `getinfo(2)` correct inside a hook, stack-growth safe, errors in the hook contained, thread argument (so any known state can be hooked from outside).
- `debug.c_breakpoint` can simply be re-registered by Dev CE (replace the retail `xor eax,eax` function in `RegisterGlobals`): it becomes an explicit "stop here" call, the same as the Frida revival.
- The Frida prototype's findings: pausing = blocking the game thread inside the hook (UI freezes, other threads continue), a timeout that auto-resumes, step logic by callinfo depth, inspector depth limits.

## Architecture
```
editor / CLI  <--JSON lines over 127.0.0.1:PORT-->  debug server thread (in the DLL)
                                                       |  mutex-protected tables: breakpoints, requests, state
                                  game thread (hooked state) <-- native debug hook (extends OnFetch / CallHook)
```
1. **Server thread** (plain Winsock, one client at a time, started only when enabled). It never touches a lua_State: Lua is not thread-safe.
2. **Stop = block in the hook.** When a stop condition is met the hooked (game) thread sets `paused`, posts a `stopped` event and runs a *request loop* on itself: it waits on a condition variable for requests (stackTrace, variables, evaluate, step, continue ...), executes them on the paused thread (the only place Lua may be touched), replies, and returns to the VM on `continue` / step. Requests that only change tables (setBreakpoints, pause) are handled by the server thread directly under the mutex.
3. **Pause while running**: a global `pauseRequested` flag checked in `OnFetch` (states with a mask only) plus the mask being set on the target state(s); the first fetch after the request stops.
4. **One stop at a time**: a global debugger lock; a second thread that wants to stop (UI state while gameplay is stopped) waits for the lock. UI thread freeze while the gameplay thread is stopped is expected and documented.
5. **Timeout / safety**: auto-resume after N seconds without a client request (default 120 s), always on client disconnect; the server removes all hooks it installed on detach.

## Protocol
One JSON object per line, requests `{id, cmd, ...}`, replies `{id, ok, ...}`, events `{event, ...}`. It mirrors DAP so the adapter is thin: `states` (list known lua_States with name / thread), `attach {state}`, `setBreakpoints {source, lines|[{line,condition,hitCount,logMessage}]}`, `continue`, `pause`, `next`, `stepIn`, `stepOut`, `stackTrace`, `scopes`/`variables {ref, depth}` (locals, upvalues, globals, table children), `evaluate {expr, frame}`, `disconnect`. Events: `stopped {reason: breakpoint|step|pause|c_breakpoint, state, frame}`, `output`, `terminated`. `frida/live/dap_adapter.py` gets a second backend (socket instead of Frida daemon); the DAP layer above it stays unchanged. No new tooling for modders besides that adapter.

## Enabling and security
Off by default. Enabled by a marker file next to the DLL (`DevCE_Debug.txt`, optional first line = port, default 4320) or by `DevCE_DebugListen(port)` from a Lua mod. Binds 127.0.0.1 only, no authentication (same trust model as the tuner socket on 4318: anything on the machine can already run code in the game). Documented as a development feature.

## Breakpoints and performance
- A breakpoint is (source, line). Source comes from `getinfo "S"` (`short_src` / chunk name: mod script path). Resolution is lazy: on the line cache miss of a pc (already exists in `LineOf`) we also look the (source, line) pair up in the breakpoint table, so the per-event cost stays one cache lookup plus a hash test only for lines that are in a file with breakpoints.
- The expensive part is that the hooked state runs the slow path for every VM instruction (about 12 ns, see M1). Mitigations in order: (a) attach to ONE state (the gameplay state or the UI state), not all; (b) hook only while at least one breakpoint exists, otherwise mask 0 (fast path, ~2 %); (c) later: set the line mask only for code that can hit (call hook on function entry enables the line hook for functions whose proto source has a breakpoint). (b) is enough for a first version; (c) only if the end-turn measurement says so.
- Step over / out use call depth from the callinfo (the same rule as the Frida version); step into a C function is a no-op that stops at the next Lua line.

## Inspector and evaluate
- Locals / upvalues through the exported `lua_getlocal` / `lua_getupvalue` family (the Frida inspector already reads locals; confirm export names and the index convention in M0). Table children via `lua_next` with a depth and size limit, userdata / functions printed as `type: address` (+ source:line for Lua functions).
- `evaluate`: compile with `loadstring`, run with `pcall` on the paused thread with an environment that falls back to locals / upvalues then globals (global `setfenv` + `__index` proxy; see D2 results: the global does not exist here). Runs inside the hook (`t_inHook` set), so no recursion of hooks.

## Phases
- **D0 (1 session)**: server thread + request loop + `stopped` on `debug.c_breakpoint` / line hook for one hard-coded state; CLI client (Python) with stack / continue. Proves the blocking model and thread handoff in the real game.
- **D1**: breakpoints table, line-cache integration, step in / over / out, pause, timeout, detach cleanup.
- **D2**: variables / locals / upvalues / table inspector, evaluate, conditions, hit counts, logpoints.
- **D3**: socket backend in `dap_adapter.py` (+ scripted self-test like `dap_selftest.py`), README for modders.
- **D4**: measurement: a full end turn with the debugger attached to the gameplay state (a) without breakpoints, (b) with one breakpoint in a rarely hit file, (c) in a hot file; decide on mitigation (c).
Everything gated by the same byte-pattern checks as the hooks; failures disable the debugger only.

## Questions to settle in D0 (small, in-game)
1. Which lua_States exist (gameplay, UI, frontend) and on which threads; how to name them for `states` (hook `RegisterGlobals` / `PushSharedGlobals` call sites, they run once per state).
2. `lua_getlocal` / `getupvalue` / `getinfo "nSlu"` behaviour on the paused frame with the hook-pc fields set (`L+0x38` / `L+0x40`).
3. Does blocking the game thread inside a hook for minutes cause the engine's own watchdogs (`civ_watchdog`, "not responding") to act. (The Frida debugger blocked the same way for minutes: no problem seen.)
4. Cost of the mask on the gameplay state during an end turn (D4 numbers need this).

## Not in scope
Exception breakpoints (no hook in the retail VM's error path: `hks::error` could be wrapped later), hooks for C code calling C code, tail-return events, variants A / C of the interpreter, remote (non-loopback) debugging, a custom editor extension.


## D0 RESULTS (2026-10-15)  VERIFIED in game (branch feature/lua-debugger, off feature/lua-hooks)
- Built: `LuaDebugger.cpp/.h` (server thread on 127.0.0.1, default port 4320; request loop on the stopped thread), `LuaHooks::GetApi()` / `SetNativeHook()` (native hook per state, used for stepping), `debug.c_breakpoint` re-registered natively (stops when a client is connected, otherwise a no-op), Lua globals `DevCE_DebugListen(port)`, `DevCE_DebugWait(seconds)`, `DevCE_DebugActive()`, client `dev-ce/tools/devce_dbg.py`, demo target `DevCE_DebugDemo.lua` (gameplay script of DevCE_Test; does nothing unless the server runs).
- Enable: marker file `DevCE_Debug.txt` next to the DLL (first line = port) or `DevCE_DebugListen()`. Remove the marker to switch the demo off (with the marker the demo waits up to 60 s for a client while the game loads).
- Proven in the real game: client connected while the game was loading; `c_breakpoint` stopped the gameplay thread; `stack` showed `[C]`, inner (line 7), outer (12), main chunk (17), `[C]`; `eval` of globals and expressions, Lua errors returned with the traceback; `status`; `step` stopped on the next line (8, then 9) through the native line hook; `continue` ran the script to the end (result 85 as expected); disconnect resumes the game thread. No crash, no hang.
- Known D0 limits: eval sees globals only (locals / upvalues are D2); one stop at a time (a second state that hits a stop meanwhile just continues); step = step into only (over / out and line breakpoints are D1); `[C]` frames carry no function name; error text of eval contains the full traceback.


## D1 RESULTS (2026-10-15)  VERIFIED in game (branch feature/lua-debugger)
- Added: line breakpoints by file suffix + line (`break FILE LINE`, `clear [FILE [LINE]]`, `breaks`; case-insensitive, `/` and `\` equal), step over (`next`) and step out (`out`) by callinfo depth (a stop inside a C function such as `c_breakpoint` uses its Lua caller as the baseline), `pause` (also interrupts a busy loop), `status` shows the number of armed states. The client waits for the next `stopped` event after step / next / out.
- Proven in the real game with `DevCE_DebugDemo.lua`: c_breakpoint stop in inner (line 8); `next` -> line 9; `out` -> caller line 14; breakpoint at line 19 hit three times (DemoI 1, 2, 3, caller frame at line 25); `clear`; `pause` during a 25 s busy loop -> stopped, `eval DemoSpin = false` ended the loop (result log "spin false"); disconnect resumed everything. Self-test of the hooks still 24/24.
- **Design change after a crash**: the first D1 build armed breakpoints by writing hook fields into every registered state from the server thread (with a `lua_close` MinHook to forget closed states). The game crashed (no dump) right after the client disconnected, most likely a write into a state freed behind our back. Replaced by *arm mode*: the stub's fast path also tests one flag byte (`cmp byte [flag], 0`); while the flag is set, every state that runs Lua code with no hook calls `ArmSelf(L)` on its own thread at its next VM instruction, which installs the native line hook there. Nothing ever writes into another thread's state; idle hooks remove themselves at their next line event (`DbgHook`), so ending a session needs no cleanup pass. No registry of states, no `lua_close` hook, coroutines are covered.
- Cost: no-hook loop (3M iterations) 28.7 ms before the patch vs 31.0 ms with the patch and the extra flag test (about 8 %, within what we measured before the flag existed, 26.7 -> 28.9); line-hooked code pays the slow path (about 0.3 us per line event, see M1). With breakpoints set EVERY state that runs Lua is hooked (D4 measures an end turn).
- Limits / next: conditions, hit counts and logpoints (D2), locals / upvalues / table inspector (D2), step into a function called from a C function, exception breakpoints, DAP backend (D3). Note for deploying test builds: right after copying a fresh DLL into the OneDrive Mods folder the first game start sometimes fails with "Failed to load GameCore DLL"; it worked after waiting a couple of minutes.


## D2 RESULTS (2026-10-15)  VERIFIED in game (branch feature/lua-debugger)
- Added: `vars [level]` (locals and upvalues of a stack frame, values shortened to 120 chars, tables marked expandable with the Lua path to expand them), `vars` with an expression (= `expand EXPR`): children of a table sorted (numbers, strings), 200 entries max, `<metatable>` entry, paths such as `(t)["y"]`; `eval` in a chosen frame (`level` parameter, default = first Lua frame; a stop inside `c_breakpoint` therefore uses its caller); breakpoint **conditions** (`if COND`, evaluated in the hooked frame; an error in the condition stops and sends an `output` event), **hit counts** (`hit N` = stop from the N-th time on), **logpoints** (`log EXPR`: no stop, an `output` event with the value).
- Evaluation design: a Lua helper chunk (compiled per request, source in `LuaDebugger.cpp`) gets the frame's variable names / values and compiles `local a, b, ... = ...; return <expr>` (retry as a statement), called with the values. **This game's Lua has no global `setfenv`** (the debug library table lists `debug.setfenv`, not tried) (first attempt used it: every eval failed with "function expected instead of nil"), so no environment table is possible; globals are the real globals (assignments to globals persist, e.g. `eval DemoSpin = false` stopped the busy loop), assignments to frame variables only change the copy. Limit 180 variables per frame.
- Proven in the real game (`DevCE_DebugDemo.lua`): `vars` in inner (a=20, b=22, sum=42) and in the caller frame (upvalue `inner`); `eval sum + a` = 62; conditional breakpoint `n == 999` never stops and `n == 103` stops in probe() with locals a, b, t, s, n and upvalue upv; `expand t` lists [1], [2], name, x, y; `expand t.y` shows z = 2; `eval t.y.z + upv` = 102; logpoint on line 26 printed i=1, i=2, i=3 without stopping; `hit 3` on line 27 stopped once with hits = 3 (locals there: only `i`, `a` is not in scope yet); pause + eval `DemoSpin = false` ended the busy loop.
- Next: D3 (DAP backend over this socket, so editors can attach), D4 (end-turn cost with breakpoints), set-variable, step into functions called from C, exception breakpoints.


## D3 RESULTS (2026-10-15)  VERIFIED in game with a scripted DAP client
- `dev-ce/tools/devce_dap.py`: DAP adapter (stdio or TCP) that talks to the DLL's socket protocol directly, no Frida. `launch` / `attach` connect to the game (port, retrySeconds, pathDepth). setBreakpoints (breakpoints may be sent before the connection and are synced on attach; condition, hitCondition `N` / `>= N` / `> N`, logMessage `text {expr}` -> Lua `"text" .. tostring(expr)`), continue / next / stepIn / stepOut / pause, stackTrace, scopes Locals + Upvalues, variables with table expansion, evaluate (frame aware). `stopped` events caused by a resume request are held back until the response has been sent. Game `output` events (logpoints, condition errors) become DAP console output; a closed connection becomes `terminated`.
- `dev-ce/tools/devce_dap_selftest.py`: 27 checks. Passed against a mock of the protocol first (adapter plumbing), then against the real game: ALL PASS (initialize, attach, stop at c_breakpoint, stack with file:line names, locals of the right frame, evaluate, evaluate error, next, stepOut, conditional breakpoint, locals + upvalues, table and nested table expansion, evaluate with upvalues, hit condition, logpoint output i=1..3 without stops, clear + continue, pause in the busy loop, assignment to a global, disconnect). The editor's file path differed from the game's (matching by file name, `pathDepth` 1).
- `dev-ce/tools/README_DEBUGGER.md` explains setup for modders. NOT tried with a real editor (VS Code needs an extension that starts a DAP executable; the adapter follows the protocol but editor quirks are unknown).
- Open: D4 (end-turn cost with breakpoints set), function names in frames, setVariable, exception breakpoints, step into functions called from C, multi-state stops (only one stop at a time), a thread per state.


## D4 RESULTS (2026-10-15)  measured in the real game (save at turn 1..6 of a normal game, 3 end turns with the debugger attached)
- Tooling: `LuaHooks::GetStats()` (VM instructions on the C++ slow path, line events delivered / filtered), a **native line filter** (`SetLineFilter`: while the debugger's hook is installed the stub only delivers a line event when the line is a breakpoint line, or stepping / pause is active; before, every line of every armed state entered the hook and built a lua_Debug record), `DevCE_DebugMark(name)` (log + event with the counters), `DevCE_TurnTimer.lua` (marks every `OnGameTurnStarted`), client `dev-ce/tools/devce_d4.py` (rotates the condition per turn, polls the counters every 250 ms). The demo script is now opt-in: second line `demo` in `DevCE_Debug.txt`.
- Results (mark = start of a game turn; wall time includes the time you took to click End Turn, so it is an upper bound of the end-turn processing):
  - A, connected, no breakpoints (hooks off, stub fast path only): 4.0 s and 3.5 s for the two measured turns without breakpoints, plus 5.3 s for one more (noise of a single click is about +-1 s).
  - B, a breakpoint in a file that never runs (all states armed, every line filtered): 6.77 M VM instructions went through the slow path, 2.45 M lines filtered, 0 delivered. This turn took 61.8 s because the player was not clicking at once, so its wall time is useless; the counters are fine.
  - C, a logpoint on a line that runs once per turn: 4.5 s, 1.43 M instructions, 0.58 M lines filtered, 1 line delivered (the logpoint). Same wall time as A within noise.
- What it means: a normal end turn runs about 1-1.5 M Lua VM instructions (idle UI Lua adds roughly 100 k instructions per second on top), peaking at about 0.9 M instructions per second. At 12 ns per slow-path instruction (M1 measurement) the armed debugger adds roughly 20-40 ms to such an end turn, below 1 % of its 4 s: end turns are dominated by native engine code, not by Lua. Setting breakpoints does NOT make end turns noticeably slower; what hurts is a stop (the game thread is blocked) and stepping with many lines delivered (about 0.3 us per delivered line).
- Without the filter the same turn would have delivered about 0.6-2.5 M line events at about 0.3 us each, i.e. 0.2-0.8 s more: the filter is worth having, but even then the end turn stays usable.
- Not measured: stopwatch times of A / B / C taken by hand (the player's note was left empty), a hot breakpoint (one that stops thousands of times), other save sizes / late game turns (more AI, more Lua).
- Regression after the line filter was added: the 27-check DAP selftest (breakpoints, conditions, hit counts, logpoints, stepping, pause, inspector) passes again against the real game (marker file with lines `4320` and `demo`).
