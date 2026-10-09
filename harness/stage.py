#!/usr/bin/env python3
"""Per-run stage tracker. Reads kyty.log + guest-printf.log (LOGF output).

Stage 0: no presented frame.  Stage 1: frames presented (boot reached first frame).
The [content] line reports whether any sampled frame was non-black; stage 2+ (title,
gameplay) still need visual / reference confirmation.
"""
import json, pathlib, re, sys

run = pathlib.Path(sys.argv[1])
def rd(n):
    f = run / n
    return f.read_text(errors="replace") if f.exists() else ""
log, guest = rd("kyty.log"), rd("guest-printf.log")
meta = json.loads(rd("run.json") or "{}")

frames = re.findall(r"FrameStats: present=(\d+) (\d+)x(\d+) .*? (ALL_BLACK|HAS_CONTENT)", guest)
alive = meta.get("timed_out") or meta.get("exit_code", 1) == 0
if frames and alive:
    stage = 1
elif not frames and meta.get("timed_out") and re.search(r"Initialized: Graphics", log) and re.search(r"Shaders: VS 1 \| PS 1", log):
    stage = 0  # pipelines built but nothing presented
else:
    stage = 0
content = any(f[3] == "HAS_CONTENT" for f in frames)
(run / "stage.txt").write_text("%d\n" % stage)
print("[stage] %d  presented_samples=%d  content=%s" % (stage, len(frames), "yes" if content else "NO (all black)" if frames else "n/a"))
