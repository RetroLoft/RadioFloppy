#!/usr/bin/env python3
"""
Browser test of the RadioFloppy web interface against a real device.

    RADIOFLOPPY_HOST=192.168.x.y [RADIOFLOPPY_TOKEN=...] tests/web/ui_test.py

Needs Google Chrome and the Python package websocket-client (for the
DevTools protocol). Slot 1 and 2 must hold valid images. Images stored
when the test starts are left alone: the test only uses free slots, frees
exactly the slots it filled, and restores the disk that was active at the
start. DRIVE_BUSY, a lost connection and a phone
screen are simulated in the browser.
"""
import http.client
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request

import websocket

HOST = os.environ.get("RADIOFLOPPY_HOST")
TOKEN = os.environ.get("RADIOFLOPPY_TOKEN")
HERE = os.path.dirname(os.path.abspath(__file__))
IMAGES = os.path.join(HERE, "..", "..", "images")
PORT = 9333

failures = 0


def check(cond, what):
    global failures
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        failures += 1


# ---- Direct API access (setup / cross-checks) --------------------------------

def api(method, path, body=None, raw=None):
    c = http.client.HTTPConnection(HOST, 80, timeout=60)
    h = {"Authorization": "Bearer " + TOKEN} if TOKEN else {}
    data = None
    if body is not None:
        data, h["Content-Type"] = json.dumps(body).encode(), "application/json"
    if raw is not None:
        data, h["Content-Type"] = raw, "application/octet-stream"
    c.request(method, path, body=data, headers=h)
    r = c.getresponse()
    d = r.read()
    c.close()
    return r.status, (json.loads(d) if d else None)


def api_upload(data, name, **opt):
    body = {"filename": name, "size": len(data), **opt}
    s, up = api("POST", "/api/v1/uploads", body)
    return api("PUT", up["upload_url"], raw=data) if s == 201 else (s, up)


# ---- Minimal DevTools client -------------------------------------------------

