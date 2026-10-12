#!/bin/sh
# p1.sh <arm-name> <sec> [ENV=VAL...] : preflight + run_arm.sh + sample at ~220 s of kyty runtime + symbolicate. Log /tmp/r8.log
cd /Users/aviral/Documents/ChatGPT/PS5/KytyPS5
L=/tmp/r8.log; n=$1; t=$2; shift 2
date +%s > /tmp/kyty_loop.heartbeat
echo "$(date +%T) START $n $t $*  memhog=${MEMHOG_GIB:-}" >> $L
P=$(pgrep -fl "kyty_emulator|run.sh|cmake"); [ -n "$P" ] && echo "$(date +%T) WARN busy: $P" >> $L
echo "$(date +%T) df $(df -k /System/Volumes/Data | tail -1 | awk '{print int($4/1048576)}') GB; load $(sysctl -n vm.loadavg); swap $(sysctl -n vm.swapusage)" >> $L
# Host audio preflight: a failed CoreAudio AudioQueueStart (-66681) makes each SDL_OpenAudioDeviceStream block
# ~15 s and the no-device path changes the game's pacing (60 Hz presents, ~50 s boot stall at present ~250,
# slower progression), so such a run is not comparable. Warm the device and flag it (see LOG 2026-10-12 M1).
A0=$(date +%s); afplay -v 0 /System/Library/Sounds/Tink.aiff >/dev/null 2>&1; AR=$?; AD=$(( $(date +%s) - A0 ))
[ $AR -ne 0 ] || [ $AD -gt 5 ] && echo "$(date +%T) WARN host audio slow/failed (afplay rc=$AR ${AD}s)" >> $L
SAMPAT=${SAMPAT:-220}
( for i in $(seq 1 120); do Q=$(pgrep -x kyty_emulator | head -1); [ -n "$Q" ] && break; sleep 1; done
  [ -n "$Q" ] && { sleep $SAMPAT; kill -0 $Q 2>/dev/null && { sample $Q 10 5 -mayDie -file /tmp/samp-$n.txt >/dev/null 2>&1; echo "$(date +%T) sampled $n" >> $L; }; } ) &
SP=$!
sh harness/memstudy/run_arm.sh $n $t "$@" > /tmp/arm-$n.out 2>&1
wait $SP
r=harness/runs/$n
[ -f /tmp/samp-$n.txt ] && { python3 harness/memstudy/symb.py /tmp/samp-$n.txt $r/samp.sym.txt 2>>$L; python3 harness/memstudy/profsum.py $r/samp.sym.txt 30 > $r/profsum.txt 2>>$L; }
python3 harness/memstudy/fpsw.py $n 20 > $r/fps20.txt 2>&1
grep -aq "SDL_OpenAudioDeviceStream failed" $r/guest-printf.log && echo "$(date +%T) INVALID $n: host audio open failed (no-device pacing; not comparable)" >> $L
echo "$(date +%T) END $n" >> $L; cat /tmp/arm-$n.out >> $L
date +%s > /tmp/kyty_loop.heartbeat
