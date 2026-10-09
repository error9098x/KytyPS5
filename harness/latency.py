#!/usr/bin/env python3
"""Summarize opt-in host-input to next-present timing for one run."""
import pathlib
import re
import statistics
import sys

text = (pathlib.Path(sys.argv[1]) / "guest-printf.log").read_text(errors="replace")
inputs = [int(t) for t in re.findall(r"InputStats:.*?t_us=(\d+)", text)]
frames = [int(t) for t in re.findall(r"FrameStats:.*?t_us=(\d+)", text)]
delays = []
for stamp in inputs:
    next_frame = next((frame for frame in frames if frame >= stamp), None)
    if next_frame is not None:
        delays.append((next_frame - stamp) / 1000.0)
if not delays:
    print("[latency] no matched input/frame timestamps")
    raise SystemExit(0)
delays.sort()
def pct(p):
    return delays[min(len(delays) - 1, int(p * len(delays)))]
print("[latency] samples=%d median_ms=%.2f p95_ms=%.2f max_ms=%.2f" %
      (len(delays), statistics.median(delays), pct(0.95), delays[-1]))
