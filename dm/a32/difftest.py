#!/usr/bin/env python3
"""Differential test of dm/a32/lift.py against Unicorn (QEMU's AArch32 CPU) as the oracle.
For every class of ARM instruction it draws random encodings and random register / flag / memory states, runs each one in
Unicorn and, lifted to AArch64 and assembled with clang, natively on this Mac, and compares r0-r14, NZCV, the next pc and
a CRC of the 64 KB test memory. Usage: difftest.py [tests-per-class] [seed] [class ...]"""
import os, random, struct, subprocess, sys, tempfile, zlib, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lift import Lifter, Unsupported, mov32
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UcError
from unicorn.arm_const import *
QREGS = [UC_ARM_REG_D0 + i for i in range(32)]
from unicorn import UC_ERR_FETCH_UNMAPPED

ADDR = 0x8000
MEM = 0x10000
HERE = os.path.dirname(os.path.abspath(__file__))
REGS = [UC_ARM_REG_R0 + i for i in range(13)] + [UC_ARM_REG_SP, UC_ARM_REG_LR]

# (mask, value) of the fixed bits of each class; every other bit is random
CLASSES = {
    'dp':     (0x0c000000, 0x00000000),
    'dp_imm': (0x0e000000, 0x02000000),
    'dp_rs':  (0x0e000090, 0x00000010),
    'dp_rsS': (0x0e100090, 0x00100010),
    'mul':    (0x0fc000f0, 0x00000090),
    'lmul':   (0x0f8000f0, 0x00800090),
    'ldst':   (0x0c000000, 0x04000000),
    'extra':  (0x0e000090, 0x00000090),
    'blk':    (0x0e000000, 0x08000000),
    'br':     (0x0e000000, 0x0a000000),
    'bx':     (0x0ffffff0, 0x012fff10),
    'blx':    (0x0ffffff0, 0x012fff30),
    'clz':    (0x0fff0ff0, 0x016f0f10),
    'rev':    (0x0fff0ff0, 0x06bf0f30),
    'ext':    (0x0f8f03f0, 0x068f0070),
    'bfx':    (0x0fa00070, 0x07a00050),
    'bfi':    (0x0fe00070, 0x07c00010),
    'movw':   (0x0fb00000, 0x03000000),
    'div':    (0x0fd0f0f0, 0x0710f010),
    'mls':    (0x0ff000f0, 0x00600090),
    'rbit':   (0x0fff0ff0, 0x06ff0f30),
    'rev16':  (0x0fbf0ff0, 0x06bf0fb0),
    'extacc': (0x0fa003f0, 0x06a00070),
    'pkh':    (0x0ff00030, 0x06800010),
    'xt16':   (0x0fb003f0, 0x06800070),
    'smla':   (0x0ff00090, 0x01000080),
    'smul':   (0x0ff0f090, 0x01600080),
    'smulw':  (0x0ff00090, 0x01200080),
    'smmul':  (0x0ff000d0, 0x07500010),
    'umaal':  (0x0ff000f0, 0x00400090),
    'ldex':   (0x0f9000ff, 0x0190009f),
    'stex':   (0x0f9000f0, 0x01800090),
    'corpus': (0, 0),
    'qop':    (0x0f900ff0, 0x01000050),
    'ndp3':   (0xfe800000, 0xf2000000),
    'nimm':   (0xfeb80090, 0xf2800010),
    'nld':    (0xff900000, 0xf4000000),
    'nvsel':  (0xff800e50, 0xfe000a00),
    'vdp':    (0x0f000e10, 0x0e000a00),
    'vxfer':  (0x0f000e10, 0x0e000a10),
    'vldst':  (0x0e000e00, 0x0c000a00),
}

