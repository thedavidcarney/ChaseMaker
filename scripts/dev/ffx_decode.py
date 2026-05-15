"""Decode AE .ffx (RIFX) animation preset binaries.

Parses keyframe records out of the RIFX chunk tree and pretty-prints
them. Confirmed against the "NWE Gamma Light Hit" preset set.

Record layout (48 bytes per keyframe, big-endian throughout):
  off  0 u32   time in preset-local ticks (rate varies per preset)
  off  4 u32   interpolation flags
                 byte 0 = in-interp, byte 1 = out-interp
                 0x01=linear, 0x02=bezier, 0x03=hold
  off  8 f64   value
  off 16 f64   in-tangent length (?)
  off 24 f64   in-tangent influence (?)
  off 32 f64   out-tangent length (?)
  off 40 f64   out-tangent influence (?)

Tangent floats are non-zero only on bezier keyframes; we don't try to
re-derive AE's exact bezier shape, since the plan is to replace these
presets with sliders rather than re-emit .ffx.

lhd3 layout (52 bytes), big-endian:
  off  0 u32   constant ~0x00d00bee
  off  4 u32   0
  off  8 u32   keyframe count
  off 12 u32   1
  off 16 u32   record size in bytes (observed: 48)

Time-rate quirk: the four "Light Hit" presets were authored across
two AE versions. The 2019 set (15, 30) uses 1024 ticks per frame; the
2020 set (60, 90) uses 800 ticks per frame. We detect the rate by
matching the largest observed keyframe time against the frame-count
hint in the filename, and report normalized + absolute frames.

Run:
    python scripts/dev/ffx_decode.py "third_party/NWE Light Hits/*.ffx"
"""

from __future__ import annotations

import glob
import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator


@dataclass
class Keyframe:
    ticks: int
    value: float
    interp_in: str
    interp_out: str

    def fmt(self, ticks_per_frame: float, total_frames: float) -> str:
        frame = self.ticks / ticks_per_frame
        frac = (self.ticks / ticks_per_frame) / total_frames if total_frames else 0.0
        return (
            f"frame={frame:6.2f} / {total_frames:.0f}  "
            f"({frac * 100:5.1f}%)  "
            f"value={self.value:8.4f}  "
            f"in={self.interp_in:<6}  out={self.interp_out}"
        )


@dataclass
class Property:
    name: str
    keyframes: list[Keyframe]


def chunks(data: bytes, start: int = 0, end: int | None = None) -> Iterator[tuple[str, int, bytes]]:
    if end is None:
        end = len(data)
    pos = start
    while pos + 8 <= end:
        chunk_id = data[pos:pos + 4].decode("ascii", errors="replace")
        size = struct.unpack(">I", data[pos + 4:pos + 8])[0]
        payload = data[pos + 8:pos + 8 + size]
        yield chunk_id, pos, payload
        pos += 8 + size
        if size & 1:
            pos += 1


def walk(data: bytes, start: int = 0, end: int | None = None):
    """Flat walk, descending LIST chunks. Yields (cid, list_type_or_None, payload)."""
    for cid, _off, payload in chunks(data, start, end):
        if cid == "LIST":
            list_type = payload[:4].decode("ascii", errors="replace")
            yield cid, list_type, payload
            yield from walk(payload, 4, len(payload))
        else:
            yield cid, None, payload


INTERP_NAMES = {0x01: "linear", 0x02: "bezier", 0x03: "hold"}


def parse_keyframe(rec: bytes) -> Keyframe:
    time_ticks = struct.unpack(">I", rec[0:4])[0]
    flags = rec[4:8]
    interp_in = INTERP_NAMES.get(flags[0], f"0x{flags[0]:02x}")
    interp_out = INTERP_NAMES.get(flags[1], f"0x{flags[1]:02x}")
    value = struct.unpack(">d", rec[8:16])[0]
    return Keyframe(
        ticks=time_ticks,
        value=value,
        interp_in=interp_in,
        interp_out=interp_out,
    )


def parse_lhd3(payload: bytes) -> tuple[int, int]:
    """Return (keyframe_count, record_size_bytes)."""
    count = struct.unpack(">I", payload[8:12])[0]
    rec_size = struct.unpack(">I", payload[16:20])[0]
    return count, rec_size


def utf8_chunk(payload: bytes) -> str:
    """tdsn payload is 'Utf8\\0..\\0<text>\\0...'. Strip the tag, then trim."""
    if payload.startswith(b"Utf8"):
        payload = payload[4:]
    return payload.lstrip(b"\x00").rstrip(b"\x00").decode("utf-8", errors="replace").strip()


def find_properties(data: bytes) -> list[Property]:
    """Walk chunks, collect properties as (tdsn name) followed eventually by lhd3 + ldat."""
    props: list[Property] = []
    pending_name: str | None = None
    pending_count = 0
    pending_rec_size = 0
    in_tdbs = False
    for cid, list_type, payload in walk(data[12:]):  # skip RIFX header + FaFX
        if cid == "LIST" and list_type == "tdbs":
            in_tdbs = True
            pending_name = None
        elif in_tdbs:
            if cid == "tdsn":
                pending_name = utf8_chunk(payload)
            elif cid == "lhd3":
                pending_count, pending_rec_size = parse_lhd3(payload)
            elif cid == "ldat" and pending_name is not None:
                kfs = []
                for i in range(pending_count):
                    rec = payload[i * pending_rec_size:(i + 1) * pending_rec_size]
                    if len(rec) >= 16:
                        kfs.append(parse_keyframe(rec))
                props.append(Property(name=pending_name, keyframes=kfs))
                pending_name = None
                in_tdbs = False
    return props


def detect_ticks_per_frame(props: list[Property], path: Path) -> tuple[float, float]:
    """Return (ticks_per_frame, total_frames).

    Strategy: extract a frame-count hint from the filename (e.g.
    "...15.ffx" -> 15). Take the max keyframe time across properties.
    Assume that's the preset's last keyframe at the hinted frame.
    Falls back to 1024 ticks/frame and the raw max if no hint found.
    """
    max_ticks = 0
    for p in props:
        for kf in p.keyframes:
            max_ticks = max(max_ticks, kf.ticks)
    m = re.search(r"(\d+)(?=\.ffx$)", path.name)
    if m:
        hint = int(m.group(1))
        if max_ticks > 0:
            return max_ticks / hint, float(hint)
    return 1024.0, max_ticks / 1024.0


def decode_file(path: Path):
    data = path.read_bytes()
    print(f"\n{'=' * 72}")
    print(f"FILE: {path.name}")
    print(f"{'=' * 72}")
    if data[:4] != b"RIFX":
        print(f"  not a RIFX file (magic={data[:4]!r})")
        return
    props = find_properties(data)
    tpf, total = detect_ticks_per_frame(props, path)
    print(f"  detected ticks/frame = {tpf:.1f}   preset duration = {total:.0f} frames")
    for p in props:
        print(f"\n  Property: {p.name!r}  ({len(p.keyframes)} keyframes)")
        for kf in p.keyframes:
            print(f"    {kf.fmt(tpf, total)}")


def main(argv):
    paths = []
    for arg in argv:
        matches = glob.glob(arg)
        paths.extend(matches if matches else [arg])
    if not paths:
        print("usage: ffx_decode.py <file_or_glob> ...")
        return 1
    for p in sorted(paths):
        decode_file(Path(p))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
