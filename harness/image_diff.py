#!/usr/bin/env python3
"""Compare two non-interlaced 8-bit RGB/RGBA PNGs without external packages."""
import argparse
import pathlib
import struct
import zlib


def read_png(path):
    data = pathlib.Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"not a PNG: {path}")
    pos = 8
    compressed = bytearray()
    width = height = channels = None
    while pos < len(data):
        size = struct.unpack(">I", data[pos : pos + 4])[0]
        kind = data[pos + 4 : pos + 8]
        chunk = data[pos + 8 : pos + 8 + size]
        pos += size + 12
        if kind == b"IHDR":
            width, height, depth, color_type, _, _, interlace = struct.unpack(">IIBBBBB", chunk)
            if depth != 8 or color_type not in (2, 6) or interlace != 0:
                raise ValueError("only non-interlaced 8-bit RGB/RGBA PNGs are supported")
            channels = 3 if color_type == 2 else 4
        elif kind == b"IDAT":
            compressed.extend(chunk)
        elif kind == b"IEND":
            break
    if width is None or channels is None:
        raise ValueError("PNG has no IHDR")
    raw = zlib.decompress(compressed)
    stride = width * channels
    rows = []
    previous = bytearray(stride)
    cursor = 0
    for _ in range(height):
        filter_type = raw[cursor]
        cursor += 1
        row = bytearray(raw[cursor : cursor + stride])
        cursor += stride
        for i in range(stride):
            left = row[i - channels] if i >= channels else 0
            up = previous[i]
            up_left = previous[i - channels] if i >= channels else 0
            if filter_type == 1:
                row[i] = (row[i] + left) & 255
            elif filter_type == 2:
                row[i] = (row[i] + up) & 255
            elif filter_type == 3:
                row[i] = (row[i] + (left + up) // 2) & 255
            elif filter_type == 4:
                estimate = left + up - up_left
                pa, pb, pc = abs(estimate - left), abs(estimate - up), abs(estimate - up_left)
                predictor = left if pa <= pb and pa <= pc else up if pb <= pc else up_left
                row[i] = (row[i] + predictor) & 255
            elif filter_type != 0:
                raise ValueError(f"unsupported PNG filter {filter_type}")
        rows.append(row)
        previous = row
    return width, height, channels, rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("reference")
    parser.add_argument("candidate")
    parser.add_argument("--stride", type=int, default=1, help="sample every Nth pixel")
    args = parser.parse_args()
    if args.stride < 1:
        parser.error("--stride must be positive")
    ref = read_png(args.reference)
    candidate = read_png(args.candidate)
    if ref[:2] != candidate[:2]:
        raise SystemExit("dimension mismatch: %sx%s vs %sx%s" % (*ref[:2], *candidate[:2]))
    width, height = ref[:2]
    total = changed = 0
    difference = maximum = 0
    for y in range(0, height, args.stride):
        a, b = ref[3][y], candidate[3][y]
        for x in range(0, width, args.stride):
            offset_a = x * ref[2]
            offset_b = x * candidate[2]
            channels = min(ref[2], candidate[2], 3)
            delta = max(abs(a[offset_a + c] - b[offset_b + c]) for c in range(channels))
            difference += sum(abs(a[offset_a + c] - b[offset_b + c]) for c in range(channels))
            maximum = max(maximum, delta)
            changed += delta > 4
            total += channels
    print("[image-diff] size=%dx%d stride=%d mae=%.3f changed_pixels=%.4f max=%d" %
          (width, height, args.stride, difference / total, changed / (total / channels), maximum))


if __name__ == "__main__":
    main()
