#!/usr/bin/env python3
"""
End-to-end tests for the RadioFloppy HTTP API against a real device.

    RADIOFLOPPY_HOST=192.168.x.y [RADIOFLOPPY_TOKEN=...] tests/api/api_test.py

Uses only the Python standard library. RADIOFLOPPY_TOKEN is only needed
when the device has a token configured. The image library must be usable
(state "valid") and have room for at least 30 blocks. Images that are
stored when the test starts are left alone (same ids, order and data): the
test adds its own images, deletes exactly those, and ends with the first
original image active (if there is one).
"""
import http.client
import json
import os
import socket
import sys
import time
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
    conn = http.client.HTTPConnection(HOST, 80, timeout=120)
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


def library():
    d = call("GET", "/api/v1/images")[1]
    return d["images"], d["storage"]


def synthetic(size, seed):
    """Deterministic test image (no BPB: accepted with a remark)."""
    out = bytearray()
    block = seed.to_bytes(4, "little")
    while len(out) < size:
        block = zlib.compress(block + len(out).to_bytes(4, "little"))[-64:]
        out += block
    return bytes(out[:size])


def crc(data):
    return "%08x" % (zlib.crc32(data) & 0xffffffff)


def main():
    if not HOST:
        sys.exit("set RADIOFLOPPY_HOST (and RADIOFLOPPY_TOKEN if the device has one)")
    cc = open(os.path.join(IMAGES, "CRYSTAL_CASTLES.ST"), "rb").read()
    rl = open(os.path.join(IMAGES, "RETROLOFT_TEST_720K.ST"), "rb").read()
    big = synthetic(901120, 1)          # 80/2/11: more than the old 800 KiB slots

    print("status / images / storage")
    s = call("GET", "/api/v1/status")
    check(s[0] == 200 and s[1]["storage"]["state"] == "valid", "GET /status: storage valid")
    orig, st0 = library()
    check(st0["block_size"] >= 65536 and st0["blocks_total"] <= 255, "geometry: %d KiB blocks, %d data blocks"
          % (st0["block_size"] // 1024, st0["blocks_total"]))
    check((st0["blocks_total"] + 1) * st0["block_size"] <= st0["capacity"], "blocks inside the detected flash")
    check(st0["max_image_size"] == 1572864, "max image size 1.5 MiB")
    check(call("GET", "/api/v1/storage")[1] == st0, "GET /storage = storage of /images")
    check(all(orig[i]["sequence"] <= orig[i + 1]["sequence"] for i in range(len(orig) - 1)),
          "images sorted by sequence")
    check(all("blocks" not in im for im in orig), "no block numbers exposed")
    orig_ids = [im["id"] for im in orig]

    auth = s[1].get("auth_required", False)
    if auth:
        print("authorisation (token configured)")
        r = call("POST", "/api/v1/uploads", {"filename": "x.st", "size": len(cc),
                                            "destination": "psram"}, token=False)
        check(r[0] == 401 and err(r) == "UNAUTHORIZED", "upload without token refused")
        r = call("DELETE", "/api/v1/images/1", token=False)
        check(r[0] == 401, "delete without token refused")
        r = call("PUT", "/api/v1/current", {"image_id": 1}, token=False)
        check(r[0] == 401, "activate without token refused")
    else:
        print("open API (no token configured)")
        r = call("POST", "/api/v1/uploads", {"filename": "x.st", "size": 1572865,
                                            "destination": "psram"}, token=False)
        check(r[0] == 413, "modifying call accepted without token (reaches validation)")
    r = call("POST", "/api/v1/uploads", raw=json.dumps({"filename": "x.st", "size": len(cc),
             "destination": "psram"}).encode(), headers={})
    check(r[0] == 415 and err(r) == "UNSUPPORTED_MEDIA_TYPE",
          "JSON sent as octet-stream: 415 (no cross-site requests)")
    check(call("GET", "/api/v1/slots")[0] in (404, 405), "old /slots path is gone")

    print("invalid requests")
    r = call("POST", "/api/v1/uploads", {"filename": "big.st", "size": 1572865, "destination": "flash"})
    check(r[0] == 413 and err(r) == "IMAGE_TOO_LARGE", "1 572 865 bytes (> 1.5 MiB): IMAGE_TOO_LARGE")
    r = call("POST", "/api/v1/uploads", {"filename": "hd.st", "size": 1474560, "destination": "flash"})
    check(r[0] == 422 and err(r) == "UNSUPPORTED_GEOMETRY", "HD 80/2/18: UNSUPPORTED_GEOMETRY")
    r = call("POST", "/api/v1/uploads", {"filename": "seventy.st", "size": 645120, "destination": "flash"})
    check(r[0] == 422 and err(r) == "UNSUPPORTED_GEOMETRY", "70/2/9 size: UNSUPPORTED_GEOMETRY")
    r = call("POST", "/api/v1/uploads", {"filename": "odd.st", "size": 1000, "destination": "flash"})
    check(r[0] == 422 and err(r) == "INVALID_IMAGE", "1000 bytes: INVALID_IMAGE")
    r = call("POST", "/api/v1/uploads", {"filename": "a.st", "size": len(cc),
                                        "destination": "flash", "replace": 65000})
    check(r[0] == 404 and err(r) == "IMAGE_NOT_FOUND", "replace unknown image: IMAGE_NOT_FOUND")
    r = call("POST", "/api/v1/uploads", {"filename": "a.st", "size": len(cc),
                                        "destination": "psram", "replace": 1})
    check(r[0] == 400, "replace with a PSRAM upload refused")
    r = call("POST", "/api/v1/uploads", {"filename": "a.st", "size": len(cc),
                                        "destination": "psram", "activate": False})
    check(r[0] == 400, "psram with activate:false refused")
    r = call("PUT", "/api/v1/current", {"image_id": 0})
    check(r[0] == 400 and err(r) == "INVALID_REQUEST", "activate image 0: INVALID_REQUEST")
    r = call("PUT", "/api/v1/current", {"image_id": 65000})
    check(r[0] == 404 and err(r) == "IMAGE_NOT_FOUND", "activate unknown image: IMAGE_NOT_FOUND")
    r = call("DELETE", "/api/v1/images/abc")
    check(r[0] == 404 and err(r) == "IMAGE_NOT_FOUND", "delete image abc: IMAGE_NOT_FOUND")
    r = call("PUT", "/api/v1/images/65000", {"sequence": 1})
    check(r[0] == 404, "move unknown image: 404")
    r = call("POST", "/api/v1/storage/format", {})
    check(r[0] == 400 and err(r) == "CONFIRMATION_REQUIRED", "format without confirmation refused")

    print("content checks")
    bad = bytearray(cc)
    bad[11:13] = (512).to_bytes(2, "little")
    bad[24:26] = (9).to_bytes(2, "little")
    bad[26:28] = (2).to_bytes(2, "little")      # BPB: 2 sides, file: 1 side
    bad[19:21] = (720).to_bytes(2, "little")
    c, d = upload(bytes(bad), "Wrong BPB.st", destination="psram")
    check(d and d[0] == 200 and current()["sides"] == 1,
          "BPB contradicts the size: accepted, geometry from the size (1 side)")
    c, d = upload(cc, "CRC.st", destination="flash", crc32="00000000")
    check(d and d[0] == 422 and d[1]["error"]["code"] == "CHECKSUM_MISMATCH",
          "wrong crc32: CHECKSUM_MISMATCH")
    check(library()[1]["blocks_used"] == st0["blocks_used"], "no blocks used by refused uploads")

    print("concurrent upload")
    c1 = call("POST", "/api/v1/uploads", {"filename": "one.st", "size": len(cc), "destination": "flash"})
    c2 = call("POST", "/api/v1/uploads", {"filename": "two.st", "size": len(cc), "destination": "flash"})
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
        time.sleep(1)
    check(st and st["state"] == "failed" and st["error"]["code"] == "UPLOAD_INCOMPLETE",
          "aborted after 100000 bytes: UPLOAD_INCOMPLETE")
    imgs, stg = library()
    check([im["id"] for im in imgs] == orig_ids and stg["blocks_used"] == st0["blocks_used"],
          "no image and no blocks added by the aborted upload")
    check(current() == before, "active disk unchanged by aborted upload")

    print("upload to PSRAM")
    c, d = upload(rl, "Retroloft test.st", destination="psram")
    check(c[0] == 201 and d and d[0] == 200 and d[1]["active"], "PSRAM upload done and active")
    cur = current()
    check(cur["source"] == "psram" and cur["name"] == "Retroloft test" and cur["sides"] == 2,
          "current: psram, Retroloft test, 2 sides")
    check(library()[1]["blocks_used"] == st0["blocks_used"], "PSRAM upload uses no flash blocks")

    mine = []
    bs = st0["block_size"]
    blocks = lambda n: (n + bs - 1) // bs

    print("add images")
    c, d = upload(cc, "Castles copy.st", destination="flash")
    check(d and d[0] == 200 and d[1].get("image_id") and not d[1]["active"], "added, not activated")
    a = d[1]["image_id"]
    mine.append(a)
    imgs, stg = library()
    check(imgs[-1]["id"] == a and imgs[-1]["sequence"] > max([im["sequence"] for im in orig] or [0]),
          "new image at the end of the order")
    check(stg["blocks_used"] == st0["blocks_used"] + blocks(len(cc)), "uses %d blocks" % blocks(len(cc)))
    check(current()["source"] == "psram", "active disk still the PSRAM image")
    c, d = upload(big, "Big 880K.st", destination="flash", activate=True)
    check(d and d[0] == 200 and d[1]["active"], "901 120-byte image (80/2/11) stored and activated")
    b = d[1]["image_id"]
    mine.append(b)
    cur = current()
    check(cur["image_id"] == b and cur["crc32"] == crc(big) and cur["sectors"] == 11, "current: the big image")

    print("sequence")
    r = call("PUT", "/api/v1/images/%d" % b, {"sequence": 1})
    check(r[0] == 200 and r[1]["sequence"] == 1, "moved to position 1")
    imgs, _ = library()
    check(imgs[0]["id"] == b and [im["sequence"] for im in imgs] == list(range(1, len(imgs) + 1)),
          "first in the list, sequences renumbered 1..n")
    check([im["id"] for im in imgs if im["id"] in orig_ids] == orig_ids, "original images keep their relative order")
    r = call("PUT", "/api/v1/current", {"image_id": b})
    check(r[0] == 200 and r[1]["crc32"] == crc(big), "data unchanged after the move (CRC)")
    call("PUT", "/api/v1/images/%d" % b, {"sequence": 9999})
    check(library()[0][-1]["id"] == b, "sequence 9999: moved to the end")

    print("replace")
    r = call("GET", "/api/v1/images/%d" % a)[1]
    seq_a = r["sequence"]
    c, d = upload(rl, "Replacement.st", destination="flash", replace=a)
    check(d and d[0] == 200 and d[1]["image_id"] == a, "image %d replaced (same id)" % a)
    r = call("GET", "/api/v1/images/%d" % a)[1]
    check(r["name"] == "Replacement" and r["size"] == len(rl) and r["crc32"] == crc(rl) and
          r["sequence"] == seq_a, "new name, size and CRC; same sequence")
    r = call("PUT", "/api/v1/current", {"image_id": a})
    check(r[0] == 200 and r[1]["crc32"] == crc(rl), "replacement activates with the right data")
    c, d = upload(cc, "Replace active.st", destination="flash", replace=a, activate=True)
    check(d and d[0] == 200 and d[1]["active"] and current()["crc32"] == crc(cc),
          "replacing the active image with activate: new data active")

    print("nearly full flash")
    need = blocks(len(big))
    fill = []
    stg = library()[1]
    while stg["blocks_free"] >= need:
        c, d = upload(big, "Fill %d.st" % len(fill), destination="flash")
        if not (d and d[0] == 200):
            check(False, "fill upload failed: %s" % (d or c)[1])
            break
        fill.append(d[1]["image_id"])
        stg = library()[1]
    mine += fill
    print("        filled with %d images, %d blocks free, %d%% used" % (len(fill), stg["blocks_free"], stg["used_percent"]))
    r = call("POST", "/api/v1/uploads", {"filename": "nospace.st", "size": len(big), "destination": "flash"})
    check(r[0] == 507 and err(r) == "NO_SPACE", "new %d-block image: NO_SPACE" % need)
    if fill:
        big2 = synthetic(len(big), 2)
        c, d = upload(big2, "Replaced when full.st", destination="flash", replace=fill[0])
        check(d and d[0] == 200 and d[1]["image_id"] == fill[0], "replacing works with the flash nearly full")
        r = call("PUT", "/api/v1/current", {"image_id": fill[0]})
        check(r[0] == 200 and r[1]["crc32"] == crc(big2), "replaced image reads back correctly")

    print("delete own images")
    first = orig_ids[0] if orig_ids else None
    if first:
        r = call("PUT", "/api/v1/current", {"image_id": first})
        check(r[0] == 200 and r[1]["image_id"] == first, "first original image activated")
    for i in mine:
        r = call("DELETE", "/api/v1/images/%d" % i)
        check(r[0] == 200, "image %d deleted" % i)
    r = call("DELETE", "/api/v1/images/%d" % mine[0])
    check(r[0] == 404 and err(r) == "IMAGE_NOT_FOUND", "deleting again: IMAGE_NOT_FOUND")
    r = call("PUT", "/api/v1/current", {"image_id": mine[0]})
    check(r[0] == 404 and err(r) == "IMAGE_NOT_FOUND", "activating a deleted image: IMAGE_NOT_FOUND")
    imgs, stg = library()
    check([im["id"] for im in imgs] == orig_ids and stg["blocks_used"] == st0["blocks_used"],
          "end state: the original images, all own blocks free again")

    print("\n%s" % ("ALL API TESTS PASSED" if not failures else "%d FAILED" % failures))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
