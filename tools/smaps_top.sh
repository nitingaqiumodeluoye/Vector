#!/system/bin/sh
# usage: sh smaps_top.sh <pid>
pid=$1
awk '
/^[0-9a-f]+-[0-9a-f]+ / { name=$6; if (name=="") name="[anon]"; }
/^Rss:/ { arr[name]+=$2 }
END { for (n in arr) printf "%8d kB  %s\n", arr[n], n }
' /proc/$pid/smaps | sort -rn | head -20
