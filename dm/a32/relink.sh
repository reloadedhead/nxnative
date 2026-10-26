#!/bin/sh
# Rebuild only the C runtime and relink against the already translated objects, then run: dm/a32/relink.sh <out dir> [seconds] [lines]
HERE="$(cd "$(dirname "$0")" && pwd)"; OUT="$1"; SECS="${2:-20}"; LINES="${3:-40}"
GFXLIBS="-framework Foundation -framework Metal -framework QuartzCore -framework CoreFoundation -framework AppKit -framework CoreGraphics -framework ImageIO -framework UniformTypeIdentifiers -framework IOSurface -framework CoreVideo -framework MetalKit -framework Accelerate -framework CoreServices -framework MetalFX -framework MetalPerformanceShaders -framework CoreText -framework CoreImage -framework GameController -framework AudioToolbox -framework VideoToolbox -framework CoreMedia"
"$HERE/gfx.sh" "$OUT" || exit 1
clang -O1 -g -arch arm64 -w -I"$HERE/../gfx/nvn/include" -I"$HERE/../gfx/nvn" -I"$HERE/../gfx" -I"$HERE/../host" -I"$OUT/gen" -Wl,-no_deduplicate -Wl,-segprot,__A32,rx,rx -o "$OUT/a32host" "$HERE"/rt/a32host.c "$HERE"/rt/a32svc.c "$HERE"/rt/a32ipc.c "$HERE"/rt/a32svcs.c "$HERE"/rt/a32force.c "$HERE"/rt/a32fs.c "$HERE"/rt/a32gfx.c "$HERE"/rt/a32movie.c "$HERE"/rt/a32audren.c "$HERE"/rt/a32rt.S "$OUT/modtab.c" $(cat "$OUT/objs.txt") $(cat "$OUT/gfxobjs.txt") $GFXLIBS 2>&1 | grep -E "error" -A3 | head -30
cd "$OUT" && perl -e "alarm $SECS; exec @ARGV" ./a32host . 2>&1 | tail -n "$LINES" | c++filt -n | cut -c1-360
