#!/usr/bin/env python3
"""
End-to-end tests for the RadioFloppy HTTP API against a real device.

    RADIOFLOPPY_HOST=192.168.x.y [RADIOFLOPPY_TOKEN=...] tests/api/api_test.py

Uses only the Python standard library. RADIOFLOPPY_TOKEN is only needed
when the device has a token configured. Needs an SD card in RadioFloppy
(the SD card part is skipped without one). Files on the card are left
alone: the test works in its own folder "RF API test" below the root
directory and removes it again. At the end the disk that was inserted
when the test started is inserted again (if it came from the SD card).
"""
import http.client
import json
import os
import socket
import sys
import time
import urllib.parse
import zlib

HOST = os.environ.get("RADIOFLOPPY_HOST")
TOKEN = os.environ.get("RADIOFLOPPY_TOKEN")
HERE = os.path.dirname(os.path.abspath(__file__))
IMAGES = os.path.join(HERE, "..", "..", "images")
T = "RF API test"

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


def q(**kw):
    return "?" + urllib.parse.urlencode(kw, quote_via=urllib.parse.quote)


def upload(data, name, **opts):
    """Two-step upload; returns (create response, data response)."""
    body = {"filename": name, "size": len(data)}
    body.update(opts)
    c = call("POST", "/api/v1/uploads", body)
    if c[0] != 201:
        return c, None
    return c, call("PUT", c[1]["upload_url"], raw=data)


def sd_upload(data, name, **opts):
    """Upload to the SD card; returns the last response."""
    body = {"filename": name, "size": len(data), "destination": "sd", "crc32": crc(data)}
    body.update(opts)
    c = call("POST", "/api/v1/uploads", body)
    return c if c[0] != 201 else call("PUT", c[1]["upload_url"], raw=data)


def current():
    return call("GET", "/api/v1/current")[1]


def crc(data):
    return "%08x" % (zlib.crc32(data) & 0xffffffff)


