#!/bin/bash
# Usage: gen-def.sh <wine ole32.dll>
# Writes ole32.def: every export Wine's ole32 has, forwarded to ole32w (a renamed
# copy of that same file), plus the one function this shim adds.
set -e
src=$1
echo "LIBRARY ole32"
echo "EXPORTS"
x86_64-w64-mingw32-objdump -p "$src" |
  awk '/\[Ordinal\/Name Pointer\] Table/{f=1;next} f&&/^\t\[/{print $NF}' |
  sort -u | grep -vx CoRegisterActivationFilter |
  while read -r n; do echo "  $n = ole32w.$n"; done
echo "  CoRegisterActivationFilter"
