#!/usr/bin/env python3
"""Which symbol does the PLT stub at <module>+<offset> bind to?  Usage: plt.py <ExeFS dir> <module> <hex offset>"""
import os, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
import nso_syms
exefs, mod, off = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
img = nso_syms.load('%s/%s' % (exefs, mod))
w = struct.unpack_from('<III', img, off)
def rot(i8, r): return ((i8 >> (2 * r)) | (i8 << (32 - 2 * r))) & 0xffffffff if r else i8
ip = off + 8 + rot(w[0] & 0xff, (w[0] >> 8) & 15)
ip += rot(w[1] & 0xff, (w[1] >> 8) & 15)
slot = ip + (w[2] & 0xfff)
mod0 = struct.unpack_from('<I', img, 4)[0]; dyn = mod0 + struct.unpack_from('<i', img, mod0 + 4)[0]
tags = {}
for i in range(4096):
    t, v = struct.unpack_from('<II', img, dyn + i * 8)
    if t == 0: break
    tags.setdefault(t, v)
symtab, strtab = tags[6], tags[5]
for tab, sz in ((tags.get(17), tags.get(18, 0)), (tags.get(23), tags.get(2, 0))):
    for i in range((sz or 0) // 8):
        o, info = struct.unpack_from('<II', img, tab + i * 8)
        if o == slot:
            nm = struct.unpack_from('<I', img, symtab + (info >> 8) * 16)[0]
            print(img[strtab + nm:img.index(b'\0', strtab + nm)].decode())
