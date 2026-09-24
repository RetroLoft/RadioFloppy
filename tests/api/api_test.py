#!/usr/bin/env python3
"""
End-to-end tests for the RadioFloppy HTTP API against a real device.

    RADIOFLOPPY_HOST=192.168.x.y [RADIOFLOPPY_TOKEN=...] tests/api/api_test.py

Uses only the Python standard library. RADIOFLOPPY_TOKEN is only needed
when the device has a token configured. Slot 1 must hold Crystal Castles.
Images that are stored when the test starts are left alone: the test only
uses the free slots, frees exactly the slots it filled, and ends with slot
1 active.
"""
import http.client
import json
import os
import socket
import sys
import zlib

HOST = os.environ.get("RADIOFLOPPY_HOST")
TOKEN = os.environ.get("RADIOFLOPPY_TOKEN")
HERE = os.path.dirname(os.path.abspath(__file__))
IMAGES = os.path.join(HERE, "..", "..", "images")

failures = 0


def check(cond, what):
    global failures
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        failures += 1


def call(method, path, body=None, token=True, raw=None, headers=None):
    conn = http.client.HTTPConnection(HOST, 80, timeout=60)
    hdr = dict(headers or {})
    if token and TOKEN:
        hdr["Authorization"] = "Bearer " + TOKEN
    data = None
    if body is not None:
        data = json.dumps(body).encode()
        hdr["Content-Type"] = "application/json"
    if raw is not None:
        data = raw
        hdr["Content-Type"] = "application/octet-stream"
    conn.request(method, path, body=data, headers=hdr)
    resp = conn.getresponse()
    text = resp.read()
    conn.close()
    try:
        return resp.status, json.loads(text)
    except ValueError:
        return resp.status, text


def err(r):
    return r[1].get("error", {}).get("code") if isinstance(r[1], dict) else None


def upload(data, name, **opts):
    """Two-step upload; returns (create response, data response)."""
    body = {"filename": name, "size": len(data)}
    body.update(opts)
    c = call("POST", "/api/v1/uploads", body)
    if c[0] != 201:
        return c, None
    return c, call("PUT", c[1]["upload_url"], raw=data)


def current():
    return call("GET", "/api/v1/current")[1]


def slots():
    return call("GET", "/api/v1/slots")[1]["slots"]


