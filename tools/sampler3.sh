#!/system/bin/sh
out=/data/local/tmp/s3.txt
: > $out
am force-stop com.xff.launch >/dev/null 2>&1
sleep 1
( sleep 1; am start -n com.xff.launch/.MainActivity >/dev/null 2>&1 ) &
pid=
end=$(( $(date +%s) + 90 ))
prev_k=0
while [ $(date +%s) -lt $end ]; do
  if [ -z "$pid" ]; then
    pid=$(pidof com.xff.launch)
    if [ -n "$pid" ]; then
      echo "$(date +%T.%N) CAUGHT $pid" >> $out
      grep -E 'IPI|tlb' /proc/interrupts > /data/local/tmp/irq_start.txt 2>/dev/null
    fi
  fi
  if [ -n "$pid" ]; then
    if [ -d /proc/$pid ]; then
      uk=$(awk '{print $3" u:"$14/100" k:"$15/100" mf:"$10" Mj:"$12}' /proc/$pid/task/$pid/stat 2>/dev/null)
      tlb=$(grep -E 'tlb_flush|nr_tlb' /proc/vmstat | tr '\n' ' ')
      ipi=$(grep -E '^IPI[0-9]|TLB|resched' /proc/interrupts | head -6 | tr '\n' ' ')
      echo "$(date +%T.%N) $uk | $tlb" >> $out
      echo "    IPI: $ipi" >> $out
    else
      echo "$(date +%T.%N) DIED" >> $out
      pid=
    fi
  fi
  sleep 0.4
done
echo DONE >> $out
