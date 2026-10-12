import os, signal, sys, time
# memhog.py <GiB> [seconds] : hold <GiB> of resident, incompressible memory (pressure test, plan phase 1).
# Each 16 KiB page gets distinct random bytes so the macOS compressor cannot shrink it.
# Pages are re-touched every 10 s so they stay hot. Exits on SIGTERM or after [seconds].
gib = float(sys.argv[1]); limit = float(sys.argv[2]) if len(sys.argv) > 2 else 1e9
size = int(gib * (1 << 30)); page = 16384
buf = bytearray(size)
chunk = os.urandom(1 << 20)
for off in range(0, size, 1 << 20):
    n = min(1 << 20, size - off)
    buf[off:off + n] = chunk[:n]
    buf[off] = (off >> 20) & 0xFF  # make every MiB distinct
for off in range(0, size, page):
    buf[off + 1] = (off // page) & 0xFF
print("memhog: holding %.1f GiB" % gib, flush=True)
signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
t0 = time.time()
while time.time() - t0 < limit:
    for off in range(0, size, page):
        buf[off + 2] ^= 1
    time.sleep(10)