def main():
    if not HOST:
        sys.exit("set RADIOFLOPPY_HOST (and RADIOFLOPPY_TOKEN if the device has one)")
    cc = open(os.path.join(IMAGES, "CRYSTAL_CASTLES.ST"), "rb").read()
    rl = open(os.path.join(IMAGES, "RETROLOFT_TEST_720K.ST"), "rb").read()

    print("status / slots / current")
    s = call("GET", "/api/v1/status")
    check(s[0] == 200 and s[1]["external_flash"]["slot_store"] == "valid", "GET /status")
    sl = slots()
    check(len(sl) == 20 and sl[0]["status"] == "valid", "GET /slots: 20 slots, slot 1 valid")
    check(sl[0].get("crc32") == "%08x" % zlib.crc32(cc), "slot 1 holds Crystal Castles")
    keep0 = [x["slot"] for x in sl if x["status"] == "valid"]

    auth = s[1].get("auth_required", False)
    if auth:
        print("authorisation (token configured)")
        r = call("POST", "/api/v1/uploads", {"filename": "x.st", "size": len(cc),
                                            "destination": "psram"}, token=False)
        check(r[0] == 401 and err(r) == "UNAUTHORIZED", "upload without token refused")
        r = call("DELETE", "/api/v1/slots/1", token=False)
        check(r[0] == 401, "delete without token refused")
        r = call("PUT", "/api/v1/current", {"slot": 1}, token=False)
        check(r[0] == 401, "activate without token refused")
    else:
        print("open API (no token configured)")
        r = call("POST", "/api/v1/uploads", {"filename": "x.st", "size": 999999,
                                            "destination": "psram"}, token=False)
        check(r[0] == 413, "modifying call accepted without token (reaches validation)")
    r = call("POST", "/api/v1/uploads", raw=json.dumps({"filename": "x.st", "size": len(cc),
             "destination": "psram"}).encode(), headers={})
    check(r[0] == 415 and err(r) == "UNSUPPORTED_MEDIA_TYPE",
          "JSON sent as octet-stream: 415 (no cross-site requests)")
    check(call("GET", "/api/v1/slots/")[0] in (404, 405), "unknown path not served")

    print("invalid requests")
    r = call("POST", "/api/v1/uploads", {"filename": "big.st", "size": 819201,
                                        "destination": "flash"})
    check(r[0] == 413 and err(r) == "IMAGE_TOO_LARGE", "819201 bytes: IMAGE_TOO_LARGE")
    r = call("POST", "/api/v1/uploads", {"filename": "ten.st", "size": 409600,
                                        "destination": "flash"})
    check(r[0] == 422 and err(r) == "UNSUPPORTED_GEOMETRY", "80/1/10 size: UNSUPPORTED_GEOMETRY")
    r = call("POST", "/api/v1/uploads", {"filename": "odd.st", "size": 1000,
                                        "destination": "flash"})
    check(r[0] == 422 and err(r) == "INVALID_IMAGE", "1000 bytes: INVALID_IMAGE")
    r = call("POST", "/api/v1/uploads", {"filename": "a.st", "size": len(cc),
                                        "destination": "flash", "slot": 21})
    check(r[0] == 400 and err(r) == "INVALID_SLOT", "slot 21: INVALID_SLOT")
    r = call("POST", "/api/v1/uploads", {"filename": "a.st", "size": len(cc),
                                        "destination": "psram", "activate": False})
    check(r[0] == 400, "psram with activate:false refused")
    r = call("PUT", "/api/v1/current", {"slot": 0})
    check(r[0] == 400 and err(r) == "INVALID_SLOT", "activate slot 0: INVALID_SLOT")
    r = call("DELETE", "/api/v1/slots/abc")
    check(r[0] == 400 and err(r) == "INVALID_SLOT", "delete slot abc: INVALID_SLOT")

    print("content checks")
    bad = bytearray(cc)
    bad[11:13] = (512).to_bytes(2, "little")
    bad[24:26] = (9).to_bytes(2, "little")
    bad[26:28] = (2).to_bytes(2, "little")      # BPB: 2 sides, file: 1 side
    bad[19:21] = (720).to_bytes(2, "little")
    c, d = upload(bytes(bad), "Wrong BPB.st", destination="flash")
    check(d and d[0] == 422 and d[1]["error"]["code"] == "UNSUPPORTED_GEOMETRY",
          "BPB contradicts size: UNSUPPORTED_GEOMETRY, nothing stored")
    c, d = upload(cc, "CRC.st", destination="flash", crc32="00000000")
    check(d and d[0] == 422 and d[1]["error"]["code"] == "CHECKSUM_MISMATCH",
          "wrong crc32: CHECKSUM_MISMATCH")
    check([x["slot"] for x in slots() if x["status"] == "valid"] == keep0, "no slot filled by refused uploads")

    print("concurrent upload")
    c1 = call("POST", "/api/v1/uploads", {"filename": "one.st", "size": len(cc),
                                         "destination": "flash"})
    c2 = call("POST", "/api/v1/uploads", {"filename": "two.st", "size": len(cc),
                                         "destination": "flash"})
    check(c1[0] == 201 and c2[0] == 409 and err(c2) == "UPLOAD_BUSY", "second upload: UPLOAD_BUSY")

    print("interrupted upload")
    before = current()
    sock = socket.create_connection((HOST, 80), timeout=30)
    sock.sendall(("PUT %s HTTP/1.1\r\nHost: %s\r\nAuthorization: Bearer %s\r\n"
                  "Content-Type: application/octet-stream\r\nContent-Length: %d\r\n\r\n"
                  % (c1[1]["upload_url"], HOST, TOKEN or "", len(cc))).encode())
    sock.sendall(cc[:100000])
    sock.close()
    st = None
    for _ in range(30):
        st = call("GET", "/api/v1/uploads/" + c1[1]["upload_id"])[1]
        if st["state"] in ("failed", "done"):
            break
        import time
        time.sleep(1)
    check(st and st["state"] == "failed" and st["error"]["code"] == "UPLOAD_INCOMPLETE",
          "aborted after 100000 bytes: UPLOAD_INCOMPLETE")
    check([x["slot"] for x in slots() if x["status"] == "valid"] == keep0, "no slot filled by aborted upload")
    check(current() == before, "active disk unchanged by aborted upload")

    print("upload to PSRAM")
    c, d = upload(rl, "Retroloft test.st", destination="psram")
    check(c[0] == 201 and d and d[0] == 200 and d[1]["active"], "PSRAM upload done and active")
    cur = current()
    check(cur["source"] == "psram" and cur["name"] == "Retroloft test" and cur["sides"] == 2,
          "current: psram, Retroloft test, 2 sides")

    keep = [x["slot"] for x in slots() if x["status"] == "valid"]    # not ours
    free = [n for n in range(1, 21) if n not in keep]
    mine = []

    print("upload to first free slot")
    c, d = upload(cc, "Castles copy.st", destination="flash")
    check(d and d[0] == 200 and d[1].get("stored_in_slot") == free[0] and not d[1]["active"],
          "stored in slot %d (first free), not activated" % free[0])
    mine.append(free[0])
    check(current()["source"] == "psram", "active disk still the PSRAM image")

    print("explicit slot overwrite + activate")
    t = free[-1]
    c, d = upload(rl, "Slot test.st", destination="flash", slot=t)
    check(d and d[0] == 200 and d[1].get("stored_in_slot") == t, "slot %d written" % t)
    c, d = upload(cc, "Slot test again.st", destination="flash", slot=t, activate=True)
    check(d and d[0] == 200 and d[1].get("stored_in_slot") == t and d[1]["active"],
          "slot %d overwritten and activated" % t)
    mine.append(t)
    st = slots()[t - 1]
    check(st["name"] == "Slot test again" and st["size"] == len(cc) and st["active"],
          "slot %d shows the new image, active" % t)

    print("fill all slots")
    for n in free:
        if slots()[n - 1]["status"] != "valid":
            c, d = upload(cc, "Fill %d.st" % n, destination="flash")
            check(d and d[0] == 200 and d[1].get("stored_in_slot") == n, "slot %d filled" % n)
            mine.append(n)
    r = call("POST", "/api/v1/uploads", {"filename": "full.st", "size": len(cc),
                                        "destination": "flash"})
    check(r[0] == 409 and err(r) == "NO_FREE_SLOT", "all 20 in use: NO_FREE_SLOT")

    print("activate existing slot, delete own slots")
    r = call("PUT", "/api/v1/current", {"slot": 1})
    check(r[0] == 200 and r[1]["source"] == "flash" and r[1]["slot"] == 1, "slot 1 activated")
    for n in sorted(set(mine)):
        r = call("DELETE", "/api/v1/slots/%d" % n)
        check(r[0] == 200, "slot %d freed" % n)
    r = call("DELETE", "/api/v1/slots/%d" % free[0])
    check(r[0] == 404 and err(r) == "SLOT_EMPTY", "freeing a free slot: SLOT_EMPTY")
    r = call("PUT", "/api/v1/current", {"slot": free[0]})
    check(r[0] == 404 and err(r) == "SLOT_EMPTY", "activating a free slot: SLOT_EMPTY")
    sl = slots()
    check(sl[0]["active"] and [x["slot"] for x in sl if x["status"] == "valid"] == keep,
          "end state: the original images, slot 1 active")

    print("\n%s" % ("ALL API TESTS PASSED" if not failures else "%d FAILED" % failures))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
