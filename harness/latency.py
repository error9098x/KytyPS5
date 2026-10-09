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
poll_records = [
    (int(buttons, 16), int(sample), int(host))
    for buttons, sample, host in re.findall(
        r"InputPoll:.*?buttons=0x([0-9a-fA-F]+).*?sample_ts=(\d+) t_us=(\d+)", text
    )
]
polls = [host for _, _, host in poll_records]
if not polls:
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
for threshold in (40.0, 100.0, 500.0):
    print("[latency] host_to_present_over_%gms=%d" %
          (threshold, sum(delay > threshold for delay in delays)))
if poll_delays:
    poll_delays.sort()
    print("[latency] input_to_guest_poll_samples=%d median_ms=%.2f p95_ms=%.2f max_ms=%.2f" %
          (len(poll_delays), statistics.median(poll_delays),
           poll_delays[min(len(poll_delays) - 1, int(0.95 * len(poll_delays)))],
           poll_delays[-1]))
    for threshold in (40.0, 100.0, 500.0):
        print("[latency] host_to_guest_poll_over_%gms=%d" %
              (threshold, sum(delay > threshold for delay in poll_delays)))

if len(polls) > 1:
    poll_gaps = sorted((b - a) / 1000.0 for a, b in zip(polls, polls[1:]))
    print("[latency] guest_poll_gap_ms median=%.2f p95=%.2f max=%.2f" %
          (statistics.median(poll_gaps), poll_gaps[min(len(poll_gaps) - 1,
                                                       int(0.95 * len(poll_gaps)))],
           poll_gaps[-1]))
    for threshold in (40.0, 100.0, 500.0):
        print("[latency] guest_poll_gap_over_%gms=%d" %
              (threshold, sum(gap > threshold for gap in poll_gaps)))

# InputState is emitted at the point a host button changes the emulated pad.
# Matching that bit against the first later PadRead sample measures when the
# guest actually observes the transition, including any queued-state delay.
states = [
    (int(mask, 16), bool(int(down)), int(host))
    for mask, down, host in re.findall(
        r"InputState: kind=button mask=0x([0-9a-fA-F]+) down=(\d+).*?t_us=(\d+)",
        text,
    )
]
observed = []
if states and poll_records:
    for mask, down, host in states:
        for buttons, _, poll_host in poll_records:
            if poll_host < host:
                continue
            active = (buttons & mask) != 0
            if active == down:
                observed.append((poll_host - host) / 1000.0)
                break
if observed:
    observed.sort()
    print("[latency] host_to_observed_button samples=%d median_ms=%.2f p95_ms=%.2f max_ms=%.2f" %
          (len(observed), statistics.median(observed),
           observed[min(len(observed) - 1, int(0.95 * len(observed)))], observed[-1]))
    for threshold in (40.0, 100.0, 500.0):
        print("[latency] host_to_observed_button_over_%gms=%d" %
              (threshold, sum(delay > threshold for delay in observed)))
