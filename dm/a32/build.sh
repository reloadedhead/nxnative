#!/bin/sh
# Translate the modules of a 32-bit game and link them with the host runtime.
# Usage: dm/a32/build.sh <ExeFS dir> <out dir> [module ...]   -> <out dir>/a32host, run as: <out dir>/a32host <out dir> [-v]
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
EXEFS="$1"; OUT="$2"; shift 2
PY="${PYTHON:-$HERE/../../.venv/bin/python}"
"$PY" "$HERE/translate.py" "$EXEFS" "$OUT" "$@"
GFXLIBS="-framework Foundation -framework Metal -framework QuartzCore -framework CoreFoundation -framework AppKit -framework CoreGraphics -framework ImageIO -framework UniformTypeIdentifiers -framework IOSurface -framework CoreVideo -framework MetalKit -framework Accelerate -framework CoreServices -framework MetalFX -framework MetalPerformanceShaders -framework CoreText -framework CoreImage -framework GameController -framework AudioToolbox -framework VideoToolbox -framework CoreMedia"
"$HERE/gfx.sh" "$OUT" || exit 1
clang -O1 -g -arch arm64 -w -I"$HERE/../gfx/nvn/include" -I"$HERE/../gfx/nvn" -I"$HERE/../gfx" -I"$HERE/../host" -I"$OUT/gen" -Wl,-no_deduplicate -Wl,-segprot,__A32,rx,rx -o "$OUT/a32host" "$HERE/rt/a32host.c" "$HERE/rt/a32svc.c" "$HERE/rt/a32ipc.c" "$HERE/rt/a32svcs.c" "$HERE/rt/a32force.c" "$HERE/rt/a32fs.c" "$HERE/rt/a32gfx.c" "$HERE/rt/a32movie.c" "$HERE/rt/a32rt.S" "$OUT/modtab.c" $(cat "$OUT/objs.txt") $(cat "$OUT/gfxobjs.txt") $GFXLIBS
echo "built $OUT/a32host"
