#!/usr/bin/env python3
"""
Browser test of the RadioFloppy web interface against a real device.

    RADIOFLOPPY_HOST=192.168.x.y [RADIOFLOPPY_TOKEN=...] tests/web/ui_test.py

Needs Google Chrome and the Python package websocket-client (for the
DevTools protocol). Needs an SD card with at least two .ST images in the
root directory. Files on the card are left alone: the test works in its
own folder "RF UI test" and removes it again, and inserts the disk that
was active at the start again. DRIVE_BUSY, a lost connection and a phone
screen are simulated in the browser.
"""
import http.client
import json
import urllib.parse
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
            [os.environ.get("CHROME", "google-chrome"), "--headless=new", "--disable-gpu", "--no-sandbox",
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
            params = {"accept": self.dialog_answer}
            if msg["params"].get("type") == "prompt":
                params["promptText"] = self.prompt_text
            self.ws.send(json.dumps({"id": 0, "method": "Page.handleJavaScriptDialog",
                                     "params": params}))
        elif msg.get("method"):
            self.events.append(msg)

    dialog_answer = True
    prompt_text = ""

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

    def set_file(self, path, selector="#file"):
        doc = self.cmd("DOM.getDocument")
        node = self.cmd("DOM.querySelector", nodeId=doc["root"]["nodeId"], selector=selector)
        self.cmd("DOM.setFileInputFiles", nodeId=node["nodeId"], files=[os.path.abspath(path)])
        self.js("document.querySelector(%s).dispatchEvent(new Event('change'))" % json.dumps(selector))

    def close(self):
        self.proc.terminate()
        self.proc.wait()
        shutil.rmtree(self.dir, ignore_errors=True)


def text(b, sel):
    return b.js("(document.querySelector(%s)||{}).innerText||''" % json.dumps(sel))


def load_temporarily(b, path):
    """Choose a file in "Load a floppy temporarily" and press Load."""
    b.set_file(path)
    b.js("document.getElementById('msg').classList.add('hidden')")
    b.js("document.getElementById('upbtn').click()")


def q(path):
    return "?path=" + urllib.parse.quote(path)


def sd_row(name):
    """JS expression: the SD list row of a file or folder (by name)."""
    return ("[...document.querySelectorAll('#sdlist li')].find(li => "
            "(li.querySelector('.sdname') || {}).textContent === %s)" % json.dumps(name))


def insert_btn(name):
    return "[...%s.querySelectorAll('button')].find(x => x.textContent === 'Insert')" % sd_row(name)


def synthetic(path, size, seed):
    import zlib
    out = bytearray()
    block = seed.to_bytes(4, "little")
    while len(out) < size:
        block = zlib.compress(block + len(out).to_bytes(4, "little"))[-64:]
        out += block
    open(path, "wb").write(bytes(out[:size]))
    return path


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
    hfe = os.path.join(tmp, "game.hfe")
    open(hfe, "wb").write(bytes(1000))
    T = "RF UI test"

    start = api("GET", "/api/v1/current")[1]
    settings = api("GET", "/api/v1/settings")[1]
    if not settings.get("sd", {}).get("present"):
        sys.exit("no SD card in RadioFloppy")
    if api("GET", "/api/v1/sd/files" + q(T))[0] == 200:      # left over from an aborted run
        api("DELETE", "/api/v1/sd/folders" + q(T) + "&recursive=true&confirm=" + urllib.parse.quote(T))
    root = api("GET", "/api/v1/sd/files")[1]["entries"]
    sts = sorted([e["name"] for e in root if e["type"] == "file" and e["name"].lower().endswith(".st")],
                 key=str.lower)
    if len(sts) < 2:
        sys.exit("need at least two .ST images in the SD card's root directory")
    b = Browser()
    try:
        print("page and SD card list")
        b.cmd("Page.navigate", url="http://%s/" % HOST)
        check(b.wait("S.sdState === 'ok' && document.querySelectorAll('#sdlist li').length == %d" % len(root)),
              "%d entries of the root directory shown" % len(root))
        check(b.wait("document.getElementById('conn').innerText == 'Connected'"), "status 'Connected'")
        check(b.wait("document.getElementById('sdcrumbs').innerText.includes(%s)"
                     % json.dumps(settings["root_dir"] or "SD card"), 10), "breadcrumb shows the root directory")
        check(not b.js("!!document.getElementById('images')"), "no 'My floppy images' list any more")
        cur = api("GET", "/api/v1/current")[1]
        if cur["inserted"]:
            check(cur["name"] in text(b, "#current") and "Active" in text(b, "#current"),
                  "Current Floppy shows the active disk")
        auth = api("GET", "/api/v1/status")[1].get("auth_required", False)
        if auth:
            check(b.js("[...document.querySelectorAll('#sdlist button')].every(x => x.disabled)"),
                  "token required, none entered: SD buttons disabled")
            check(b.js("document.getElementById('upbtn').disabled"), "token required: Load disabled")
            check("view only" in text(b, "#authstate"), "token required: 'view only' shown")
        else:
            check(b.js("document.getElementById('authsec').classList.contains('hidden')"),
                  "no token on the device: no 'Admin access' section")
            check(not b.js("document.getElementById('upbtn').disabled"), "no token needed: Load enabled")

        print("change made by another client shows up by polling")
        other = sts[0] if cur.get("sd_path") != sts[0] else sts[1]
        for _ in range(5):          # the Atari may be using drive B: (DRIVE_BUSY)
            st, r = api("PUT", "/api/v1/current", {"sd_path": other})
            if st == 200:
                break
            print("  (external PUT: %s, retrying)" % (r or {}).get("error", {}).get("code"))
            time.sleep(2)
        check(st == 200, "external client inserted %s" % other)
        check(b.wait(sd_row(other) + " && " + sd_row(other) + ".className == 'active'", 20),
              "%s marked active after an external change" % other)

        target = sts[1] if other == sts[0] else sts[0]
        if auth:
            print("wrong token")
            b.js("document.getElementById('token').value='wrong'; document.getElementById('authform').requestSubmit()")
            b.js(insert_btn(target) + ".click()")
            check(b.wait("document.getElementById('msg').innerText.includes('token is missing or wrong')"),
                  "401: understandable message")
            b.js("document.getElementById('token').value=%s; document.getElementById('authform').requestSubmit()"
                 % json.dumps(TOKEN))

        print("insert from the SD card")
        check(b.wait("!" + insert_btn(target) + ".disabled"), "Insert enabled")
        b.js(insert_btn(target) + ".click()")
        check(b.wait("document.getElementById('msg').innerText.startsWith('Loading floppy')", 5),
              "progress 'Loading floppy…' shown")
        ok = b.wait("document.getElementById('msg').innerText.includes('is now the active floppy')")
        check(ok, "insert succeeded" + ("" if ok else " (page said: %r)" % text(b, "#msg")[:120]))
        check(b.wait(sd_row(target) + ".className == 'active'"), "active mark moved")
        check("From the SD card" in text(b, "#current"), "Current Floppy: from the SD card")

        print("DRIVE_BUSY (simulated in the browser)")
        b.js("""window._fetch = window.fetch; window.fetch = (u, o) =>
                (o && o.method == 'PUT' && u.endsWith('/current'))
                ? Promise.resolve(new Response(JSON.stringify({error:{code:'DRIVE_BUSY',message:'x'}}), {status:409}))
                : window._fetch(u, o);""")
        b.js(insert_btn(other) + ".click()")
        check(b.wait("document.getElementById('msg').innerText.includes('still writing to drive B')"),
              "DRIVE_BUSY: 'still writing to drive B:' shown")
        check(b.js("!!document.getElementById('retry')"), "DRIVE_BUSY: 'Try again' button")
        check(b.js(sd_row(target) + ".className") == "active", "DRIVE_BUSY: active mark unchanged")
        b.js("window.fetch = window._fetch")

        print("load a floppy temporarily")
        b.js("""window._busyGets = 0; const f = window.fetch;
                window.fetch = (u, o) => { if (S.busy && (!o || o.method == 'GET' || !o.method)) window._busyGets++;
                                           return f(u, o); };""")
        load_temporarily(b, rl)
        check(b.wait("document.getElementById('msg').innerText.includes('temporary floppy')", 60),
              "temporary upload succeeded")
        check(b.js("window._busyGets") == 0, "no polling requests during the upload")
        check(b.wait("document.getElementById('current').innerText.includes('Temporary')"),
              "Current Floppy: temporary (PSRAM)")

        print("error messages")
        load_temporarily(b, bad)
        check(b.wait("document.getElementById('msg').innerText.includes('not a valid .ST')", 30),
              "invalid image: understandable message")
        load_temporarily(b, tenspt)
        check(b.wait("document.getElementById('msg').innerText.includes('is not supported')", 30),
              "unsupported geometry: understandable message")
        check("probably a format that is not supported" in text(b, "#fileinfo"),
              "file size warning shown right after choosing the file")
        b.set_file(hfe)
        check("cannot be loaded temporarily" in text(b, "#fileinfo"), "HFE chosen: 'upload it to the SD card'")

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

        print("SD card: folders, upload, new disk")
        check(b.wait("S.sdState === 'ok' && !document.getElementById('sdup').disabled", 20),
              "SD card buttons enabled")
        b.dialog_answer = True
        b.prompt_text = T
        b.js("document.getElementById('sdnewdir').click()")
        check(b.wait("S.sdData && S.sdData.entries.some(e => e.name === %s && e.type === 'folder')" % json.dumps(T), 20),
              "New folder: created and listed")
        b.js("[...document.querySelectorAll('#sdlist a.sdname')].find(a => a.textContent === %s).click()" % json.dumps(T))
        check(b.wait("S.sdPath === %s && S.sdState === 'ok' && document.getElementById('sdcrumbs').innerText.includes(%s)"
                     % (json.dumps(T), json.dumps(T)), 20), "folder opened, breadcrumb follows")
        check(b.wait("document.getElementById('sdlist').innerText.includes('empty')", 10), "empty folder says so")
        b.set_file(cc, "#sdfile")
        name = os.path.basename(cc)
        check(b.wait("S.sdData && S.sdData.entries.some(e => e.name === %s && e.size === %d)"
                     % (json.dumps(name), os.path.getsize(cc)), 60),
              "Upload here: the file is listed with its size")
        check(b.wait("document.getElementById('msg').innerText.includes('stored on the SD card')", 10),
              "upload message")
        b.js(insert_btn(name) + ".click()")
        check(b.wait("S.current && S.current.source === 'sd' && S.current.sd_path === %s"
                     % json.dumps(T + "/" + name), 30), "Insert: the uploaded file is the current floppy")
        check(b.wait(sd_row(name) + ".className == 'active'", 10), "marked Active in the SD list")
        b.set_file(cc, "#sdfile")     # same name again: confirm "Replace it?" (answered yes)
        check(b.wait("document.getElementById('msg').innerText.includes('1 file stored')", 60), "replacing asks and works")
        b.js("document.getElementById('newname').value = 'Save games'; "
             "document.getElementById('newform').requestSubmit()")
        check(b.wait("document.getElementById('msg').innerText.includes('is created on the SD card and is now "
                     "the active floppy')", 30), "New disk: created in the open folder and inserted")
        check(b.wait("S.current && S.current.sd_path === %s && S.current.write.state === 'writable'"
                     % json.dumps(T + "/Save games.st"), 10), "the new disk is the current floppy, writable")
        check(b.wait("S.sdData && S.sdData.entries.some(e => e.name === 'Save games.st' && e.size === 737280)", 20),
              "New disk listed (Save games.st, 720 KiB)")
        b.js("[...document.querySelectorAll('#sdcrumbs a')][0].click()")
        check(b.wait("S.sdPath === '' && S.sdState === 'ok'", 20), "breadcrumb back to the root directory")
        b.js(sd_row(T) + ".querySelector('button.danger').click()")
        check(b.wait("S.sdData && !S.sdData.entries.some(e => e.name === %s)" % json.dumps(T), 60),
              "non-empty folder deleted after two confirmations")
        check(api("GET", "/api/v1/sd/files" + q(T))[0] == 404, "gone on the card")

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
        if api("GET", "/api/v1/sd/files" + q(T))[0] == 200:
            api("DELETE", "/api/v1/sd/folders" + q(T) + "&recursive=true&confirm=" + urllib.parse.quote(T))
        if start.get("source") == "sd" and start.get("sd_path"):
            api("PUT", "/api/v1/current", {"sd_path": start["sd_path"]})
        print("  restored: " + json.dumps(api("GET", "/api/v1/current")[1]))
        check(sorted(e["name"] for e in api("GET", "/api/v1/sd/files")[1]["entries"]) ==
              sorted(e["name"] for e in root), "end state: the root directory as it was")

    print("\n%s" % ("ALL UI TESTS PASSED" if not failures else "%d FAILED" % failures))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
