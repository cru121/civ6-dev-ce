#!/usr/bin/env python3
"""Debug Adapter Protocol (DAP) adapter for the Dev CE Lua debugger (LuaDebugger.cpp, loopback JSON lines, default port 4320).

    python devce_dap.py --stdio              # editor starts it and talks DAP over stdin / stdout
    python devce_dap.py --port 4711          # listen for an editor on 127.0.0.1:4711 (one session at a time)

Needs: Civilization VI running with the Dev CE DLL and the debugger enabled (file DevCE_Debug.txt next to the DLL, see dev-ce/DESIGN_LUA_DEBUGGER.md).
`launch` and `attach` both connect to the game: arguments {"port": 4320, "retrySeconds": 60, "pathDepth": 1}. pathDepth = how many trailing path components of a
source file are sent to the game for matching its chunk names (1 = the file name only, which works when the editor's copy of the script lives somewhere else).

Mapping: setBreakpoints (conditions, hit conditions "N" / ">= N", log messages "text {expr}") -> break / clear; continue / next / stepIn / stepOut / pause;
stackTrace; scopes Locals + Upvalues; variables (tables expand); evaluate (frame aware, assignments to globals persist). No setVariable, one thread (id 1),
frames have no function names. Tested only with devce_dap_selftest.py (a scripted client), not with an editor.
"""
import argparse, json, os, re, socket, sys, threading, time

STOP_REASONS = {"breakpoint": "breakpoint", "c_breakpoint": "breakpoint", "step": "step", "pause": "pause"}


class Game:
    """JSON-lines connection to the DLL: replies are matched by id, events go to a callback."""
    def __init__(self, port, retry):
        end = time.time() + retry
        while True:
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=2); break
            except OSError:
                if time.time() > end: raise ConnectionError("cannot connect to the game debugger on 127.0.0.1:%d" % port)
                time.sleep(0.5)
        self.s.settimeout(None)
        self.id = 0; self.replies = {}; self.cv = threading.Condition(); self.closed = False; self.on_event = lambda m: None
        threading.Thread(target=self.reader, daemon=True).start()

    def reader(self):
        buf = b""
        while True:
            try: d = self.s.recv(65536)
            except OSError: d = b""
            if not d:
                with self.cv: self.closed = True; self.cv.notify_all()
                self.on_event({"event": "closed"}); return
            buf += d
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                try: m = json.loads(line.decode("utf-8", "replace"))
                except ValueError: continue
                if "id" in m:
                    with self.cv: self.replies[m["id"]] = m; self.cv.notify_all()
                else: self.on_event(m)

    def call(self, cmd, timeout=30, **kw):
        with self.cv:
            self.id += 1; i = self.id
        try: self.s.sendall((json.dumps(dict(id=i, cmd=cmd, **kw)) + "\n").encode())
        except OSError: return {"ok": False, "error": "connection to the game lost"}
        end = time.time() + timeout
        with self.cv:
            while i not in self.replies:
                if self.closed or time.time() > end: return {"ok": False, "error": "no reply from the game"}
                self.cv.wait(0.2)
            return self.replies.pop(i)

    def close(self):
        try: self.s.close()
        except OSError: pass


def hit_count(cond):
    """DAP hitCondition -> N for the DLL ('stop from the N-th hit on'). Accepts "N", ">= N", ">N" (N+1); anything else is ignored."""
    m = re.match(r"^\s*(>=|>|==)?\s*(\d+)\s*$", cond or "")
    if not m: return 0
    n = int(m.group(2))
    return n + 1 if m.group(1) == ">" else n


def log_expr(msg):
    """DAP logMessage 'text {expr} more' -> Lua expression  'text ' .. tostring(expr) .. ' more'."""
    parts, pos = [], 0
    for m in re.finditer(r"\{([^{}]*)\}", msg):
        if m.start() > pos: parts.append("%s" % json.dumps(msg[pos:m.start()]))
        parts.append("tostring(%s)" % m.group(1))
        pos = m.end()
    if pos < len(msg): parts.append(json.dumps(msg[pos:]))
    return " .. ".join(parts) if parts else '""'


