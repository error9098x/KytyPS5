#!/usr/bin/env python3
"""Summarize opt-in NGS2 and SDL audio signal telemetry for one run."""
import pathlib
import re
import statistics
import sys

text = (pathlib.Path(sys.argv[1]) / "guest-printf.log").read_text(errors="replace")

def values(pattern):
    return [float(value) for value in re.findall(pattern, text)]

sdl = values(r"AudioStats:.*?rms=([0-9]+(?:\.[0-9]+)?)")
ngs2 = values(r"Ngs2AudioStats:.*?rms=([0-9]+(?:\.[0-9]+)?)")

def report(name, samples):
    if not samples:
        return "%s_samples=0" % name
    nonzero = sum(value > 1e-6 for value in samples)
    return "%s_samples=%d %s_nonzero=%d %s_median_rms=%.6f %s_peak_rms=%.6f" % (
        name, len(samples), name, nonzero, name, statistics.median(samples), name, max(samples))

print("[audio] %s %s" % (report("sdl", sdl), report("ngs2", ngs2)))
