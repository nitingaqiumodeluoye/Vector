#!/system/bin/sh
out=/data/local/tmp/s4.txt
: > $out
am force-stop com.xff.launch >/dev/null 2>&1
sleep 1
( sleep 1; am start -n com.xff.launch/.MainActivity >/dev/null 2>&1 ) &
pid=
end=$(( $(date +%s) + 100 ))
while [ $(date +%s) -lt $end ]; do
  if [ -z "$pid" ]; then
    pid=$(pidof com.xff.launch)
    [ -n "$pid" ] && echo "$(date +%T.%N) CAUGHT $pid" >> $out
  fi
  if [ -n "$pid" ]; then
    if [ -d /proc/$pid ]; then
      echo "$(date +%T.%N) $(awk '{print $3" u:"$14/100" k:"$15/100" mf:"$10" Mj:"$12}' /proc/$pid/task/$pid/stat 2>/dev/null)" >> $out
    else
      echo "$(date +%T.%N) DIED $pid" >> $out
      pid=
    fi
  fi
  sleep 0.3
done
echo DONE >> $out
