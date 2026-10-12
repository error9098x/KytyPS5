import csv, re, statistics, sys, time
# memcorr.py <run> [window_s]: fps per window (FrameTiming, steady clock mapped to wall) joined with mem.csv
# (kyty footprint, swap used, swapins/pageins per second over the window). Ends with Pearson r of fps vs each.
R = "/Users/aviral/Documents/ChatGPT/PS5/KytyPS5/harness/runs/"
run = sys.argv[1]; W = float(sys.argv[2]) if len(sys.argv) > 2 else 40
off = time.time() - time.clock_gettime(time.CLOCK_MONOTONIC_RAW)  # steady_clock (libc++ on Apple) -> wall
pres = []
with open(R + run + "/guest-printf.log", errors="replace") as f:
    for line in f:
        if "FrameTiming:" in line:
            m = re.search(r"present=(\d+) t_us=(\d+)", line)
            if m:
                pres.append(int(m.group(2)) / 1e6 + off)
mem = [r for r in csv.DictReader(open(R + run + "/mem.csv")) if r["fp_mb"]]
if not pres or not mem:
    sys.exit("no data")
t0 = float(mem[0]["wall_s"])  # first sample with the process alive ~ launch
rows = []
k = 0
while t0 + k * W < max(pres[-1], float(mem[-1]["wall_s"])):
    a, b = t0 + k * W, t0 + (k + 1) * W
    n = sum(1 for p in pres if a <= p < b)
    ms = [r for r in mem if a <= float(r["wall_s"]) < b]
    if len(ms) >= 2:
        dt = float(ms[-1]["wall_s"]) - float(ms[0]["wall_s"]) or 1
        rate = lambda c: (int(ms[-1][c]) - int(ms[0][c])) / dt
        rows.append((k * W, n / W, max(int(r["fp_mb"]) for r in ms) / 1024, max(int(r["swap_mb"]) for r in ms) / 1024,
                     min(int(r["free_pct"] or 0) for r in ms), rate("swapins"), rate("pageins"), rate("swapouts")))
    k += 1
print("%-9s %6s %7s %7s %5s %9s %9s %9s" % ("window", "fps", "fp_GB", "swap_GB", "free%", "swapin/s", "pagein/s", "swapout/s"))
for r in rows:
    print("%4d-%-4d %6.2f %7.1f %7.1f %5d %9.0f %9.0f %9.0f" % (r[0], r[0] + W, *r[1:]))
def pearson(x, y):
    if len(x) < 3 or statistics.pstdev(x) == 0 or statistics.pstdev(y) == 0:
        return float("nan")
    mx, my = statistics.mean(x), statistics.mean(y)
    return sum((a - mx) * (b - my) for a, b in zip(x, y)) / (len(x) * statistics.pstdev(x) * statistics.pstdev(y))
fps = [r[1] for r in rows]
for i, name in [(2, "footprint"), (3, "swap"), (5, "swapin/s"), (6, "pagein/s"), (7, "swapout/s")]:
    print("r(fps, %s) = %+.2f" % (name, pearson(fps, [r[i] for r in rows])))
late = [r for r in rows if r[0] >= 160]
print("r(fps, swapin/s) after 160 s = %+.2f" % pearson([r[1] for r in late], [r[5] for r in late]))
