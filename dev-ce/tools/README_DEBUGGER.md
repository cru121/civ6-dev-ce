# Dev CE Lua debugger: how to use it

Works with the Dev CE GameCore DLL (build 15038592 / 15296837 of Civ VI). Not an official tool; design notes and measurements: `dev-ce/DESIGN_LUA_DEBUGGER.md`, `dev-ce/DESIGN_LUA_HOOKS.md`.

## Switch it on
1. Install the Dev CE DLL as usual (the `GameCore_XP2_CE_FinalRelease.dll` of your mod).
2. Create a text file `DevCE_Debug.txt` next to that DLL. Optional first line: the TCP port (default 4320); a second line `demo` also enables the demo script of the DevCE_Test mod. Remove the file to switch the debugger off.
3. Start the game. The log (`DevBridge.log` next to the DLL) says `[LuaDebugger] listening on 127.0.0.1:4320`. The server only accepts connections from the same PC.

## Command line client
`python devce_dbg.py` (interactive) or `python devce_dbg.py -x "break MyScript.lua 42 if n > 3" -x wait -x vars -x continue`:
`status`, `stack`, `vars [frame]`, `expand EXPR`, `eval EXPR`, `level N`, `break FILE LINE [if COND] [hit N] [log EXPR]`, `clear [FILE [LINE]]`, `breaks`, `pause`, `step`, `next`, `out`, `continue`, `wait` (for the next stop), `quit`.
A stop freezes the Lua state's thread (for gameplay scripts the whole game); the game resumes by itself after 120 s without a request, and when you disconnect.

## Editors (Debug Adapter Protocol)
`python devce_dap.py --stdio` (editor starts it) or `python devce_dap.py --port 4711`. Launch / attach arguments: `{"port": 4320, "retrySeconds": 60, "pathDepth": 1}`. Supports breakpoints with conditions, hit conditions and log messages, step in / over / out, pause, call stack, locals and upvalues, table expansion, evaluate / hover (globals can be assigned, locals not). Only tested with the scripted client `devce_dap_selftest.py`, not with a real editor yet.

## From Lua
`debug.c_breakpoint()` stops where it is called (when a client is connected, otherwise it does nothing). Dev CE replaces the retail function, an `int3` stub that breaks into a native debugger (and ends the game without one), in every Lua state. `debug.sethook / gethook` work (call, return, line and count events, also for coroutines and C functions), `DevCE_DebugActive()`, `DevCE_DebugWait(seconds)`, `DevCE_DebugListen(port)`.

## Good to know
- Breakpoints match the file by its trailing path (the file name by default), case-insensitive.
- With breakpoints set every Lua state runs slower (about 12 ns per instruction) while the client is connected; without breakpoints the cost is around 8 % on Lua-heavy code.
- No global `setfenv` in this game's Lua (`debug.setfenv` is listed in its debug library, untested): `eval` sees frame variables as copies. Exceptions (errors) are not breakpoints yet.
- Try it with the demo: mod `DevCE_Test` (`dev-ce/mod/DevCE_Test`), script `DevCE_DebugDemo.lua` waits 60 s for a client while the game loads.
