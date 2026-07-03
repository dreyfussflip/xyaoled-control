#!/usr/bin/env python3
"""Verify the Home Assistant integration's vendored protocol builders stay
byte-identical to the xyaoled library. Run from the repo root:

    python tools/check_vendor_sync.py

Exits 0 if in sync, 1 otherwise. Handy as a pre-commit / CI check because the
frame logic lives in two places (custom_components must be self-contained).
"""
import importlib.util
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from xyaoled import core, gif, text  # noqa: E402


def _load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


ven = _load(
    os.path.join("custom_components", "xyaoled", "protocol.py"), "vendored_protocol"
)

# A sample 16x64 grid and some pseudo-GIF blobs of different sizes.
GRID = [[(r + c) % 2 for c in range(core.W)] for r in range(core.H)]
BLOBS = {n: bytes((i * 13) & 0xFF for i in range(n)) for n in (300, 9976, 15000, 30000)}

# Duplicated wire-protocol constants: (name, library value, vendored value).
CONSTANTS = [
    ("W", core.W, ven.W),
    ("H", core.H, ven.H),
    ("NAME_PREFIX", core.NAME_PREFIX, ven.NAME_PREFIX),
    ("SERVICE_UUID", True, ven.SERVICE_UUID.startswith(core.SERVICE_PREFIX)),
    ("WRITE_CHAR_UUID", True, ven.WRITE_CHAR_UUID.startswith(core.WRITE_CHAR_PREFIX)),
    ("NOTIFY_CHAR_UUID", True, ven.NOTIFY_CHAR_UUID.startswith(core.NOTIFY_CHAR_PREFIX)),
    ("MAGIC", core.MAGIC, ven.MAGIC),
    ("DEFAULT_INIT_HEX", core.DEFAULT_INIT_HEX, ven.DEFAULT_INIT_HEX),
    ("IMAGE_TYPE", core.IMAGE_TYPE, ven.IMAGE_TYPE),
    ("NOTIFY_IMAGE_DONE", core.NOTIFY_IMAGE_DONE, ven.NOTIFY_IMAGE_DONE),
    ("PL_HEAD", text.PL_HEAD, ven.PL_HEAD),
    ("PL_MID", text.PL_MID, ven.PL_MID),
    ("COLOR_CMD_PARAMS", text.COLOR_CMD_PARAMS, ven.COLOR_CMD_PARAMS),
    ("MAX_MONO_FRAMES", text.MAX_MONO_FRAMES, ven.MAX_MONO_FRAMES),
    ("STATIC_HDR_TAIL", gif.STATIC_HDR_TAIL, ven.STATIC_HDR_TAIL),
    ("MAX_0207_GIF", gif.MAX_0207_GIF, ven.MAX_0207_GIF),
    ("MAX_0209_GIF", gif.MAX_0209_GIF, ven.MAX_0209_GIF),
    ("MAX_GIF_BYTES", gif.MAX_GIF_BYTES, ven.MAX_GIF_BYTES),
    # core matches handshake notifies by prefix; the vendored parse_status must
    # accept a frame with that same prefix (and reject anything else).
    ("NOTIFY_HANDSHAKE", True,
     ven.parse_status(core.NOTIFY_HANDSHAKE + bytes(10)) is not None
     and ven.parse_status(b"\x88\xff\x01\x05" + bytes(10)) is None),
]

checks: list[tuple[str, bytes | list, bytes | list]] = [
    ("frame", core.frame(0x1234, 7, b"\x01\x02\x03"), ven.frame(0x1234, 7, b"\x01\x02\x03")),
    ("clear_cmd", core.clear_cmd(), ven.clear_cmd()),
    ("power_on", core.power_cmd(True), ven.power_cmd(True)),
    ("power_off", core.power_cmd(False), ven.power_cmd(False)),
    ("pack_bitmap", text.pack_bitmap(GRID), ven.pack_bitmap(GRID)),
]
for lvl in (0, 1, 50, 100, 250):
    checks.append((f"brightness({lvl})", core.brightness_cmd(lvl), ven.brightness_cmd(lvl)))
for scroll in (False, True):
    checks.append((f"mono(scroll={scroll})",
                   text.build_mono([GRID, GRID], (10, 20, 30), 2, dur=0x22, scroll=scroll),
                   ven.mono_cmds([GRID, GRID], (10, 20, 30), 2, dur=0x22, scroll=scroll)))
for n, blob in BLOBS.items():
    checks.append((f"gif_static({n})", gif.build_cmds(blob, 1, 2), ven.gif_cmds(blob, 1, 2)))
    checks.append((f"gif_anim({n})", gif.build_cmds(blob, 8, 2), ven.gif_cmds(blob, 8, 2)))


def _err(fn, *args):
    try:
        fn(*args)
        return "no error"
    except ValueError as e:
        return str(e)


checks.append(("mono_max_frames",
               text.build_mono([GRID] * text.MAX_MONO_FRAMES, (1, 2, 3), 2)[1][:64],
               ven.mono_cmds([GRID] * ven.MAX_MONO_FRAMES, (1, 2, 3), 2)[1][:64]))
checks.append(("mono_overflow",
               _err(text.build_mono, [GRID] * (text.MAX_MONO_FRAMES + 1), (1, 2, 3), 2),
               _err(ven.mono_cmds, [GRID] * (ven.MAX_MONO_FRAMES + 1), (1, 2, 3), 2)))
checks.append(("mono_zero_frames",
               _err(text.build_mono, [], (1, 2, 3), 2),
               _err(ven.mono_cmds, [], (1, 2, 3), 2)))
checks.append(("mono_bad_rgb",
               _err(text.build_mono, [GRID], (255, 0), 2),
               _err(ven.mono_cmds, [GRID], (255, 0), 2)))
checks.append(("gif_overflow",
               _err(gif.build_cmds, bytes(70000), 8, 2),
               _err(ven.gif_cmds, bytes(70000), 8, 2)))

checks = CONSTANTS + checks
bad = [name for name, a, b in checks if a != b]
for name, a, b in checks:
    print(f"  {'OK ' if a == b else 'DIFF'}  {name}")
if bad:
    print(f"\nOUT OF SYNC: {', '.join(bad)}")
    sys.exit(1)
print(f"\nAll {len(checks)} constants and frame builders in sync.")