CORPUS = []
def rand_word(rng, cls):
    if cls == 'corpus':
        if not CORPUS: CORPUS.extend(struct.unpack('<%dI' % (os.path.getsize(os.environ['A32_CORPUS']) // 4), open(os.environ['A32_CORPUS'], 'rb').read()))
        return rng.choice(CORPUS)
    mask, val = CLASSES[cls]
    w = (rng.getrandbits(32) & ~mask) | val
    if cls == 'extra' and (w >> 5) & 3 == 0: w |= 1 << 5
    for lo in (16, 12, 8, 0):                                     # keep register fields off pc most of the time
        if (w >> lo) & 15 == 15 and (mask >> lo) & 15 == 0 and rng.random() < 0.95 and cls not in ('movw', 'br'): w = (w & ~(15 << lo)) | (rng.randrange(15) << lo)
    if cls in ('ldex', 'stex'):
        kind = rng.choice([0xf, 0xe, 0xc]); w = (w & ~0xf00) | (kind << 8)
        if cls == 'stex' and kind == 0xc: w |= 0xf000
    cond = 14 if rng.random() < 0.6 else rng.randrange(14)
    if cls in ('ndp3', 'nimm', 'nld', 'nvsel'): cond = 15
    if cls == 'nld': w = (w & ~0xf00) | (rng.choice([7, 10, 6, 2]) << 8)
    return (w & 0x0fffffff) | (cond << 28)

def rand_fp_lane(rng):
    t = rng.random()
    if t < 0.3: return struct.unpack('<I', struct.pack('<f', rng.choice([0.0, -0.0, 1.0, -1.0, 0.5, 2.0, 3.5, 100.25, -7.75, 1e10, 1e-10, 65504.0, 0.1])))[0]
    if t < 0.5: return rng.choice([0x7f800000, 0xff800000, 0x7fc00000, 0x7f800001, 0xffc12345, 0x00000001, 0x007fffff, 0x80000001, 0x7f7fffff])
    if t < 0.75: return struct.unpack('<I', struct.pack('<f', rng.uniform(-1000, 1000)))[0]
    return rng.getrandbits(32)

def rand_fp_state(rng):
    q = []
    for _ in range(32):                                                    # 32 d registers
        if rng.random() < 0.35:
            x = rng.choice([0.0, -0.0, 1.0, -2.5, 1e300, 1e-310, float('inf'), float('nan'), rng.uniform(-1e6, 1e6), rng.uniform(-1, 1)])
            q.append(struct.unpack('<Q', struct.pack('<d', x))[0])
        else: q.append(rand_fp_lane(rng) | rand_fp_lane(rng) << 32)
    fpscr = (rng.getrandbits(4) << 28) | (rng.getrandbits(2) << 22) | (rng.getrandbits(1) << 24) | (rng.getrandbits(1) << 25)
    return q, fpscr

def rand_state(rng):
    regs = []
    for _ in range(15):
        t = rng.random()
        regs.append(rng.randrange(0x2000, 0xd000) if t < 0.5 else rng.randrange(0, 256) if t < 0.75 else rng.getrandbits(32))
    return regs, rng.getrandbits(4) << 28

def run_unicorn(w, regs, nzcv, mem, fp):
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    uc.reg_write(UC_ARM_REG_C1_C0_2, 0xf00000); uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)       # enable VFP / NEON
    for r_, v_ in zip(QREGS, fp[0]): uc.reg_write(r_, v_)
    uc.reg_write(UC_ARM_REG_FPSCR, fp[1])
    uc.mem_map(0, MEM); uc.mem_write(0, mem); uc.mem_write(ADDR, struct.pack('<I', w))      # the instruction sits in the test memory
    for r, v in zip(REGS, regs): uc.reg_write(r, v)
    cpsr = uc.reg_read(UC_ARM_REG_CPSR)
    uc.reg_write(UC_ARM_REG_CPSR, (cpsr & 0x0fffffff) | nzcv)
    try: uc.emu_start(ADDR, ADDR + 4, timeout=200000, count=1)
    except UcError as e:
        if e.errno != UC_ERR_FETCH_UNMAPPED: raise                 # a branch to unmapped code still tells us the target pc
    out = [uc.reg_read(r) for r in REGS]
    return out, uc.reg_read(UC_ARM_REG_CPSR) & 0xf0000000, uc.reg_read(UC_ARM_REG_PC), zlib.crc32(bytes(uc.mem_read(0, MEM))), bool(uc.reg_read(UC_ARM_REG_CPSR) & 0x20), [uc.reg_read(r_) for r_ in QREGS], uc.reg_read(UC_ARM_REG_FPSCR) & 0xf7c0009f

PRO = ['stp x19, x20, [sp, #-96]!', 'stp x21, x22, [sp, #16]', 'stp x23, x24, [sp, #32]', 'stp x25, x26, [sp, #48]', 'stp x27, x28, [sp, #64]', 'stp x29, x30, [sp, #80]',
       'mov x27, x0', 'mov x28, x1', 'ldr w16, [x27, #60]', 'msr nzcv, x16']
PRO += ['ldr w%d, [x27, #%d]' % (i, 4 * i) for i in range(15)]
PRO += ['ldp q%d, q%d, [x27, #%d]' % (2 * i, 2 * i + 1, 80 + 32 * i) for i in range(8)]
PRO += ['ldr w16, [x27, #76]', 'and x24, x16, #0xf0000000', 'and x17, x16, #0x07c00000', 'msr fpcr, x17', 'msr fpsr, xzr']
EPI = ['mrs x17, nzcv', 'str w17, [x27, #60]', 'str w21, [x27, #64]'] + ['mrs x15, fpcr', 'and x15, x15, #0x07c00000', 'mrs x17, fpsr', 'movz x20, #0x9f', 'and x17, x17, x20', 'orr x15, x15, x17', 'and x17, x24, #0xf0000000', 'orr x15, x15, x17', 'str w15, [x27, #76]'] + ['stp q%d, q%d, [x27, #%d]' % (2 * i, 2 * i + 1, 80 + 32 * i) for i in range(8)] + ['str w%d, [x27, #%d]' % (i, 4 * i) for i in range(15)] + [
       'ldp x29, x30, [sp, #80]', 'ldp x27, x28, [sp, #64]', 'ldp x25, x26, [sp, #48]', 'ldp x23, x24, [sp, #32]', 'ldp x21, x22, [sp, #16]', 'ldp x19, x20, [sp], #96', 'ret']

def asm_test(k, lines, targets):
    s = ['.globl _t_%d' % k, '.p2align 2', '_t_%d:' % k] + PRO + lines + mov32('w21', ADDR + 4) + ['Lexit_%d:' % k] + EPI
    for t in sorted(targets): s += ['Ltarget_%d_%x:' % (k, t)] + mov32('w21', t) + ['b Lexit_%d' % k]
    return s

def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    only = sys.argv[3:] or list(CLASSES)
    rng = random.Random(seed)
    mem = bytes(rng.getrandbits(8) for _ in range(MEM))
    cases = []; stats = collections.defaultdict(collections.Counter); reasons = collections.Counter()
    for cls in only:
        tried = 0
        while sum(1 for c in cases if c['cls'] == cls) < n and tried < n * 60:
            tried += 1
            w = rand_word(rng, cls)
            k = len(cases)
            tg = set()
            lf = Lifter(lambda a, k=k, tg=tg: (tg.add(a), 'Ltarget_%d_%x' % (k, a))[1], 'Lexit_%d' % k)
            try: lines = lf.lift(w, ADDR)
            except Unsupported as e:
                stats[cls]['unsupported'] += 1; reasons[(cls, str(e).split('%')[0][:48])] += 1; continue
            for _ in range(25):
                regs, nzcv = rand_state(rng); fp = rand_fp_state(rng)
                try: exp = run_unicorn(w, regs, nzcv, mem, fp); break
                except UcError: exp = None
            if exp is None: stats[cls]['unicorn fault'] += 1; continue
            if exp[4]: stats[cls]['thumb target'] += 1; continue          # interworking branch to Thumb: not translated (the game has none)
            cases.append(dict(cls=cls, w=w, regs=regs, nzcv=nzcv, exp=exp, lines=lines, tg=tg, k=k, fp=fp))
        # keep indices dense: renumber below
    if not cases: print('no cases'); return 1
    tmp = tempfile.mkdtemp(prefix='a32test_')
    for i, c in enumerate(cases): c['k'] = i
    # regenerate the lines with the final numbering (labels carry the test number)
    for c in cases:
        c['tg'] = set(); k = c['k']
        lf = Lifter(lambda a, k=k, tg=c['tg']: (tg.add(a), 'Ltarget_%d_%x' % (k, a))[1], 'Lexit_%d' % k)
        c['lines'] = lf.lift(c['w'], ADDR)
    asm = ['.text']
    for c in cases: asm += asm_test(c['k'], c['lines'], c['tg'])
    open(tmp + '/tests.s', 'w').write('\n'.join(asm) + '\n')
    tbl = ['typedef struct S S; typedef void (*TestFn)(S *, unsigned char *);'] + ['extern void t_%d(S *, unsigned char *);' % c['k'] for c in cases]
    tbl += ['const TestFn tests[] = {'] + ['t_%d,' % c['k'] for c in cases] + ['};']
    open(tmp + '/table.c', 'w').write('\n'.join(tbl) + '\n')
    with open(tmp + '/tests.bin', 'wb') as f:
        for c in cases: f.write(struct.pack('<18I', *c['regs'], c['nzcv'], c['w'], c['fp'][1]) + b''.join(struct.pack('<Q', v) for v in c['fp'][0]))
    open(tmp + '/mem.bin', 'wb').write(mem)
    r = subprocess.run(['clang', '-O1', '-o', tmp + '/harness', HERE + '/harness.c', tmp + '/table.c', tmp + '/tests.s', '-lz'], capture_output=True, text=True)
    if r.returncode: print(r.stderr[:3000]); print('asm in', tmp); return 1
    r = subprocess.run([tmp + '/harness', tmp + '/tests.bin', tmp + '/mem.bin', tmp + '/out.bin', str(len(cases))], capture_output=True, text=True)
    if r.returncode: print('harness failed', r.returncode, r.stderr[:500]); return 1
    out = open(tmp + '/out.bin', 'rb').read()
    bad = collections.defaultdict(list)
    for c in cases:
        REC = 336
        got = struct.unpack_from('<20I', out, c['k'] * REC); gq = struct.unpack_from('<32Q', out, c['k'] * REC + 80)
        eregs, enzcv, epc, ecrc, _, eq, efpscr = c['exp']
        problems = []
        if got[18]: problems.append('segfault')
        else:
            for i in range(15):
                if got[i] != eregs[i]: problems.append('r%d got %08x want %08x' % (i, got[i], eregs[i]))
            if got[15] & 0xf0000000 != enzcv: problems.append('nzcv got %x want %x' % (got[15] >> 28, enzcv >> 28))
            if got[16] != epc: problems.append('pc got %08x want %08x' % (got[16], epc))
            if got[17] != ecrc: problems.append('memory differs')
            if got[19] != efpscr: problems.append('fpscr got %08x want %08x' % (got[19], efpscr))
            for i in range(32):
                if gq[i] != eq[i]: problems.append('d%d got %016x want %016x' % (i, gq[i], eq[i]))
        if problems: bad[c['cls']].append((c, problems))
        else: stats[c['cls']]['pass'] += 1
    for cls in only:
        st = stats[cls]
        print('%-7s pass %5d  FAIL %4d  unsupported %6d  unicorn-fault %5d  thumb %3d' % (cls, st['pass'], len(bad[cls]), st['unsupported'], st['unicorn fault'], st['thumb target']))
    for cls, lst in bad.items():
        for c, p in lst[:3]:
            print('  FAIL %s %08x cond=%d regs=%s nzcv=%x\n     %s\n     %s' % (cls, c['w'], c['w'] >> 28, ' '.join('%x' % v for v in c['regs']), c['nzcv'] >> 28, '; '.join(p[:4]), ' | '.join(c['lines'])))
    print('top unsupported reasons:', reasons.most_common(8)); print('work dir', tmp)
    return 1 if bad else 0

if __name__ == '__main__': sys.exit(main())
