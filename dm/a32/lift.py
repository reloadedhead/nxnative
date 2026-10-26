#!/usr/bin/env python3
"""AArch32 (ARM mode) -> AArch64 assembly lifter, proof of concept for ahead-of-time recompilation of 32-bit Switch titles.

One ARM instruction word at a known guest address becomes a list of AArch64 assembly lines (Apple/clang syntax).
Register mapping:  r0-r14 = x0-x14 (the guest uses the w views, the upper halves stay zero), pc is never a register:
reads of pc become the constant addr + 8, writes of pc become a dispatch (the caller names the label that takes a guest
address in w16). Guest memory is x28 + a 32-bit guest address (`[x28, w15, uxtw]`). Scratch: x15-x17, x19-x21.
The AArch32 NZCV flags are the AArch64 NZCV flags (carry = NOT borrow in both), so add/sub/adc/sbc/cmp/cmn with S map
one to one; the logical instructions with S need the shifter carry-out and keep V (and C when there is no shifter carry).
Anything the lifter does not cover raises Unsupported (counted by the tests, never guessed)."""

from lift_base import Unsupported, INV, CC, M32, mov32, bits, sext, ror32
from vfp import VfpMixin
from neon import NeonMixin

class Lifter(VfpMixin, NeonMixin):
    """lift(word, addr) -> lines. `label(addr)` names the code label of a guest address (direct branches);
    `dispatch` is the label that takes a guest address in w16 (indirect branches, returns)."""
    def __init__(self, label, dispatch):
        self.label, self.dispatch = label, dispatch

    # ---- helpers -------------------------------------------------------------------------------------------------
    def reg(self, n, tmp, out, addr):
        """Name of a w register holding register n for reading; r15 reads as addr + 8 in `tmp`."""
        if n < 15: return 'w%d' % n
        out += mov32(tmp, addr + 8); return tmp

    def wreg(self, n):
        assert n < 15; return 'w%d' % n

    def to_pc(self, src, out):
        # The guest address travels in w21: ld64 inserts branch islands that clobber x16 on branches beyond 128 MB, which these runtime-stub branches can be
        if src != 'w21': out.append('mov w21, %s' % src)
        out.append('b %s' % self.dispatch)

    def flags_logical(self, res, out, carry):
        """N, Z from the 32-bit result `res`; C from w17 (0/1) when carry is True, else unchanged; V unchanged."""
        out += ['mrs x19, nzcv', 'and x19, x19, #%#x' % (0x10000000 if carry else 0x30000000),
                'tst %s, %s' % (res, res), 'mrs x20, nzcv', 'orr x19, x19, x20']
        if carry: out.append('orr x19, x19, x17, lsl #29')
        out.append('msr nzcv, x19')

    # ---- shifter operand -----------------------------------------------------------------------------------------
    def shift_imm(self, rm, typ, n, out, want_carry, addr):
        """Immediate-shifted register into w16 (or the register itself). Returns (operand, carry) where carry is
        True when w17 now holds the shifter carry-out, False when the carry flag is unchanged by the shifter."""
        r = self.reg(rm, 'w16', out, addr)
        if typ == 0:                                   # LSL
            if n == 0: return r, False
            out.append('lsl w16, %s, #%d' % (r, n))
            if want_carry: out.append('ubfx w17, %s, #%d, #1' % (r, 32 - n))
            return 'w16', True
        if typ == 1:                                   # LSR
            if n == 0:
                out.append('mov w16, wzr')
                if want_carry: out.append('lsr w17, %s, #31' % r)
                return 'w16', True
            out.append('lsr w16, %s, #%d' % (r, n))
            if want_carry: out.append('ubfx w17, %s, #%d, #1' % (r, n - 1))
            return 'w16', True
        if typ == 2:                                   # ASR
            if n == 0:
                out.append('asr w16, %s, #31' % r)
                if want_carry: out.append('lsr w17, %s, #31' % r)
                return 'w16', True
            out.append('asr w16, %s, #%d' % (r, n))
            if want_carry: out.append('ubfx w17, %s, #%d, #1' % (r, n - 1))
            return 'w16', True
        if n == 0:                                     # RRX: the carry flag shifts in at the top
            out += ['mrs x19, nzcv', 'ubfx w19, w19, #29, #1', 'lsl w19, w19, #31', 'orr w16, w19, %s, lsr #1' % r]
            if want_carry: out.append('and w17, %s, #1' % r)
            return 'w16', True
        out.append('ror w16, %s, #%d' % (r, n))
        if want_carry: out.append('ubfx w17, %s, #%d, #1' % (r, n - 1))
        return 'w16', True

    def shift_reg(self, rm, typ, rs, out, addr):
        """Register-shifted register (shift amount = low byte of Rs) into w16. Flags are saved around the compare."""
        if 15 in (rm, rs): raise Unsupported('pc in a register-shifted operand')
        out += ['and w17, w%d, #0xff' % rs, 'mrs x19, nzcv']
        if typ == 0:
            out += ['lsl w16, w%d, w17' % rm, 'cmp w17, #32', 'csel w16, wzr, w16, hs']
        elif typ == 1:
            out += ['lsr w16, w%d, w17' % rm, 'cmp w17, #32', 'csel w16, wzr, w16, hs']
        elif typ == 2:
            out += ['mov w21, #31', 'cmp w17, #32', 'csel w17, w21, w17, hs', 'asr w16, w%d, w17' % rm]
        else:
            out.append('ror w16, w%d, w17' % rm)
        out.append('msr nzcv, x19')
        return 'w16'

    def shift_reg_carry(self, rm, typ, rs, out):
        """Register-shifted register with the shifter carry-out (logical instructions with S): value in w16, carry (0/1) in w17.
        Amount 0 keeps the carry flag; the flags are saved in x21 and restored for the flag routine that follows."""
        if 15 in (rm, rs): raise Unsupported('pc in a register-shifted operand')
        R = 'w%d' % rm
        out += ['mrs x21, nzcv', 'and w20, w%d, #0xff' % rs, 'ubfx w17, w21, #29, #1', 'mov w16, %s' % R, 'cbz w20, 9f']
        if typ == 0:                                                              # lsl
            out += ['cmp w20, #32', 'b.hs 5f', 'lsl w16, %s, w20' % R, 'movz w19, #32', 'sub w19, w19, w20', 'lsr w17, %s, w19' % R, 'and w17, w17, #1', 'b 9f',
                    '5:', 'mov w16, wzr', 'and w17, %s, #1' % R, 'csel w17, w17, wzr, eq', 'b 9f']
        elif typ == 1:                                                            # lsr
            out += ['cmp w20, #32', 'b.hs 5f', 'lsr w16, %s, w20' % R, 'sub w19, w20, #1', 'lsr w17, %s, w19' % R, 'and w17, w17, #1', 'b 9f',
                    '5:', 'mov w16, wzr', 'lsr w17, %s, #31' % R, 'csel w17, w17, wzr, eq', 'b 9f']
        elif typ == 2:                                                            # asr
            out += ['cmp w20, #32', 'b.hs 5f', 'asr w16, %s, w20' % R, 'sub w19, w20, #1', 'lsr w17, %s, w19' % R, 'and w17, w17, #1', 'b 9f',
                    '5:', 'asr w16, %s, #31' % R, 'lsr w17, %s, #31' % R, 'b 9f']
        else:                                                                     # ror
            out += ['and w19, w20, #31', 'cbz w19, 5f', 'ror w16, %s, w19' % R, 'sub w19, w19, #1', 'lsr w17, %s, w19' % R, 'and w17, w17, #1', 'b 9f',
                    '5:', 'lsr w17, %s, #31' % R]
        out += ['9:', 'msr nzcv, x21']
        return 'w16'

    def operand2(self, w, out, addr, want_carry):
        """Decode the data-processing shifter operand. Returns (operand, carry) as in shift_imm."""
        if bits(w, 25, 25):
            rot = bits(w, 11, 8) * 2; v = ror32(bits(w, 7, 0), rot)
            out += mov32('w16', v)
            if rot and want_carry: out += mov32('w17', v >> 31)
            return 'w16', bool(rot)
        rm, typ = bits(w, 3, 0), bits(w, 6, 5)
        if bits(w, 4, 4):
            if want_carry: return self.shift_reg_carry(rm, typ, bits(w, 11, 8), out), True
            return self.shift_reg(rm, typ, bits(w, 11, 8), out, addr), False
        return self.shift_imm(rm, typ, bits(w, 11, 7), out, want_carry, addr)

    # ---- instruction classes -------------------------------------------------------------------------------------
    def data_processing(self, w, addr, out):
        op, s, rn, rd = bits(w, 24, 21), bits(w, 20, 20), bits(w, 19, 16), bits(w, 15, 12)
        logical = op in (0, 1, 8, 9, 12, 13, 14, 15)
        if s and rd == 15 and op not in (8, 9, 10, 11): raise Unsupported('S with pc destination (exception return)')
        need_rn = op not in (13, 15)
        a, carry = self.operand2(w, out, addr, bool(s and logical))
        n = self.reg(rn, 'w15', out, addr) if need_rn else None
        tests = op in (8, 9, 10, 11)
        d = 'w%d' % rd if rd < 15 else 'w15' if not tests else 'wzr'
        sfx = 's' if s else ''
        if op == 0 or op == 8: out.append('and %s, %s, %s' % ('w21' if tests else d, n, a))
        elif op == 1 or op == 9: out.append('eor %s, %s, %s' % ('w21' if tests else d, n, a))
        elif op == 12: out.append('orr %s, %s, %s' % (d, n, a))
        elif op == 14: out.append('bic %s, %s, %s' % (d, n, a))
        elif op == 13: out.append('mov %s, %s' % (d, a))
        elif op == 15: out.append('mvn %s, %s' % (d, a))
        elif op == 2: out.append('sub%s %s, %s, %s' % (sfx, d, n, a))
        elif op == 3: out.append('sub%s %s, %s, %s' % (sfx, d, a, n))
        elif op == 4: out.append('add%s %s, %s, %s' % (sfx, d, n, a))
        elif op == 5: out.append('adc%s %s, %s, %s' % (sfx, d, n, a))
        elif op == 6: out.append('sbc%s %s, %s, %s' % (sfx, d, n, a))
        elif op == 7: out.append('sbc%s %s, %s, %s' % (sfx, d, a, n))
        elif op == 10: out.append('cmp %s, %s' % (n, a))
        elif op == 11: out.append('cmn %s, %s' % (n, a))
        if s and logical: self.flags_logical('w21' if tests else d, out, carry)
        if rd == 15 and not tests: self.to_pc('w15', out)

    def multiply(self, w, addr, out):
        rd, ra, rm, rn, acc, s = bits(w, 19, 16), bits(w, 15, 12), bits(w, 11, 8), bits(w, 3, 0), bits(w, 21, 21), bits(w, 20, 20)
        if 15 in (rd, rm, rn) or (acc and ra == 15): raise Unsupported('pc in multiply')
        if acc: out.append('madd w%d, w%d, w%d, w%d' % (rd, rn, rm, ra))
        else: out.append('mul w%d, w%d, w%d' % (rd, rn, rm))
        if s: self.flags_logical('w%d' % rd, out, False)

    def long_multiply(self, w, addr, out):
        hi, lo, rm, rn = bits(w, 19, 16), bits(w, 15, 12), bits(w, 11, 8), bits(w, 3, 0)
        signed, acc, s = bits(w, 22, 22), bits(w, 21, 21), bits(w, 20, 20)
        if s: raise Unsupported('long multiply with S')
        if 15 in (hi, lo, rm, rn) or hi == lo: raise Unsupported('pc or equal halves in long multiply')
        if acc: out += ['mov w17, w%d' % lo, 'orr x17, x17, x%d, lsl #32' % hi]
        base = 'smaddl' if signed else 'umaddl'
        if acc: out.append('%s x16, w%d, w%d, x17' % (base, rn, rm))
        else: out.append('%s x16, w%d, w%d, xzr' % (base, rn, rm))
        out += ['mov w%d, w16' % lo, 'lsr x16, x16, #32', 'mov w%d, w16' % hi]

    def single_transfer(self, w, addr, out):
        p, u, b, wb, l = (bits(w, i, i) for i in (24, 23, 22, 21, 20))
        rn, rt = bits(w, 19, 16), bits(w, 15, 12)
        if bits(w, 25, 25) and bits(w, 4, 4): raise Unsupported('media instruction')
        if rt == 15 and not l: raise Unsupported('str pc')
        if (not p) and wb: raise Unsupported('translated access (ldrt/strt)')
        writeback = (not p) or wb
        if writeback and (rn == 15 or rn == rt): raise Unsupported('unpredictable writeback')
        base = self.reg(rn, 'w15', out, addr)
        if bits(w, 25, 25):
            rm = bits(w, 3, 0)
            if rm == 15: raise Unsupported('pc offset register')
            off, _ = self.shift_imm(rm, bits(w, 6, 5), bits(w, 11, 7), out, False, addr)
            out.append('%s w15, %s, %s' % ('add' if u else 'sub', base, off))
        else:
            imm = bits(w, 11, 0)
            out.append('%s w15, %s, #%d' % ('add' if u else 'sub', base, imm) if imm else 'mov w15, %s' % base)
        ea = 'w15' if p else base
        if not p and base == 'w15': raise Unsupported('post-index from pc')
        if p:
            acc_ea = 'w15'
        else:
            # post-indexed: the access uses the old base; the new base sits in w15
            out.append('mov w17, %s' % base); acc_ea = 'w17'
        sz = 'b' if b else ''
        if l:
            tgt = 'w21' if rt == 15 else 'w%d' % rt
            out.append('ldr%s %s, [x28, %s, uxtw]' % (sz, tgt, acc_ea))
        else:
            out.append('str%s w%d, [x28, %s, uxtw]' % (sz, rt, acc_ea))
        if writeback: out.append('mov w%d, w15' % rn)
        if l and rt == 15: self.to_pc('w21', out)

    def extra_transfer(self, w, addr, out):
        """Halfword, signed byte/halfword and doubleword transfers."""
        p, u, i, wb, l = (bits(w, k, k) for k in (24, 23, 22, 21, 20))
        rn, rt, sh = bits(w, 19, 16), bits(w, 15, 12), bits(w, 6, 5)
        if (not p) and wb: raise Unsupported('translated access')
        writeback = (not p) or wb
        if writeback and (rn == 15 or rn == rt): raise Unsupported('unpredictable writeback')
        dbl = sh in (2, 3) and not l
        if dbl and (rt & 1 or rt == 14): raise Unsupported('ldrd/strd register pair')
        if dbl and writeback and rn in (rt, rt + 1): raise Unsupported('unpredictable writeback')
        if dbl and not i and bits(w, 3, 0) in (rt, rt + 1): raise Unsupported('unpredictable ldrd/strd offset register')
        if 15 in (rt,) or (dbl and rt + 1 == 15): raise Unsupported('pc transfer')
        base = self.reg(rn, 'w15', out, addr)
        if i:
            imm = (bits(w, 11, 8) << 4) | bits(w, 3, 0)
            out.append('%s w15, %s, #%d' % ('add' if u else 'sub', base, imm) if imm else 'mov w15, %s' % base)
        else:
            rm = bits(w, 3, 0)
            if rm == 15 or bits(w, 11, 8): raise Unsupported('pc offset register')
            out.append('%s w15, %s, w%d' % ('add' if u else 'sub', base, rm))
        if p: acc_ea = 'w15'
        else:
            if base == 'w15': raise Unsupported('post-index from pc')
            out.append('mov w17, %s' % base); acc_ea = 'w17'
        if sh == 1:
            out.append(('ldrh w%d' if l else 'strh w%d') % rt + ', [x28, %s, uxtw]' % acc_ea)
        elif sh == 2 and l: out.append('ldrsb w%d, [x28, %s, uxtw]' % (rt, acc_ea))
        elif sh == 3 and l: out.append('ldrsh w%d, [x28, %s, uxtw]' % (rt, acc_ea))
        elif sh == 2:                                  # ldrd
            out += ['ldr w%d, [x28, %s, uxtw]' % (rt, acc_ea), 'add w16, %s, #4' % acc_ea, 'ldr w%d, [x28, w16, uxtw]' % (rt + 1)]
        else:                                          # strd
            out += ['str w%d, [x28, %s, uxtw]' % (rt, acc_ea), 'add w16, %s, #4' % acc_ea, 'str w%d, [x28, w16, uxtw]' % (rt + 1)]
        if sh == 2 and not l:
            pass
        if writeback: out.append('mov w%d, w15' % rn)

    def block_transfer(self, w, addr, out):
        p, u, s, wb, l = (bits(w, k, k) for k in (24, 23, 22, 21, 20))
        rn, lst = bits(w, 19, 16), bits(w, 15, 0)
        regs = [r for r in range(16) if lst >> r & 1]
        if s: raise Unsupported('ldm/stm with S')
        if not regs or rn == 15: raise Unsupported('empty list or pc base')
        if wb and (rn in regs if l else False): raise Unsupported('unpredictable writeback')
        if not l and 15 in regs: raise Unsupported('stm pc')
        if not l and wb and rn in regs and regs[0] != rn: raise Unsupported('unpredictable store of the base')
        n = len(regs)
        base = 'w%d' % rn
        if u: start = 4 if p else 0
        else: start = -4 * n if p else -4 * n + 4
        if start > 0: out.append('add w15, %s, #%d' % (base, start))
        elif start < 0: out.append('sub w15, %s, #%d' % (base, -start))
        else: out.append('mov w15, %s' % base)
        for k, r in enumerate(regs):
            if k: out.append('add w15, w15, #4')
            if l: out.append('ldr %s, [x28, w15, uxtw]' % ('w21' if r == 15 else 'w%d' % r))
            else: out.append('str w%d, [x28, w15, uxtw]' % r)
        if wb: out.append('%s %s, %s, #%d' % ('add' if u else 'sub', base, base, 4 * n))
        if l and 15 in regs: self.to_pc('w21', out)

    def branch(self, w, addr, out):
        tgt = (addr + 8 + sext(bits(w, 23, 0), 24) * 4) & M32
        if bits(w, 24, 24): out += mov32('w14', addr + 4)
        out.append('b %s' % self.label(tgt))

    def misc(self, w, addr, out):
        """Branch-and-exchange, clz, rev, extends, bitfields, movw/movt. Returns True when it handled the word."""
        if self.half_multiply(w, addr, out): return True
        if w & 0x0f900df0 == 0x01000040 and bits(w, 22, 21) < 3:                  # crc32{b,h,w} / crc32c{b,h,w} (ARMv8 CRC extension): the accumulator is Rn
            rd, rn, rm, c = bits(w, 15, 12), bits(w, 19, 16), bits(w, 3, 0), bits(w, 9, 9)
            if 15 in (rd, rn, rm): raise Unsupported('pc in crc32')
            out.append('crc32%s%s w%d, w%d, w%d' % ('c' if c else '', 'bhw'[bits(w, 22, 21)], rd, rn, rm)); return True
        if w & 0x0f900ff0 == 0x01000050:                                      # qadd / qsub / qdadd / qdsub (the Q flag is not modeled)
            op, rd, rn, rm = bits(w, 22, 21), bits(w, 15, 12), bits(w, 19, 16), bits(w, 3, 0)
            if 15 in (rd, rn, rm): raise Unsupported('pc in saturating add')
            out.append('mrs x19, nzcv')
            def sat(a, b, sub, dst):                                          # dst = signed-saturate(a +/- b); on overflow the sign of a picks the limit
                return ['%s w20, %s, %s' % ('subs' if sub else 'adds', a, b), 'asr w21, %s, #31' % a, 'eor w21, w21, #0x7fffffff', 'csel %s, w21, w20, vs' % dst]
            if op >= 2:                                                       # qd*: the second operand (Rn) is doubled with saturation first
                out += sat('w%d' % rn, 'w%d' % rn, 0, 'w17')
                out += sat('w%d' % rm, 'w17', op == 3, 'w%d' % rd) if True else []
            else: out += sat('w%d' % rm, 'w%d' % rn, op == 1, 'w%d' % rd)
            out.append('msr nzcv, x19'); return True
        if w & 0x0ff000f0 == 0x00400090:                                      # umaal
            hi, lo, rm, rn = bits(w, 19, 16), bits(w, 15, 12), bits(w, 11, 8), bits(w, 3, 0)
            if 15 in (hi, lo, rm, rn) or hi == lo: raise Unsupported('umaal')
            out += ['umull x16, w%d, w%d' % (rn, rm), 'mov w17, w%d' % lo, 'add x16, x16, x17', 'mov w17, w%d' % hi, 'add x16, x16, x17',
                    'mov w%d, w16' % lo, 'lsr x16, x16, #32', 'mov w%d, w16' % hi]; return True
        if w & 0x0ffffff0 == 0x012fff10:               # bx
            rm = bits(w, 3, 0); self.to_pc(self.reg(rm, 'w21', out, addr), out); return True
        if w & 0x0ffffff0 == 0x012fff30:               # blx register
            rm = bits(w, 3, 0)
            if rm == 15: out += mov32('w21', addr + 8)
            else: out.append('mov w21, w%d' % rm)              # blx lr reads lr before it is overwritten
            out += mov32('w14', addr + 4); out.append('b %s' % self.dispatch); return True
        if w & 0x0fff0ff0 == 0x016f0f10:              # clz
            rd, rm = bits(w, 15, 12), bits(w, 3, 0)
            if 15 in (rd, rm): raise Unsupported('pc in clz')
            out.append('clz w%d, w%d' % (rd, rm)); return True
        if w & 0x0fff0ff0 == 0x06bf0f30:              # rev
            rd, rm = bits(w, 15, 12), bits(w, 3, 0)
            if 15 in (rd, rm): raise Unsupported('pc in rev')
            out.append('rev w%d, w%d' % (rd, rm)); return True
        if w & 0x0fa003f0 == 0x06a00070:                                       # uxtb/uxth/sxtb/sxth (Rn = pc) and the accumulating uxtab/uxtah/sxtab/sxtah
            kind = {0x6e: 'uxtb', 0x6f: 'uxth', 0x6a: 'sxtb', 0x6b: 'sxth'}.get(bits(w, 27, 20))
            if kind:
                rd, rm, rn, rot = bits(w, 15, 12), bits(w, 3, 0), bits(w, 19, 16), bits(w, 11, 10) * 8
                if 15 in (rd, rm): raise Unsupported('pc in extend')
                src = 'w%d' % rm
                if rot: out.append('ror w16, w%d, #%d' % (rm, rot)); src = 'w16'
                if rn == 15: out.append('%s w%d, %s' % (kind, rd, src))
                else: out += ['%s w16, %s' % (kind, src), 'add w%d, w%d, w16' % (rd, rn)]
                return True
        if w & 0x0ff00030 == 0x06800010:                                       # pkhbt / pkhtb
            rn, rd, rm, imm, tb = bits(w, 19, 16), bits(w, 15, 12), bits(w, 3, 0), bits(w, 11, 7), bits(w, 6, 6)
            if 15 in (rn, rd, rm): raise Unsupported('pc in pkh')
            if not tb: out += ['lsl w16, w%d, #%d' % (rm, imm), 'and w16, w16, #0xffff0000', 'and w17, w%d, #0xffff' % rn, 'orr w%d, w16, w17' % rd] if imm else ['and w16, w%d, #0xffff0000' % rm, 'and w17, w%d, #0xffff' % rn, 'orr w%d, w16, w17' % rd]
            else: out += ['asr w16, w%d, #%d' % (rm, imm or 32 if imm else 31), 'and w16, w16, #0xffff', 'and w17, w%d, #0xffff0000' % rn, 'orr w%d, w16, w17' % rd]
            return True
        if bits(w, 27, 20) in (0x68, 0x6c) and bits(w, 9, 4) == 0x07:           # sxtb16 / uxtb16 and the accumulating sxtab16 / uxtab16
            rd, rm, rn, rot, uns = bits(w, 15, 12), bits(w, 3, 0), bits(w, 19, 16), bits(w, 11, 10) * 8, bits(w, 22, 22)
            if 15 in (rd, rm): raise Unsupported('pc in extend16')
            out.append('ror w16, w%d, #%d' % (rm, rot) if rot else 'mov w16, w%d' % rm)
            if uns: out.append('and w16, w16, #0x00ff00ff')
            else: out += ['sxtb w17, w16', 'and w17, w17, #0xffff', 'sbfx w16, w16, #16, #8', 'lsl w16, w16, #16', 'orr w16, w16, w17']
            if rn == 15: out.append('mov w%d, w16' % rd)
            else: out += ['and w17, w%d, #0xffff' % rn, 'and w15, w16, #0xffff', 'add w17, w17, w15', 'and w17, w17, #0xffff', 'and w15, w%d, #0xffff0000' % rn, 'and w16, w16, #0xffff0000', 'add w15, w15, w16', 'orr w%d, w15, w17' % rd]
            return True
        if bits(w, 27, 21) in (0x35, 0x37) and bits(w, 5, 4) == 1:   # ssat / usat Rd, #sat, Rn{, shift}
            uns = bits(w, 22, 22); rd, rn, sat, imm, sh = bits(w, 15, 12), bits(w, 3, 0), bits(w, 20, 16), bits(w, 11, 7), bits(w, 6, 6)
            if 15 in (rd, rn): raise Unsupported('pc in ssat')
            n = sat + (0 if uns else 1)
            src = 'w%d' % rn
            if imm or sh:
                out.append('asr w16, w%d, #%d' % (rn, imm or 32) if sh and imm else ('asr w16, w%d, #31' % rn if sh else 'lsl w16, w%d, #%d' % (rn, imm))); src = 'w16'
            hi = (1 << n) - 1 if uns else (1 << (n - 1)) - 1; lo = 0 if uns else -(1 << (n - 1))
            out += ['dup v16.4s, %s' % src] + mov32('w17', hi & 0xffffffff) + ['dup v17.4s, w17'] + mov32('w17', lo & 0xffffffff) + ['dup v18.4s, w17', 'smin v16.4s, v16.4s, v17.4s', 'smax v16.4s, v16.4s, v18.4s', 'mov w%d, v16.s[0]' % rd]
            return True
        if bits(w, 27, 21) in (0x3d, 0x3f) and bits(w, 6, 4) == 5:     # sbfx / ubfx
            rd, rn, lsb, wm1 = bits(w, 15, 12), bits(w, 3, 0), bits(w, 11, 7), bits(w, 20, 16)
            if 15 in (rd, rn) or lsb + wm1 > 31: raise Unsupported('bitfield extract')
            out.append('%s w%d, w%d, #%d, #%d' % ('ubfx' if bits(w, 22, 22) else 'sbfx', rd, rn, lsb, wm1 + 1)); return True
        if bits(w, 27, 21) == 0x3e and bits(w, 6, 4) == 1:             # bfi / bfc
            rd, rn, lsb, msb = bits(w, 15, 12), bits(w, 3, 0), bits(w, 11, 7), bits(w, 20, 16)
            if rd == 15 or msb < lsb: raise Unsupported('bitfield insert')
            if rn == 15: out.append('bfc w%d, #%d, #%d' % (rd, lsb, msb - lsb + 1))
            else: out.append('bfi w%d, w%d, #%d, #%d' % (rd, rn, lsb, msb - lsb + 1))
            return True
        if bits(w, 27, 20) in (0x30, 0x34):            # movw / movt
            rd, imm = bits(w, 15, 12), (bits(w, 19, 16) << 12) | bits(w, 11, 0)
            if rd == 15: raise Unsupported('movw pc')
            out.append('movz w%d, #%d' % (rd, imm) if bits(w, 27, 20) == 0x30 else 'movk w%d, #%d, lsl #16' % (rd, imm)); return True
        if w & 0x0ff0f0f0 == 0x0730f010 or w & 0x0ff0f0f0 == 0x0710f010:      # udiv / sdiv
            rd, rm, rn = bits(w, 19, 16), bits(w, 11, 8), bits(w, 3, 0)
            if 15 in (rd, rm, rn): raise Unsupported('pc in divide')
            out.append('%s w%d, w%d, w%d' % ('udiv' if bits(w, 21, 21) else 'sdiv', rd, rn, rm)); return True
        if w & 0x0ff000f0 == 0x00600090:                                      # mls
            rd, ra, rm, rn = bits(w, 19, 16), bits(w, 15, 12), bits(w, 11, 8), bits(w, 3, 0)
            if 15 in (rd, ra, rm, rn): raise Unsupported('pc in mls')
            out.append('msub w%d, w%d, w%d, w%d' % (rd, rn, rm, ra)); return True
        if w & 0x0fff0ff0 == 0x06ff0f30:                                      # rbit
            rd, rm = bits(w, 15, 12), bits(w, 3, 0)
            if 15 in (rd, rm): raise Unsupported('pc in rbit')
            out.append('rbit w%d, w%d' % (rd, rm)); return True
        if w & 0x0fff0ff0 == 0x06bf0fb0 or w & 0x0fff0ff0 == 0x06ff0fb0:     # rev16 / revsh
            rd, rm = bits(w, 15, 12), bits(w, 3, 0)
            if 15 in (rd, rm): raise Unsupported('pc in rev16')
            out.append('rev16 w%d, w%d' % (rd, rm))
            if bits(w, 22, 22): out.append('sxth w%d, w%d' % (rd, rd))
            return True
        return False

    def half_multiply(self, w, addr, out):
        """smulxy / smlaxy / smulwy / smlawy / smmul / smmla (the signed 16-bit and most-significant-word multiplies). True if handled."""
        top = bits(w, 27, 20)
        rd, ra, rm, rn = bits(w, 19, 16), bits(w, 15, 12), bits(w, 11, 8), bits(w, 3, 0)
        x, y = bits(w, 5, 5), bits(w, 6, 6)
        def half(r, hi, t):
            out.append('%s %s, w%d, #%d, #16' % ('sbfx', t, r, 16 if hi else 0)); return t
        if top in (0x10, 0x16) and bits(w, 7, 7) and not bits(w, 4, 4) and (top == 0x10 or ra == 0):    # smlaxy / smulxy
            if 15 in (rd, rm, rn) or (top == 0x10 and ra == 15): raise Unsupported('pc in smla')
            a, b = half(rn, x, 'w16'), half(rm, y, 'w17')
            if top == 0x10: out += ['smaddl x16, %s, %s, xzr' % (a, b), 'add w%d, w16, w%d' % (rd, ra)]   # the accumulate does not saturate (the Q flag is not modeled)
            else: out.append('mul w%d, %s, %s' % (rd, a, b))
            return True
        if top == 0x12 and bits(w, 7, 7) and not bits(w, 4, 4):                                        # smulwy (bit 5 set, Ra = 0) / smlawy (bit 5 clear)
            if 15 in (rd, rm, rn) or (not x and ra == 15): raise Unsupported('pc in smulw')
            if x and ra != 0: raise Unsupported('smulw with Ra')
            b = half(rm, y, 'w17')
            out += ['smull x16, w%d, %s' % (rn, b), 'asr x16, x16, #16']
            if not x: out.append('add w16, w16, w%d' % ra)
            out.append('mov w%d, w16' % rd); return True
        if top == 0x75 and bits(w, 7, 6) == 0 and bits(w, 4, 4) == 1:   # smmul / smmla (and the rounding forms)
            if 15 in (rd, rm, rn): raise Unsupported('pc in smmul')
            rnd = bits(w, 5, 5)
            if ra == 15: out.append('smull x16, w%d, w%d' % (rn, rm))
            else: out += ['smull x16, w%d, w%d' % (rn, rm), 'lsl x17, x%d, #32' % ra, 'add x16, x16, x17']
            if rnd: out += ['movz x19, #0x8000, lsl #16', 'add x16, x16, x19']
            out += ['lsr x16, x16, #32', 'mov w%d, w16' % rd]; return True
        return False

    def exclusive(self, w, addr, out):
        """ldrex / strex families and the acquire / release forms. True if handled."""
        top = bits(w, 27, 20)
        if bits(w, 27, 24) != 1 or bits(w, 23, 23) != 1: return False
        rn = bits(w, 19, 16); op = bits(w, 22, 21); ld = bits(w, 20, 20)
        if rn == 15: raise Unsupported('pc base exclusive')
        sfx = {0: '', 1: 'p', 2: 'b', 3: 'h'}[op]
        # bits 11:8: 1111 = ldrex/strex, 1110 = ldaex/stlex, 1100 = lda/stl ; bits 7:4 = 1001
        if bits(w, 7, 4) != 9: return False
        kind = bits(w, 11, 8)
        if kind not in (0xf, 0xe, 0xc): return False
        out.append('add x15, x28, w%d, uxtw' % rn)
        if ld:
            rt = bits(w, 15, 12)
            if rt == 15: raise Unsupported('pc target exclusive')
            if kind == 0xc:
                if op == 1: return False
                out.append('ldar%s w%d, [x15]' % ({0: '', 2: 'b', 3: 'h'}[op], rt)); return True
            mn = {0xf: 'ldxr', 0xe: 'ldaxr'}[kind]
            if op == 1:
                if rt & 1 or rt == 14: raise Unsupported('ldrexd pair')
                out.append('%s w%d, w%d, [x15]' % (mn.replace('r', 'p'), rt, rt + 1))
            else: out.append('%s%s w%d, [x15]' % (mn, {0: '', 2: 'b', 3: 'h'}[op], rt))
            return True
        rd, rt = bits(w, 15, 12), bits(w, 3, 0)
        if kind == 0xc:
            if op == 1: return False
            out.append('stlr%s w%d, [x15]' % ({0: '', 2: 'b', 3: 'h'}[op], rt)); return True
        if 15 in (rd, rt): raise Unsupported('pc in strex')
        if rd == rt or rd == rn: raise Unsupported('unpredictable strex')
        mn = {0xf: 'stxr', 0xe: 'stlxr'}[kind]
        if op == 1:
            if rt & 1 or rt == 14 or rd == rt + 1: raise Unsupported('strexd pair')
            out.append('%s w%d, w%d, w%d, [x15]' % (mn.replace('r', 'p'), rd, rt, rt + 1))
        else: out.append('%s%s w%d, w%d, [x15]' % (mn, {0: '', 2: 'b', 3: 'h'}[op], rd, rt))
        return True

    def system(self, w, addr, out):
        """svc, the thread-pointer read, barriers. True if handled."""
        cond = bits(w, 31, 28)
        if cond == 15:
            if w & 0xfffffff0 == 0xf57ff050: out.append('dmb ish'); return True
            if w & 0xfffffff0 == 0xf57ff040: out.append('dsb ish'); return True
            if w & 0xfffffff0 == 0xf57ff060: out.append('isb'); return True
            if w == 0xf57ff01f: out.append('clrex'); return True
            if bits(w, 27, 24) == 5 and bits(w, 15, 12) == 15: out.append('nop'); return True        # pld / pli
            return False
        if bits(w, 27, 24) == 15: out += mov32('w21', bits(w, 23, 0)); out.append('bl _a32_svc_gate'); return True
        if w & 0x0ff00fee == 0x0c500f0e:                                      # mrrc p15, 1, Rt, Rt2, c14 (CNTVCT: the system counter, 19.2 MHz on the Switch, 24 MHz here)
            rt, rt2 = bits(w, 15, 12), bits(w, 19, 16)
            if 15 in (rt, rt2) or rt == rt2: raise Unsupported('cntvct into pc')
            out += ['mrs x16, cntvct_el0', 'lsl x16, x16, #2', 'movz x17, #5', 'udiv x16, x16, x17', 'mov w%d, w16' % rt, 'lsr x16, x16, #32', 'mov w%d, w16' % rt2]; return True
        if w & 0x0fff0fdf == 0x0e070f9a and bits(w, 3, 0) == 10:             # mcr p15, 0, Rt, c7, c10, {4,5}: the pre-ARMv7 barrier encodings (dsb / dmb)
            out.append('dsb ish' if bits(w, 5, 5) == 0 else 'dmb ish'); return True
        if w & 0x0fff0fff == 0x0e070f95:                                      # mcr p15, 0, Rt, c7, c5, 4 (isb)
            out.append('isb'); return True
        if w & 0x0fff0fff == 0x0e0d0f50:                                      # mcr p15, 0, Rt, c13, c0, 2 (TPIDRURW: the SDK keeps its thread pointer here)
            rt = bits(w, 15, 12)
            if rt == 15: raise Unsupported('cp15 write from pc')
            out.append('mov w25, w%d' % rt); return True
        if w & 0x0fff0fff == 0x0e1d0f50:                                      # mrc p15, 0, Rt, c13, c0, 2
            rt = bits(w, 15, 12)
            if rt == 15: raise Unsupported('cp15 read into pc')
            out.append('mov w%d, w25' % rt); return True
        if w & 0x0fff0fff == 0x0e1d0f70:                                      # mrc p15, 0, Rt, c13, c0, 3 (TPIDRURO)
            rt = bits(w, 15, 12)
            if rt == 15: raise Unsupported('cp15 read into pc')
            out.append('mov w%d, w26' % rt); return True
        return False

    # ---- entry point ---------------------------------------------------------------------------------------------
    def lift(self, w, addr):
        cond = bits(w, 31, 28)
        body = []
        if cond == 15:
            if w & 0xfd30f000 == 0xf510f000: return ['nop']                           # pld / pldw (immediate or register)
            if self.system(w, addr, body) or self.neon(w, addr, body): return body
            raise Unsupported('unconditional space (blx imm, ...)')
        cls = bits(w, 27, 25)
        if self.system(w, addr, body): return self.wrap(cond, body)
        if cls in (6, 7) and self.vfp(w, addr, body): return self.wrap(cond, body)
        if cls == 0:
            if bits(w, 7, 4) == 9:                                    # multiplies, swap, exclusives
                if bits(w, 27, 22) == 0: self.multiply(w, addr, body)
                elif bits(w, 27, 23) == 1: self.long_multiply(w, addr, body)
                elif bits(w, 27, 24) == 1 and self.exclusive(w, addr, body): pass
                elif not self.misc(w, addr, body): raise Unsupported('swap / exclusive')
            elif bits(w, 7, 7) and bits(w, 4, 4) and bits(w, 6, 5): self.extra_transfer(w, addr, body)
            elif bits(w, 24, 23) == 2 and not bits(w, 20, 20):         # TST/TEQ/CMP/CMN without S: the misc space
                if not self.misc(w, addr, body): raise Unsupported('misc instruction %#010x' % w)
            else: self.data_processing(w, addr, body)
        elif cls == 1:
            if bits(w, 24, 23) == 2 and not bits(w, 20, 20):           # movw / movt / msr immediate
                if not self.misc(w, addr, body): raise Unsupported('misc instruction %#010x' % w)
            else: self.data_processing(w, addr, body)
        elif cls == 2: self.single_transfer(w, addr, body)
        elif cls == 3:
            if bits(w, 4, 4):
                if not self.misc(w, addr, body): raise Unsupported('media instruction %#010x' % w)
            else: self.single_transfer(w, addr, body)
        elif cls == 4: self.block_transfer(w, addr, body)
        elif cls == 5: self.branch(w, addr, body)
        else: raise Unsupported('coprocessor / vfp / svc')
        return self.wrap(cond, body)

    def wrap(self, cond, body):
        if cond == 14: return body
        return ['b.%s 1f' % CC[INV[cond]]] + body + ['1:']
