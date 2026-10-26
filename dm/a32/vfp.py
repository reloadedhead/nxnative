"""VFP and Advanced SIMD (NEON) for the AArch32 -> AArch64 lifter (a mixin of lift.Lifter).

Register mapping: the VFP/NEON file is the AArch64 vector file, q0-q15 = v0-v15, with the overlay AArch32 has:
d(2n) = v(n).d[0], d(2n+1) = v(n).d[1], s(4n+k) = v(n).s[k]. A scalar AArch64 operation zeroes the rest of its destination register,
which would destroy the neighbouring s/d registers, so a result is computed in v16 (operands that do not sit in lane 0 are first
moved to v17-v19) and inserted into its lane with `mov v(n).s[k], v16.s[0]`.
The AArch32 FPSCR condition flags live in x24 as an NZCV image: vcmp sets them, vmrs APSR_nzcv copies them out. Rounding mode,
flush-to-zero and default-NaN are the AArch64 FPCR bits, which have the same positions as the FPSCR bits."""
from lift_base import Unsupported, mov32, bits, M32

def mov64(reg, v):
    """Lines that load a 64-bit constant into x register `reg`."""
    v &= (1 << 64) - 1
    lines = []
    for i in range(4):
        p = (v >> (16 * i)) & 0xffff
        if p or (i == 3 and not lines): lines.append(('movz %s, #%d, lsl #%d' if not lines else 'movk %s, #%d, lsl #%d') % (reg, p, 16 * i))
    return lines or ['movz %s, #0' % reg]

