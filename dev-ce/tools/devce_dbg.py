#!/usr/bin/env python3
"""Client for the Dev CE Lua debugger (LuaDebugger.cpp, loopback JSON lines). Usage:
  devce_dbg.py                      interactive: status | stack | eval EXPR | break FILE LINE | clear [FILE [LINE]] | breaks | pause
                                    | step (s, into) | next (n, over) | out (o) | continue (c) | quit
  devce_dbg.py -x wait -x stack -x "eval 1+2" -x continue     scripted; `wait` blocks until a `stopped` event, `sleep N` pauses
Options: --port N (default 4320), --retry SECONDS (keep trying to connect while the game starts, default 120)."""
import argparse, functools, json, socket, sys, threading, time
print = functools.partial(print, flush=True)

class Client:
    def __init__(self, port, retry):
        end = time.time() + retry
        while True:
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=2)
                break
            except OSError:
                if time.time() > end: raise SystemExit("cannot connect to 127.0.0.1:%d" % port)
                time.sleep(0.5)
        self.s.settimeout(None)
        self.id = 0; self.replies = {}; self.cv = threading.Condition(); self.stops = 0; self.consumed = 0; self.closed = False
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
                except ValueError: print("?? " + line.decode("utf-8", "replace")); continue
                with self.cv:
                    if "id" in m: self.replies[m["id"]] = m
                    elif m.get("event") == "stopped": self.stops += 1; print("** stopped: %s in state %s" % (m.get("reason"), m.get("state")), flush=True)
                    else: print("** %s" % json.dumps(m))
                    self.cv.notify_all()

    def call(self, cmd, timeout=60, **kw):
        self.id += 1; i = self.id
        self.s.sendall((json.dumps(dict(id=i, cmd=cmd, **kw)) + "\n").encode())
        end = time.time() + timeout
        with self.cv:
            while i not in self.replies:
                if self.closed or time.time() > end: return {"ok": False, "error": "no reply (connection closed or timeout)"}
                self.cv.wait(0.2)
            return self.replies.pop(i)

    def wait_stop(self, timeout=180):
        """Blocks until a `stopped` event that has not been waited for yet has arrived."""
        end = time.time() + timeout
        with self.cv:
            while self.stops <= self.consumed:
                if self.closed or time.time() > end: return False
                self.cv.wait(0.2)
            self.consumed = self.stops
        return True

def show(r):
    if not r.get("ok"): print("error: " + str(r.get("error"))); return
    if "states" in r:
        for x in r["states"]: print("  state %s mask %s hook %s" % (x["state"], x["mask"], x["hook"]))
        print("  tracking=%s hasBps=%s" % (r.get("tracking"), r.get("hasBps")))
    elif "breakpoints" in r:
        for b in r["breakpoints"]: print("  break %s:%s" % (b["file"], b["line"]))
        if not r["breakpoints"]: print("  (no breakpoints)")
    elif "frames" in r:
        for f in r["frames"]: print("  #%d %s:%s" % (f["level"], f["src"], f["line"]))
    elif "result" in r: print(r["result"])
    else: print("ok " + json.dumps({k: v for k, v in r.items() if k not in ("id", "ok")}))

def run(c, line):
    line = line.strip()
    if not line: return True
    cmd, _, arg = line.partition(" ")
    if cmd == "quit": return False
    if cmd == "wait": print("waiting for a stop..."); print("stopped" if c.wait_stop() else "timeout / closed"); return True
    if cmd == "sleep": time.sleep(float(arg or 1)); return True
    cmd = {"s": "step", "c": "continue", "bt": "stack", "n": "next", "o": "out"}.get(cmd, cmd)
    kw = {}
    if cmd == "eval": kw = dict(expr=arg)
    elif cmd in ("break", "clear") and arg:
        parts = arg.rsplit(None, 1)
        if cmd == "break" or (len(parts) == 2 and parts[1].isdigit()):
            kw = dict(file=parts[0], line=int(parts[1]))
        else: kw = dict(file=arg)
    r = c.call(cmd, **kw)
    show(r)
    if cmd in ("step", "next", "out") and r.get("ok"):          # these end with the next `stopped` event: wait for it so the next command sees the new frame
        print("stopped" if c.wait_stop(30) else "no stop within 30 s")
    return True

if __name__ == "__main__":
    ap = argparse.ArgumentParser(); ap.add_argument("--port", type=int, default=4320); ap.add_argument("--retry", type=float, default=120)
    ap.add_argument("-x", action="append", help="command (repeatable)")
    a = ap.parse_args()
    c = Client(a.port, a.retry); print("connected")
    if a.x:
        for line in a.x: print("> " + line); run(c, line)
    else:
        for line in sys.stdin:
            if not run(c, line): break
