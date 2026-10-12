#!/bin/sh
# run_arm.sh <name> <seconds> [ENV=VAL ...] : one Silent Hill arm on the best480-4 route with memory sampling.
# Adds BEST flags + route + stats; caller env/args override. Writes harness/runs/<name>/{mem.csv,footprint.txt}
# and prints fps-vs-memory windows (memcorr.py). MEMHOG_GIB=<n> runs memhog.py alongside (pressure test).
# Requires df >= 20 GB. Starts memmon.sh (kills kyty at footprint > 24 GB, swap > 12 GB, free < 6 %).
set -u
H=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd); ROOT=$(cd "$H/../.." && pwd); cd "$ROOT"
n=$1; t=$2; shift 2
B="KYTY_FAULT_COALESCE=1 KYTY_BDA_SYNC_SKIP=1 KYTY_GC_DRAIN_INTERVAL=16 KYTY_UNMAP_FASTPATH=1 KYTY_FAULT_WAIT_ON_CALLER=1 KYTY_BDA_DIRTY_REGIONS=1 KYTY_THREAD_QOS=1 KYTY_FLUSH_ON_LABEL=1"
R="KYTY_INPUT_PROBE_KEY=F10 KYTY_INPUT_PROBE_BOOTSTRAP_KEY=J KYTY_INPUT_PROBE_BOOTSTRAP_COUNT=12 KYTY_INPUT_PROBE_BOOTSTRAP_WARMUP_MS=100000000"
X="KYTY_REMAP_STATS=1 KYTY_READBACK_STATS=1 KYTY_INPUT_PROBE_KEY=J KYTY_INPUT_PROBE_START_MS=60000 KYTY_INPUT_PROBE_INTERVAL_MS=3000 KYTY_INPUT_PROBE_HOLD_MS=100"
M="KYTY_EVICT_STATS=1 KYTY_BUFFER_CACHE_STATS=1"
F=$(df -k /System/Volumes/Data | tail -1 | awk '{print int($4/1048576)}')
[ "$F" -lt 20 ] && { echo "STOP lowdisk $F GB"; exit 2; }
pkill -x kyty_emulator; sleep 20
sh "$H/memmon.sh" & MON=$!
sh "$H/memsamp.sh" /tmp/mem-$n.csv &
HOG=""; if [ -n "${MEMHOG_GIB:-}" ]; then python3 -I "$H/memhog.py" "$MEMHOG_GIB" & HOG=$!; sleep 15; fi
( sh "$H/tr2.sh" $n $t KYTY_FRAME_STATS=0 KYTY_FRAME_DUMP=1 KYTY_FRAME_SAMPLE_SECONDS=20 KYTY_FRAME_TIMING=1 $X $R $B $M "$@" ) & RP=$!
i=0; lim=$(( t / 5 + 40 ))
while [ $i -lt $lim ]; do sleep 5; i=$((i+1))
  [ $i -eq 36 ] && { Q=$(pgrep -x kyty_emulator | head -1); [ -n "$Q" ] && footprint -p $Q > /tmp/fp180-$n.txt 2>&1; }
  [ $i -eq $(( t * 8 / 50 )) ] && { Q=$(pgrep -x kyty_emulator | head -1); [ -n "$Q" ] && footprint -p $Q > /tmp/fp-$n.txt 2>&1; }
  grep -q DONE /tmp/shrun-$n.out 2>/dev/null && break; done
pkill -x kyty_emulator; wait $RP 2>/dev/null
[ -n "$HOG" ] && kill $HOG; touch /tmp/memsamp.stop /tmp/r7mem.stop; sleep 6
r=harness/runs/$n; cp /tmp/mem-$n.csv $r/mem.csv; cp /tmp/fp-$n.txt $r/footprint.txt 2>/dev/null; cp /tmp/fp180-$n.txt $r/footprint-180s.txt 2>/dev/null
cat $r/run.json; python3 -I "$H/memcorr.py" $n 40
grep -a -h "EvictStats:" $r/guest-printf.log | tail -1 | cut -c1-400
# Trim frames: every 240th + last, as JPEG.
d=$r/frames; last=$(ls $d 2>/dev/null | tail -1)
for f in $d/*.png; do [ -f "$f" ] || continue; b=$(basename $f .png); k=$(echo $b | sed 's/frame_0*//'); k=${k:-0}
  if [ "$b.png" = "$last" ] || [ $((k % 240)) -eq 0 ]; then sips -s format jpeg -s formatOptions 70 "$f" --out "$d/$b.jpg" >/dev/null 2>&1; fi; rm -f "$f"; done
rm -rf $r/shaders $r/command-dumps