class VfpMixin:
    # ---- operand access -------------------------------------------------------------------------------------------------------
    def sread(self, n, tmp, out):
        """The A64 scalar single register that holds s(n): itself for lane 0, otherwise v(tmp) after moving the lane there."""
        if n & 3 == 0: return 's%d' % (n >> 2)
        out.append('mov v%d.s[0], v%d.s[%d]' % (tmp, n >> 2, n & 3)); return 's%d' % tmp

    def dread(self, n, tmp, out):
        if n & 1 == 0: return 'd%d' % (n >> 1)
        out.append('mov v%d.d[0], v%d.d[1]' % (tmp, n >> 1)); return 'd%d' % tmp

    def swrite(self, n, out, src=16): out.append('mov v%d.s[%d], v%d.s[0]' % (n >> 2, n & 3, src))
    def dwrite(self, n, out, src=16): out.append('mov v%d.d[%d], v%d.d[0]' % (n >> 1, n & 1, src))

    def vfp(self, w, addr, out):
        """VFP encodings with a condition (coprocessor 10 / 11). True when handled."""
        cls = bits(w, 27, 25)
        if bits(w, 11, 9) != 5: return False
        if cls == 7 and not bits(w, 24, 24):
            if not bits(w, 4, 4): self.vfp_dp(w, addr, out); return True
            return self.vfp_xfer(w, addr, out)
        if cls == 6: return self.vfp_ldst(w, addr, out)
        return False

    # ---- data processing --------------------------------------------------------------------------------------------------------
    def vfp_dp(self, w, addr, out):
        dbl = bits(w, 8, 8) == 1
        Vd, Vn, Vm = bits(w, 15, 12), bits(w, 19, 16), bits(w, 3, 0)
        D, N, M = bits(w, 22, 22), bits(w, 7, 7), bits(w, 5, 5)
        if dbl: d, n, m = (D << 4) | Vd, (N << 4) | Vn, (M << 4) | Vm
        else: d, n, m = (Vd << 1) | D, (Vn << 1) | N, (Vm << 1) | M
        t = 'd' if dbl else 's'
        rd = (lambda k, tmp: self.dread(k, tmp, out)) if dbl else (lambda k, tmp: self.sread(k, tmp, out))
        wr = (lambda: self.dwrite(d, out)) if dbl else (lambda: self.swrite(d, out))
        p, q, r, s = bits(w, 23, 23), bits(w, 21, 21), bits(w, 20, 20), bits(w, 6, 6)
        key = (p, q, r, s)
        if (p, q, r) != (1, 1, 1):
            A, B = rd(n, 17), rd(m, 18)
            if key in ((0, 0, 0, 0), (0, 0, 0, 1), (0, 0, 1, 0), (0, 0, 1, 1)):
                Dv = rd(d, 19)
                out.append('fmul %s16, %s, %s' % (t, A, B))                                             # the product is rounded before the accumulate
                if key == (0, 0, 0, 0): out.append('fadd %s16, %s, %s16' % (t, Dv, t))                  # vmla:  FPAdd(d, product)
                elif key == (0, 0, 0, 1): out += ['fneg %s16, %s16' % (t, t), 'fadd %s16, %s, %s16' % (t, Dv, t)]          # vmls:  FPAdd(d, FPNeg(product))
                elif key == (0, 0, 1, 0): out += ['fneg %s17, %s' % (t, Dv), 'fadd %s16, %s17, %s16' % (t, t, t)]          # vnmls: FPAdd(FPNeg(d), product)
                else: out += ['fneg %s16, %s16' % (t, t), 'fneg %s17, %s' % (t, Dv), 'fadd %s16, %s17, %s16' % (t, t, t)]  # vnmla: FPAdd(FPNeg(d), FPNeg(product))
            elif key == (0, 1, 0, 0): out.append('fmul %s16, %s, %s' % (t, A, B))
            elif key == (0, 1, 0, 1): out += ['fmul %s16, %s, %s' % (t, A, B), 'fneg %s16, %s16' % (t, t)]
            elif key == (0, 1, 1, 0): out.append('fadd %s16, %s, %s' % (t, A, B))
            elif key == (0, 1, 1, 1): out.append('fsub %s16, %s, %s' % (t, A, B))
            elif key == (1, 0, 0, 0): out.append('fdiv %s16, %s, %s' % (t, A, B))
            elif key in ((1, 0, 1, 0), (1, 0, 1, 1), (1, 1, 0, 0), (1, 1, 0, 1)):                      # fused multiply-accumulate family
                Dv = rd(d, 19)
                mn = {(1, 1, 0, 0): 'fmadd', (1, 1, 0, 1): 'fmsub', (1, 0, 1, 0): 'fnmsub', (1, 0, 1, 1): 'fnmadd'}[key]
                out.append('%s %s16, %s, %s, %s' % (mn, t, A, B, Dv))
            else: raise Unsupported('vfp data processing %s' % (key,))
            wr(); return
        if bits(w, 7, 4) == 0:                                                                         # vmov immediate
            imm8 = (bits(w, 19, 16) << 4) | bits(w, 3, 0)
            nb = (imm8 >> 6) & 1
            if dbl:
                out += mov64('x16', ((imm8 >> 7) << 63) | ((nb ^ 1) << 62) | ((0xff if nb else 0) << 54) | ((imm8 & 0x3f) << 48))
                out.append('mov v%d.d[%d], x16' % (d >> 1, d & 1))
            else:
                out += mov32('w16', ((imm8 >> 7) << 31) | ((nb ^ 1) << 30) | ((0x1f if nb else 0) << 25) | ((imm8 & 0x3f) << 19))
                out.append('mov v%d.s[%d], w16' % (d >> 2, d & 3))
            return
        opc2, op = bits(w, 19, 16), bits(w, 7, 6)
        if opc2 == 0 and op == 1:                                                                      # vmov register
            if dbl: out.append('mov v%d.d[%d], v%d.d[%d]' % (d >> 1, d & 1, m >> 1, m & 1))
            else: out.append('mov v%d.s[%d], v%d.s[%d]' % (d >> 2, d & 3, m >> 2, m & 3))
            return
        if (opc2, op) in ((0, 3), (1, 1), (1, 3)):                                                     # vabs, vneg, vsqrt
            B = rd(m, 18)
            out.append('%s %s16, %s' % ({(0, 3): 'fabs', (1, 1): 'fneg', (1, 3): 'fsqrt'}[(opc2, op)], t, B)); wr(); return
        if opc2 in (4, 5) and op in (1, 3):                                                            # vcmp / vcmpe, register or #0.0
            Dv = rd(d, 17)
            out.append('mrs x19, nzcv')
            mn = 'fcmpe' if op == 3 else 'fcmp'                                                       # vcmpe signals on any NaN, vcmp only on signalling NaNs
            out.append('%s %s, %s' % (mn, Dv, rd(m, 18)) if opc2 == 4 else '%s %s, #0.0' % (mn, Dv))
            out += ['mrs x24, nzcv', 'msr nzcv, x19']; return
        if opc2 == 7 and op == 3:                                                                      # vcvt between double and single
            if dbl:                                                                                    # source Dm -> Sd
                out += ['fcvt s16, %s' % self.dread((M << 4) | Vm, 18, out)]; self.swrite((Vd << 1) | D, out)
            else:                                                                                      # source Sm -> Dd
                out += ['fcvt d16, %s' % self.sread((Vm << 1) | M, 18, out)]; self.dwrite((D << 4) | Vd, out)
            return
        if opc2 == 8 and op in (1, 3):                                                                 # vcvt integer -> float: the source is an S register
            src = (Vm << 1) | M
            out += ['mov w16, v%d.s[%d]' % (src >> 2, src & 3), '%s %s16, w16' % ('scvtf' if bits(w, 7, 7) else 'ucvtf', t)]
            wr(); return
        if opc2 in (12, 13) and op in (1, 3):                                                          # vcvt float -> integer: the result is an S register
            B = rd(m, 18)
            dst = (Vd << 1) | D
            if bits(w, 7, 7) == 0: out.append('frintx %s17, %s' % (t, B)); B = '%s17' % t           # vcvtr rounds with the FPSCR mode
            out += ['%s w16, %s' % ('fcvtzs' if opc2 == 13 else 'fcvtzu', B), 'mov v%d.s[%d], w16' % (dst >> 2, dst & 3)]; return
        raise Unsupported('vfp extension %#010x' % w)

    # ---- transfers between core and vfp registers -------------------------------------------------------------------------------------
    def vfp_xfer(self, w, addr, out):
        rt = bits(w, 15, 12)
        if bits(w, 27, 21) == 0x70 and bits(w, 6, 5) == 0 and bits(w, 3, 0) == 0 and bits(w, 11, 8) == 10:      # vmov Sn <-> Rt
            n = (bits(w, 19, 16) << 1) | bits(w, 7, 7)
            if rt == 15: raise Unsupported('vmov with pc')
            if bits(w, 20, 20): out.append('mov w%d, v%d.s[%d]' % (rt, n >> 2, n & 3))
            else: out.append('mov v%d.s[%d], w%d' % (n >> 2, n & 3, rt))
            return True
        if bits(w, 27, 21) in (0x77, 0x76) and bits(w, 11, 8) == 10 and bits(w, 3, 0) == 0 and bits(w, 6, 5) == 0 and bits(w, 4, 4):   # vmrs / vmsr
            reg = bits(w, 19, 16)
            if reg != 1: raise Unsupported('vmrs/vmsr of register %d' % reg)
            if bits(w, 20, 20):                                                                       # vmrs
                if rt == 15: out.append('msr nzcv, x24')                                              # vmrs APSR_nzcv, FPSCR
                else: out += ['mrs x16, fpcr', 'and x16, x16, #0x07c00000', 'mrs x17, fpsr', 'movz x20, #0x9f', 'and x17, x17, x20', 'orr x16, x16, x17',
                              'and x17, x24, #0xf0000000', 'orr x16, x16, x17', 'mov w%d, w16' % rt]
            else:                                                                                     # vmsr FPSCR, Rt
                if rt == 15: raise Unsupported('vmsr from pc')
                out += ['and x24, x%d, #0xf0000000' % rt, 'and x16, x%d, #0x07c00000' % rt, 'msr fpcr, x16', 'movz x20, #0x9f', 'and x16, x%d, x20' % rt, 'msr fpsr, x16']
            return True
        if bits(w, 27, 24) == 0xe and bits(w, 11, 8) == 11 and bits(w, 4, 4) and bits(w, 3, 0) == 0:
            U, opc1, L, opc2 = bits(w, 23, 23), bits(w, 22, 21), bits(w, 20, 20), bits(w, 6, 5)
            if bits(w, 31, 28) == 15: return False
            if L == 0 and U == 1:                                                                     # vdup (core register)
                return self.vdup_core(w, out)
            dn = (bits(w, 7, 7) << 4) | bits(w, 19, 16)
            if rt == 15: raise Unsupported('vmov scalar with pc')
            if opc1 & 2: size, idx = 8, ((opc1 & 1) << 2) | opc2
            elif opc2 & 1: size, idx = 16, ((opc1 & 1) << 1) | (opc2 >> 1)
            elif opc2 == 0: size, idx = 32, opc1 & 1
            else: raise Unsupported('vmov scalar encoding')
            per = {8: 8, 16: 4, 32: 2}[size]
            lane = (dn & 1) * per + idx; v = dn >> 1; ch = {8: 'b', 16: 'h', 32: 's'}[size]
            if L == 0: out.append('mov v%d.%s[%d], w%d' % (v, ch, lane, rt))
            elif size == 32: out.append('mov w%d, v%d.s[%d]' % (rt, v, lane))
            else: out.append('%s w%d, v%d.%s[%d]' % ('umov' if U else 'smov', rt, v, ch, lane))
            return True
        return False

    def vdup_core(self, w, out):
        rt = bits(w, 15, 12)
        if rt == 15: raise Unsupported('vdup from pc')
        b, e, Q = bits(w, 22, 22), bits(w, 5, 5), bits(w, 21, 21)
        d = (bits(w, 7, 7) << 4) | bits(w, 19, 16)
        arr = {(1, 0): '16b', (0, 1): '8h', (0, 0): '4s'}.get((b, e))
        if not arr: raise Unsupported('vdup size')
        if Q:
            if d & 1: raise Unsupported('vdup q odd')
            out.append('dup v%d.%s, w%d' % (d >> 1, arr, rt))
        else:
            out += ['dup v16.%s, w%d' % ({'16b': '8b', '8h': '4h', '4s': '2s'}[arr], rt), 'mov v%d.d[%d], v16.d[0]' % (d >> 1, d & 1)]
        return True

    def vfp_ldst(self, w, addr, out):
        """vldr / vstr / vldm / vstm / vpush / vpop and the two-register transfers (vmov Rt, Rt2, Dm / Sm, Sm1)."""
        dbl = bits(w, 8, 8) == 1
        if bits(w, 27, 21) == 0x62:                                                                    # vmov two core registers <-> d or two s registers
            rt, rt2, L = bits(w, 15, 12), bits(w, 19, 16), bits(w, 20, 20)
            if 15 in (rt, rt2): raise Unsupported('vmov pair with pc')
            m = (bits(w, 5, 5) << 4 | bits(w, 3, 0)) if dbl else (bits(w, 3, 0) << 1 | bits(w, 5, 5))
            if dbl:
                if L: out += ['mov x16, v%d.d[%d]' % (m >> 1, m & 1), 'mov w%d, w16' % rt, 'lsr x16, x16, #32', 'mov w%d, w16' % rt2]
                else: out += ['mov w16, w%d' % rt, 'orr x16, x16, x%d, lsl #32' % rt2, 'mov v%d.d[%d], x16' % (m >> 1, m & 1)]
            else:
                if m == 31: raise Unsupported('vmov s pair at s31')
                if L: out += ['mov w%d, v%d.s[%d]' % (rt, m >> 2, m & 3), 'mov w%d, v%d.s[%d]' % (rt2, (m + 1) >> 2, (m + 1) & 3)]
                else: out += ['mov v%d.s[%d], w%d' % (m >> 2, m & 3, rt), 'mov v%d.s[%d], w%d' % ((m + 1) >> 2, (m + 1) & 3, rt2)]
            return True
        P, U, W, L, rn = bits(w, 24, 24), bits(w, 23, 23), bits(w, 21, 21), bits(w, 20, 20), bits(w, 19, 16)
        D, Vd, imm8 = bits(w, 22, 22), bits(w, 15, 12), bits(w, 7, 0)
        d = ((D << 4) | Vd) if dbl else ((Vd << 1) | D)
        if bits(w, 27, 25) != 6: return False
        if P == 1 and W == 0:                                                                          # vldr / vstr
            if rn == 15: out += mov32('w15', (addr + 8) & ~3); base = 'w15'
            else: base = 'w%d' % rn
            if imm8: out.append('%s w15, %s, #%d' % ('add' if U else 'sub', base, imm8 * 4))
            elif base != 'w15': out.append('mov w15, %s' % base)
            t = 'd' if dbl else 's'
            if L:
                out.append('ldr %s16, [x28, w15, uxtw]' % t)
                (self.dwrite if dbl else self.swrite)(d, out)
            else:
                R = (self.dread if dbl else self.sread)(d, 16, out)
                out.append('str %s, [x28, w15, uxtw]' % R)
            return True
        if (P, U, W) in ((0, 1, 0), (0, 1, 1), (1, 0, 1)):                                              # vldm / vstm (ia, ia!, db!), vpush / vpop
            if rn == 15: raise Unsupported('vldm with pc base')
            words = imm8; cnt = words // 2 if dbl else words
            if cnt == 0 or (dbl and words & 1) or (dbl and cnt > 16) or (d + cnt > (32 if dbl else 32)): raise Unsupported('vldm list')
            base = 'w%d' % rn
            if U: out.append('mov w15, %s' % base)
            else: out.append('sub w15, %s, #%d' % (base, 4 * words))
            for k in range(cnt):
                reg = d + k
                if k: out.append('add w15, w15, #%d' % (8 if dbl else 4))
                t = 'd' if dbl else 's'
                if L:
                    out.append('ldr %s16, [x28, w15, uxtw]' % t); (self.dwrite if dbl else self.swrite)(reg, out)
                else:
                    R = (self.dread if dbl else self.sread)(reg, 16, out); out.append('str %s, [x28, w15, uxtw]' % R)
            if W: out.append('%s %s, %s, #%d' % ('add' if U else 'sub', base, base, 4 * words))
            return True
        return False
