#!/usr/bin/env python3
"""Checks that a rendered frame is a picture, not a blank one.

CI can run a renderer and get a green exit code while the picture is black: a pipeline that never draws, a camera
inside the mesh, a pass that clears the wrong target. This reads a PNG with nothing but the standard library (zlib and
struct) and fails when the frame has too little variety, which is what an empty or uniform image looks like.

    python3 tools/frame_check.py renders/player-frame.png
"""
import struct
import sys
import zlib


def read_png(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{path}: not a PNG")
    pos = 8
    width = height = bit_depth = color_type = 0
    idat = b""
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, bit_depth, color_type = struct.unpack(">IIBB", body[:10])
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    if bit_depth != 8 or color_type not in (2, 6):
        raise SystemExit(f"{path}: expected 8-bit RGB or RGBA, got depth {bit_depth} type {color_type}")
    channels = 3 if color_type == 2 else 4
    raw = zlib.decompress(idat)
    stride = width * channels
    # Undo the per-row filters. The engine writes with stb_image_write, which uses the standard five filters.
    out = bytearray(width * height * channels)
    prev = bytearray(stride)
    pos = 0
    for y in range(height):
        ftype = raw[pos]
        pos += 1
        row = bytearray(raw[pos:pos + stride])
        pos += stride
        if ftype == 1:      # Sub
            for i in range(channels, stride):
                row[i] = (row[i] + row[i - channels]) & 0xFF
        elif ftype == 2:    # Up
            for i in range(stride):
                row[i] = (row[i] + prev[i]) & 0xFF
        elif ftype == 3:    # Average
            for i in range(stride):
                a = row[i - channels] if i >= channels else 0
                row[i] = (row[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:    # Paeth
            for i in range(stride):
                a = row[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                row[i] = (row[i] + pred) & 0xFF
        out[y * stride:(y + 1) * stride] = row
        prev = row
    return width, height, channels, out


def main(argv):
    if len(argv) < 2:
        raise SystemExit("usage: frame_check.py <frame.png> [min_colours] [min_non_background]")
    path = argv[1]
    min_colours = int(argv[2]) if len(argv) > 2 else 64
    width, height, channels, pixels = read_png(path)

    # The colour buckets: 5 bits a channel, which is coarse enough to ignore dithering and fine enough to tell a flat
    # fill from a picture.
    buckets = set()
    for i in range(0, len(pixels), channels):
        buckets.add((pixels[i] >> 3, pixels[i + 1] >> 3, pixels[i + 2] >> 3))

    # The share of pixels that are not the most common colour. A frame that is one colour end to end is a failed render
    # even when it has a few antialiased edges.
    counts = {}
    for i in range(0, len(pixels), channels):
        key = (pixels[i] >> 4, pixels[i + 1] >> 4, pixels[i + 2] >> 4)
        counts[key] = counts.get(key, 0) + 1
    total = width * height
    most = max(counts.values())
    variety = 1.0 - most / total

    print(f"{path}: {width}x{height}, {len(buckets)} colour buckets, {variety * 100:.1f}% not the dominant colour")
    if len(buckets) < min_colours:
        print(f"FAIL: fewer than {min_colours} colour buckets: the frame is nearly uniform", file=sys.stderr)
        return 1
    if variety < 0.25:
        print("FAIL: more than 75% of the frame is one colour", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
