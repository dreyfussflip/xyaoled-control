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

bad = [name for name, a, b in checks if a != b]
for name, a, b in checks:
    print(f"  {'OK ' if a == b else 'DIFF'}  {name}")
if bad:
    print(f"\nOUT OF SYNC: {', '.join(bad)}")
    sys.exit(1)
print(f"\nAll {len(checks)} frame builders in sync.")
