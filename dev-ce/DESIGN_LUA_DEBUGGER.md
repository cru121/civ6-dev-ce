# Dev CE Lua debugger over a socket (plan)

Status: PLAN, nothing built. Written 2026-10-15 on top of the working native hooks (`LuaHooks.cpp`, branch `feature/lua-hooks`, `DESIGN_LUA_HOOKS.md`, self-test 24/24). Reference implementation of the behaviour: the Frida debugger `frida/live/cmds/luadbg.js` + `frida/live/dap_adapter.py` (breakpoints, step in/over/out, stack, locals, value inspector, `debug.c_breakpoint`, DAP front end).

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
- `evaluate`: compile with `loadstring`, run with `pcall` on the paused thread with an environment that falls back to locals / upvalues then globals (`setfenv` + `__index` proxy). Runs inside the hook (`t_inHook` set), so no recursion of hooks.

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
