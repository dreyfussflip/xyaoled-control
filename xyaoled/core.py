"""
Core protocol + BLE transport for XYAO-LED style 64x16 BLE pixel matrices.

Reverse-engineered, unofficial. No affiliation with the vendor. For use with
hardware you own. See PROTOCOL.md for the wire format.
"""
from __future__ import annotations

import asyncio
import os

from bleak import BleakClient, BleakScanner

# Panel geometry
W, H = 64, 16

# BLE identifiers (16-bit shorts inside the standard base UUID)
NAME_PREFIX = "XyaoLED"          # advertised device-name prefix used for auto-discovery
SERVICE_PREFIX = "0000ae00"      # control service
WRITE_CHAR_PREFIX = "0000ae01"   # write commands here (write-without-response)
NOTIFY_CHAR_PREFIX = "0000ae02"  # status/ack notifications

# Every command frame starts with this constant header (magic + protocol constant).
MAGIC = bytes.fromhex("99aa002eff88")

# Known-good handshake (command type 0x0000). The device validates a token that is
# bound to the timestamp embedded in this frame, so an exact, previously-captured
# frame is required. The bundled sample works for the reference unit; if your device
# rejects it (you never get a "88 ff 00 05 ..." notify), capture your own init frame
# from the official app and provide it via the XYAO_INIT_HEX environment variable.
DEFAULT_INIT_HEX = "99aa002eff881f00000001001a06030b1a2f030100a1a9df647ff700000000"


class XyaoError(Exception):
    """Device/transport error. The CLI mains map this to a clean exit message."""


def validate_init(data: bytes) -> None:
    """Structurally validate a captured init frame; raises ValueError.

    A handshake frame is MAGIC + total_len(u16) + type 0x0000 + seq + token,
    so we can catch copy-paste accidents before ever talking to the device.
    """
    if len(data) < 11:
        raise ValueError(f"init frame is {len(data)} bytes, expected at least 11")
    if not data.startswith(MAGIC):
        raise ValueError(f"init frame must start with the magic header {MAGIC.hex()}")
    total = int.from_bytes(data[6:8], "little")
    if total != len(data):
        raise ValueError(f"embedded length {total} does not match frame length {len(data)}")
    if int.from_bytes(data[8:10], "little") != 0x0000:
        raise ValueError("not a handshake frame (command type must be 0x0000)")


def get_init() -> bytes:
    hex_str = os.environ.get("XYAO_INIT_HEX", DEFAULT_INIT_HEX)
    try:
        data = bytes.fromhex(hex_str)
        validate_init(data)
    except ValueError as e:
        raise XyaoError(f"Invalid XYAO_INIT_HEX: {e}") from e
    return data


def parse_color(s: str) -> tuple[int, int, int]:
    """Parse 'R,G,B' (0..255 each); raises ValueError for argparse to report."""
    try:
        rgb = tuple(int(p) for p in s.split(","))
    except ValueError:
        rgb = ()
    if len(rgb) != 3 or not all(0 <= v <= 255 for v in rgb):
        raise ValueError(f"expected R,G,B with values 0..255, got {s!r}")
    return rgb


def frame(type_le: int, seq: int, params: bytes) -> bytes:
    """Build a command frame: MAGIC + total_len(u16) + type(u16) + seq + params."""
    rest = type_le.to_bytes(2, "little") + bytes([seq & 0xFF]) + bytes(params)
    total = len(MAGIC) + 2 + len(rest)
    return MAGIC + total.to_bytes(2, "little") + rest


def clear_cmd(seq: int = 1) -> bytes:
    """Clear screen + playlist (command type 0x0005)."""
    return frame(0x0005, seq, bytes([0x01, 0x01, 0x00, 0x00, 0x00, 0x00]))


def power_cmd(on: bool, seq: int = 1) -> bytes:
    """Panel power (command type 0x0011)."""
    return frame(0x0011, seq, bytes([0x01 if on else 0x02, 0x01, 0x01]))


def brightness_cmd(level: int, seq: int = 1) -> bytes:
    """Brightness 0..100 (command type 0x0012), params `01 [level]`."""
    return frame(0x0012, seq, bytes([0x01, max(0, min(100, int(level)))]))


