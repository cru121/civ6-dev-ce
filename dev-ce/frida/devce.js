// Dev CE test support (the Dev CE test build exports DevBridgeStates[2] = { gameplay lua_State*, UI lua_State* }).
//
//   devstates                     the two Lua states the Dev CE DLL recorded when the game created them
//   stub RVA|name RETVAL          replace a native function by a recording stub (all calls recorded while `stubon` is true, else forwarded to the original)
//   stubon 0|1                    switch recording on/off (off = stubs forward to the original function, i.e. the game behaves normally)
//   stubcalls                     recorded calls since the last read (cleared): per rva a list of [this, a1..a7] as decimal strings
// stub RVA RET [DEREFMASK] [RETBUF]: see the command. Stubs are never removed (detaching hot hooks hung the game once); with stubon 0 they only forward.
(function () {
  const CE = 'GameCore_XP2_CE_FinalRelease.dll';
  state.stubs = state.stubs || {};
  if (state.stubOn === undefined) state.stubOn = false;

  function states() {
    const m = Process.findModuleByName(CE);
    if (!m) throw new Error('the Dev CE DLL is not loaded (enable the "Dev CE test build" mod and start a game)');
    const a = m.getExportByName('DevBridgeStates');
    const ps = Process.pointerSize;
    return { registerArg: a.readPointer(), ui: a.add(ps).readPointer(), gameplay: a.add(2 * ps).readPointer(), gameplayTid: a.add(3 * ps).readPointer() };
  }
  state.devStates = states;

  defcmd('devstates', 'lua_State pointers recorded by the Dev CE DLL (gameplay, ui)', function () {
    const s = states();
    const magic = function (p) { try { return p.isNull() ? 'null' : (p.readU32() === 0x3a038 ? 'looks like a lua_State' : 'NOT a lua_State (first dword 0x' + p.readU32().toString(16) + ')'); } catch (e) { return 'unreadable'; } };
    return { gameplay: s.gameplay.toString(), gameplayCheck: magic(s.gameplay), gameplayTid: s.gameplayTid.toString(), ui: s.ui.toString(), uiCheck: magic(s.ui), registerArg: s.registerArg.toString(), registerArgCheck: magic(s.registerArg) };
  });

  defcmd('stub', 'stub RVA|name RETVAL: record calls of a native function instead of running it (while stubon 1)', function (a) {
    const rva = rvaOf(/^0x/i.test(a[0]) ? parseInt(a[0], 16) : a[0]);
    const ret = a.length > 1 ? parseInt(a[1]) : 0;
    const derefMask = a.length > 2 ? parseInt(a[2]) : 0;      // bit s set: slot s is a pointer to an int32 (by-value class passed by pointer): record the int32, not the pointer
    const retBuf = a.length > 3 && a[3] === '1';               // the function returns a class through a hidden buffer in slot 1: write `ret` there and return the buffer address
    if (state.stubs[rva]) { state.stubs[rva].ret = ret; return 'stub for 0x' + rva.toString(16) + ' updated, ret=' + ret; }
    const target = addr(rva);
    const T = ['uint64', 'uint64', 'uint64', 'uint64', 'uint64', 'uint64', 'uint64', 'uint64'];
    const orig = new NativeFunction(target, 'uint64', T);
    const rec = state.stubs[rva] = { ret: ret, calls: [] };
    // Only calls made by the Dev CE bridge itself (return address inside the Dev CE DLL) are recorded/swallowed. Calls from the engine (other threads, hot
    // functions such as the game-state lock) always go to the original function: swallowing them once corrupted the game state and froze the whole machine.
    const cb = new NativeCallback(function (a0, a1, a2, a3, a4, a5, a6, a7) {
      let mine = false;
      if (state.stubOn) {
        const mod = Process.findModuleByName(CE);
        const ra = this.returnAddress;
        mine = !!mod && ra.compare(mod.base) >= 0 && ra.compare(mod.base.add(mod.size)) < 0;
      }
      if (mine) {
        const v = [a0, a1, a2, a3, a4, a5, a6, a7];
        const row = v.map(String);
        for (let s = 0; s < 8; s++) {
          if (derefMask & (1 << s)) { try { row[s] = String(ptr('0x' + v[s].toString(16)).readS32()); } catch (e) { row[s] = 'unreadable'; } }
        }
        rec.calls.push(row);
        if (retBuf) { try { ptr('0x' + a1.toString(16)).writeS32(rec.ret); } catch (e) {} return a1; }
        return rec.ret;
      }
      return orig(a0, a1, a2, a3, a4, a5, a6, a7);
    }, 'uint64', T);
    Interceptor.replace(target, cb);
    state.stubKeep = state.stubKeep || [];
    state.stubKeep.push(cb);       // keep the callback alive
    return 'stub installed for 0x' + rva.toString(16);
  });

  defcmd('stubon', 'stubon 0|1', function (a) { state.stubOn = a[0] === '1'; return 'recording ' + (state.stubOn ? 'ON' : 'off'); });

  defcmd('stubcalls', 'recorded stub calls since the last read', function () {
    const out = {};
    Object.keys(state.stubs).forEach(function (k) {
      const r = state.stubs[k];
      if (r.calls.length) { out['0x' + parseInt(k).toString(16)] = r.calls; r.calls = []; }
    });
    return out;
  });

  // Marker hook: the gameplay self-test calls DevCE_RecordState(1) to open and DevCE_RecordState(2) to close a test window (the DLL's exported DevCE_Marker(code) is
  // called). Opening it switches recording on and confirms the handshake to the Lua script by writing DevBridgeArmed = 2; closing it switches recording off.
  defcmd('devmarker', 'hook the Dev CE DLL marker function (once per DLL load)', function () {
    const m = Process.getModuleByName(CE);
    const marker = m.getExportByName('DevCE_Marker');
    if (state.markerAt === marker.toString()) return 'marker hook already installed';
    state.markerAt = marker.toString();
    state.armedAddr = m.getExportByName('DevBridgeArmed');
    Interceptor.attach(marker, { onEnter: function (args) {
      const c = args[0].toInt32();
      if (c === 1) {
        Object.keys(state.stubs).forEach(function (k) { state.stubs[k].calls = []; });
        state.stubOn = true;
        if (state.armedAddr.readS32() === 1) state.armedAddr.writeS32(2);
      } else if (c === 2) {
        state.stubOn = false;
        state.armedAddr.writeS32(3);
      }
    } });
    return 'marker hooked at ' + marker;
  });

  defcmd('devarm', 'devarm 0|1: arm (1) or disarm (0) the DLL for the level-2 window; reads back the value', function (a) {
    const addrA = Process.getModuleByName(CE).getExportByName('DevBridgeArmed');
    if (a.length) addrA.writeS32(parseInt(a[0]));
    return { armed: addrA.readS32(), stubs: Object.keys(state.stubs).length, stubOn: state.stubOn };
  });
})();
