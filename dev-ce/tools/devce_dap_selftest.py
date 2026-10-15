#!/usr/bin/env python3
"""Scripted DAP client for devce_dap.py (no editor needed). Run it BEFORE starting the game (or while the game loads): it connects as soon as the debugger
server is up, because dev-ce/mod/DevCE_Test/Scripts/DevCE_DebugDemo.lua waits 60 s for a client during load.

    python devce_dap_selftest.py [--retry 600]

Needs the DevCE_Test mod deployed and the marker file DevCE_Debug.txt next to the DLL with two lines: 4320 and demo. Prints PASS / FAIL per check, exit code 0 when all pass.
Line numbers refer to DevCE_DebugDemo.lua (22 / 23 in probe(), 26 / 27 in work()).
"""
import argparse, json, os, queue, subprocess, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))


class Client:
    def __init__(self):
        self.p = subprocess.Popen([sys.executable, "-u", os.path.join(HERE, "devce_dap.py"), "--stdio"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.seq = 1; self.responses = {}; self.events = queue.Queue(); self.cv = threading.Condition(); self.outputs = []
        threading.Thread(target=self.reader, daemon=True).start()
        threading.Thread(target=lambda: [sys.stderr.write("[adapter] " + l.decode()) for l in self.p.stderr], daemon=True).start()

    def reader(self):
        f = self.p.stdout
        while True:
            n = None
            while True:
                line = f.readline()
                if not line: return
                line = line.strip()
                if not line: break
                if line.lower().startswith(b"content-length:"): n = int(line.split(b":")[1])
            m = json.loads(f.read(n).decode())
            if m["type"] == "response":
                with self.cv: self.responses[m["request_seq"]] = m; self.cv.notify_all()
            elif m["event"] == "output": self.outputs.append(m["body"]["output"].strip())
            else: self.events.put(m)

    def req(self, command, arguments=None, timeout=60):
        s = self.seq; self.seq += 1
        body = json.dumps({"seq": s, "type": "request", "command": command, "arguments": arguments or {}}).encode()
        self.p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body); self.p.stdin.flush()
        with self.cv:
            end = time.time() + timeout
            while s not in self.responses:
                if time.time() > end: raise TimeoutError(command)
                self.cv.wait(0.2)
        return self.responses.pop(s)

    def wait_event(self, name, timeout=60):
        end = time.time() + timeout
        while time.time() < end:
            try:
                e = self.events.get(timeout=0.5)
                if e["event"] == name: return e
            except queue.Empty: pass
        return None


def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name + (("  " + str(extra)) if extra != "" else ""), flush=True)
    return bool(cond)


