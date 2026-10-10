#!/usr/bin/env python3
"""Summarize opt-in per-present timing for one harness run."""
import pathlib
import re
import statistics
import sys

text = (pathlib.Path(sys.argv[1]) / "guest-printf.log").read_text(errors="replace")
stamps = [int(t) for t in re.findall(r"FrameTiming: present=\d+ t_us=(\d+)", text)]
if len(stamps) < 2:
    print("[fps] no frame-timing samples")
    raise SystemExit(0)
deltas = [(b - a) / 1000.0 for a, b in zip(stamps, stamps[1:]) if b >= a]
if not deltas:
    print("[fps] no valid frame intervals")
    raise SystemExit(0)
median = statistics.median(deltas)
fps = 1000.0 / median if median else 0.0
print("[fps] frames=%d median_frame_ms=%.2f median_fps=%.2f p95_frame_ms=%.2f max_frame_ms=%.2f" %
      (len(stamps), median, fps, sorted(deltas)[int(0.95 * (len(deltas) - 1))], max(deltas)))

# Keep the fixed-size startup window separate from sustained pacing. The first
# 120 intervals cover initialization on the current 60 Hz route; this avoids
# hiding a multi-second startup stall inside an otherwise useful warm metric.
startup_count = min(120, len(deltas))
startup = deltas[:startup_count]
warm = deltas[startup_count:]
if len(warm) >= 2:
    warm_sorted = sorted(warm)
    warm_median = statistics.median(warm)
    print("[fps] startup_window_intervals=%d startup_max_ms=%.2f warm_intervals=%d "
          "warm_median_frame_ms=%.2f warm_fps=%.2f warm_p95_frame_ms=%.2f warm_max_frame_ms=%.2f" %
          (startup_count, max(startup), len(warm), warm_median,
           1000.0 / warm_median if warm_median else 0.0,
           warm_sorted[int(0.95 * (len(warm_sorted) - 1))], max(warm)))
