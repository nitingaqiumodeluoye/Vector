#!/system/bin/sh
# Self-contained: launch target + sample main thread in foreground
out=/data/local/tmp/s2.txt
: > $out
am force-stop com.xff.launch >/dev/null 2>&1
sleep 1
( sleep 1; am start -n com.xff.launch/.MainActivity >/dev/null 2>&1 ) &
pid=
end=$(( $(date +%s) + 40 ))
while [ $(date +%s) -lt $end ]; do
  if [ -z "$pid" ]; then
    pid=$(pidof com.xff.launch)
    if [ -n "$pid" ]; then
      echo "$(date +%T.%N) CAUGHT $pid" >> $out
    fi
  fi
  if [ -n "$pid" ]; then
    if [ -d /proc/$pid ]; then
      uk=$(awk '{print $3" u:"$14/100" k:"$15/100" mf:"$10" Mj:"$12}' /proc/$pid/task/$pid/stat 2>/dev/null)
      sys=$(head -c 44 /proc/$pid/task/$pid/syscall 2>/dev/null)
      stk=$(head -3 /proc/$pid/task/$pid/stack 2>/dev/null | tr '\n' '|')
      echo "$(date +%T.%N) u/k=$uk sys=[$sys]" >> $out
      echo "    stk=[$stk]" >> $out
    else
      echo "$(date +%T.%N) DIED" >> $out
      pid=
    fi
  fi
  sleep 0.25
done
echo DONE >> $out
