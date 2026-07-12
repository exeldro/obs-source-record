#!/usr/bin/env python3
"""Smoke tests for the Source Record obs-websocket vendor API.

Validates the Phase 2/3 fixes that touch websocket paths, by driving the
`source-record` vendor requests and inspecting filter state — no manual clicks.

Connection via env: OBS_WS_HOST (default localhost), OBS_WS_PORT (4455),
OBS_WS_PASSWORD. Creates a throwaway scene + color source and removes it at the
end, so it does not touch your real scene collection.

Run:  OBS_WS_PASSWORD=... python tests/smoke_websocket.py
"""
import logging
import os
import sys
import time

import obsws_python as obs

logging.getLogger("obsws_python").setLevel(logging.CRITICAL)  # quiet caught request errors

HOST = os.environ.get("OBS_WS_HOST", "localhost")
PORT = int(os.environ.get("OBS_WS_PORT", "4455"))
PASSWORD = os.environ.get("OBS_WS_PASSWORD", "")

# Unique per run: the plugin (B14, not yet fixed) retains the parent source via
# its private view after recording, so RemoveInput leaves a ghost. Unique names
# avoid the collision until B14 lands.
_SUFFIX = str(os.getpid())
SCENE = "SR_SMOKE_SCENE_" + _SUFFIX
SRC = "SR_SMOKE_SRC_" + _SUFFIX
VENDOR = "source-record"

results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}" + (f" — {detail}" if detail else ""))


def vendor(cl, request_type, data=None):
    """Call a source-record vendor request, return its nested responseData dict."""
    resp = cl.send(
        "CallVendorRequest",
        {"vendorName": VENDOR, "requestType": request_type, "requestData": data or {}},
        raw=True,
    )
    # send(raw=True) already returns the CallVendorRequest responseData
    # ({vendorName, requestType, responseData}); the vendor payload is one level in.
    return resp.get("responseData", {})


def sr_filter(cl, source):
    """Return (name, settings, enabled) of the source_record filter on `source`, or (None,{},None)."""
    lst = cl.get_source_filter_list(source).filters
    for f in lst:
        if f.get("filterKind") == "source_record_filter":
            name = f["filterName"]
            gf = cl.get_source_filter(source, name)
            return name, gf.filter_settings, gf.filter_enabled
    return None, {}, None


def _cleanup(cl):
    for fn in (lambda: cl.remove_input(SRC), lambda: cl.remove_scene(SCENE)):
        try:
            fn()
        except Exception:
            pass


def setup(cl):
    # fresh scene + color source (idempotent: removing a scene doesn't remove the
    # orphan input, so clear the input explicitly)
    _cleanup(cl)
    cl.create_scene(SCENE)
    kinds = cl.get_input_kind_list(False).input_kinds
    color_kind = next((k for k in kinds if k.startswith("color_source")), "color_source_v3")
    cl.create_input(SCENE, SRC, color_kind, {"width": 320, "height": 240}, True)
    # record into a temp dir so we never clutter the real recordings folder
    tmp = os.path.join(os.environ.get("TMPDIR", "/tmp"), "sr_smoke_rec")
    os.makedirs(tmp, exist_ok=True)
    # pre-create the SR filter with a temp path so any test recording lands there
    cl.create_source_filter(SRC, "SR_SMOKE_FILTER", "source_record_filter",
                            {"path": tmp, "record_mode": 0, "stream_mode": 0})
    return tmp


def teardown(cl):
    _cleanup(cl)


def main():
    if not PASSWORD:
        print("OBS_WS_PASSWORD not set", file=sys.stderr)
        return 2
    cl = obs.ReqClient(host=HOST, port=PORT, password=PASSWORD, timeout=5)
    v = cl.get_version()
    print(f"Connected: OBS {v.obs_version}, ws {v.obs_web_socket_version}\n")

    tmp = setup(cl)
    try:
        # --- N7: replay_buffer_start writes replay_filename_formatting, not filename_formatting
        vendor(cl, "replay_buffer_start", {"source": SRC, "filename": "replay_%CCYY-%MM-%DD"})
        time.sleep(0.3)
        name, st, en = sr_filter(cl, SRC)
        check("N7 replay filename -> replay_filename_formatting",
              st.get("replay_filename_formatting") == "replay_%CCYY-%MM-%DD",
              f"replay_filename_formatting={st.get('replay_filename_formatting')!r}")
        check("N7 record filename NOT clobbered",
              st.get("filename_formatting") != "replay_%CCYY-%MM-%DD",
              f"filename_formatting={st.get('filename_formatting')!r}")
        vendor(cl, "replay_buffer_stop", {"source": SRC})
        time.sleep(0.2)

        # --- B18e: record_pause with no active recording -> success:false + error string
        vd = vendor(cl, "record_pause", {"source": SRC})
        check("B18e record_pause returns success:false", vd.get("success") is False,
              f"success={vd.get('success')!r}")
        check("B18e record_pause sets an error message", bool(vd.get("error")),
              f"error={vd.get('error')!r}")

        # --- N2: record_stop must not re-enable a disabled filter
        vendor(cl, "record_start", {"source": SRC})
        time.sleep(0.3)
        name, st, en = sr_filter(cl, SRC)
        if name:
            cl.set_source_filter_enabled(SRC, name, False)
            time.sleep(0.2)
            vendor(cl, "record_stop", {"source": SRC})
            time.sleep(0.3)
            _, _, en2 = sr_filter(cl, SRC)
            check("N2 disabled filter stays disabled after record_stop", en2 is False,
                  f"enabled_after_stop={en2!r}")
        else:
            check("N2 disabled filter stays disabled after record_stop", False, "no filter created")

        # --- N3/B4: fan-out with no 'source' exercises find_source_by_filter refs.
        # WARNING: this stops EVERY active Source Record recording, so it is
        # opt-in (SR_SMOKE_FANOUT=1) to avoid disrupting a real recording.
        if os.environ.get("SR_SMOKE_FANOUT") == "1":
            vendor(cl, "record_stop", {})
            alive = cl.get_version().obs_version
            check("N3/B4 fan-out record_stop (no source) keeps OBS alive", bool(alive),
                  f"obs still responding: {alive}")
        else:
            print("  [SKIP] N3/B4 fan-out (set SR_SMOKE_FANOUT=1; would stop all SR recordings)")
        # named-source path still exercised find_filter's in-callback ref above.

    finally:
        teardown(cl)

    print()
    passed = sum(1 for _, ok, _ in results if ok)
    print(f"== {passed}/{len(results)} checks passed ==")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
