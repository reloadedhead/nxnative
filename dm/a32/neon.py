"""Advanced SIMD (NEON) and the unconditional-space floating-point instructions (vsel) for the AArch32 -> AArch64 lifter.
A mixin of lift.Lifter, see vfp.py for the register mapping (q(n) = v(n); a d register is half of a v register). A Q operation
writes its destination v register directly; a D operation computes in v16 and inserts the result into its half, because
AArch64 vector operations on 64 bits zero the other half. Only the instruction forms the code of the games uses are covered; anything
else raises Unsupported."""
from lift_base import Unsupported, mov32, bits, M32
from vfp import mov64

ARR = {(1, 0): '16b', (1, 1): '8h', (1, 2): '4s', (1, 3): '2d', (0, 0): '8b', (0, 1): '4h', (0, 2): '2s', (0, 3): '1d'}

def imm_expand(op, cmode, imm8):
    """AdvSIMDExpandImm: the 64-bit replicated immediate of one-register-and-modified-immediate, and the effective operation
    ('mov' | 'mvn' | 'orr' | 'bic') for the (op, cmode) pair."""
    cm = cmode
    if cm >> 1 == 0: v = imm8; rep = 'i32'
    elif cm >> 1 == 1: v = imm8 << 8; rep = 'i32'
    elif cm >> 1 == 2: v = imm8 << 16; rep = 'i32'
    elif cm >> 1 == 3: v = imm8 << 24; rep = 'i32'
    elif cm >> 1 == 4: v = imm8; rep = 'i16'
    elif cm >> 1 == 5: v = imm8 << 8; rep = 'i16'
    elif cm == 12: v = (imm8 << 8) | 0xff; rep = 'i32'
    elif cm == 13: v = (imm8 << 16) | 0xffff; rep = 'i32'
    elif cm == 14 and op == 0: v = imm8; rep = 'i8'
    elif cm == 14 and op == 1:
        v = 0
        for i in range(8):
            if imm8 >> i & 1: v |= 0xff << (8 * i)
        return v, 'mov'
    elif cm == 15 and op == 0:
        a = imm8 >> 7; b = (imm8 >> 6) & 1
        f = (a << 31) | ((b ^ 1) << 30) | ((0x1f if b else 0) << 25) | ((imm8 & 0x3f) << 19)
        return (f << 32) | f, 'mov'
    else: raise Unsupported('modified immediate cmode %d op %d' % (cm, op))
    if rep == 'i32': full = (v << 32) | v
    elif rep == 'i16': full = (v << 48) | (v << 32) | (v << 16) | v
    else: full = int.from_bytes(bytes([v]) * 8, 'little')
    if cm < 12 and cm & 1: return full, 'bic' if op else 'orr'
    return full, 'mvn' if op else 'mov'