def sd_tests(cc):
    """The SD card: folders, uploads, inserting, new disks (in folder T, removed again)."""
    if call("GET", "/api/v1/sd/files" + q(path=T))[0] == 200:     # left over from an aborted run
        call("DELETE", "/api/v1/sd/folders" + q(path=T, recursive="true", confirm=T))
    root = call("GET", "/api/v1/sd/files")
    check(root[0] == 200, "list the root directory")
    root_hfe = [e["name"] for e in root[1].get("entries", []) if e["name"].lower().endswith(".hfe")]

    print("SD card: folders")
    r = call("POST", "/api/v1/sd/folders", {"path": T + "/L"})
    check(r[0] == 201, "create a nested folder")
    r = call("POST", "/api/v1/sd/folders", {"path": T + "/L"})
    check(r[0] == 409 and err(r) == "FOLDER_EXISTS", "again: FOLDER_EXISTS")
    for bad in ["../x", "/abs", T + "/../..", "a//b", ".hidden"]:
        r = call("POST", "/api/v1/sd/folders", {"path": bad})
        check(r[0] == 400 and err(r) == "INVALID_PATH", "folder %r refused" % bad)
    check(call("GET", "/api/v1/sd/files" + q(path="../"))[0] == 400, "list ../ refused")
    check(call("GET", "/api/v1/sd/files" + q(path="nope " + T))[0] == 404, "list a missing folder: 404")

    print("SD card: uploads")
    name = "Crystal Castles [test].st"
    r = sd_upload(cc, name, path=T + "/L")
    check(r[0] == 200 and r[1]["state"] == "done", "upload .ST into a subfolder")
    ents = call("GET", "/api/v1/sd/files" + q(path=T + "/L"))[1]["entries"]
    check([(e["name"], e.get("size"), e.get("read_only")) for e in ents] == [(name, len(cc), False)],
          "listed with its size, writable, no temporary file")
    r = call("POST", "/api/v1/uploads", {"filename": name, "size": len(cc), "destination": "sd", "path": T + "/L"})
    check(r[0] == 409 and err(r) == "FILE_EXISTS", "same name again: FILE_EXISTS")
    check(sd_upload(cc, name, path=T + "/L", overwrite=True)[0] == 200, "overwrite: true replaces it")
    check(sd_upload(b"x" * 1000, "tiny.st", path=T)[0] in (413, 422), "not an image: refused")
    check(sd_upload(cc, "wrong.hfe", path=T)[0] == 422, ".ST content named .hfe: refused")
    r = sd_upload(cc, "bad?name.st", path=T)
    check(r[0] == 422 and err(r) == "INVALID_NAME", "name with '?': refused")
    r = sd_upload(cc, "x.msa", path=T)
    check(r[0] == 422 and err(r) == "UNSUPPORTED_FORMAT", ".msa: refused")
    check(sd_upload(cc, "x.st", path=T, activate=True)[0] == 400, "activate with the upload: refused")
    c = call("POST", "/api/v1/uploads", {"filename": "crc.st", "size": len(cc), "destination": "sd",
                                         "path": T, "crc32": "00000000"})
    r = call("PUT", c[1]["upload_url"], raw=cc)
    check(r[0] == 422 and err(r) == "CHECKSUM_MISMATCH", "wrong CRC: refused")
    names = sorted(e["name"] for e in call("GET", "/api/v1/sd/files" + q(path=T))[1]["entries"])
    check(names == ["L"], "only the good uploads are there: %s" % names)

    print("SD card: inserting")
    path = T + "/L/" + name
    r = call("PUT", "/api/v1/current", {"sd_path": path})
    check(r[0] == 200 and r[1]["source"] == "sd" and r[1]["sd_path"] == path and
          r[1]["crc32"] == crc(cc) and not r[1]["image_changed_since"], "insert the .ST from the SD card")
    check(r[0] == 200 and r[1]["write"]["state"] == "writable", "writable for the Atari")

    print("SD card: read-only attribute")
    wp = lambda: current()["write"]
    r = call("PUT", "/api/v1/sd/files" + q(path=path), {"read_only": True})
    check(r[0] == 200 and r[1] == {"path": path, "read_only": True}, "make the inserted file read-only")
    check(wp()["state"] == "read_only" and wp()["reason"] == "the file is read-only",
          "the inserted disk is write-protected now")
    ents = call("GET", "/api/v1/sd/files" + q(path=T + "/L"))[1]["entries"]
    check(ents[0].get("read_only") is True, "listed as read-only")
    r = call("POST", "/api/v1/uploads", {"filename": name, "size": len(cc), "destination": "sd",
                                         "path": T + "/L", "overwrite": True})
    if r[0] == 201:
        r = call("PUT", r[1]["upload_url"], raw=cc)
    check(r[0] == 409 and err(r) == "FILE_READ_ONLY", "a read-only file is not overwritten by an upload")
    names = [e["name"] for e in call("GET", "/api/v1/sd/files" + q(path=T + "/L"))[1]["entries"]]
    check(names == [name], "nothing else left in the folder")
    check(call("PUT", "/api/v1/current", {"sd_path": path})[0] == 200 and wp()["state"] == "read_only",
          "inserted again: still write-protected")
    r = call("PUT", "/api/v1/sd/files" + q(path=path), {"read_only": False})
    check(r[0] == 200 and wp()["state"] == "writable", "writable again")
    check(call("PUT", "/api/v1/sd/files" + q(path=path), {"read_only": "yes"})[0] == 400,
          "read_only not a boolean: 400")
    check(call("PUT", "/api/v1/sd/files" + q(path=T + "/nope.st"), {"read_only": True})[0] == 404,
          "missing file: 404")
    check(call("PUT", "/api/v1/sd/files" + q(path="../x.st"), {"read_only": True})[0] == 400,
          "bad path: 400")
    check(call("PUT", "/api/v1/sd/files" + q(path=path), {"read_only": True}, token=False)[0] in (200, 401),
          "without token: only refused when a token is configured")
    call("PUT", "/api/v1/sd/files" + q(path=path), {"read_only": False})
    for bad, code in [("nope.st", 404), ("../x.st", 400), (T + "/L", 422)]:
        check(call("PUT", "/api/v1/current", {"sd_path": bad})[0] == code, "insert %r: %d" % (bad, code))
    check(call("PUT", "/api/v1/current", {})[0] == 400, "insert without sd_path: 400")
    check(call("PUT", "/api/v1/current", {"image_id": 1})[0] == 400, "image_id (old library): 400")
    if root_hfe:
        r = call("PUT", "/api/v1/current", {"sd_path": root_hfe[0]})
        check(r[0] == 200 and r[1]["source"] == "sd" and r[1]["sides"] in (1, 2) and r[1]["cylinders"] > 0,
              "insert .HFE from the SD card (%s)" % root_hfe[0])
        check(r[0] == 200 and r[1]["write"]["state"] == "read_only" and "HFE" in r[1]["write"]["reason"],
              "an HFE disk is write-protected")
    else:
        print("        (no .HFE file in the root directory: HFE insert skipped)")
    r = call("PUT", "/api/v1/current", {"sd_path": path})
    check(r[0] == 200, "insert the .ST again")
    check(call("DELETE", "/api/v1/sd/files" + q(path=path))[0] == 200, "delete the inserted file")
    check(current()["image_changed_since"], "deleting the active file: flagged as changed")
    check(wp()["state"] == "read_only" and "replaced or deleted" in wp()["reason"],
          "and write-protected")
    check(call("DELETE", "/api/v1/sd/files" + q(path=path))[0] == 404, "delete it again: 404")

    print("SD card: new disk")
    r = call("POST", "/api/v1/sd/disks", {"path": T, "name": "Save games"})
    check(r[0] == 201 and r[1]["path"] == T + "/Save games.st" and r[1]["size"] == 737280,
          "new disk Save games.st (720 KiB)")
    r = call("POST", "/api/v1/sd/disks", {"path": T, "name": "Save games.st"})
    check(r[0] == 409 and err(r) == "FILE_EXISTS", "same name again: FILE_EXISTS")
    check(call("POST", "/api/v1/sd/disks", {"path": T, "name": "a/b"})[0] == 422, "name with '/': refused")
    check(call("POST", "/api/v1/sd/disks", {"path": "nope " + T, "name": "x"})[0] == 404, "missing folder: 404")
    r = call("PUT", "/api/v1/current", {"sd_path": T + "/Save games.st"})
    check(r[0] == 200 and r[1]["sides"] == 2 and r[1]["sectors"] == 9 and r[1]["cylinders"] == 80,
          "the new disk can be inserted (80/2/9)")

    print("SD card: deleting folders")
    r = call("DELETE", "/api/v1/sd/folders" + q(path=T))
    check(r[0] == 409 and err(r) == "FOLDER_NOT_EMPTY", "folder with contents: FOLDER_NOT_EMPTY")
    r = call("DELETE", "/api/v1/sd/folders" + q(path=T, recursive="true"))
    check(r[0] == 400 and err(r) == "CONFIRMATION_REQUIRED", "with contents, no confirm: refused")
    check(call("DELETE", "/api/v1/sd/folders" + q(path=T, recursive="true", confirm="x"))[0] == 400,
          "wrong confirm: refused")
    check(call("DELETE", "/api/v1/sd/folders" + q(path="", recursive="true", confirm=""))[0] == 400,
          "the root directory itself: refused")
    check(call("DELETE", "/api/v1/sd/folders" + q(path=T, recursive="true", confirm=T))[0] == 200,
          "with contents and confirm: deleted")
    check(T not in [e["name"] for e in call("GET", "/api/v1/sd/files")[1]["entries"]], "test folder gone")
    check(current()["image_changed_since"], "the inserted disk's folder deleted: flagged as changed")


