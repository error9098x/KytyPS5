#!/usr/bin/env python3
# gts.py <run> [from_s] [to_s] : average GpuThreadStats lines (KYTY_GPU_THREAD_STATS) over a window of the run
# (seconds since the first line). Prints mean ms per 2 s and % of wall per category, plus drain sites.
import re, sys
R = "/Users/aviral/Documents/ChatGPT/PS5/KytyPS5/harness/runs/"
run = sys.argv[1]; a = float(sys.argv[2]) if len(sys.argv) > 2 else 180; b = float(sys.argv[3]) if len(sys.argv) > 3 else 1e9
L = [l for l in open(R + run + "/guest-printf.log", errors="replace") if "GpuThreadStats:" in l]
if not L: print("no GpuThreadStats"); sys.exit()
acc = {}; sites = {}; wall = 0; n = 0; T = 0
for l in L:
    dt = float(re.search(r"dt_ms=([\d.]+)", l).group(1)); T += dt / 1000
    if not (a <= T < b): continue
    n += 1; wall += dt
    body = l.split("[")[0] + " " + l.split("]")[-1] if "[" in l else l
    for k, v in re.findall(r" (\w+)=([\d.]+)\(", l): acc[k] = acc.get(k, 0) + float(v)
    for k, v in re.findall(r"(\w+)=(\d+)", l[l.find("draws="):]): acc[k] = acc.get(k, 0) + float(v)
    m = re.search(r"\[([^\]]*)\]", l)
    if m:
        for k, v in re.findall(r"(\w+)=([\d.]+)", m.group(1)): sites[k] = sites.get(k, 0) + float(v)
if not n: print("no lines in window"); sys.exit()
print("GpuThreadStats %s %g-%gs: %d lines, wall %.0f s" % (run, a, min(b, T), n, wall / 1000))
for k in ["idle", "drain", "submit", "encode", "commit", "pipeline", "prep", "bda", "rt", "cmd", "other"]:
    if k in acc: print("  %-9s %6.1f%%" % (k, 100 * acc[k] / wall))
print("  drain sites (%% of wall): " + " ".join("%s=%.1f" % (k, 100 * v / wall) for k, v in sorted(sites.items(), key=lambda x: -x[1])))
for k in ["draws", "dispatches"]:
    if k in acc: print("  %s/s %.0f" % (k, acc[k] / (wall / 1000)))
