#!/usr/bin/env python3
"""Feasibility census of a 32-bit (AArch32) NSO: recursive ARM/Thumb discovery and instruction classes.
Usage: a32_census.py <ExeFS dir> <module> [<module> ...]   (needs capstone + lz4; nso_syms.load from the nxnative repo)"""
import collections, os, struct, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
import nso_syms
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB, CS_MODE_LITTLE_ENDIAN
from capstone.arm import *

def parse(path):
    d = open(path, 'rb').read()
    text_off, text_sz = struct.unpack_from('<II', d, 0x14)[0], struct.unpack_from('<I', d, 0x18)[0]
    mem_off = struct.unpack_from('<I', d, 0x14)[0]
    segs = [struct.unpack_from('<III', d, 0x10 + i * 0x10) for i in range(3)]
    return nso_syms.load(path), segs

def dyn32(img):
    mod0 = struct.unpack_from('<I', img, 4)[0]
    dyn = mod0 + struct.unpack_from('<i', img, mod0 + 4)[0]
    tags = {}
    for i in range(4096):
        t, v = struct.unpack_from('<II', img, dyn + i * 8)
        if t == 0: break
        tags.setdefault(t, v)
    return tags

def seeds(img, text_lo, text_hi):
    tags = dyn32(img)
    S = {}
    def add(v, why):
        a, th = v & ~1, v & 1
        if text_lo <= a < text_hi: S.setdefault((a, th), why)
    S[(0, 0)] = 'entry'   # NSO text starts with a branch to the entry
    # dynamic symbols defined in the module
    if 6 in tags and 5 in tags:
        symtab, strtab = tags[6], tags[5]
        n = struct.unpack_from('<I', img, tags[4] + 4)[0] if 4 in tags else (strtab - symtab) // 16
        for i in range(1, n):
            name, value, size, info, other, shndx = struct.unpack_from('<IIIBBH', img, symtab + i * 16)
            if shndx and (info & 0xf) in (0, 2): add(value, 'dynsym')
    # relative relocations (vtables, function pointer tables): the stored word is a code address
    rel = tags.get(17); relsz = tags.get(18, 0)
    nrel = 0
    if rel:
        for i in range(relsz // 8):
            off, info = struct.unpack_from('<II', img, rel + i * 8)
            if info & 0xff == 23 and off + 4 <= len(img):   # R_ARM_RELATIVE
                v = struct.unpack_from('<I', img, off)[0]; add(v, 'reloc'); nrel += 1
    for tag, szt in ((25, 27), (26, 28)):   # init_array, fini_array
        if tag in tags:
            for i in range(tags.get(szt, 0) // 4): add(struct.unpack_from('<I', img, tags[tag] + i * 4)[0], 'init')
    return S, tags, nrel

LR, PC = ARM_REG_LR, ARM_REG_PC
def census(name, path):
    t0 = time.time()
    img, segs = parse(path)
    text_lo, text_hi = segs[0][1], segs[0][1] + segs[0][2]
    S, tags, nrel = seeds(img, text_lo, text_hi)
    md = {0: Cs(CS_ARCH_ARM, CS_MODE_ARM | CS_MODE_LITTLE_ENDIAN), 1: Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_LITTLE_ENDIAN)}
    for m in md.values(): m.detail = True
    seen = set(); seen_fn = set(); work = list(S); cov = {0: 0, 1: 0}
    st = collections.Counter(); insn_mode = collections.Counter(); grp = collections.Counter()
    mn_special = collections.Counter(); cond_arm = 0; undec = 0
    while work:
        a, th = work.pop()
        if (a, th) in seen_fn: continue
        seen_fn.add((a, th))
        while text_lo <= a < text_hi:
            if (a, th) in seen: break
            code = img[a:a + 64]
            ins = next(md[th].disasm(code, a, 1), None)
            if ins is None:
                undec += 1; break
            seen.add((a, th)); cov[th] += ins.size; insn_mode[th] += 1
            mn = ins.mnemonic.split('.')[0]
            gnames = {ins.group_name(g) for g in ins.groups}
            for g in gnames & {'vfp2', 'vfp3', 'vfp4', 'neon', 'fparmv8', 'crypto', 'thumb2dsp', 'dsp', 'virtualization', 'mp', 'trustzone'}: grp[g] += 1
            if th == 0 and ins.cc not in (ARM_CC_AL, ARM_CC_INVALID): cond_arm += 1
            if mn in ('ldrex', 'strex', 'ldrexb', 'strexb', 'ldrexh', 'strexh', 'ldrexd', 'strexd', 'clrex', 'ldaex', 'stlex', 'lda', 'stl'): mn_special['exclusive/acq-rel'] += 1
            elif mn in ('dmb', 'dsb', 'isb'): mn_special['barrier'] += 1
            elif mn in ('mrc', 'mcr', 'mrrc', 'mcrr', 'cdp'): mn_special['coprocessor'] += 1
            elif mn == 'svc': mn_special['svc'] += 1
            elif mn == 'it' or mn.startswith('it') and len(mn) <= 5 and set(mn[2:]) <= {'t', 'e'}: mn_special['IT block'] += 1
            elif mn in ('udf', 'bkpt'): mn_special['udf/bkpt'] += 1
            elif mn in ('tbb', 'tbh'): mn_special['tbb/tbh jump table'] += 1
            ops = ins.operands
            nxt = a + ins.size; stop = False
            is_branch = mn in ('b', 'bl', 'blx', 'bx', 'cbz', 'cbnz', 'bxj')
            if mn in ('b', 'bl', 'cbz', 'cbnz') or (mn == 'blx' and ops and ops[-1].type == ARM_OP_IMM):
                tgt = ops[-1].imm
                if mn == 'blx': ntm = 1 - th
                else: ntm = th
                if mn == 'blx' or mn == 'bl': st['direct call'] += 1
                else: st['direct branch'] += 1
                if text_lo <= tgt < text_hi: work.append((tgt, ntm))
                if mn == 'b' and ins.cc in (ARM_CC_AL, ARM_CC_INVALID): stop = True
            elif mn == 'bx':
                if ops and ops[0].type == ARM_OP_REG and ops[0].reg == LR: st['return (bx lr)'] += 1
                else: st['indirect jump (bx reg)'] += 1
                if ins.cc in (ARM_CC_AL, ARM_CC_INVALID): stop = True
            elif mn == 'blx': st['indirect call (blx reg)'] += 1
            elif mn in ('pop', 'ldm', 'ldmia', 'ldmdb', 'ldmda', 'ldmib', 'ldr', 'mov', 'add', 'sub', 'ldrd') and ops and any(o.type == ARM_OP_REG and o.reg == PC for o in ops[:1] if mn in ('mov', 'add', 'sub', 'ldr')) or (mn in ('pop', 'ldm', 'ldmia', 'ldmdb', 'ldmda', 'ldmib') and any(o.type == ARM_OP_REG and o.reg == PC for o in ops)):
                if mn in ('pop',) or mn.startswith('ldm'): st['return (pop/ldm pc)'] += 1
                elif mn == 'ldr' and len(ops) > 1 and ops[1].type == ARM_OP_MEM and ops[1].mem.base == PC and ops[1].mem.index == 0 and mn == 'ldr' and False: pass
                else: st['indirect jump (%s pc)' % mn] += 1
                if ins.cc in (ARM_CC_AL, ARM_CC_INVALID): stop = True
            if stop: break
            a = nxt
    total = cov[0] + cov[1]
    n = insn_mode[0] + insn_mode[1]
    print('== %s: text 0x%x-0x%x (%.1f MB), seeds %d (%d relative relocs), functions explored %d' % (name, text_lo, text_hi, (text_hi - text_lo) / 1048576, len(S), nrel, len(seen_fn)))
    print('   instructions found %d: ARM %d (%.0f%%), Thumb %d (%.0f%%); bytes covered %.1f MB = %.0f%% of text; undecodable stops %d' % (n, insn_mode[0], 100 * insn_mode[0] / max(n, 1), insn_mode[1], 100 * insn_mode[1] / max(n, 1), total / 1048576, 100 * total / (text_hi - text_lo), undec))
    print('   conditional ARM instructions (non-AL): %d (%.1f%% of ARM)' % (cond_arm, 100 * cond_arm / max(insn_mode[0], 1)))
    for k, v in sorted(st.items(), key=lambda x: -x[1]): print('   %-32s %d' % (k, v))
    for k, v in sorted(grp.items(), key=lambda x: -x[1]): print('   group %-26s %d (%.1f%%)' % (k, v, 100 * v / max(n, 1)))
    for k, v in sorted(mn_special.items(), key=lambda x: -x[1]): print('   %-32s %d' % (k, v))
    print('   (%.0f s)' % (time.time() - t0)); sys.stdout.flush()

if __name__ == '__main__':
    E = sys.argv[1]
    for m in sys.argv[2:]: census(m, '%s/%s' % (E, m))
