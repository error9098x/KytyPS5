#!/usr/bin/env python3
# gtsab.py <run> [from_s] : GpuThreadStats averaged per KYTY_AB_TOGGLE phase (lines after from_s of GPU-thread time)
import re, sys
R = "/Users/aviral/Documents/ChatGPT/PS5/KytyPS5/harness/runs/"
run = sys.argv[1]; a = float(sys.argv[2]) if len(sys.argv) > 2 else 160
ph = None; T = 0; acc = {"on": {}, "off": {}}; wall = {"on": 0, "off": 0}
for l in open(R + run + "/guest-printf.log", errors="replace"):
    m = re.search(r"ABToggle: (on|off) ", l)
    if m: ph = m.group(1); continue
    if "GpuThreadStats:" not in l or ph is None: continue
    dt = float(re.search(r"dt_ms=([\d.]+)", l).group(1)); T += dt / 1000
    if T < a: continue
    wall[ph] += dt; d = acc[ph]
    for k, v in re.findall(r" (\w+)=([\d.]+)\(", l): d[k] = d.get(k, 0) + float(v)
    mm = re.search(r"\[([^\]]*)\]", l)
    if mm:
        for k, v in re.findall(r"(\w+)=([\d.]+)", mm.group(1)): d["d_" + k] = d.get("d_" + k, 0) + float(v)
    for k, v in re.findall(r"(draws|dispatches)=(\d+)", l): d[k] = d.get(k, 0) + float(v)
keys = sorted(set(acc["on"]) | set(acc["off"]), key=lambda k: (k.startswith("d_"), k))
print("%-14s %8s %8s   (%% of wall; draws per s)" % ("cat", "on", "off"))
for k in keys:
    f = (lambda p: acc[p].get(k, 0) / (wall[p] / 1000)) if k in ("draws", "dispatches") else (lambda p: 100 * acc[p].get(k, 0) / wall[p] if wall[p] else 0)
    print("%-14s %8.1f %8.1f" % (k, f("on"), f("off")))
print("wall s: on %.0f off %.0f" % (wall["on"] / 1000, wall["off"] / 1000))
