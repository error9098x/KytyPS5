#!/usr/bin/env python3
"""Per-run stage tracker. Reads kyty.log + guest-printf.log (LOGF output).

Stage 0: no presented frame. Stage 1: a frame was presented. Stage 2: sampled
content was non-black (title/menu evidence). Stage 3: a clean 10-minute run
also shows gameplay/audio activity. This is per-run evidence; three-run gates
are evaluated by the harness log, not inferred here.
"""
import json, pathlib, re, sys

run = pathlib.Path(sys.argv[1])
def rd(n):
    f = run / n
    return f.read_text(errors="replace") if f.exists() else ""
log, guest = rd("kyty.log"), rd("guest-printf.log")
meta = json.loads(rd("run.json") or "{}")

frames = re.findall(r"FrameStats: present=(\d+) (\d+)x(\d+) .*? (ALL_BLACK|HAS_CONTENT)", guest)
timings = re.findall(r"FrameTiming: present=(\d+) t_us=\d+", guest)
alive = meta.get("timed_out") or meta.get("exit_code", 1) == 0
presented = len(frames) or len(timings)
content = any(f[3] == "HAS_CONTENT" for f in frames)
audio_signal = bool(re.search(r"(?:Ngs2AudioStats|AudioStats):.*?rms=(?!0(?:\.0+)?(?:\s|$))[0-9]", guest))
input_signal = bool(re.search(r"InputStats:.*(?:controller|key=)", guest))
gameplay_signal = meta.get("route") == "gameplay" or audio_signal
long_clean = bool(meta.get("timed_out")) and float(meta.get("elapsed_seconds", 0)) >= 600
if presented and alive and long_clean and gameplay_signal:
    stage = 3
elif presented and alive and content:
    stage = 2
elif presented and alive:
    stage = 1
else:
    stage = 0
(run / "stage.txt").write_text("%d\n" % stage)
print("[stage] %d  presented_samples=%d  content=%s gameplay_signal=%s input_signal=%s" %
      (stage, presented, "yes" if content else "NO (all black)" if frames else "n/a",
       "yes" if gameplay_signal else "no", "yes" if input_signal else "no"))