class NeonMixin:
    def dsrc(self, n, tmp, out):
        """A D operand as the low half of an A64 vector register (a copy in v(tmp) when it is the upper half)."""
        if n & 1 == 0: return 'v%d' % (n >> 1)
        out.append('mov v%d.d[0], v%d.d[1]' % (tmp, n >> 1)); return 'v%d' % tmp

    def neon(self, w, addr, out):
        """Unconditional-space words: Advanced SIMD data processing, element/structure loads and stores, vsel. True when handled."""
        if bits(w, 31, 24) == 0xf4: return self.neon_ldst(w, addr, out)
        if bits(w, 31, 25) == 0x79: return self.neon_dp(w, addr, out)
        if bits(w, 31, 23) == 0x1fc and bits(w, 11, 9) == 5 and not bits(w, 6, 6) and not bits(w, 4, 4): return self.vsel(w, out)
        if bits(w, 31, 23) == 0x1fd and bits(w, 11, 9) == 5: return self.armv8_fp(w, out)
        return False

    def armv8_fp(self, w, out):
        """The ARMv8 floating-point additions in the unconditional space: vmaxnm / vminnm, vrinta/n/p/m, vcvta/n/p/m."""
        dbl = bits(w, 8, 8) == 1
        Vd, Vn, Vm, D, N, M = bits(w, 15, 12), bits(w, 19, 16), bits(w, 3, 0), bits(w, 22, 22), bits(w, 7, 7), bits(w, 5, 5)
        if bits(w, 21, 20) == 0 and not bits(w, 4, 4):                                            # vmaxnm / vminnm
            op = 'fminnm' if bits(w, 6, 6) else 'fmaxnm'
            if dbl:
                d, n, m = (D << 4) | Vd, (N << 4) | Vn, (M << 4) | Vm
                A, B = self.dread(n, 17, out), self.dread(m, 18, out); out.append('%s d16, %s, %s' % (op, A, B)); self.dwrite(d, out)
            else:
                d, n, m = (Vd << 1) | D, (Vn << 1) | N, (Vm << 1) | M
                A, B = self.sread(n, 17, out), self.sread(m, 18, out); out.append('%s s16, %s, %s' % (op, A, B)); self.swrite(d, out)
            return True
        rm = bits(w, 17, 16)
        if bits(w, 21, 18) == 14 and bits(w, 7, 6) == 1 and not bits(w, 4, 4):                    # vrinta / vrintn / vrintp / vrintm
            op = ['frinta', 'frintn', 'frintp', 'frintm'][rm]
            if dbl:
                d, m = (D << 4) | Vd, (M << 4) | Vm
                A = self.dread(m, 17, out); out.append('%s d16, %s' % (op, A)); self.dwrite(d, out)
            else:
                d, m = (Vd << 1) | D, (Vm << 1) | M
                A = self.sread(m, 17, out); out.append('%s s16, %s' % (op, A)); self.swrite(d, out)
            return True
        if bits(w, 21, 18) == 15 and bits(w, 6, 6) == 1 and not bits(w, 4, 4):                    # vcvta / vcvtn / vcvtp / vcvtm (float to integer, rounding as named)
            signed = bits(w, 7, 7)
            op = 'fcvt' + ['a', 'n', 'p', 'm'][rm] + ('s' if signed else 'u')
            d = (Vd << 1) | D
            if dbl: A = self.dread((M << 4) | Vm, 17, out)
            else: A = self.sread((Vm << 1) | M, 17, out)
            out += ['%s w16, %s' % (op, A), 'fmov s16, w16']; self.swrite(d, out)
            return True
        return False

    def vsel(self, w, out):
        dbl = bits(w, 8, 8) == 1
        cc = ['eq', 'vs', 'ge', 'gt'][bits(w, 21, 20)]
        Vd, Vn, Vm, D, N, M = bits(w, 15, 12), bits(w, 19, 16), bits(w, 3, 0), bits(w, 22, 22), bits(w, 7, 7), bits(w, 5, 5)
        if dbl:
            d, n, m = (D << 4) | Vd, (N << 4) | Vn, (M << 4) | Vm
            A, B = self.dread(n, 17, out), self.dread(m, 18, out)
            out += ['fcsel d16, %s, %s, %s' % (A, B, cc)]; self.dwrite(d, out)
        else:
            d, n, m = (Vd << 1) | D, (Vn << 1) | N, (Vm << 1) | M
            A, B = self.sread(n, 17, out), self.sread(m, 18, out)
            out += ['fcsel s16, %s, %s, %s' % (A, B, cc)]; self.swrite(d, out)
        return True

    # ---- loads and stores of vectors --------------------------------------------------------------------------------------------
    def neon_ldst(self, w, addr, out):
        if bits(w, 23, 23): return self.neon_lane(w, addr, out)                                   # single element / all lanes
        L, rn, rm = bits(w, 21, 21), bits(w, 19, 16), bits(w, 3, 0)
        typ = bits(w, 11, 8); d = (bits(w, 22, 22) << 4) | bits(w, 15, 12)
        nregs = {7: 1, 10: 2, 6: 3, 2: 4}.get(typ)
        if nregs is None: return False                                                            # vld2 / vld3 / vld4 (de-interleaving) not covered yet
        if rn == 15: raise Unsupported('vld1 with pc base')
        if d + nregs > 32: raise Unsupported('vld1 list')
        out.append('mov w15, w%d' % rn)
        k = 0
        while k < nregs:
            reg = d + k
            if reg & 1 == 0 and nregs - k >= 2:                                                   # a whole q register in one access
                if L: out += ['ldr q%d, [x28, w15, uxtw]' % (reg >> 1)]
                else: out += ['str q%d, [x28, w15, uxtw]' % (reg >> 1)]
                out.append('add w15, w15, #16'); k += 2; continue
            if L: out += ['ldr d16, [x28, w15, uxtw]', 'mov v%d.d[%d], v16.d[0]' % (reg >> 1, reg & 1)]
            else: out += ['mov v16.d[0], v%d.d[%d]' % (reg >> 1, reg & 1), 'str d16, [x28, w15, uxtw]']
            out.append('add w15, w15, #8'); k += 1
        if rm == 13: out.append('add w%d, w%d, #%d' % (rn, rn, 8 * nregs))
        elif rm != 15:
            if rm == rn: raise Unsupported('vld1 with the index register as the base')
            out.append('add w%d, w%d, w%d' % (rn, rn, rm))
        return True

    # ---- data processing ----------------------------------------------------------------------------------------------------------
    def neon_dp(self, w, addr, out):
        U, Q = bits(w, 24, 24), bits(w, 6, 6)
        if not bits(w, 23, 23): return self.neon_3same(w, U, Q, out)
        if bits(w, 21, 19) == 0 and bits(w, 7, 7) == 0 and bits(w, 4, 4): return self.neon_imm(w, U, Q, out)
        if bits(w, 4, 4): return self.neon_shift(w, U, Q, out)
        if not bits(w, 4, 4) and bits(w, 21, 20) != 3: return self.neon_3diff(w, U, out)
        return False

    def neon_regs(self, w, Q):
        d = (bits(w, 22, 22) << 4) | bits(w, 15, 12); n = (bits(w, 7, 7) << 4) | bits(w, 19, 16); m = (bits(w, 5, 5) << 4) | bits(w, 3, 0)
        if Q and (d | n | m) & 1: raise Unsupported('q register with an odd number')
        return d, n, m

    def neon_3same(self, w, U, Q, out):
        size, A, B = bits(w, 21, 20), bits(w, 11, 8), bits(w, 4, 4)
        d, n, m = self.neon_regs(w, Q)
        arr = ARR[(Q, size)]
        def src(x, tmp): return 'v%d' % (x >> 1) if Q else self.dsrc(x, tmp, out)
        def fin(res):                                                                             # place the result of a D operation (in v16) into the destination half
            if not Q: out.append('mov v%d.d[%d], v16.d[0]' % (d >> 1, d & 1))
        dst = 'v%d' % (d >> 1) if Q else 'v16'
        lo = 'b' if False else None
        if A == 1 and B == 1:                                                                     # bitwise: vand vbic vorr vorn / veor vbsl vbit vbif
            ab = '16b' if Q else '8b'; a, b = src(n, 17), src(m, 18)
            if not U:
                if size == 2 and n == m: out.append('mov %s.%s, %s.%s' % (dst, ab, b, ab)); fin(dst); return True       # vmov register
                mn = ['and', 'bic', 'orr', 'orn'][size]; out.append('%s %s.%s, %s.%s, %s.%s' % (mn, dst, ab, a, ab, b, ab)); fin(dst); return True
            if size == 0: out.append('eor %s.%s, %s.%s, %s.%s' % (dst, ab, a, ab, b, ab)); fin(dst); return True
            # vbsl: d = (d & n) | (~d & m);  vbit: d = (d & ~m) | (n & m);  vbif: d = (d & m) | (n & ~m) -- the destination is also an input
            if not Q: out.append('mov v16.d[0], v%d.d[%d]' % (d >> 1, d & 1))
            out.append('%s %s.%s, %s.%s, %s.%s' % (['bsl', 'bit', 'bif'][size - 1], dst, ab, a, ab, b, ab)); fin(dst); return True
        a, b = src(n, 17), src(m, 18)
        if A == 8 and B == 0 and size < 4:                                                        # vadd.i / vsub.i
            if size == 3 and not Q:                                                               # a 64-bit D operation: A64 has no 1d arrangement, use the scalar form
                out.append('%s d16, d%s, d%s' % ('sub' if U else 'add', a[1:], b[1:])); fin(dst); return True
            out.append('%s %s.%s, %s.%s, %s.%s' % ('sub' if U else 'add', dst, arr, a, arr, b, arr)); fin(dst); return True
        if A == 8 and B == 1:                                                                     # vtst.i / vceq.i
            if size == 3: raise Unsupported('vceq.i64')
            out.append('%s %s.%s, %s.%s, %s.%s' % ('cmeq' if U else 'cmtst', dst, arr, a, arr, b, arr)); fin(dst); return True
        if A == 3 and size < 3:                                                                   # vcgt / vcge, signed or unsigned
            mn = {(0, 0): 'cmgt', (0, 1): 'cmge', (1, 0): 'cmhi', (1, 1): 'cmhs'}[(U, B)]
            out.append('%s %s.%s, %s.%s, %s.%s' % (mn, dst, arr, a, arr, b, arr)); fin(dst); return True
        if A == 6 and size < 3:                                                                   # vmax / vmin
            mn = ('u' if U else 's') + ('min' if B else 'max'); out.append('%s %s.%s, %s.%s, %s.%s' % (mn, dst, arr, a, arr, b, arr)); fin(dst); return True
        if A == 0 and B == 1 and size < 4:                                                        # vqadd
            out.append('%s %s.%s, %s.%s, %s.%s' % ('uqadd' if U else 'sqadd', dst, arr, a, arr, b, arr)); fin(dst); return True
        if A == 2 and B == 1 and size < 4:                                                        # vqsub
            out.append('%s %s.%s, %s.%s, %s.%s' % ('uqsub' if U else 'sqsub', dst, arr, a, arr, b, arr)); fin(dst); return True
        if A == 9 and B == 0 and size < 3:                                                        # vmla.i / vmls.i (accumulate into the destination)
            if not Q: out.append('mov v16.d[0], v%d.d[%d]' % (d >> 1, d & 1))
            out.append('%s %s.%s, %s.%s, %s.%s' % ('mls' if U else 'mla', dst, arr, a, arr, b, arr)); fin(dst); return True
        if A == 9 and B == 1 and size < 3 and not U:                                              # vmul.i
            out.append('mul %s.%s, %s.%s, %s.%s' % (dst, arr, a, arr, b, arr)); fin(dst); return True
        if A == 11 and B == 1 and not U and size < 3:                                             # vpadd.i
            out.append('addp %s.%s, %s.%s, %s.%s' % (dst, arr, a, arr, b, arr)); fin(dst); return True
        if A == 13 and B == 0 and not U and size in (0, 2):                                       # vadd.f32 / vsub.f32 (NEON arithmetic always runs with flush-to-zero and default NaN)
            f = '4s' if Q else '2s'; self.std_fpcr(out, '%s %s.%s, %s.%s, %s.%s' % ('fsub' if size == 2 else 'fadd', dst, f, a, f, b, f)); fin(dst); return True
        if A == 15 and B == 0 and not U and size in (0, 2):                                       # vmax.f32 / vmin.f32
            f = '4s' if Q else '2s'; self.std_fpcr(out, '%s %s.%s, %s.%s, %s.%s' % ('fmin' if size == 2 else 'fmax', dst, f, a, f, b, f)); fin(dst); return True
        if A == 13 and B == 1 and U and size in (0, 2):                                           # vmul.f32
            f = '4s' if Q else '2s'; self.std_fpcr(out, 'fmul %s.%s, %s.%s, %s.%s' % (dst, f, a, f, b, f)); fin(dst); return True
        raise Unsupported('neon three-register A=%d B=%d U=%d size=%d' % (A, B, U, size))

    # ---- shifts by immediate, narrowing and widening ------------------------------------------------------------------------------------------
    def neon_shift(self, w, U, Q, out): return self.neon_shift2(w, U, Q, out)

    def neon_shift2(self, w, U, Q, out):
        A, L, imm6, op = bits(w, 11, 8), bits(w, 7, 7), bits(w, 21, 16), bits(w, 6, 6)
        d = (bits(w, 22, 22) << 4) | bits(w, 15, 12); m = (bits(w, 5, 5) << 4) | bits(w, 3, 0)
        def elem():
            if L: return 64, imm6
            if imm6 >= 32: return 32, imm6 - 32
            if imm6 >= 16: return 16, imm6 - 16
            if imm6 >= 8: return 8, imm6 - 8
            return None, 0
        TQ = {8: '16b', 16: '8h', 32: '4s', 64: '2d'}; TD = {8: '8b', 16: '4h', 32: '2s'}
        if A in (8, 9):                                                                            # narrowing: bit 6 selects the rounding form (op) for 8 / 9; the destination element size is esize, the source 2 * esize
            if L: return False
            if imm6 >= 32: es, sh = 32, 64 - imm6
            elif imm6 >= 16: es, sh = 16, 32 - imm6
            elif imm6 >= 8: es, sh = 8, 16 - imm6
            else: return False
            if m & 1: raise Unsupported('narrowing shift of an odd q register')
            if A == 8: mn = {(0, 0): 'shrn', (0, 1): 'rshrn', (1, 0): 'sqshrun', (1, 1): 'sqrshrun'}[(U, op)]
            else: mn = {(0, 0): 'sqshrn', (0, 1): 'sqrshrn', (1, 0): 'uqshrn', (1, 1): 'uqrshrn'}[(U, op)]
            out.append('%s v16.%s, v%d.%s, #%d' % (mn, TD[es], m >> 1, TQ[es * 2], sh))
            out.append('mov v%d.d[%d], v16.d[0]' % (d >> 1, d & 1)); return True
        if A == 10 and not op:                                                                     # vshll / vmovl (widening, source D, result Q)
            if L: return False
            if imm6 >= 32: es, sh = 32, imm6 - 32
            elif imm6 >= 16: es, sh = 16, imm6 - 16
            elif imm6 >= 8: es, sh = 8, imm6 - 8
            else: return False
            if d & 1: raise Unsupported('widening shift into an odd q register')
            src = self.dsrc(m, 17, out)
            out.append('%s v%d.%s, %s.%s, #%d' % ('ushll' if U else 'sshll', d >> 1, TQ[es * 2], src, TD[es], sh)); return True
        es, sh = elem()
        if es is None: return False
        Q = op
        if Q and (d | m) & 1: raise Unsupported('q register with an odd number')
        def dreg(x, tmp): return ('v%d' % (x >> 1)) if Q else self.dsrc(x, tmp, out)
        right = A in (0, 1, 2, 3)
        if right: sh = 2 * es - imm6 if not L else 64 - imm6
        else: sh = imm6 - es if not L else imm6
        if A == 5 and not U: mn = 'shl'
        elif A == 0: mn = 'ushr' if U else 'sshr'
        elif A == 1: mn = 'usra' if U else 'ssra'
        elif A == 2: mn = 'urshr' if U else 'srshr'
        elif A == 3: mn = 'ursra' if U else 'srsra'
        else: return False
        src = dreg(m, 17)
        dst = 'v%d' % (d >> 1) if Q else 'v16'
        if mn in ('ssra', 'usra', 'srsra', 'ursra') and not Q: out.append('mov v16.d[0], v%d.d[%d]' % (d >> 1, d & 1))
        if not Q and es == 64: out.append('%s d16, d%s, #%d' % (mn, src[1:], sh))
        else: out.append('%s %s.%s, %s.%s, #%d' % (mn, dst, (TQ if Q else TD)[es] if (Q or es < 64) else '', src, (TQ if Q else TD)[es], sh))
        if not Q: out.append('mov v%d.d[%d], v16.d[0]' % (d >> 1, d & 1))
        return True

    def neon_3diff(self, w, U, out):
        size, A = bits(w, 21, 20), bits(w, 11, 8)
        d = (bits(w, 22, 22) << 4) | bits(w, 15, 12); n = (bits(w, 7, 7) << 4) | bits(w, 19, 16); m = (bits(w, 5, 5) << 4) | bits(w, 3, 0)
        TQ = {0: '8h', 1: '4s', 2: '2d'}; TD = {0: '8b', 1: '4h', 2: '2s'}
        mn = {8: 'smlal', 10: 'smlsl', 12: 'smull', 0: 'saddl', 2: 'ssubl', 7: 'sabdl', 5: 'sabal'}.get(A)
        if mn is None: return False
        if U: mn = 'u' + mn[1:]
        if d & 1: raise Unsupported('long operation into an odd q register')
        a, b = self.dsrc(n, 17, out), self.dsrc(m, 18, out)
        out.append('%s v%d.%s, %s.%s, %s.%s' % (mn, d >> 1, TQ[size], a, TD[size], b, TD[size])); return True

    # ---- single-element and all-lanes loads and stores (vld1 .. vld4 lane, vst1 .. vst4 lane, vld1 dup) ----------------------------------------------
    def neon_lane(self, w, addr, out):
        L, rn, rm = bits(w, 21, 21), bits(w, 19, 16), bits(w, 3, 0)
        d = (bits(w, 22, 22) << 4) | bits(w, 15, 12); size = bits(w, 11, 10)
        if rn == 15: raise Unsupported('vld lane with pc base')
        if size == 3:                                                                              # all lanes: vld1 only (one or two registers)
            if not L or bits(w, 9, 8) != 0: return False
            es = 1 << bits(w, 7, 6); nreg = 2 if bits(w, 5, 5) else 1
            if es > 4 or (es == 4 and bits(w, 4, 4) and False): return False
            arr = {1: '8b', 2: '4h', 4: '2s'}[es]
            out.append('add x17, x28, w%d, uxtw' % rn)
            for k in range(nreg):
                out += ['ld1r {v16.%s}, [x17]' % arr, 'mov v%d.d[%d], v16.d[0]' % ((d + k) >> 1, (d + k) & 1)]
            bytes_ = es * 1
            if rm == 13: out.append('add w%d, w%d, #%d' % (rn, rn, es))
            elif rm != 15: out.append('add w%d, w%d, w%d' % (rn, rn, rm))
            return True
        nreg = bits(w, 9, 8) + 1; es = 1 << size
        if size == 0: idx, inc = bits(w, 7, 5), 1
        elif size == 1: idx, inc = bits(w, 7, 6), 2 if bits(w, 5, 5) else 1
        else: idx, inc = bits(w, 7, 7), 2 if bits(w, 6, 6) else 1
        T = {1: 'b', 2: 'h', 4: 's'}[es]; perd = 8 // es
        out.append('mov w15, w%d' % rn)
        for k in range(nreg):
            reg = d + k * inc
            if reg > 31: raise Unsupported('vld lane register list')
            vr = reg >> 1; lane = (reg & 1) * perd + idx
            if L: out += ['ldr %s16, [x28, w15, uxtw]' % T, 'mov v%d.%s[%d], v16.%s[0]' % (vr, T, lane, T)]
            else: out += ['mov v16.%s[0], v%d.%s[%d]' % (T, vr, T, lane), 'str %s16, [x28, w15, uxtw]' % T]
            out.append('add w15, w15, #%d' % es)
        if rm == 13: out.append('add w%d, w%d, #%d' % (rn, rn, es * nreg))
        elif rm != 15:
            if rm == rn: raise Unsupported('vld lane with the index register as the base')
            out.append('add w%d, w%d, w%d' % (rn, rn, rm))
        return True

    def std_fpcr(self, out, op):
        """Run a NEON floating-point operation with the 'standard FPSCR value' (flush-to-zero and default NaN on), restoring the guest's FPCR after."""
        out += ['mrs x19, fpcr', 'movz x20, #0x300, lsl #16', 'msr fpcr, x20', op, 'msr fpcr, x19']

    def neon_imm(self, w, U, Q, out):
        op, cmode = bits(w, 5, 5), bits(w, 11, 8)
        imm8 = (U << 7) | (bits(w, 18, 16) << 4) | bits(w, 3, 0)
        d = (bits(w, 22, 22) << 4) | bits(w, 15, 12)
        if Q and d & 1: raise Unsupported('q register with an odd number')
        v, kind = imm_expand(op, cmode, imm8)
        if kind == 'mvn': v = ~v & ((1 << 64) - 1); kind = 'mov'
        out += mov64('x16', v)
        if kind == 'mov':
            if Q: out += ['dup v%d.2d, x16' % (d >> 1)]
            else: out += ['mov v%d.d[%d], x16' % (d >> 1, d & 1)]
            return True
        # vorr / vbic with an immediate: the destination is an input
        if Q:
            out += ['dup v16.2d, x16', '%s v%d.16b, v%d.16b, v16.16b' % ('orr' if kind == 'orr' else 'bic', d >> 1, d >> 1)]
        else:
            out += ['mov v16.d[0], x16', 'mov v17.d[0], v%d.d[%d]' % (d >> 1, d & 1), '%s v17.8b, v17.8b, v16.8b' % ('orr' if kind == 'orr' else 'bic'), 'mov v%d.d[%d], v17.d[0]' % (d >> 1, d & 1)]
        return True
