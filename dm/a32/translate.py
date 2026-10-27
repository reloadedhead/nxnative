#!/usr/bin/env python3
"""Whole-module ahead-of-time translation of 32-bit (AArch32, ARM mode) NSO modules to AArch64 assembly.

Every aligned word of a module's text segment becomes one 16-byte *slot* of four AArch64 instructions, so a guest address maps
to its translated code by arithmetic alone: slot = <module>_slots + ((addr - text_start) >> 2) * 16. A word whose translation
needs more than four instructions leaves a `b` in its slot and the real code in an out-of-line section (ending with a branch to
the next slot); a word the lifter does not cover (data in the text, instructions not yet supported) becomes a trap that names
its guest address, so nothing is guessed. Direct branches are direct branches; indirect ones go through `_a32_dispatch`.

Usage: translate.py <ExeFS dir> <out dir> [-j N] [module ...]   (default modules: rtld main subsdk0 sdk, in load order)
Writes <out>/asm/<module>_<n>.s, <out>/layout.tsv (module, guest base, text size, image size, slots symbol) and <out>/modtab.c."""
import collections, concurrent.futures, os, struct, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, '..', 'tools'))
import nso_syms
from lift import Lifter, Unsupported, mov32

GUEST_BASE = 0x00200000
CHUNK = 32768          # slots per assembly file
SLOT_INSNS = 4

def is_insn(line): return not (line.endswith(':') or line.startswith('//'))

def layout(exefs, mods):
    out, base = [], GUEST_BASE
    for m in mods:
        path = os.path.join(exefs, m)
        d = open(path, 'rb').read()
        segs = [struct.unpack_from('<III', d, 0x10 + i * 0x10) for i in range(3)]
        bss = struct.unpack_from('<I', d, 0x3c)[0]
        image = segs[2][1] + segs[2][2] + bss
        out.append(dict(name=m, path=path, base=base, text=segs[0][2], text_off=segs[0][1], ro_off=segs[1][1], data_off=segs[2][1], image=image, filesz=segs[2][1] + segs[2][2]))
        base += (image + 0xfff) & ~0xfff
    return out