def top(c):
    fr = c.req("stackTrace", {"threadId": 1})["body"]["stackFrames"]
    return fr


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--retry", type=float, default=600)
    a = ap.parse_args()
    c = Client(); ok = True
    r = c.req("initialize", {"adapterID": "devce-lua"})
    ok &= check("initialize", r["success"] and r["body"].get("supportsConditionalBreakpoints") and r["body"].get("supportsLogPoints") and r["body"].get("supportsHitConditionalBreakpoints"))
    ok &= check("initialized event", c.wait_event("initialized", 5) is not None)
    f = "C:/some/editor/copy/DevCE_DebugDemo.lua"                      # the editor's path differs from the game's: matching is by file name
    r = c.req("setBreakpoints", {"source": {"path": f}, "breakpoints": [
        {"line": 22, "condition": "n == 999"}, {"line": 23, "condition": "n == 103"}, {"line": 26, "logMessage": "i={i}"}, {"line": 27, "hitCondition": "3"}]})
    ok &= check("setBreakpoints before attach (stored)", r["success"])
    r = c.req("attach", {"port": 4320, "retrySeconds": a.retry}, timeout=a.retry + 30); ok &= check("attach", r["success"], r.get("message", ""))
    if not r["success"]: return 1
    ok &= check("configurationDone", c.req("configurationDone")["success"])
    e = c.wait_event("stopped", 120)
    ok &= check("stopped: debug.c_breakpoint()", e is not None and e["body"]["reason"] == "breakpoint" and "c_breakpoint" in e["body"]["description"], e and e["body"])
    if e is None: return 1
    ok &= check("threads", c.req("threads")["body"]["threads"][0]["id"] == 1)
    fr = top(c); ok &= check("stackTrace", len(fr) >= 4 and fr[1]["name"] == "DevCE_DebugDemo.lua:9", [x["name"] for x in fr])
    sc = c.req("scopes", {"frameId": 1})["body"]["scopes"]; ok &= check("scopes", [s["name"] for s in sc] == ["Locals", "Upvalues"])
    vs = c.req("variables", {"variablesReference": sc[0]["variablesReference"]})["body"]["variables"]
    ok &= check("variables: locals of inner", {v["name"]: v["value"] for v in vs} == {"a": "20", "b": "22", "sum": "42"}, [(v["name"], v["value"]) for v in vs])
    r = c.req("evaluate", {"expression": "sum + a", "frameId": 1, "context": "repl"}); ok &= check("evaluate in frame", r["success"] and r["body"]["result"] == "62", r.get("body", r.get("message")))
    r = c.req("evaluate", {"expression": "nosuch()", "frameId": 1}); ok &= check("evaluate error", not r["success"], r.get("message", "")[:60])
    r = c.req("next", {"threadId": 1}); e = c.wait_event("stopped", 30)
    ok &= check("next", r["success"] and e is not None and e["body"]["reason"] == "step" and top(c)[0]["line"] == 10, top(c)[0]["name"] if e else None)
    r = c.req("stepOut", {"threadId": 1}); e = c.wait_event("stopped", 30)
    ok &= check("stepOut", r["success"] and e is not None and top(c)[0]["line"] == 15, top(c)[0]["name"] if e else None)
    r = c.req("continue", {"threadId": 1}); ok &= check("continue", r["success"])
    e = c.wait_event("stopped", 30)
    fr = top(c) if e else []
    ok &= check("conditional breakpoint (n == 103; n == 999 never)", e is not None and e["body"]["reason"] == "breakpoint" and fr and fr[0]["line"] == 23, [x["name"] for x in fr[:2]])
    sc = c.req("scopes", {"frameId": 0})["body"]["scopes"]
    loc = {v["name"]: v for v in c.req("variables", {"variablesReference": sc[0]["variablesReference"]})["body"]["variables"]}
    up = {v["name"]: v for v in c.req("variables", {"variablesReference": sc[1]["variablesReference"]})["body"]["variables"]}
    ok &= check("variables: locals / upvalues of probe", {"a", "b", "t", "s", "n"} <= set(loc) and loc["n"]["value"] == "103" and up.get("upv", {}).get("value") == "100", sorted(loc))
    kids = {v["name"]: v for v in c.req("variables", {"variablesReference": loc["t"]["variablesReference"]})["body"]["variables"]}
    ok &= check("expand table", {"x", "y", "name"} <= set(kids) and kids["x"]["value"] == "1", sorted(kids))
    sub = c.req("variables", {"variablesReference": kids["y"]["variablesReference"]})["body"]["variables"]
    ok &= check("expand nested table", len(sub) == 1 and sub[0]["name"] == "z" and sub[0]["value"] == "2", [(v["name"], v["value"]) for v in sub])
    r = c.req("evaluate", {"expression": "t.y.z + upv", "frameId": 0}); ok &= check("evaluate with locals and upvalues", r["success"] and r["body"]["result"] == "102", r.get("body"))
    r = c.req("evaluate", {"expression": "t", "frameId": 0, "context": "hover"}); ok &= check("evaluate table gives a reference", r["success"] and r["body"]["variablesReference"] > 0)
    r = c.req("continue", {"threadId": 1}); e = c.wait_event("stopped", 30)
    fr = top(c) if e else []
    ok &= check("hit condition 3 (stops once, DemoI == 3)", e is not None and fr and fr[0]["line"] == 27 and c.req("evaluate", {"expression": "DemoI", "frameId": 0})["body"]["result"] == "3")
    ok &= check("logpoint output (no stop)", [o for o in c.outputs if o.startswith("i=")] == ["i=1", "i=2", "i=3"], c.outputs)
    c.req("setBreakpoints", {"source": {"path": f}, "breakpoints": []})
    ok &= check("continue after clearing", c.req("continue", {"threadId": 1})["success"])
    time.sleep(1)
    ok &= check("pause request", c.req("pause", {"threadId": 1})["success"])
    e = c.wait_event("stopped", 30); ok &= check("stopped (pause) in the busy loop", e is not None and e["body"]["reason"] == "pause")
    r = c.req("evaluate", {"expression": "DemoSpin = false", "context": "repl"}); ok &= check("evaluate assignment to a global", r["success"], r.get("message", ""))
    ok &= check("continue", c.req("continue", {"threadId": 1})["success"])
    time.sleep(1)
    ok &= check("disconnect", c.req("disconnect")["success"])
    print("ALL PASS" if ok else "SOME CHECKS FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
