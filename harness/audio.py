#!/usr/bin/env python3
"""Summarize opt-in NGS2 and SDL audio signal telemetry for one run.

Telemetry exists only when the run set KYTY_AUDIO_STATS. Without any AudioDevice:/AudioStats:/
Ngs2AudioStats: lines the run is reported as unmeasured, never as silent.
"""
import pathlib
import re
import statistics
import sys

text = (pathlib.Path(sys.argv[1]) / "guest-printf.log").read_text(errors="replace")

# Anchored: "AudioStats:" must not also match "Ngs2AudioStats:" lines.
SDL_RE = re.compile(r"(?m)^AudioStats:([^\n]*?)rms=([0-9]+(?:\.[0-9]+)?)")
NGS2_RE = re.compile(r"(?m)^Ngs2AudioStats:[^\n]*?rms=([0-9]+(?:\.[0-9]+)?)")
DEVICE_RE = re.compile(r"(?m)^AudioDevice:")

sdl_groups = {}
sdl = []
for head, rms in SDL_RE.findall(text):
    value = float(rms)
    sdl.append(value)
    t = re.search(r"type=(\d+)", head)
    c = re.search(r"channels=(\d+)", head)
    key = "t%sc%s" % (t.group(1) if t else "?", c.group(1) if c else "?")
    sdl_groups.setdefault(key, []).append(value)
ngs2 = [float(v) for v in NGS2_RE.findall(text)]
devices = len(DEVICE_RE.findall(text))

def report(name, samples):
    if not samples:
        return "%s_samples=0" % name
    nonzero = sum(value > 1e-6 for value in samples)
    return "%s_samples=%d %s_nonzero=%d %s_median_rms=%.6f %s_peak_rms=%.6f" % (
        name, len(samples), name, nonzero, name, statistics.median(samples), name, max(samples))

if not sdl and not ngs2 and devices == 0:
    print("[audio] unmeasured (KYTY_AUDIO_STATS not set)")
else:
    parts = [report("sdl", sdl)]
    for key in sorted(sdl_groups):
        samples = sdl_groups[key]
        nonzero = sum(value > 1e-6 for value in samples)
        parts.append("sdl[%s]=%d/%d" % (key, nonzero, len(samples)))
    parts.append(report("ngs2", ngs2) if ngs2 else "ngs2=n/a")
    parts.append("devices=%d" % devices)
    print("[audio] " + " ".join(parts))
