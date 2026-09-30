#!/bin/bash
# Compares the upscalers' test options on some recordings: baseline, lean=easu, shapes (meanweight=0 gradweight=30), both.
# usage: tools/sr_option_check.sh <shrink> <first> <count> <recording>...   (sharpen 50, rest 50, the DLSS backend)
shrink=$1; first=$2; count=$3; shift 3
exe="$(dirname "$0")/../addons/DLSS5NR01/build/Release/nr_sreval.exe"
for opt in "" "lean=easu" "meanweight=0 gradweight=30" "lean=easu meanweight=0 gradweight=30"; do
  line=""
  for clip in "$@"; do
    r=$("$exe" "$clip" "${TMPDIR:-/tmp}/sropt" first=$first count=$count shrink=$shrink backend=dlss sharpen=50 restmix=50 $opt 2>&1 | grep "^average" | sed 's/.*upscaled \([0-9.]*\) dB (coarse [0-9.]*, steady \([0-9.]*\)).*/\1\/\2/')
    line="$line  $r"
  done
  echo "[${opt:-baseline}] $line"
done
