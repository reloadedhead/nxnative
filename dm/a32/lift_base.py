"""Shared helpers of the AArch32 lifter (lift.py, vfp.py)."""
class Unsupported(Exception): pass

INV = [1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12]
CC = ['eq', 'ne', 'hs', 'lo', 'mi', 'pl', 'vs', 'vc', 'hi', 'ls', 'ge', 'lt', 'gt', 'le']
M32 = 0xffffffff

def mov32(reg, v):
    """Lines that load a 32-bit constant (Apple's assembler has no multi-instruction `mov`)."""
    v &= M32
    if v < 0x10000: return ['movz %s, #%d' % (reg, v)]
    if v & 0xffff == 0: return ['movz %s, #%d, lsl #16' % (reg, v >> 16)]
    return ['movz %s, #%d' % (reg, v & 0xffff), 'movk %s, #%d, lsl #16' % (reg, v >> 16)]

def bits(w, hi, lo): return (w >> lo) & ((1 << (hi - lo + 1)) - 1)
def sext(v, n): return v - (1 << n) if v >> (n - 1) & 1 else v
def ror32(v, n): n &= 31; return ((v >> n) | (v << (32 - n))) & M32 if n else v & M32