class Browser:
    def __init__(self):
        self.dir = tempfile.mkdtemp()
        self.proc = subprocess.Popen(
            ["google-chrome", "--headless=new", "--disable-gpu", "--no-sandbox",
             "--remote-debugging-port=%d" % PORT, "--user-data-dir=" + self.dir, "about:blank"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(50):
            try:
                pages = json.load(urllib.request.urlopen("http://127.0.0.1:%d/json" % PORT))
                url = [p for p in pages if p["type"] == "page"][0]["webSocketDebuggerUrl"]
                break
            except Exception:
                time.sleep(0.2)
        self.ws = websocket.create_connection(url, timeout=120, suppress_origin=True)
        self.id = 0
        self.events = []
        for m in ("Page.enable", "Runtime.enable", "Network.enable", "DOM.enable"):
            self.cmd(m)

    def cmd(self, method, **params):
        self.id += 1
        self.ws.send(json.dumps({"id": self.id, "method": method, "params": params}))
        while True:
            msg = json.loads(self.ws.recv())
            if msg.get("id") == self.id:
                return msg.get("result", {})
            self.on_event(msg)

    def on_event(self, msg):
        if msg.get("method") == "Page.javascriptDialogOpening":
            self.events.append(msg)
            # answered by the test via self.dialog_answer
            self.ws.send(json.dumps({"id": 0, "method": "Page.handleJavaScriptDialog",
                                     "params": {"accept": self.dialog_answer}}))
        elif msg.get("method"):
            self.events.append(msg)

    dialog_answer = True

    def js(self, expr, await_promise=False):
        r = self.cmd("Runtime.evaluate", expression=expr, awaitPromise=await_promise,
                     returnByValue=True)
        return r.get("result", {}).get("value")

    def pump(self, seconds):
        """Let time pass while handling events (dialogs)."""
        end = time.time() + seconds
        self.ws.settimeout(0.2)
        while time.time() < end:
            try:
                self.on_event(json.loads(self.ws.recv()))
            except websocket.WebSocketTimeoutException:
                pass
        self.ws.settimeout(120)

    def wait(self, expr, timeout=30):
        end = time.time() + timeout
        while time.time() < end:
            if self.js(expr):
                return True
            self.pump(0.3)
        return False

    def set_file(self, path):
        doc = self.cmd("DOM.getDocument")
        node = self.cmd("DOM.querySelector", nodeId=doc["root"]["nodeId"], selector="#file")
        self.cmd("DOM.setFileInputFiles", nodeId=node["nodeId"], files=[os.path.abspath(path)])
        self.js("document.getElementById('file').dispatchEvent(new Event('change'))")

    def close(self):
        self.proc.terminate()
        self.proc.wait()
        shutil.rmtree(self.dir, ignore_errors=True)


def text(b, sel):
    return b.js("(document.querySelector(%s)||{}).innerText||''" % json.dumps(sel))


def upload_via_ui(b, path, dest, slot=None, activate=False):
    b.set_file(path)
    b.js("document.querySelector('input[name=dest][value=%s]').click()" % dest)
    if slot:
        b.js("document.getElementById('slotsel').value='%d'" % slot)
    b.js("document.getElementById('activate').checked=%s" % ("true" if activate else "false"))
    b.js("document.getElementById('msg').classList.add('hidden')")
    b.js("document.getElementById('upbtn').click()")


def main():
    if not HOST:
        sys.exit("set RADIOFLOPPY_HOST (and RADIOFLOPPY_TOKEN if the device has one)")
    tmp = tempfile.mkdtemp()
    cc = os.path.join(IMAGES, "CRYSTAL_CASTLES.ST")
    rl = os.path.join(IMAGES, "RETROLOFT_TEST_720K.ST")
    bad = os.path.join(tmp, "notadisk.st")
    open(bad, "wb").write(os.urandom(1000))
    tenspt = os.path.join(tmp, "seventy tracks.st")
    open(tenspt, "wb").write(bytes(645120))

    start = api("GET", "/api/v1/current")[1]
    keep = [x["slot"] for x in api("GET", "/api/v1/slots")[1]["slots"] if x["status"] == "valid"]
    first = [n for n in range(1, 21) if n not in keep][0]
    b = Browser()
    try:
        print("page and slot list")
        b.cmd("Page.navigate", url="http://%s/" % HOST)
        check(b.wait("document.querySelectorAll('#slots li').length == 20"), "20 slots shown")
        check(b.wait("document.getElementById('conn').innerText == 'Connected'"), "status 'Connected'")
        cur = api("GET", "/api/v1/current")[1]
        check(cur["name"] in text(b, "#current") and "Active" in text(b, "#current"),
              "Current Floppy shows the active disk")
        if cur.get("slot"):
            check(b.js("document.querySelectorAll('#slots li')[%d].className" % (cur["slot"] - 1)) == "active",
                  "active slot marked in the list")
        auth = api("GET", "/api/v1/status")[1].get("auth_required", False)
        if auth:
            check(b.js("[...document.querySelectorAll('#slots button')].every(x => x.disabled)"),
                  "token required, none entered: Load buttons disabled")
            check(b.js("document.getElementById('upbtn').disabled"), "token required: Upload disabled")
            check("view only" in text(b, "#authstate"), "token required: 'view only' shown")
        else:
            check(b.js("document.getElementById('authsec').classList.contains('hidden')"),
                  "no token on the device: no 'Admin access' section")
            check(not b.js("document.getElementById('upbtn').disabled"), "no token needed: Upload enabled")
            check(b.js("[...document.querySelectorAll('#slots li:not(.active) button')].every(x => !x.disabled)"),
                  "no token needed: Load buttons enabled")

        print("change made by another client shows up by polling")
        other = 1 if cur.get("slot") != 1 else 2
        for _ in range(5):          # the Atari may be using drive B: (DRIVE_BUSY)
            st, r = api("PUT", "/api/v1/current", {"slot": other})
            if st == 200:
                break
            print("  (external PUT: %s, retrying)" % (r or {}).get("error", {}).get("code"))
            time.sleep(2)
        check(st == 200, "external client activated slot %d" % other)
        check(b.wait("document.querySelectorAll('#slots li')[%d].className == 'active'" % (other - 1), 20),
              "slot %d marked active after an external change" % other)

        target = 2 if other == 1 else 1
        if auth:
            print("wrong token")
            b.js("document.getElementById('token').value='wrong'; document.getElementById('authform').requestSubmit()")
            b.js("document.querySelectorAll('#slots li')[%d].querySelector('button').click()" % (target - 1))
            check(b.wait("document.getElementById('msg').innerText.includes('token is missing or wrong')"),
                  "401: understandable message")

        print("load a stored slot")
        if auth:
            b.js("document.getElementById('token').value=%s; document.getElementById('authform').requestSubmit()"
                 % json.dumps(TOKEN))
        check(b.wait("!document.querySelectorAll('#slots li')[%d].querySelector('button').disabled" % (target - 1)),
              "with token: Load enabled")
        b.js("document.querySelectorAll('#slots li')[%d].querySelector('button').click()" % (target - 1))
        check(b.wait("document.getElementById('msg').innerText.startsWith('Loading floppy')", 5),
              "progress 'Loading floppy…' shown")
        ok = b.wait("document.getElementById('msg').innerText.includes('is now the active floppy')")
        check(ok, "load succeeded" + ("" if ok else " (page said: %r)" % text(b, "#msg")[:120]))
        check(b.wait("document.querySelectorAll('#slots li')[%d].className == 'active'" % (target - 1)),
              "active mark moved after the confirmed load")

        print("DRIVE_BUSY (simulated in the browser)")
        b.js("""window._fetch = window.fetch; window.fetch = (u, o) =>
                (o && o.method == 'PUT' && u.endsWith('/current'))
                ? Promise.resolve(new Response(JSON.stringify({error:{code:'DRIVE_BUSY',message:'x'}}), {status:409}))
                : window._fetch(u, o);""")
        b.js("document.querySelectorAll('#slots li')[%d].querySelector('button').click()" % (other - 1))
        check(b.wait("document.getElementById('msg').innerText.includes('still using drive B')"),
              "DRIVE_BUSY: 'still using drive B:' shown")
        check(b.js("!!document.getElementById('retry')"), "DRIVE_BUSY: 'Try again' button")
        check(b.js("document.querySelectorAll('#slots li')[%d].className" % (target - 1)) == "active",
              "DRIVE_BUSY: active mark unchanged")
        b.js("window.fetch = window._fetch")

        print("PSRAM upload")
        stored = lambda: [(x["slot"], x["status"], x.get("name"), x.get("size"))
                          for x in api("GET", "/api/v1/slots")[1]["slots"]]
        before = stored()
        b.js("""window._n = 0; window._busyGets = 0; const f = window.fetch;
                window.fetch = (u, o) => { if (S.busy && (!o || o.method == 'GET' || !o.method)) window._busyGets++;
                                           return f(u, o); };""")
        upload_via_ui(b, rl, "psram")
        check(b.wait("document.getElementById('msg').innerText.includes('temporary floppy')", 60),
              "PSRAM upload succeeded")
        check(b.js("window._busyGets") == 0, "no polling requests during the upload")
        check(b.wait("document.getElementById('current').innerText.includes('Temporary')"),
              "Current Floppy: temporary (PSRAM)")
        check(stored() == before, "no flash slot used by the PSRAM upload")

        print("upload to the first free slot")
        upload_via_ui(b, cc, "free")
        check(b.wait("document.getElementById('msg').innerText.includes('is stored in slot %d')" % first, 60),
              "stored in slot %d (first free)" % first)
        check(b.wait("document.querySelectorAll('#slots li')[%d].innerText.includes('CRYSTAL_CASTLES')" % (first - 1)),
              "slot list refreshed")

        print("explicit slot: confirmation before overwriting")
        b.dialog_answer = False
        b.events.clear()
        upload_via_ui(b, rl, "slot", slot=first)
        b.pump(2)
        dialogs = [e for e in b.events if e["method"] == "Page.javascriptDialogOpening"]
        check(dialogs and "CRYSTAL_CASTLES" in dialogs[0]["params"]["message"],
              "confirmation names the image that would be replaced")
        check(api("GET", "/api/v1/slots")[1]["slots"][first - 1]["name"] == "CRYSTAL_CASTLES",
              "cancelled: slot %d unchanged" % first)
        b.dialog_answer = True
        upload_via_ui(b, rl, "slot", slot=first, activate=True)
        check(b.wait("document.getElementById('msg').innerText.includes('is stored in slot %d and is now the active')" % first, 60),
              "confirmed: slot %d overwritten and loaded" % first)

        print("error messages")
        upload_via_ui(b, bad, "free")
        check(b.wait("document.getElementById('msg').innerText.includes('not a valid .ST')", 30),
              "invalid image: understandable message")
        upload_via_ui(b, tenspt, "free")
        check(b.wait("document.getElementById('msg').innerText.includes('is not supported')", 30),
              "unsupported geometry: understandable message")
        check("probably a format that is not supported" in text(b, "#fileinfo"),
              "file size warning shown right after choosing the file")
        for n in range(first + 1, 21):
            if n not in keep:
                api_upload(open(cc, "rb").read(), "Fill %d.st" % n, destination="flash")
        upload_via_ui(b, cc, "free")
        check(b.wait("document.getElementById('msg').innerText.includes('All 20 slots are in use')", 30),
              "full flash: NO_FREE_SLOT explained")

        print("connection lost and restored (simulated)")
        b.cmd("Network.emulateNetworkConditions", offline=True, latency=0,
              downloadThroughput=-1, uploadThroughput=-1)
        check(b.wait("!document.getElementById('offline').classList.contains('hidden')", 20),
              "'Connection lost' banner shown")
        check(b.js("document.getElementById('current').classList.contains('stale')"),
              "old data shown as stale")
        b.cmd("Network.emulateNetworkConditions", offline=False, latency=0,
              downloadThroughput=-1, uploadThroughput=-1)
        check(b.wait("document.getElementById('offline').classList.contains('hidden')", 20),
              "reconnected automatically")

        print("phone screen")
        b.cmd("Emulation.setDeviceMetricsOverride", width=390, height=844, deviceScaleFactor=2, mobile=True)
        b.pump(1)
        check(b.js("document.documentElement.scrollWidth <= window.innerWidth"),
              "390 px wide: no horizontal scrolling")
        shot = b.cmd("Page.captureScreenshot", captureBeyondViewport=True)
        open(os.path.join(tmp, "phone.png"), "wb").write(__import__("base64").b64decode(shot["data"]))
        print("  (screenshot: %s)" % os.path.join(tmp, "phone.png"))
    finally:
        b.close()
        print("cleanup")
        for x in api("GET", "/api/v1/slots")[1]["slots"]:
            if x["status"] == "valid" and x["slot"] not in keep:
                api("DELETE", "/api/v1/slots/%d" % x["slot"])
        if start.get("slot"):
            api("PUT", "/api/v1/current", {"slot": start["slot"]})
        print("  restored: " + json.dumps(api("GET", "/api/v1/current")[1]))

    print("\n%s" % ("ALL UI TESTS PASSED" if not failures else "%d FAILED" % failures))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
