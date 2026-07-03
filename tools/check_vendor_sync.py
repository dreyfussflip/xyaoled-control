#!/usr/bin/env python3
"""Verify the Home Assistant integration's vendored protocol builders stay
byte-identical to the xyaoled library. Run from the repo root:

    python tools/check_vendor_sync.py

Exits 0 if in sync, 1 otherwise. The same checks run as a pytest suite via
tests/test_vendor_sync.py; this standalone form stays handy as a quick
pre-commit command. The frame logic lives in two places because
custom_components must be self-contained (HACS installs only that directory).
"""
import importlib.util
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from xyaoled import core, gif, text  # noqa: E402


def _load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _err(fn, *args):
    try:
        fn(*args)
        return "no error"
    except ValueError as e:
        return str(e)


def build_checks() -> list[tuple[str, object, object]]:
    """(name, library value, vendored value) triples; a check passes if equal."""
    ven = _load(
        os.path.join(ROOT, "custom_components", "xyaoled", "protocol.py"),
        "vendored_protocol",
    )

    # A sample 16x64 grid and some pseudo-GIF blobs of different sizes.
    grid = [[(r + c) % 2 for c in range(core.W)] for r in range(core.H)]
    blobs = {n: bytes((i * 13) & 0xFF for i in range(n)) for n in (300, 9976, 15000, 30000)}

    # Duplicated wire-protocol constants.
    checks: list[tuple[str, object, object]] = [
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
        # core matches handshake notifies by prefix; the vendored parse_status
        # must accept a frame with that same prefix (and reject anything else).
        ("NOTIFY_HANDSHAKE", True,
         ven.parse_status(core.NOTIFY_HANDSHAKE + bytes(10)) is not None
         and ven.parse_status(b"\x88\xff\x01\x05" + bytes(10)) is None),
    ]

    # Frame builders must produce byte-identical output.
    checks += [
        ("frame", core.frame(0x1234, 7, b"\x01\x02\x03"), ven.frame(0x1234, 7, b"\x01\x02\x03")),
        ("clear_cmd", core.clear_cmd(), ven.clear_cmd()),
        ("power_on", core.power_cmd(True), ven.power_cmd(True)),
        ("power_off", core.power_cmd(False), ven.power_cmd(False)),
        ("pack_bitmap", text.pack_bitmap(grid), ven.pack_bitmap(grid)),
    ]
    for lvl in (0, 1, 50, 100, 250):
        checks.append((f"brightness({lvl})", core.brightness_cmd(lvl), ven.brightness_cmd(lvl)))
    for scroll in (False, True):
        checks.append((f"mono(scroll={scroll})",
                       text.build_mono([grid, grid], (10, 20, 30), 2, dur=0x22, scroll=scroll),
                       ven.mono_cmds([grid, grid], (10, 20, 30), 2, dur=0x22, scroll=scroll)))
    for n, blob in blobs.items():
        checks.append((f"gif_static({n})", gif.build_cmds(blob, 1, 2), ven.gif_cmds(blob, 1, 2)))
        checks.append((f"gif_anim({n})", gif.build_cmds(blob, 8, 2), ven.gif_cmds(blob, 8, 2)))

    # Validation behaviour (limits, messages) must match as well.
    checks += [
        ("mono_max_frames",
         text.build_mono([grid] * text.MAX_MONO_FRAMES, (1, 2, 3), 2)[1][:64],
         ven.mono_cmds([grid] * ven.MAX_MONO_FRAMES, (1, 2, 3), 2)[1][:64]),
        ("mono_overflow",
         _err(text.build_mono, [grid] * (text.MAX_MONO_FRAMES + 1), (1, 2, 3), 2),
         _err(ven.mono_cmds, [grid] * (ven.MAX_MONO_FRAMES + 1), (1, 2, 3), 2)),
        ("mono_zero_frames",
         _err(text.build_mono, [], (1, 2, 3), 2),
         _err(ven.mono_cmds, [], (1, 2, 3), 2)),
        ("mono_bad_rgb",
         _err(text.build_mono, [grid], (255, 0), 2),
         _err(ven.mono_cmds, [grid], (255, 0), 2)),
        ("gif_overflow",
         _err(gif.build_cmds, bytes(70000), 8, 2),
         _err(ven.gif_cmds, bytes(70000), 8, 2)),
    ]
    return checks


def main() -> None:
    checks = build_checks()
    bad = [name for name, a, b in checks if a != b]
    for name, a, b in checks:
        print(f"  {'OK ' if a == b else 'DIFF'}  {name}")
    if bad:
        print(f"\nOUT OF SYNC: {', '.join(bad)}")
        sys.exit(1)
    print(f"\nAll {len(checks)} constants and frame builders in sync.")


if __name__ == "__main__":
    main()
