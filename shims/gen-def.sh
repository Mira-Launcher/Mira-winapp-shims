#!/bin/bash
# Usage: gen-def.sh <wine dll> <name> <forward-to|-> [own export...]
# Writes a .def for a stand-in <name>.dll that has the same exports as Wine's dll,
# with the same ordinals (apps import some functions by ordinal). Each export is
# forwarded to <forward-to> (a renamed copy of Wine's dll), except the ones
# listed as own, which the stand-in implements itself. With "-" nothing is forwarded.
set -e
src=$1 name=$2 fwd=$3
shift 3
echo "LIBRARY $name"
echo "EXPORTS"
seen=" "
list=$(x86_64-w64-mingw32-objdump -p "$src" |
  sed -nE '/\[Ordinal\/Name Pointer\] Table/,$ s/^\t\[ *[0-9]+\] \+base\[ *([0-9]+)\] +[0-9a-f]+ +([A-Za-z_0-9@?$]+)$/\1 \2/p' |
  sort -n)
while read -r ord sym; do
  seen="$seen$sym "
  own=0
  for o in "$@"; do [ "$o" = "$sym" ] && own=1; done
  if [ "$fwd" = "-" ] || [ $own = 1 ]; then echo "  $sym @$ord"; else echo "  $sym = $fwd.$sym @$ord"; fi
done <<< "$list"
# Own exports Wine does not have get the next free ordinals.
for o in "$@"; do
  case "$seen" in *" $o "*) ;; *) echo "  $o" ;; esac
done