_img = {}
OUTLINE_ALL = [False]
OUTLINE_MODS = []
COUNT_MODS = []   # modules whose bl / blx instructions count themselves (a32_callcnt, see a32host.c): a cheap profile of which call sites run
OUTLINE_RANGES = []   # (module, lo, hi) text offsets whose instructions are always outlined (debugging)
ONLY = []
TRACE = []   # (module, lo, hi): text offsets whose instructions call the trace gate first
def translate(mod, outdir, stats, firsts):
    name, base = mod['name'], mod['base']
    if name not in _img: _img[name] = nso_syms.load(mod['path'])
    img = _img[name]
    nslots = mod['text'] // 4
    sym = '_a32_%s_slots' % name
    def label(tgt):
        t = tgt - base
        if not (0 <= t < mod['text']) or t & 3: raise Unsupported('branch target outside the module text')
        return slot_ref(t >> 2)
    def slot_ref(idx): return '_a32_%s_c%d+%d' % (name, idx // CHUNK, (idx % CHUNK) * 16)
    lf = Lifter(label, '_a32_dispatch')
    files = []
    for first in firsts:
        slots, outl = [], []
        cs = '_a32_%s_c%d' % (name, first // CHUNK)
        slots += ['.globl %s' % cs, '.p2align 4', '%s:' % cs]
        if first == 0: slots += ['.globl %s' % sym, '%s:' % sym]
        for idx in range(first, min(first + CHUNK, nslots)):
            addr = base + idx * 4
            w = struct.unpack_from('<I', img, idx * 4)[0]
            try:
                lines = lf.lift(w, addr)
                kind = 'ok'
            except Unsupported as e:
                lines = mov32('w21', addr) + ['b _a32_unsup']; kind = 'trap'; stats['trap:' + str(e).split('0x')[0][:36]] += 1
            traced = any(m == name and lo <= idx * 4 < hi for m, lo, hi in TRACE)
            if traced: lines = mov32('w21', addr) + ['bl _a32_trace_gate'] + lines
            counted = name in COUNT_MODS and (((w >> 24) & 15) == 11 and (w >> 28) != 15 or (w & 0x0ffffff0) == 0x012fff30)
            if counted: lines = mov32('w21', addr) + ['bl _a32_count_gate'] + lines
            n = sum(1 for l in lines if is_insn(l))
            stats[kind] += 1
            if n <= SLOT_INSNS and not traced and not counted and not OUTLINE_ALL[0] and name not in OUTLINE_MODS and not any(m == name and lo <= idx * 4 < hi for m, lo, hi in OUTLINE_RANGES):
                slots += lines + ['nop'] * (SLOT_INSNS - n)
            else:
                stats['outline'] += 1
                nxt = slot_ref(idx + 1) if idx + 1 < nslots else '_a32_unsup'
                slots += ['b Lol%d' % idx] + ['nop'] * (SLOT_INSNS - 1)
                outl += ['Lol%d:' % idx] + lines + ['b %s' % nxt]
        cn = first // CHUNK
        sub = {'_a32_dispatch': 'Ld_%s_%d' % (name, cn), '_a32_unsup': 'Lu_%s_%d' % (name, cn), '_a32_svc_gate': 'Ls_%s_%d' % (name, cn), '_a32_trace_gate': 'Lt_%s_%d' % (name, cn)}
        def fix(line):
            for k, v in sub.items():
                if k in line: return line.replace(k, v)
            return line
        slots = [fix(l) for l in slots]; outl = [fix(l) for l in outl]
        for k, v in sub.items():                    # a stub per runtime entry: adrp/add/br reach the whole address space, and a branch to it stays inside the module's own sections
            outl += ['.p2align 2', v + ':', 'adrp x17, %s@PAGE' % k, 'add x17, x17, %s@PAGEOFF' % k, 'br x17']
        text = ['.section __A32,__a32_%s,regular,pure_instructions' % name] + slots + ['.section __A32,__a32_%s_ol,regular,pure_instructions' % name, '.p2align 2'] + outl
        # numeric local labels (1:) only live inside one function body; keep each use inside its own slot or outline (they are)
        p = os.path.join(outdir, 'asm', '%s_%03d.s' % (name, first // CHUNK))
        open(p, 'w').write('\n'.join(text) + '\n')
        files.append(p)
    return files

def main():
    args = sys.argv[1:]
    jobs = os.cpu_count()
    if '-j' in args: i = args.index('-j'); jobs = int(args[i + 1]); del args[i:i + 2]
    while '--outline-mod' in args: i = args.index('--outline-mod'); OUTLINE_MODS.append(args[i + 1]); del args[i:i + 2]
    while '--outline-range' in args:
        i = args.index('--outline-range'); m, lo, hi = args[i + 1].split(':'); OUTLINE_RANGES.append((m, int(lo, 0), int(hi, 0))); del args[i:i + 2]
    while '--count-calls' in args: i = args.index('--count-calls'); COUNT_MODS.append(args[i + 1]); del args[i:i + 2]
    while '--only' in args: i = args.index('--only'); ONLY.append(args[i + 1]); del args[i:i + 2]
    if '--outline-all' in args: args.remove('--outline-all'); OUTLINE_ALL[0] = True
    while '--trace' in args:                                              # --trace main:0x1000:0x2000 (text offsets): every instruction in the range reports itself
        i = args.index('--trace'); m, lo, hi = args[i + 1].split(':'); TRACE.append((m, int(lo, 0), int(hi, 0))); del args[i:i + 2]
    exefs, outdir = args[0], args[1]
    mods = args[2:] or ['rtld', 'main', 'subsdk0', 'sdk']
    os.makedirs(os.path.join(outdir, 'asm'), exist_ok=True); os.makedirs(os.path.join(outdir, 'obj'), exist_ok=True)
    lay = layout(exefs, mods)
    with open(os.path.join(outdir, 'layout.tsv'), 'w') as f:
        for m in lay: f.write('%s\t%#x\t%#x\t%#x\t%#x\t%#x\t%#x\t%#x\n' % (m['name'], m['base'], m['text'], m['image'], m['text_off'], m['ro_off'], m['data_off'], m['filesz']))
    os.makedirs(os.path.join(outdir, 'image'), exist_ok=True)
    for m in lay: open(os.path.join(outdir, 'image', m['name'] + '.bin'), 'wb').write(nso_syms.load(m['path'])[:m['filesz']])
    with open(os.path.join(outdir, 'modtab.c'), 'w') as f:
        f.write('#include <stdint.h>\ntypedef struct { uint32_t lo, size; const void *slots; } ModEnt;\n')
        for m in lay: f.write('extern char a32_%s_slots[] __asm("_a32_%s_slots");\n' % (m['name'], m['name']))
        for m in lay:
            n = (m['text'] // 4 + CHUNK - 1) // CHUNK
            for k in range(n): f.write('extern char a32_%s_c%d[] __asm("_a32_%s_c%d");\n' % (m['name'], k, m['name'], k))
            f.write('const void *const a32_%s_chunks[] = { %s };\nconst unsigned a32_%s_nchunks = %d;\n' % (m['name'], ', '.join('a32_%s_c%d' % (m['name'], k) for k in range(n)), m['name'], n))
        f.write('const void *const *const a32_chunk_tab[] = { %s };\nconst unsigned a32_chunk_n[] = { %s };\nconst unsigned a32_chunk_size = %d;\n' % (', '.join('a32_%s_chunks' % m['name'] for m in lay), ', '.join('%d' % ((m['text'] // 4 + CHUNK - 1) // CHUNK) for m in lay), CHUNK * 16))
        f.write('const ModEnt a32_modtab[] = {\n')
        for m in lay: f.write('  { %#x, %#x, a32_%s_slots },\n' % (m['base'], m['text'], m['name']))
        f.write('  { 0, 0, 0 } };\n')
    t0 = time.time(); stats = collections.Counter(); allfiles = []
    with concurrent.futures.ProcessPoolExecutor(max_workers=jobs) as ex:
        futs = {ex.submit(translate_job, m, outdir, first, list(TRACE), OUTLINE_ALL[0], list(OUTLINE_MODS), list(OUTLINE_RANGES), list(COUNT_MODS)): m for m in lay if not ONLY or m['name'] in ONLY for first in range(0, m['text'] // 4, CHUNK)}
        done = 0
        for fu in concurrent.futures.as_completed(futs):
            fl, st = fu.result(); allfiles += fl; stats.update(st); done += 1
            if done % 20 == 0 or done == len(futs): print('%d / %d chunks translated (%.0f s)' % (done, len(futs), time.time() - t0)); sys.stdout.flush()
    print('slots: ok %d, trap %d, outlined %d' % (stats['ok'], stats['trap'], stats['outline']))
    reasons = [(k[5:], v) for k, v in stats.items() if k.startswith('trap:')]
    print('trap reasons:', sorted(reasons, key=lambda kv: -kv[1])[:12])
    # assemble every chunk in parallel
    def asm(p):
        o = os.path.join(outdir, 'obj', os.path.basename(p)[:-2] + '.o')
        r = subprocess.run(['clang', '-c', '-arch', 'arm64', '-o', o, p], capture_output=True, text=True)
        return o, r.returncode, r.stderr[:600]
    objs = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        for o, rc, err in ex.map(asm, sorted(allfiles)):
            if rc: print('assemble failed:', o, err); return 1
            objs.append(o)
    print('assembled %d objects (%.0f s total)' % (len(objs), time.time() - t0))
    import glob
    open(os.path.join(outdir, 'objs.txt'), 'w').write('\n'.join(sorted(glob.glob(os.path.join(outdir, 'obj', '*.o')))) + '\n')
    return 0

def translate_job(m, outdir, first, trace=(), outline_all=False, outline_mods=(), outline_ranges=(), count_mods=()):
    TRACE[:] = list(trace); OUTLINE_ALL[0] = outline_all; OUTLINE_MODS[:] = list(outline_mods); OUTLINE_RANGES[:] = list(outline_ranges); COUNT_MODS[:] = list(count_mods)
    st = collections.Counter()
    return translate(m, outdir, st, [first]), st

if __name__ == '__main__': sys.exit(main())
