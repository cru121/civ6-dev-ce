#!/usr/bin/env python3
"""D4 measurement client: cost of the Dev CE Lua debugger during real end turns (dev-ce/DESIGN_LUA_DEBUGGER.md).

    python devce_d4.py [--port 4320] [--turns 4] [--csv d4_timeline.csv]

Needs: the DevCE_Test mod (DevCE_TurnTimer.lua marks every game turn start with DevCE_DebugMark) and the marker file DevCE_Debug.txt (first line 4320, NO `demo` line)
next to the DLL. Start this client, load a save, then end turns one after another. The condition changes at every turn start:
    end turn 1  A  connected, no breakpoints          (hooks off, stub fast path only: the counters stay at 0)
    end turn 2  B  breakpoint in a file that never runs (every state armed, the VM filters every line out)
    end turn 3  C  logpoint on a line that runs once per turn (armed, one line per turn is delivered)
    end turn 4  A  again   (the first mark appears at the start of the turn after end turn 1; the results of turns 2-4 are printed)
Per turn it prints the mark-to-mark time, the counter deltas (VM instructions on the slow path, line events delivered / filtered) and the busiest 250 ms slice of the
timeline (the end-turn burst). Note the wall time of the end turn yourself (stopwatch) for A / B / C: that is the number that matters.
"""
import argparse, json, socket, sys, threading, time

class Conn:
    def __init__(self, port, retry):
        end = time.time() + retry
        while True:
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=2); break
            except OSError:
                if time.time() > end: raise SystemExit("cannot connect to 127.0.0.1:%d" % port)
                time.sleep(0.5)
        self.s.settimeout(None); self.id = 0; self.replies = {}; self.cv = threading.Condition(); self.marks = []; self.closed = False
        threading.Thread(target=self.reader, daemon=True).start()

    def reader(self):
        buf = b""
        while True:
            try: d = self.s.recv(65536)
            except OSError: d = b""
            if not d:
                with self.cv: self.closed = True; self.cv.notify_all()
                return
            buf += d
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                try: m = json.loads(line.decode("utf-8", "replace"))
                except ValueError: continue
                with self.cv:
                    if "id" in m: self.replies[m["id"]] = m
                    elif m.get("event") == "mark": self.marks.append((time.time(), m))
                    elif m.get("event") == "stopped": threading.Thread(target=self.call, args=("continue",), daemon=True).start(); print("** unexpected stop (%s): continuing" % m.get("reason"), flush=True)
                    self.cv.notify_all()

    def call(self, cmd, timeout=20, **kw):
        with self.cv: self.id += 1; i = self.id
        self.s.sendall((json.dumps(dict(id=i, cmd=cmd, **kw)) + "\n").encode())
        end = time.time() + timeout
        with self.cv:
            while i not in self.replies:
                if self.closed or time.time() > end: return {"ok": False, "error": "no reply"}
                self.cv.wait(0.2)
            return self.replies.pop(i)

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--port", type=int, default=4320); ap.add_argument("--turns", type=int, default=3); ap.add_argument("--csv", default="d4_timeline.csv")
    ap.add_argument("--retry", type=float, default=1800)
    a = ap.parse_args()
    c = Conn(a.port, a.retry); print("connected", flush=True)
    rows = []                                                        # (t, fetchSlow, delivered, filtered)
    stop = threading.Event()
    def poll():
        while not stop.is_set() and not c.closed:
            r = c.call("status", timeout=5)
            if r.get("ok"): rows.append((time.time(), r.get("fetchSlow", 0), r.get("lineDelivered", 0), r.get("lineFiltered", 0)))
            time.sleep(0.25)
    threading.Thread(target=poll, daemon=True).start()
    conds = ["A  connected, no breakpoints", "B  breakpoint in a file that never runs (all states armed)", "C  logpoint on a line that runs once per turn", "A  connected, no breakpoints"]
    seen = 0; cur = 0; last_mark = None; results = []
    print("condition for the first end turn: %s -- load a save, then end turns (%d)" % (conds[0], a.turns), flush=True)
    while seen < a.turns + 1 and not c.closed:
        time.sleep(0.2)
        with c.cv: new = c.marks[seen:]
        for t, m in new:
            seen += 1
            if last_mark is not None:
                t0, m0 = last_mark
                seg = [r for r in rows if t0 <= r[0] <= t]
                best = 0
                for i in range(len(seg)):
                    for j in range(i + 1, len(seg)):
                        if seg[j][0] - seg[i][0] >= 0.25: best = max(best, seg[j][1] - seg[i][1]); break
                d = lambda k: m[k] - m0[k]
                line = "turn %d [%s]: %s -> %s  wall(mark to mark) %.1f s | slow-path instr %d, lines delivered %d, filtered %d | busiest 250 ms: %d instr" % (
                    seen - 1, conds[cur][0], m0["name"], m["name"], t - t0, d("fetchSlow"), d("lineDelivered"), d("lineFiltered"), best)
                print(line, flush=True); results.append(line)
            last_mark = (t, m)
            if seen - 1 < a.turns:
                c.call("clear")
                nxt = seen % len(conds)
                if conds[nxt].startswith("B"): c.call("break", file="NoSuchFileDevCE.lua", line=1)
                elif conds[nxt].startswith("C"): c.call("break", file="DevCE_TurnTimer.lua", line=3, log="turn")
                cur = nxt
                print("condition for the next end turn: %s" % conds[nxt], flush=True)
    stop.set()
    with open(a.csv, "w") as f:
        f.write("t,fetchSlow,lineDelivered,lineFiltered\n")
        for r in rows: f.write("%.3f,%d,%d,%d\n" % r)
    c.call("clear")
    print("timeline written to %s (%d rows)" % (a.csv, len(rows)), flush=True)

if __name__ == "__main__":
    main()
