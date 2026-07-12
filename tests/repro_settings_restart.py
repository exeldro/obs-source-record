#!/usr/bin/env python3
"""Regression test: changing the recording path/format while a Source Record
filter is actively recording must apply immediately by restarting the output
(splitting the file), instead of being silently ignored until the filter is
disabled/re-enabled (forum report: settings "revert unless disabled/re-enabled").

Flow: record into dir A, confirm growth, then set the filter's "path" to dir B.
Expect: a new file appears in B and grows, and the file in A finalizes (moov).

Exit: 0 pass, 1 fail (change not applied), 2 setup abort. Env: OBS_WS_PASSWORD.
"""
import glob
import logging
import os
import struct
import sys
import time

import obsws_python as obs

logging.getLogger("obsws_python").setLevel(logging.CRITICAL)
HOST = os.environ.get("OBS_WS_HOST", "localhost")
PORT = int(os.environ.get("OBS_WS_PORT", "4455"))
PW = os.environ.get("OBS_WS_PASSWORD", "")
VENDOR = "source-record"
SUF = str(os.getpid())
SCENE, SRC, FILT = f"SR_RESTART_SC_{SUF}", f"SR_RESTART_SRC_{SUF}", "SR_RESTART_F"


def vendor(cl, rt, data=None):
    r = cl.send("CallVendorRequest", {"vendorName": VENDOR, "requestType": rt, "requestData": data or {}}, raw=True)
    return r.get("responseData", {})


def newest(d):
    fs = glob.glob(os.path.join(d, "*"))
    return max(fs, key=os.path.getmtime) if fs else None


def grow(d, secs, step=1.5):
    out, prev = [], os.path.getsize(newest(d)) if newest(d) else 0
    end = time.time() + secs
    while time.time() < end:
        time.sleep(step)
        cur = os.path.getsize(newest(d)) if newest(d) else 0
        out.append(cur - prev)
        prev = cur
    return out


def has_moov(p):
    try:
        off, end = 0, os.path.getsize(p)
        with open(p, "rb") as f:
            while off + 8 <= end:
                f.seek(off)
                h = f.read(16)
                sz = struct.unpack(">I", h[:4])[0]
                nm = h[4:8].decode("latin1")
                if sz == 1:
                    sz = struct.unpack(">Q", h[8:16])[0]
                elif sz == 0:
                    sz = end - off
                if nm == "moov":
                    return True
                if sz < 8:
                    break
                off += sz
    except Exception:
        pass
    return False


def main():
    if not PW:
        print("OBS_WS_PASSWORD not set", file=sys.stderr)
        return 2
    base = os.environ.get("TMPDIR", "/tmp")
    dirA = os.path.join(base, f"sr_restart_A_{SUF}")
    dirB = os.path.join(base, f"sr_restart_B_{SUF}")
    os.makedirs(dirA, exist_ok=True)
    os.makedirs(dirB, exist_ok=True)
    cl = obs.ReqClient(host=HOST, port=PORT, password=PW, timeout=5)
    print("Connected:", cl.get_version().obs_version, flush=True)
    for fn in (lambda: cl.remove_input(SRC), lambda: cl.remove_scene(SCENE)):
        try:
            fn()
        except Exception:
            pass
    try:
        cl.create_scene(SCENE)
        kinds = cl.get_input_kind_list(False).input_kinds
        ck = next((k for k in kinds if k.startswith("color_source")), "color_source_v3")
        cl.create_input(SCENE, SRC, ck, {"width": 640, "height": 360}, True)
        cl.create_source_filter(SRC, FILT, "source_record_filter",
                                {"path": dirA, "record_mode": 0, "stream_mode": 0})
        time.sleep(1.0)
        vendor(cl, "record_start", {"source": SRC})
        time.sleep(2.0)
        a1 = newest(dirA)
        gA = grow(dirA, 6)
        print(f"A growth KB={[d // 1024 for d in gA]} file={os.path.basename(a1) if a1 else None}", flush=True)
        if not a1 or sum(gA) <= 0:
            print("RESULT: not recording into A at baseline — abort", flush=True)
            return 2

        # --- change the path to B while recording ---
        print("--- set path -> B while recording ---", flush=True)
        cl.set_source_filter_settings(SRC, FILT, {"path": dirB}, overlay=True)
        time.sleep(5.0)

        b = newest(dirB)
        gB = grow(dirB, 6)
        print(f"B growth KB={[d // 1024 for d in gB]} file={os.path.basename(b) if b else None}", flush=True)
        a_final = has_moov(a1) if a1 else False
        print(f"A finalized (moov)={a_final}", flush=True)

        ok = bool(b) and sum(gB) > 0
        print(f"VERDICT={'APPLIED' if ok else 'IGNORED'} (new file in B growing={ok})", flush=True)
        vendor(cl, "record_stop", {"source": SRC})
        time.sleep(2)
        return 0 if ok else 1
    finally:
        for fn in (lambda: cl.remove_input(SRC), lambda: cl.remove_scene(SCENE)):
            try:
                fn()
            except Exception:
                pass
        import shutil
        shutil.rmtree(dirA, ignore_errors=True)
        shutil.rmtree(dirB, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
