#!/bin/sh
# memmon7.sh -- r7 runner: every 10 s log kyty RSS/footprint, swap, free%, GPU -> /tmp/r7mem.log; stops when /tmp/r7mem.stop exists
# critical: kyty footprint > 24 GB, or swap used > 12 GB, or free% < 6; 5 s poll -> pkill kyty_emulator (MEMKILL). (21 GB limit killed best480-3 at 293 s with tunnel alive at fp 22.5 GB / swap 8.5 GB / free 12%)
L=/tmp/r7mem.log; rm -f /tmp/r7mem.stop
while [ ! -f /tmp/r7mem.stop ]; do
  T=$(date +%H:%M:%S); A=$(cat /tmp/r7.status 2>/dev/null)
  P=$(pgrep -x kyty_emulator | head -1)
  SW=$(sysctl -n vm.swapusage | sed 's/  */ /g')
  SWU=$(sysctl -n vm.swapusage | sed 's/.*used = \([0-9.]*\)M.*/\1/' | cut -d. -f1)
  FREE=$(memory_pressure -Q 2>/dev/null | grep -o "percentage: [0-9]*" | grep -o "[0-9]*")
  GPU=$(ioreg -r -d 1 -c IOAccelerator 2>/dev/null | grep -o '"In use system memory"=[0-9]*' | head -1)
  if [ -n "$P" ]; then
    RSS=$(ps -o rss= -p $P | tr -d ' ')
    FPB=$(footprint -p $P 2>/dev/null | grep -m1 " Footprint: " | sed 's/.*Footprint: \([0-9.]* [KMG]*B\).*/\1/')
    echo "$T [$A] pid=$P rss_kb=$RSS fp=[$FPB] free%=$FREE swap=[$SW] gpu=[$GPU]" >> $L
    FPMB=$(echo "$FPB" | awk '{v=$1; if($2=="GB")v*=1024; if($2=="KB")v/=1024; printf "%d", v}')
    if [ "${FPMB:-0}" -gt 24576 ] || [ "${SWU:-0}" -gt 12288 ] || [ "${FREE:-100}" -lt 6 ]; then
      echo "$T MEMKILL fp=$FPMB swap=$SWU free=$FREE [$A]" >> $L; echo "MEMKILL $A" >> /tmp/r7.memkill; pkill -x kyty_emulator
    fi
  else
    echo "$T [$A] noproc free%=$FREE swap=[$SW] gpu=[$GPU]" >> $L
  fi
  sleep 5
done
echo "$(date +%H:%M:%S) stop" >> $L
