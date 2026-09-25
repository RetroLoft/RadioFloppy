#!/usr/bin/env python3
"""
Browser test of the RadioFloppy web interface against a real device.

    RADIOFLOPPY_HOST=192.168.x.y [RADIOFLOPPY_TOKEN=...] tests/web/ui_test.py

Needs Google Chrome and the Python package websocket-client (for the
DevTools protocol). The image library must hold at least two valid images
and have room for some more. Images stored when the test starts are left
alone (ids, order, data): the test adds its own images, deletes exactly
those, and restores the disk that was active at the start. DRIVE_BUSY, a
lost connection and a phone screen are simulated in the browser.
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


def upload_via_ui(b, path, dest, activate=False):
    b.set_file(path)
    b.js("document.querySelector('input[name=dest][value=%s]').click()" % dest)
    b.js("document.getElementById('activate').checked=%s" % ("true" if activate else "false"))
    b.js("document.getElementById('msg').classList.add('hidden')")
    b.js("document.getElementById('upbtn').click()")


def images():
    return api("GET", "/api/v1/images")[1]["images"]


def row(image_id):
    """JS expression: the list row of an image (by id)."""
    return "document.querySelectorAll('#images li')[S.images.findIndex(x => x.id == %d)]" % image_id


def btn(image_id, label):
    return "[...%s.querySelectorAll('button')].find(x => x.textContent.startsWith(%s))" % (
        row(image_id), json.dumps(label))


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
    big = synthetic(os.path.join(tmp, "Big test 880K.st"), 901120, 7)

    start = api("GET", "/api/v1/current")[1]
    orig = images()
    orig_ids = [x["id"] for x in orig]
    valid = [x["id"] for x in orig if x["status"] == "valid"]
    b = Browser()
    try:
        print("page and image list")
        b.cmd("Page.navigate", url="http://%s/" % HOST)
        check(b.wait("document.querySelectorAll('#images li').length == %d" % max(len(orig), 1)),
              "%d images shown" % len(orig))
        check(b.wait("document.getElementById('conn').innerText == 'Connected'"), "status 'Connected'")
        storage = api("GET", "/api/v1/storage")[1]
        check(("Storage: %d%% used" % storage["used_percent"]) in text(b, "#storage"),
              "'Storage: %d%% used' shown" % storage["used_percent"])
        check(b.js("[...document.querySelectorAll('#images .slotname')].map(x => x.innerText).join('|')")
              == "|".join(x["name"] for x in orig), "list in sequence order")
        cur = api("GET", "/api/v1/current")[1]
        check(cur["name"] in text(b, "#current") and "Active" in text(b, "#current"),
              "Current Floppy shows the active disk")
        if cur.get("image_id"):
            check(b.js(row(cur["image_id"]) + ".className") == "active", "active image marked in the list")
        auth = api("GET", "/api/v1/status")[1].get("auth_required", False)
        if auth:
            check(b.js("[...document.querySelectorAll('#images button')].every(x => x.disabled)"),
                  "token required, none entered: image buttons disabled")
            check(b.js("document.getElementById('upbtn').disabled"), "token required: Upload disabled")
            check("view only" in text(b, "#authstate"), "token required: 'view only' shown")
        else:
            check(b.js("document.getElementById('authsec').classList.contains('hidden')"),
                  "no token on the device: no 'Admin access' section")
            check(not b.js("document.getElementById('upbtn').disabled"), "no token needed: Upload enabled")
            check(b.js("[...document.querySelectorAll('#images li:not(.active)')].every(li => "
                       "!li.querySelector('button:nth-child(3)').disabled)"),
                  "no token needed: Load buttons enabled")

        print("change made by another client shows up by polling")
        other = valid[0] if cur.get("image_id") != valid[0] else valid[1]
        for _ in range(5):          # the Atari may be using drive B: (DRIVE_BUSY)
            st, r = api("PUT", "/api/v1/current", {"image_id": other})
            if st == 200:
                break
            print("  (external PUT: %s, retrying)" % (r or {}).get("error", {}).get("code"))
            time.sleep(2)
        check(st == 200, "external client activated image %d" % other)
        check(b.wait(row(other) + ".className == 'active'", 20),
              "image %d marked active after an external change" % other)

        target = valid[1] if other == valid[0] else valid[0]
        if auth:
            print("wrong token")
            b.js("document.getElementById('token').value='wrong'; document.getElementById('authform').requestSubmit()")
            b.js(btn(target, "Load") + ".click()")
            check(b.wait("document.getElementById('msg').innerText.includes('token is missing or wrong')"),
                  "401: understandable message")

        print("load a stored image")
        if auth:
            b.js("document.getElementById('token').value=%s; document.getElementById('authform').requestSubmit()"
                 % json.dumps(TOKEN))
        check(b.wait("!" + btn(target, "Load") + ".disabled"), "Load enabled")
        b.js(btn(target, "Load") + ".click()")
        check(b.wait("document.getElementById('msg').innerText.startsWith('Loading floppy')", 5),
              "progress 'Loading floppy…' shown")
        ok = b.wait("document.getElementById('msg').innerText.includes('is now the active floppy')")
        check(ok, "load succeeded" + ("" if ok else " (page said: %r)" % text(b, "#msg")[:120]))
        check(b.wait(row(target) + ".className == 'active'"), "active mark moved after the confirmed load")

        print("DRIVE_BUSY (simulated in the browser)")
        b.js("""window._fetch = window.fetch; window.fetch = (u, o) =>
                (o && o.method == 'PUT' && u.endsWith('/current'))
                ? Promise.resolve(new Response(JSON.stringify({error:{code:'DRIVE_BUSY',message:'x'}}), {status:409}))
                : window._fetch(u, o);""")
        b.js(btn(other, "Load") + ".click()")
        check(b.wait("document.getElementById('msg').innerText.includes('still using drive B')"),
              "DRIVE_BUSY: 'still using drive B:' shown")
        check(b.js("!!document.getElementById('retry')"), "DRIVE_BUSY: 'Try again' button")
        check(b.js(row(target) + ".className") == "active", "DRIVE_BUSY: active mark unchanged")
        b.js("window.fetch = window._fetch")

        print("PSRAM upload")
        used = lambda: api("GET", "/api/v1/storage")[1]["blocks_used"]
        before = used()
        b.js("""window._busyGets = 0; const f = window.fetch;
                window.fetch = (u, o) => { if (S.busy && (!o || o.method == 'GET' || !o.method)) window._busyGets++;
                                           return f(u, o); };""")
        upload_via_ui(b, rl, "psram")
        check(b.wait("document.getElementById('msg').innerText.includes('temporary floppy')", 60),
              "PSRAM upload succeeded")
        check(b.js("window._busyGets") == 0, "no polling requests during the upload")
        check(b.wait("document.getElementById('current').innerText.includes('Temporary')"),
              "Current Floppy: temporary (PSRAM)")
        check(used() == before, "no flash blocks used by the PSRAM upload")

        print("add to my floppy images")
        upload_via_ui(b, cc, "flash")
        check(b.wait("document.getElementById('msg').innerText.includes('added to my floppy images')", 60),
              "added")
        new = images()[-1]
        check(new["name"] == "CRYSTAL_CASTLES" and new["id"] not in orig_ids, "new image at the end of the list")
        check(b.wait("document.querySelectorAll('#images li').length == %d" % (len(orig) + 1)), "list refreshed")
        pct = api("GET", "/api/v1/storage")[1]["used_percent"]
        check(b.wait("document.getElementById('storage').innerText.includes('Storage: %d%% used')" % pct),
              "storage percentage updated")
        upload_via_ui(b, big, "flash", activate=True)
        check(b.wait("document.getElementById('msg').innerText.includes('is now the active floppy')", 90),
              "880 KiB image added and loaded")
        big_img = images()[-1]

        print("change the order")
        crc_before = {x["id"]: x.get("crc32") for x in images()}
        b.js(btn(big_img["id"], "↑") + ".click()")
        check(b.wait("S.images.length > 1 && S.images[S.images.length - 2].id == %d" % big_img["id"], 20),
              "Move up: one place up")
        check(images()[-2]["id"] == big_img["id"], "order changed on the device")
        check({x["id"]: x.get("crc32") for x in images()} == crc_before, "no image data changed")
        b.js(btn(big_img["id"], "↓") + ".click()")
        check(b.wait("S.images[S.images.length - 1].id == %d" % big_img["id"], 20), "Move down: back again")
        check(b.js("[...document.querySelectorAll('#images li')].slice(0, %d).map(li => "
                   "li.querySelector('.slotname').innerText).join('|')" % len(orig))
              == "|".join(x["name"] for x in orig), "original images keep their order")

        print("replace")
        b.dialog_answer = False
        b.events.clear()
        b.js("document.getElementById('msg').classList.add('hidden')")
        b.js(btn(new["id"], "Replace") + ".click(); replaceTarget = S.images.find(x => x.id == %d)" % new["id"])
        b.set_file(rl, "#replacefile")
        b.pump(2)
        dialogs = [e for e in b.events if e["method"] == "Page.javascriptDialogOpening"]
        check(dialogs and "CRYSTAL_CASTLES" in dialogs[0]["params"]["message"] and
              "RETROLOFT_TEST_720K" in dialogs[0]["params"]["message"],
              "confirmation names the old image and the new file")
        check(api("GET", "/api/v1/images/%d" % new["id"])[1]["name"] == "CRYSTAL_CASTLES",
              "cancelled: image unchanged")
        b.dialog_answer = True
        b.js("replaceTarget = S.images.find(x => x.id == %d)" % new["id"])
        b.set_file(rl, "#replacefile")
        check(b.wait("document.getElementById('msg').innerText.includes('stored (replaced)')", 60),
              "confirmed: replaced")
        r = api("GET", "/api/v1/images/%d" % new["id"])[1]
        check(r["name"] == "RETROLOFT_TEST_720K" and r["sequence"] == new["sequence"],
              "same id and position, new contents")

        print("delete")
        b.dialog_answer = False
        b.js(btn(new["id"], "Delete") + ".click()")
        b.pump(1)
        check(api("GET", "/api/v1/images/%d" % new["id"])[0] == 200, "cancelled: image kept")
        b.dialog_answer = True
        b.js(btn(new["id"], "Delete") + ".click()")
        check(b.wait("document.getElementById('msg').innerText.includes('deleted')", 20), "deleted")
        check(api("GET", "/api/v1/images/%d" % new["id"])[0] == 404, "gone on the device")
        check(b.wait("!S.images.some(x => x.id == %d)" % new["id"], 20), "gone from the list")

        print("error messages")
        upload_via_ui(b, bad, "flash")
        check(b.wait("document.getElementById('msg').innerText.includes('not a valid .ST')", 30),
              "invalid image: understandable message")
        upload_via_ui(b, tenspt, "flash")
        check(b.wait("document.getElementById('msg').innerText.includes('is not supported')", 30),
              "unsupported geometry: understandable message")
        check("probably a format that is not supported" in text(b, "#fileinfo"),
              "file size warning shown right after choosing the file")
        data = open(big, "rb").read()
        while api("GET", "/api/v1/storage")[1]["blocks_free"] >= 14:
            s, u = api_upload(data, "Fill.st", destination="flash")
            if s != 200:
                break
        upload_via_ui(b, big, "flash")
        check(b.wait("document.getElementById('msg').innerText.includes('Not enough free storage')", 30),
              "full flash: NO_SPACE explained")

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
        for x in images():
            if x["id"] not in orig_ids:
                api("DELETE", "/api/v1/images/%d" % x["id"])
        if start.get("image_id"):
            api("PUT", "/api/v1/current", {"image_id": start["image_id"]})
        print("  restored: " + json.dumps(api("GET", "/api/v1/current")[1]))
        check([x["id"] for x in images()] == orig_ids, "end state: the original images in their order")

    print("\n%s" % ("ALL UI TESTS PASSED" if not failures else "%d FAILED" % failures))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
