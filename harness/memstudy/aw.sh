#!/bin/sh
# aw.sh <tag> [ENV=VAL...] : Animal Well 3x45 s regression (plan P2.3); results appended to /tmp/r8.log
cd /Users/aviral/Documents/ChatGPT/PS5/KytyPS5
tag=$1; shift; L=/tmp/r8.log
for i in 1 2 3; do date +%s > /tmp/kyty_loop.heartbeat; n=aw-regress-$i-$tag
  env MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=2 KYTY_FRAME_STATS=1 KYTY_FRAME_DUMP=1 KYTY_TIMEOUT_SECONDS=45 "$@" caffeinate -i ./harness/run.sh $n > /tmp/$n.out 2>&1
  echo "$(date +%T) $n $(cat harness/runs/$n/run.json) $(grep -E '^\[stage\]|^\[frames\]' /tmp/$n.out | tr '\n' ' ') exc=$(grep -c -i exception harness/runs/$n/scan.txt)" >> $L
  rm -rf harness/runs/$n/frames harness/runs/$n/shaders harness/runs/$n/command-dumps
done
echo "$(date +%T) AWDONE $tag" >> $L
