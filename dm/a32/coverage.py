#!/usr/bin/env python3
"""How much of a 32-bit NSO's text the lifter covers: lifts every aligned word of the text segment and reports the
instructions it refuses, grouped by capstone mnemonic. Usage: coverage.py <ExeFS dir> <module> [<module> ...]"""
import collections, os, struct, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, '..', 'tools'))
import nso_syms
from lift import Lifter, Unsupported
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM

def main():
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    lf = Lifter(lambda a: 'L%x' % a, 'dispatch')
    for name in sys.argv[2:]:
        t0 = time.time()
        d = open('%s/%s' % (sys.argv[1], name), 'rb').read()
        tsz = struct.unpack_from('<I', d, 0x18)[0]
        img = nso_syms.load('%s/%s' % (sys.argv[1], name))
        ok = 0; undec = 0; bad = collections.Counter(); reasons = collections.Counter()
        n = tsz // 4
        for i in range(n):
            w = struct.unpack_from('<I', img, i * 4)[0]
            try: lf.lift(w, i * 4); ok += 1
            except Unsupported as e:
                ins = next(md.disasm(w.to_bytes(4, 'little'), i * 4), None)
                bad[ins.mnemonic if ins else '(undecodable)'] += 1; reasons[str(e).split('0x')[0][:40]] += 1
        print('== %s: %d words, lifted %d (%.1f%%), refused %d  (%.0f s)' % (name, n, ok, 100 * ok / n, n - ok, time.time() - t0))
        print('   top refused mnemonics:', ', '.join('%s %d' % kv for kv in bad.most_common(40)))
        print('   reasons:', ', '.join('%s %d' % kv for kv in reasons.most_common(12)))
if __name__ == '__main__': main()