class Adapter:
    def __init__(self, rfile, wfile):
        self.rfile, self.wfile = rfile, wfile
        self.out_lock = threading.Lock(); self.seq = 1
        self.game = None; self.attached = False; self.running = True
        self.bps = {}                       # source path -> list of DAP breakpoint dicts
        self.refs = {}; self.next_ref = 1   # variablesReference -> ("scope", level) | ("kids", level, path)
        self.stopped = False
        self.hold = False; self.deferred = []; self.evlock = threading.Lock()
        self.path_depth = 1

    # ---- DAP wire ------------------------------------------------------------------------------------------
    def send(self, msg):
        with self.out_lock:
            msg["seq"] = self.seq; self.seq += 1
            body = json.dumps(msg).encode("utf-8")
            self.wfile.write(b"Content-Length: %d\r\n\r\n" % len(body) + body); self.wfile.flush()

    def respond(self, req, ok=True, body=None, message=None):
        r = {"type": "response", "request_seq": req["seq"], "success": ok, "command": req["command"]}
        if body is not None: r["body"] = body
        if message: r["message"] = message
        self.send(r)

    def event(self, name, body=None):
        e = {"type": "event", "event": name}
        if body is not None: e["body"] = body
        self.send(e)

    def read(self):
        n = None
        while True:
            line = self.rfile.readline()
            if not line: return None
            line = line.strip()
            if not line: break
            if line.lower().startswith(b"content-length:"): n = int(line.split(b":")[1])
        return json.loads(self.rfile.read(n).decode("utf-8")) if n else None

    # ---- events from the game -----------------------------------------------------------------------------
    def on_game_event(self, m):
        ev = m.get("event")
        if ev == "stopped":
            self.stopped = True
            self.refs.clear(); self.next_ref = 1
            out = ("stopped", {"reason": STOP_REASONS.get(m.get("reason"), "breakpoint"), "threadId": 1, "allThreadsStopped": True,
                               "description": "debug.c_breakpoint()" if m.get("reason") == "c_breakpoint" else m.get("reason")})
        elif ev == "output":
            out = ("output", {"category": "console", "output": str(m.get("text", "")) + "\n"})
        elif ev == "closed":
            out = ("terminated", {})
        else:
            return
        with self.evlock:
            if self.hold: self.deferred.append(out); return
        self.event(*out)

    def begin_resume(self):
        with self.evlock: self.hold = True
        self.stopped = False; self.refs.clear(); self.next_ref = 1

    def end_resume(self):
        with self.evlock:
            self.hold = False; pending, self.deferred = self.deferred, []
        for e in pending: self.event(*e)

    # ---- helpers ----------------------------------------------------------------------------------------------
    def new_ref(self, *what):
        r = self.next_ref; self.next_ref += 1; self.refs[r] = what; return r

    def short_path(self, path):
        parts = [p for p in re.split(r"[\\/]+", path) if p]
        return "/".join(parts[-max(1, self.path_depth):])

    def sync_breakpoints(self, path):
        if not self.attached: return [{"verified": False, "message": "not attached"} for _ in self.bps.get(path, [])]
        name = self.short_path(path)
        self.game.call("clear", file=name)
        out = []
        for b in self.bps.get(path, []):
            kw = dict(file=name, line=b["line"])
            if b.get("condition"): kw["cond"] = b["condition"]
            if b.get("logMessage"): kw["log"] = log_expr(b["logMessage"])
            n = hit_count(b.get("hitCondition"))
            if n: kw["hit"] = n
            r = self.game.call("break", **kw)
            out.append({"verified": bool(r.get("ok")), "line": b["line"]} if r.get("ok") else {"verified": False, "line": b["line"], "message": str(r.get("error"))})
        return out

    @staticmethod
    def frame_of(a):
        f = a.get("frameId")
        return f if isinstance(f, int) and f >= 0 else None

    # ---- request handlers ------------------------------------------------------------------------------------
    def handle(self, req):
        c = req["command"]; a = req.get("arguments") or {}
        if c == "initialize":
            self.respond(req, body={"supportsConfigurationDoneRequest": True, "supportsEvaluateForHovers": True, "supportsConditionalBreakpoints": True,
                                    "supportsHitConditionalBreakpoints": True, "supportsLogPoints": True, "supportsSetVariable": False,
                                    "supportsTerminateRequest": False, "supportsStepBack": False})
            self.event("initialized")
        elif c in ("launch", "attach"):
            self.path_depth = int(a.get("pathDepth", 1))
            try:
                self.game = Game(int(a.get("port", 4320)), float(a.get("retrySeconds", 60)))
            except ConnectionError as e:
                return self.respond(req, False, message=str(e))
            self.game.on_event = self.on_game_event
            self.attached = True
            for path in list(self.bps): self.sync_breakpoints(path)       # breakpoints sent before the connection
            self.respond(req)
        elif c == "setBreakpoints":
            path = (a.get("source") or {}).get("path", "") or (a.get("source") or {}).get("name", "")
            self.bps[path] = list(a.get("breakpoints") or [{"line": ln} for ln in a.get("lines", [])])
            self.respond(req, body={"breakpoints": self.sync_breakpoints(path)})
        elif c in ("setExceptionBreakpoints", "configurationDone"):
            self.respond(req)
        elif c == "threads":
            self.respond(req, body={"threads": [{"id": 1, "name": "Lua"}]})
        elif c == "stackTrace":
            if not self.stopped: return self.respond(req, body={"stackFrames": [], "totalFrames": 0})
            r = self.game.call("stack")
            if not r.get("ok"): return self.respond(req, False, message=str(r.get("error")))
            frames = []
            for f in r["frames"]:
                src, line = f["src"], f["line"]
                base = os.path.basename(src.replace("\\", "/"))
                fr = {"id": f["level"], "name": ("%s:%s" % (base, line)) if line > 0 else base, "line": max(line, 0), "column": 1}
                if src.startswith("["): fr["presentationHint"] = "subtle"
                else: fr["source"] = {"name": base, "path": src}
                frames.append(fr)
            self.respond(req, body={"stackFrames": frames, "totalFrames": len(frames)})
        elif c == "scopes":
            lvl = a.get("frameId", 0)
            self.respond(req, body={"scopes": [{"name": "Locals", "variablesReference": self.new_ref("locals", lvl), "expensive": False},
                                               {"name": "Upvalues", "variablesReference": self.new_ref("upvalues", lvl), "expensive": False}]})
        elif c == "variables":
            what = self.refs.get(a.get("variablesReference"))
            if not what: return self.respond(req, body={"variables": []})
            lvl = what[1]
            r = self.game.call("vars", level=lvl, **({} if what[0] in ("locals", "upvalues") else {"expr": what[2]}))
            if not r.get("ok"): return self.respond(req, False, message=str(r.get("error")))
            rows = r.get("children") if what[0] == "kids" else r.get("locals" if what[0] == "locals" else "upvalues")
            out = []
            for x in rows or []:
                v = {"name": x["name"], "value": x["value"], "type": x["type"], "variablesReference": 0}
                if x.get("expandable") and x.get("path"): v["variablesReference"] = self.new_ref("kids", lvl, x["path"])
                out.append(v)
            self.respond(req, body={"variables": out})
        elif c == "evaluate":
            expr = (a.get("expression") or "").strip()
            kw = {"expr": expr}
            f = self.frame_of(a)
            if f is not None: kw["level"] = f
            if not self.stopped: return self.respond(req, False, message="not stopped")
            r = self.game.call("eval", **kw)
            if not r.get("ok"): return self.respond(req, False, message=str(r.get("error")).splitlines()[0])
            body = {"result": r["result"], "variablesReference": 0}
            if r["result"].startswith("table:"): body["variablesReference"] = self.new_ref("kids", f if f is not None else 0, expr)
            self.respond(req, body=body)
        elif c in ("continue", "next", "stepIn", "stepOut", "pause"):
            cmd = {"continue": "continue", "next": "next", "stepIn": "step", "stepOut": "out", "pause": "pause"}[c]
            self.begin_resume() if c != "pause" else None
            r = self.game.call(cmd)
            if c == "pause": self.respond(req)
            else:
                self.respond(req, body={"allThreadsContinued": True} if c == "continue" else None) if r.get("ok") else self.respond(req, False, message=str(r.get("error")))
                self.end_resume()
        elif c in ("disconnect", "terminate"):
            if self.game: self.game.close()
            self.attached = False; self.running = False
            self.respond(req)
        else:
            self.respond(req, False, message="unsupported request: " + c)

    def run(self):
        while self.running:
            req = self.read()
            if req is None: break
            if req.get("type") != "request": continue
            try: self.handle(req)
            except Exception as e:                       # never kill the session on a handler bug
                self.respond(req, False, message="adapter error: %r" % (e,))
        if self.game: self.game.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=4711, help="TCP port for an editor (without --stdio)")
    ap.add_argument("--stdio", action="store_true")
    a = ap.parse_args()
    if a.stdio:
        Adapter(sys.stdin.buffer, sys.stdout.buffer).run(); return
    srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", a.port)); srv.listen(1)
    print("Dev CE DAP adapter listening on 127.0.0.1:%d" % a.port, file=sys.stderr)
    while True:
        conn, _ = srv.accept()
        Adapter(conn.makefile("rb"), conn.makefile("wb")).run()
        conn.close()


if __name__ == "__main__":
    main()
