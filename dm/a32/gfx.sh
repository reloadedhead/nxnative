#!/bin/sh
# Compile the host NVN-on-Metal implementation (dm/gfx/nvn) and the adapters into <out dir>/gfx/*.o; writes <out dir>/gfxobjs.txt.
# Usage: dm/a32/gfx.sh <out dir> [-f]      (-f: rebuild everything)
HERE="$(cd "$(dirname "$0")" && pwd)"; OUT="$1"; ROOT="$HERE/../.."
PY="${PYTHON:-python3}"
mkdir -p "$OUT/gfx" "$OUT/gen"
[ -f "$OUT/gen/game_profile.h" ] || "$PY" "$ROOT/dm/games/game.py" header 0100770008DD8000 "$OUT/gen/game_profile.h"
"$PY" "$HERE/gen_nvn_adapters.py" "$OUT/gfx/a32nvn_gen.c" 2>/dev/null
FL="-O1 -g -arch arm64 -fobjc-arc -fno-objc-arc-exceptions -w -I$ROOT/dm/gfx/nvn/include -I$ROOT/dm/gfx/nvn -I$ROOT/dm/gfx -I$ROOT/dm/gfx/tex -I$ROOT/dm/host -I$HERE/rt -I$OUT/gen"
: > "$OUT/gfxobjs.txt"
for src in "$ROOT"/dm/gfx/nvn/hle_nvn.c "$ROOT"/dm/gfx/nvn/nvn_mtl.m "$ROOT"/dm/gfx/nvn/nvn_tex.c "$ROOT"/dm/gfx/nvn/nvn_dirty.c "$ROOT"/dm/gfx/nvn/nvn_refl.m "$ROOT"/dm/gfx/tex/*.c "$ROOT"/dm/gfx/tex/*.m "$HERE"/rt/a32shim.m "$ROOT"/dm/host/front_mac.m "$ROOT"/dm/host/hle_movie_avc.c "$ROOT"/dm/host/hle_audio.c "$OUT"/gfx/a32nvn_gen.c; do
  [ -f "$src" ] || continue
  o="$OUT/gfx/$(basename "$src").o"
  if [ "$2" = "-f" ] || [ ! -f "$o" ] || [ "$src" -nt "$o" ]; then
    clang $FL -c "$src" -o "$o" 2> "$OUT/gfx/$(basename "$src").err" || { echo "compile failed: $src"; head -20 "$OUT/gfx/$(basename "$src").err"; exit 1; }
  fi
  echo "$o" >> "$OUT/gfxobjs.txt"
done
