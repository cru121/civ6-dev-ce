"""One-off stack dump of a hung Civ VI process: python hang_dump.py  (attaches, prints every thread's backtrace as module+offset, resolves GameCore/DevCE names via frida/live/symbols.json).
Attaching to a HUNG game only: the detach at exit may crash it, which is fine when it has to be killed anyway."""
import frida, json, bisect, sys, os, time

JS = r"""
rpc.exports = {
  dump: function () {
    var out = [];
    Process.enumerateThreads().forEach(function (t) {
      var bt = [];
      try {
        bt = Thread.backtrace(t.context, Backtracer.ACCURATE).map(function (a) {
          var m = Process.findModuleByAddress(a);
          return m ? [m.name, a.sub(m.base).toString(16)] : [null, a.toString(16)];
        });
      } catch (e) { bt = [['ERR', String(e)]]; }
      // raw stack scan (the unwinder gives up on frames without unwind info): pointers into game modules are probable return addresses
      var scan = [];
      try {
        var sp = t.context.sp, buf = sp.readByteArray(0x6000), dv = new DataView(buf);
        for (var o = 0; o + 8 <= 0x6000; o += 8) {
          var v = ptr('0x' + dv.getBigUint64(o, true).toString(16));
          var m = Process.findModuleByAddress(v);
          if (m && /GameCore|Havok|CivilizationVI|Lua|CE/i.test(m.name)) scan.push([m.name, v.sub(m.base).toString(16), o]);
        }
      } catch (e) {}
      var pc = Process.findModuleByAddress(t.context.pc);
      out.push({ id: t.id, state: t.state, pc: pc ? [pc.name, t.context.pc.sub(pc.base).toString(16)] : [null, t.context.pc.toString(16)], bt: bt, scan: scan });
    });
    return out;
  }
};
"""

pid = int(sys.argv[1]) if len(sys.argv) > 1 else None
dev = frida.get_local_device()
if pid is None:
    pid = [p.pid for p in dev.enumerate_processes() if p.name == 'CivilizationVI.exe'][0]
sess = dev.attach(pid)
sc = sess.create_script(JS)
sc.load()
threads = sc.exports_sync.dump()

syms = json.load(open(r'C:\stuff\claude\DLL\frida\live\symbols.json'))
rv = sorted((v, k) for k, vs in syms.items() for v in (vs if isinstance(vs, list) else [vs]))
keys = [r[0] for r in rv]

def name(mod, off):
    if mod and mod.lower().startswith('gamecore') and '_ce_' not in mod.lower():
        o = int(off, 16)
        i = bisect.bisect_right(keys, o) - 1
        if i >= 0:
            return '%s+0x%x (%s)' % (rv[i][1], o - rv[i][0], mod)
    return '%s+0x%s' % (mod, off)

for t in threads:
    if t['state'] != 'waiting' and False:
        continue
    print('--- thread', t['id'], t['state'], 'pc', name(*t['pc']))
    for m, o in t['bt'][:14]:
        print('     ', name(m, o))
    for m, o, d in t['scan'][:25]:
        print('      stack+0x%x: %s' % (d, name(m, o)))
sys.stdout.flush()
os._exit(0)   # do not detach cleanly
