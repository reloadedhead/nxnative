#!/bin/sh
# One development round: retranslate (all modules), relink, run, and show the tail of the log with demangled short names.
# Usage: dm/a32/iter.sh [seconds] [lines] [extra translate.py args...]
HERE="$(cd "$(dirname "$0")" && pwd)"; EXEFS="${A32_EXEFS:-/Volumes/T5/Games/Nintendo Switch/MHGU/0100770008DD8000_survey/extract/ExeFS}"; OUT="${A32_OUT:-/Volumes/T5/a32build/mhgu}"
SECS="${1:-60}"; LINES="${2:-30}"; shift 2 2>/dev/null
"$HERE/build.sh" "$EXEFS" "$OUT" "$@" 2>&1 | grep -E "error|failed|Traceback" | head -5
cd "$OUT" && perl -e "alarm $SECS; exec @ARGV" ./a32host . 2>&1 | tail -n "$LINES" | c++filt -n | sed -E 's/\(([^()]|\([^()]*\))*\)//g' | cut -c1-260