def main():
    if not HOST:
        sys.exit("set RADIOFLOPPY_HOST (and RADIOFLOPPY_TOKEN if the device has one)")
    cc = open(os.path.join(IMAGES, "CRYSTAL_CASTLES.ST"), "rb").read()
    rl = open(os.path.join(IMAGES, "RETROLOFT_TEST_720K.ST"), "rb").read()

    print("status")
    s = call("GET", "/api/v1/status")
    check(s[0] == 200 and "sd" in s[1] and "storage" not in s[1], "GET /status: sd block, no flash storage")
    have_sd = s[1].get("sd", {}).get("present", False)
    start = current()
    for old in ["/api/v1/images", "/api/v1/storage", "/api/v1/images/1"]:
        check(call("GET", old)[0] in (404, 405), "old library path %s is gone" % old)

    auth = s[1].get("auth_required", False)
    if auth:
        print("authorisation (token configured)")
        r = call("POST", "/api/v1/uploads", {"filename": "x.st", "size": len(cc),
                                            "destination": "psram"}, token=False)
        check(r[0] == 401 and err(r) == "UNAUTHORIZED", "upload without token refused")
        r = call("POST", "/api/v1/sd/folders", {"path": "x"}, token=False)
        check(r[0] == 401, "creating a folder without token refused")
        r = call("PUT", "/api/v1/current", {"sd_path": "x.st"}, token=False)
        check(r[0] == 401, "inserting without token refused")
    else:
        print("open API (no token configured)")
        r = call("POST", "/api/v1/uploads", {"filename": "x.st", "size": 1572865,
                                            "destination": "psram"}, token=False)
        check(r[0] == 413, "modifying call accepted without token (reaches validation)")
    r = call("POST", "/api/v1/uploads", raw=json.dumps({"filename": "x.st", "size": len(cc),
             "destination": "psram"}).encode(), headers={})
    check(r[0] == 415 and err(r) == "UNSUPPORTED_MEDIA_TYPE",
          "JSON sent as octet-stream: 415 (no cross-site requests)")

    print("invalid requests")
    r = call("POST", "/api/v1/uploads", {"filename": "big.st", "size": 1572865, "destination": "psram"})
    check(r[0] == 413 and err(r) == "IMAGE_TOO_LARGE", "1 572 865 bytes (> 1.5 MiB): IMAGE_TOO_LARGE")
    r = call("POST", "/api/v1/uploads", {"filename": "hd.st", "size": 1474560, "destination": "psram"})
    check(r[0] == 422 and err(r) == "UNSUPPORTED_GEOMETRY", "HD 80/2/18: UNSUPPORTED_GEOMETRY")
    r = call("POST", "/api/v1/uploads", {"filename": "seventy.st", "size": 645120, "destination": "psram"})
    check(r[0] == 422 and err(r) == "UNSUPPORTED_GEOMETRY", "70/2/9 size: UNSUPPORTED_GEOMETRY")
    r = call("POST", "/api/v1/uploads", {"filename": "odd.st", "size": 1000, "destination": "psram"})
    check(r[0] == 422 and err(r) == "INVALID_IMAGE", "1000 bytes: INVALID_IMAGE")
    r = call("POST", "/api/v1/uploads", {"filename": "a.st", "size": len(cc), "destination": "flash"})
    check(r[0] == 400 and err(r) == "INVALID_REQUEST", "destination flash (old library): refused")
    r = call("POST", "/api/v1/uploads", {"filename": "a.st", "size": len(cc),
                                        "destination": "psram", "activate": False})
    check(r[0] == 400, "psram with activate:false refused")
    r = call("POST", "/api/v1/uploads", {"filename": "a.hfe", "size": 2000000, "destination": "psram"})
    check(r[0] == 422 and err(r) == "UNSUPPORTED_FORMAT", "HFE to PSRAM: UNSUPPORTED_FORMAT")

    print("content checks")
    bad = bytearray(cc)
    bad[11:13] = (512).to_bytes(2, "little")
    bad[24:26] = (9).to_bytes(2, "little")
    bad[26:28] = (2).to_bytes(2, "little")      # BPB: 2 sides, file: 1 side
    bad[19:21] = (720).to_bytes(2, "little")
    c, d = upload(bytes(bad), "Wrong BPB.st", destination="psram")
    check(d and d[0] == 200 and current()["sides"] == 1,
          "BPB contradicts the size: accepted, geometry from the size (1 side)")
    c, d = upload(cc, "CRC.st", destination="psram", crc32="00000000")
    check(d and d[0] == 422 and d[1]["error"]["code"] == "CHECKSUM_MISMATCH",
          "wrong crc32: CHECKSUM_MISMATCH")

    print("concurrent upload")
    c1 = call("POST", "/api/v1/uploads", {"filename": "one.st", "size": len(cc), "destination": "psram"})
    c2 = call("POST", "/api/v1/uploads", {"filename": "two.st", "size": len(cc), "destination": "psram"})
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
    check(current() == before, "active disk unchanged by the aborted upload")

    print("upload to PSRAM")
    c, d = upload(rl, "Retroloft test.st", destination="psram")
    check(c[0] == 201 and d and d[0] == 200 and d[1]["active"], "PSRAM upload done and active")
    cur = current()
    check(cur["source"] == "psram" and cur["name"] == "Retroloft test" and cur["sides"] == 2,
          "current: psram, Retroloft test, 2 sides")
    check(cur["write"]["state"] == "read_only", "a temporary disk is write-protected")

    if have_sd:
        sd_tests(cc)
    else:
        print("SD card\n        (no SD card: skipped)")

    if start.get("source") == "sd" and start.get("sd_path"):
        r = call("PUT", "/api/v1/current", {"sd_path": start["sd_path"]})
        check(r[0] == 200, "the disk of the start inserted again (%s)" % start["sd_path"])

    print("\n%s" % ("ALL API TESTS PASSED" if not failures else "%d FAILED" % failures))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
