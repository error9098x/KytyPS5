#!/usr/bin/env python3
"""Summarize opt-in host-input to next-present timing for one run."""
import pathlib
import re
import statistics
import sys

text = (pathlib.Path(sys.argv[1]) / "guest-printf.log").read_text(errors="replace")
inputs = [int(t) for t in re.findall(r"InputStats:.*?kind=controller_(?:button|axis).*?t_us=(\d+)", text)]
if not inputs:
    inputs = [int(t) for t in re.findall(r"InputStats:.*?kind=mouse_button.*?t_us=(\d+)", text)]
if not inputs:
    inputs = [int(t) for t in re.findall(r"InputStats:.*?key=\d+.*?t_us=(\d+)", text)]
# FrameTiming is emitted for every present. FrameStats is an optional sparse
# diagnostic, so only use it when the per-present stream is unavailable.
frames = [int(t) for t in re.findall(r"FrameTiming: present=\d+ t_us=(\d+)", text)]
if not frames:
    frames = [int(t) for t in re.findall(r"FrameStats:.*?t_us=(\d+)", text)]
polls = [int(t) for t in re.findall(r"InputPoll:.*?t_us=(\d+)", text)]
delays = []
poll_delays = []
for stamp in inputs:
    next_frame = next((frame for frame in frames if frame >= stamp), None)
    if next_frame is not None:
        delays.append((next_frame - stamp) / 1000.0)
    next_poll = next((poll for poll in polls if poll >= stamp), None)
    if next_poll is not None:
        poll_delays.append((next_poll - stamp) / 1000.0)
if not delays:
    print("[latency] no matched input/frame timestamps")
    raise SystemExit(0)
delays.sort()
def pct(p):
    return delays[min(len(delays) - 1, int(p * len(delays)))]
print("[latency] samples=%d median_ms=%.2f p95_ms=%.2f max_ms=%.2f" %
      (len(delays), statistics.median(delays), pct(0.95), delays[-1]))
if poll_delays:
    poll_delays.sort()
    print("[latency] input_to_guest_poll_samples=%d median_ms=%.2f p95_ms=%.2f max_ms=%.2f" %
          (len(poll_delays), statistics.median(poll_delays),
           poll_delays[min(len(poll_delays) - 1, int(0.95 * len(poll_delays)))],
           poll_delays[-1]))
