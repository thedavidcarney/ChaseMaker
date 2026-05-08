#!/usr/bin/env python3
"""
Compute per-layer luminance centroid for a multilayer EXR and print the
layers sorted left-to-right by x-centroid. A standalone diagnostic to
evaluate whether luminance-weighted centroid is a good proxy for
"where this light sits in the frame" before plumbing the math into the
C++ plugin.

Handles both single-part multilayer EXRs (Blender pre-5.x) and multipart
EXRs (Blender 5.x), via OpenEXR's modern File API (>= 3.x).

Usage:
    python luminance_centroid.py <path/to/file.exr>

Skips:
  - Cryptomatte layers (hash-encoded, not luminance)
  - Image / Alpha (beauty pass spans the whole frame, biased everywhere)
  - Layers without a full R+G+B set
  - All-black layers (no light contribution this frame)

Dependencies:
    pip install "OpenEXR>=3" numpy
"""
import argparse
import sys
from collections import OrderedDict

try:
    import OpenEXR
except ImportError:
    sys.exit('Missing dependency: pip install "OpenEXR>=3"')

if not hasattr(OpenEXR, "File"):
    sys.exit("OpenEXR>=3 required (need OpenEXR.File for multipart support).")

try:
    import numpy as np
except ImportError:
    sys.exit("Missing dependency: pip install numpy")

# Rec.709 luma weights — same convention as cv2.cvtColor(..., COLOR_RGB2GRAY).
REC709 = (0.2126, 0.7152, 0.0722)


def collect_layers(path):
    """Walk every part of the EXR and return {display_name: (R, G, B)} as
    float32 2D numpy arrays. Mirrors the plugin's display-name logic
    (last-dot grouping + 'X.X' -> 'X' de-dup).
    """
    f = OpenEXR.File(path)

    # First pass: bucket channels by layer key.
    # by_layer[key] = {'R': arr2d, 'G': arr2d, 'B': arr2d, ...}
    by_layer = OrderedDict()

    def stash(key, sub, arr):
        if key not in by_layer:
            by_layer[key] = {}
        by_layer[key][sub] = arr

    for part in f.parts:
        part_name = part.header.get("name", None)
        for ch_name, ch in part.channels.items():
            px = ch.pixels
            if px.ndim == 3 and px.shape[2] >= 3:
                # API auto-bundled an <name>.R/.G/.B[/.A] set.
                # ch_name is the bare layer name in this case; for multipart
                # files the part name is the same.
                layer_key = ch_name if "." in ch_name else (part_name or ch_name)
                stash(layer_key, "R", px[:, :, 0])
                stash(layer_key, "G", px[:, :, 1])
                stash(layer_key, "B", px[:, :, 2])
                if px.shape[2] >= 4:
                    stash(layer_key, "A", px[:, :, 3])
            elif px.ndim == 2:
                dot = ch_name.rfind(".")
                if dot >= 0:
                    layer_key = ch_name[:dot]
                    sub = ch_name[dot + 1 :].upper()  # normalize r/g/b -> R/G/B
                else:
                    layer_key = part_name or ch_name
                    sub = "Y"  # bare scalar — likely a Z/depth-style pass
                stash(layer_key, sub, px)

    # Second pass: 'X.X' -> 'X' de-dup.
    result = OrderedDict()
    for key, subs in by_layer.items():
        disp = key
        if "." in key:
            left, right = key.rsplit(".", 1)
            if left == right:
                disp = left
        result[disp] = subs
    return result


def centroid(r, g, b):
    """Luminance-weighted (cx, cy, total) in normalized [0,1] coords.
    Returns None if the layer has no positive luminance.
    """
    r = r.astype(np.float32, copy=False)
    g = g.astype(np.float32, copy=False)
    b = b.astype(np.float32, copy=False)
    L = REC709[0] * r + REC709[1] * g + REC709[2] * b
    np.maximum(L, 0.0, out=L)        # negatives (filter ringing) -> 0
    total = float(L.sum())
    if total <= 0.0:
        return None
    h, w = L.shape
    xs = np.arange(w, dtype=np.float64)
    ys = np.arange(h, dtype=np.float64)
    cx = float((L.sum(axis=0) * xs).sum() / total)
    cy = float((L.sum(axis=1) * ys).sum() / total)
    return cx / max(w - 1, 1), cy / max(h - 1, 1), total


def bar(cx, width=40):
    """ASCII strip showing centroid_x along [0,1]."""
    pos = max(0, min(width - 1, int(round(cx * (width - 1)))))
    return "[" + "-" * pos + "|" + "-" * (width - 1 - pos) + "]"


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("path", help="path to a multilayer EXR")
    args = ap.parse_args()

    try:
        layers = collect_layers(args.path)
    except Exception as e:
        sys.exit(f"Could not read EXR: {args.path}\n  {e}")

    rows = []
    skipped = []
    for disp, subs in layers.items():
        low = disp.lower()
        if "crypto" in low:
            skipped.append((disp, "cryptomatte"))
            continue
        if low in ("image", "alpha"):
            skipped.append((disp, "beauty/alpha"))
            continue
        if not all(k in subs for k in ("R", "G", "B")):
            skipped.append((disp, f"not RGB (subs: {sorted(subs.keys())})"))
            continue
        c = centroid(subs["R"], subs["G"], subs["B"])
        if c is None:
            skipped.append((disp, "all black"))
            continue
        rows.append((disp, c[0], c[1], c[2]))

    rows.sort(key=lambda r: r[1])

    print(f"\n{len(rows)} layers ranked left -> right by luminance centroid:\n")
    header_line = (
        f"  {'#':>3}  {'cx':>5}  {'cy':>5}  {'total L':>10}  "
        f"{'position 0.0 ............... 1.0':<44}  layer"
    )
    print(header_line)
    print("  " + "-" * (len(header_line) - 2))
    for i, (n, cx, cy, t) in enumerate(rows, 1):
        print(f"  {i:>3}  {cx:>5.3f}  {cy:>5.3f}  {t:>10.4g}  {bar(cx):<44}  {n}")

    if skipped:
        print(f"\nSkipped {len(skipped)}:")
        for n, why in skipped:
            print(f"  - {n}  ({why})")


if __name__ == "__main__":
    main()
