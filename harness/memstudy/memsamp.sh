#!/bin/sh
# memsamp.sh <out.csv> : every 5 s while /tmp/memsamp.stop is absent, log kyty footprint + machine swap/paging.
# Columns: wall_s, kyty_pid, footprint_mb, rss_mb, swap_used_mb, free_pct, swapins, swapouts, pageins, pageouts, compressed_pages
O=$1; rm -f /tmp/memsamp.stop
echo "wall_s,pid,fp_mb,rss_mb,swap_mb,free_pct,swapins,swapouts,pageins,pageouts,compressed" > $O
while [ ! -f /tmp/memsamp.stop ]; do
  T=$(date +%s)
  P=$(pgrep -x kyty_emulator | head -1)
  SWU=$(sysctl -n vm.swapusage | sed 's/.*used = \([0-9.]*\)M.*/\1/' | cut -d. -f1)
  FREE=$(memory_pressure -Q 2>/dev/null | grep -o "percentage: [0-9]*" | grep -o "[0-9]*")
  VM=$(vm_stat | awk -F: '
    /^Swapins/{si=$2} /^Swapouts/{so=$2} /^Pageins/{pi=$2} /^Pageouts/{po=$2} /occupied by compressor/{c=$2}
    END{gsub(/[ .]/,"",si);gsub(/[ .]/,"",so);gsub(/[ .]/,"",pi);gsub(/[ .]/,"",po);gsub(/[ .]/,"",c);print si","so","pi","po","c}')
  FP=""; RSS=""
  if [ -n "$P" ]; then
    RSS=$(( $(ps -o rss= -p $P | tr -d ' ') / 1024 ))
    FP=$(footprint -p $P 2>/dev/null | grep -m1 " Footprint: " | sed 's/.*Footprint: \([0-9.]* [KMG]*B\).*/\1/' |
         awk '{v=$1; if($2=="GB")v*=1024; if($2=="KB")v/=1024; printf "%d", v}')
  fi
  echo "$T,$P,$FP,$RSS,$SWU,$FREE,$VM" >> $O
  sleep 5
done
