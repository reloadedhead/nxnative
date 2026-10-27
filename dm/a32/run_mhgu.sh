#!/bin/sh
# Run the translated MHGU in a window with sound. Usage: dm/a32/run_mhgu.sh [build dir]   (A32_NAME=<name> skips the name box)
OUT="${1:-/Volumes/T5/a32build/mhgu}"
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$OUT" || exit 1
export DM_WINDOW=1 DM_AUDIO=1 \
  DM_NVN_SHMAP="$HERE/../games/0100770008DD8000/shader_map.tsv" DM_MSL_DIR="${DM_MSL_DIR:-/Volumes/T5/a32build/mhgu_cache}" \
  A32_ROMFS_DIR="${A32_ROMFS_DIR:-/Volumes/T5/a32build/mhgu_extract/out/RomFS}" A32_SVC=0
exec ./a32host . 2>&1 | grep --line-buffered -v "IHidServer command"