def checksum(data: bytes) -> int:
    """Device notification frames end with sum(of preceding bytes) mod 256."""
    return sum(data) & 0xFF


async def resolve_address(address: str | None = None) -> str:
    """Address from arg -> XYAO_ADDRESS env -> BLE scan by name prefix."""
    address = address or os.environ.get("XYAO_ADDRESS")
    if address:
        return address
    print(f"Scanning for {NAME_PREFIX}* ...")
    devices = await BleakScanner.discover(timeout=8.0)
    for d in devices:
        if d.name and d.name.startswith(NAME_PREFIX):
            print(f"Found {d.name} @ {d.address}")
            return d.address
    raise XyaoError(
        f"No '{NAME_PREFIX}*' device found. Make sure it is on and not connected to "
        f"the phone app, or set XYAO_ADDRESS / pass --address."
    )


# Notification prefixes the device sends (see PROTOCOL.md).
NOTIFY_HANDSHAKE = b"\x88\xff\x00\x05"   # status frame after a valid init
NOTIFY_IMAGE_DONE = b"\x88\xff\x05\x01"  # image/animation transfer finished

# Command type that carries image data and therefore triggers a completion notify.
IMAGE_TYPE = 0x0207


def _frame_type(cmd: bytes) -> int:
    return int.from_bytes(cmd[8:10], "little") if len(cmd) >= 10 else -1


async def _wait(event: asyncio.Event, timeout: float) -> None:
    """Wait for an event, but never hang: fall back to the timeout."""
    try:
        await asyncio.wait_for(event.wait(), timeout)
    except asyncio.TimeoutError:
        pass


async def send(cmds, clear_first=False, address=None, do_init=True, on_notify=None):
    """Connect, run the handshake, optionally clear, then write the given command frames.

    Large frames are split into <=512-byte ATT writes; the device reassembles them by
    the length field in each frame's header. Rather than sleeping a fixed time, we wait
    for the device's own notifications (handshake status, image-complete) with a
    generous timeout fallback so a quiet device never hangs the call.
    """
    addr = await resolve_address(address)
    async with BleakClient(addr, timeout=20.0) as c:
        write_char = notify_char = None
        for s in c.services:
            if s.uuid.lower().startswith(SERVICE_PREFIX):
                for ch in s.characteristics:
                    if ch.uuid.lower().startswith(WRITE_CHAR_PREFIX):
                        write_char = ch
                    if ch.uuid.lower().startswith(NOTIFY_CHAR_PREFIX):
                        notify_char = ch
        if write_char is None:
            raise XyaoError("Control characteristic (ae01) not found on this device.")

        handshaked = asyncio.Event()
        image_done = asyncio.Event()

        def _notify(char, data):
            b = bytes(data)
            if b.startswith(NOTIFY_HANDSHAKE):
                handshaked.set()
            elif b.startswith(NOTIFY_IMAGE_DONE):
                image_done.set()
            if on_notify:
                on_notify(char, data)

        have_notify = notify_char is not None
        if have_notify:
            await c.start_notify(notify_char, _notify)
            await asyncio.sleep(0.3)

        chunk = min(512, (getattr(c, "mtu_size", 515) or 515) - 3)

        if do_init:
            await c.write_gatt_char(write_char, get_init(), response=False)
            if have_notify:
                await _wait(handshaked, 3.0)
            else:
                await asyncio.sleep(1.0)
        if clear_first:
            await c.write_gatt_char(write_char, clear_cmd(), response=False)
            await asyncio.sleep(0.5)  # clear sends no completion notify

        expect_done = have_notify and any(_frame_type(cmd) == IMAGE_TYPE for cmd in cmds)
        image_done.clear()
        for cmd in cmds:
            for i in range(0, len(cmd), chunk):
                await c.write_gatt_char(write_char, cmd[i:i + chunk], response=False)
                await asyncio.sleep(0.02)  # pace writes so the BLE buffer keeps up
            await asyncio.sleep(0.15)      # small gap between frames
        if expect_done:
            await _wait(image_done, 8.0)   # image transfer sends a completion notify
        else:
            await asyncio.sleep(0.3)       # power/brightness/clear settle
