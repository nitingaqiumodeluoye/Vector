#!/system/bin/sh
# Root sampler: catch main-thread kernel state during launch stall.
# usage: sh sampler.sh <package> <seconds>
pkg=$1
dur=${2:-60}
out=/data/local/tmp/sampler_out.txt
: > $out
pid=0
end=$(( $(date +%s) + dur ))
while [ $(date +%s) -lt $end ]; do
  if [ -z "$pid" ]; then
    pid=$(pidof $pkg)
    [ -z "$pid" ] && { sleep 0.3; continue; }
    echo "$(date '+%H:%M:%S.%N') CAUGHT pid=$pid" >> $out
  fi
  # stop when process dies
  if [ ! -d /proc/$pid ]; then
    echo "$(date '+%H:%M:%S.%N') PROC_GONE" >> $out
    pid=0
    sleep 0.3
    continue
  fi
  ts=$(date '+%H:%M:%S.%N')
  cpu=$(awk '{print "u="$14/100"s k="$15/100"s"}' /proc/$pid/task/$pid/stat 2>/dev/null)
  sys=$(head -c 60 /proc/$pid/task/$pid/syscall 2>/dev/null)
  stk=$(head -3 /proc/$pid/task/$pid/stack 2>/dev/null | tr '\n' '|')
  echo "$ts $cpu sys=[$sys] stack=[$stk]" >> $out
  sleep 0.25
done
echo DONE >> $out
