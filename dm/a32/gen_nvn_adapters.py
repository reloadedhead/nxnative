#!/usr/bin/env python3
"""Generate rt/a32nvn_gen.c: adapters from the 32-bit guest's calls (AAPCS, hard-float) to the host's NVN implementation (dm/gfx/nvn).

For each NVN function the host implements (prototype in nvn_protos.h, or an NVN_NOPROTO definition) the adapter reads the arguments out of the guest
frame (r0-r3, then the stack; 64-bit values in even register pairs; floats in s0..s15), converts guest addresses to host pointers (g_mem + addr), calls
the host function and converts the result (pointers back to guest addresses). 64-bit values (NVNbufferAddress, texture handles, ...) pass through unchanged:
they are opaque to the guest. Functions that need more than that (callbacks, structs holding pointers) are written by hand in a32gfx.c (HAND below).
"""
import re, sys, os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'gfx', 'nvn')
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'rt', 'a32nvn_gen.c')

HAND = {'nvnDeviceGetProcAddress', 'nvnDeviceInstallDebugCallback', 'nvnDeviceWalkDebugDatabase', 'nvnProgramSetShaders', 'nvnProgramSetShadersExt',
        'nvnCommandBufferSetMemoryCallback', 'nvnCommandBufferGetMemoryCallback', 'nvnVertexAttribStateGetFormat'}
S64 = {'uint64_t', 'int64_t', 'NVNbufferAddress', 'NVNtextureAddress', 'NVNtextureHandle', 'NVNimageHandle', 'NVNcommandHandle'}
STRUCT64 = {'NVNseparateTextureHandle', 'NVNseparateSamplerHandle'}      # struct { uint64_t value; }: by value in an even pair, returned through memory (sret)
S32 = {'int', 'uint32_t', 'size_t', 'ptrdiff_t', 'NVNboolean', 'unsigned', 'unsigned int', 'int32_t'}

nvn_h = open(os.path.join(ROOT, 'include', 'nvn.h')).read()
ENUMS = set(re.findall(r'typedef int (\w+);', nvn_h)) | set(re.findall(r'typedef enum[^{]*\{[^}]*\}\s*(NVN\w+);', nvn_h))

def split_args(a):
    a = a.strip()
    return [] if a in ('', 'void') else [x.strip() for x in a.split(',')]

protos = {}
for m in re.finditer(r'typedef (.+?) \(\*NVNT_(\w+)\)\((.*?)\);', open(os.path.join(ROOT, 'nvn_protos.h')).read()):
    protos[m.group(2)] = (m.group(1).strip(), split_args(m.group(3)), True)
# NVN_NOPROTO(name, ret, args...) definitions in the implementation have no typedef; take the signature from the definition (parameter names dropped)
src = ''.join(open(os.path.join(ROOT, f)).read() for f in ('hle_nvn.c', 'nvn_mtl.m'))
for m in re.finditer(r'NVN_NOPROTO\((\w+),\s*([^,]+),([^)]*)\)', src):
    name, ret, args = m.groups()
    if name == 'name': continue
    out = []
    for a in split_args(args):
        a = re.sub(r'\s*\b\w+$', '', a) if re.search(r'\b\w+$', a) and ' ' in a.strip() or '*' in a else a
        out.append(re.sub(r'\s+', ' ', a.replace('*', ' * ').replace('  ', ' ')).strip())
    protos[name] = (ret.strip(), [re.sub(r'\s+', ' ', x).strip() for x in out], False)

names = [l.strip() for l in open(os.path.join(ROOT, 'names.txt')) if l.strip()]

def norm(t):
    t = re.sub(r'\bconst\b', '', t).replace('*', ' * ')
    return re.sub(r'\s+', ' ', t).strip()

def classify(t):
    n = norm(t)
    if n == 'void': return 'void'
    if n.startswith('PFN'): return 'cb'
    if n == 'float': return 'f32'
    if n.replace(' ', '') == 'ptrdiff_t*': return 'ptrdiff_out'
    if n.endswith('* *') : return 'ptrarray'
    if '*' in n or n in ('NVNnativeWindow',): return 'ptr'
    if n in S64: return 's64'
    if n in STRUCT64: return 'struct64'
    if n in S32 or n in ENUMS: return 'u32'
    raise SystemExit('unclassified type %r' % t)

def gen(name, ret, args):
    kinds = [classify(a) for a in args]
    rk = classify(ret)
    L = []
    L.append('static NVNT_%s impl_%s;' % (name, name) if name in PROTO_TYPED else 'static %s (*impl_%s)(%s);' % (ret, name, ', '.join(args) if args else 'void'))
    L.append('static void ad_%s(Frame *f) {' % name)
    L.append('    if (g_nvn_trace) a32_nvn_trace(f);')
    L.append('    AC c = {f, 0, 0, 0, f->r[13]};')
    if rk == 'struct64': L.append('    uint32_t sret = ac32(&c);')
    last_int = None
    for i, (a, k) in enumerate(zip(args, kinds)):
        v = 'a%d' % i
        if k == 'u32':
            L.append('    uint32_t %s = ac32(&c);' % v); last_int = v
            if norm(a) == 'NVNboolean': L.append('    %s &= 0xff;' % v)
        elif k == 's64': L.append('    uint64_t %s = ac64(&c);' % v)
        elif k == 'struct64': L.append('    uint64_t %s = ac64(&c);' % v)
        elif k == 'f32': L.append('    float %s = acf(&c);' % v)
        elif k == 'ptr': L.append('    void *%s = gp(ac32(&c));' % v)
        elif k == 'ptrdiff_out': L.append('    uint32_t %s_g = ac32(&c); ptrdiff_t %s_v = 0; void *%s = &%s_v;' % (v, v, v, v))
        elif k == 'ptrarray':
            L.append('    void *%s_t[16]; { uint32_t g = ac32(&c); const uint32_t *s = g ? (const uint32_t *)(g_mem + g) : NULL; unsigned n = %s > 16 ? 16 : %s;' % (v, last_int, last_int))
            L.append('      for (unsigned k = 0; k < n; k++) %s_t[k] = s ? gp(s[k]) : NULL; } void *%s = %s_t;' % (v, v, v))
        else: raise SystemExit('%s: argument kind %s' % (name, k))
    if name == 'nvnWindowSetPresentInterval':                                                    # the game is a 30 fps title that asks for 1: its logic steps once per presented frame, so 1 runs it at twice the speed
        L.append('    { static int ov = -1; if (ov < 0) { const char *e = getenv("A32_PRESENT_INTERVAL"); ov = e ? atoi(e) : 2; } if (ov > 0) a1 = (uint32_t)ov; }')
    call = 'impl_%s(%s)' % (name, ', '.join(('(%s)a%d' % (args[i], i)) if kinds[i] in ('u32',) else
                                         ('(%s)a%d' % (args[i], i) if kinds[i] in ('ptr', 'ptrarray', 'ptrdiff_out') else
                                          ('(NVN%s){a%d}' % ('separateTextureHandle' if 'Texture' in args[i] else 'separateSamplerHandle', i) if kinds[i] == 'struct64' else
                                           'a%d' % i)) for i in range(len(args))))
    if rk == 'void': L.append('    %s;' % call)
    elif rk == 'u32': L.append('    %s r = %s; f->r[0] = (uint32_t)r;' % (ret, call))
    elif rk == 's64': L.append('    uint64_t r = %s; f->r[0] = (uint32_t)r; f->r[1] = (uint32_t)(r >> 32);' % call)
    elif rk == 'f32': L.append('    float r = %s; memcpy(f->q[0], &r, 4);' % call)
    elif rk == 'ptr': L.append('    f->r[0] = hp((const void *)%s);' % call)
    elif rk == 'struct64': L.append('    %s r = %s; uint64_t v = r.value; memcpy(g_mem + sret, &v, 8); f->r[0] = sret;' % (ret, call))
    else: raise SystemExit('%s: return kind %s' % (name, rk))
    for i, k in enumerate(kinds):
        if k == 'ptrdiff_out': L.append('    wr32(a%d_g, (uint32_t)a%d_v);' % (i, i))
    L.append('}')
    return '\n'.join(L)

PROTO_TYPED = {n for n, p in protos.items() if p[2]}
body, table, skipped = [], [], []
for n in names:
    if n in HAND: continue
    if n not in protos: skipped.append(n); continue
    ret, args, _ = protos[n]
    try: body.append(gen(n, ret, args))
    except SystemExit as e: skipped.append('%s (%s)' % (n, e)); continue
    table.append('    { "%s", ad_%s, (void **)&impl_%s },' % (n, n, n))

hdr = '''// Generated by dm/a32/gen_nvn_adapters.py; do not edit. Adapters from the 32-bit guest's NVN calls to the host's NVN implementation.
#define _DARWIN_C_SOURCE
#include "a32host.h"
#include "a32gfx.h"
#include "nvn.h"
#include "nvn_protos.h"
#include "nvn_count.h"

typedef struct { Frame *f; unsigned nr, ns, nf; uint32_t sp; } AC;
static inline uint32_t ac32(AC *c) { if (c->nr < 4) return c->f->r[c->nr++]; return rd32(c->sp + 4 * c->ns++); }
static inline uint64_t ac64(AC *c) {
    if (c->nr < 4) { c->nr = (c->nr + 1) & ~1u; if (c->nr < 4) { uint64_t v = c->f->r[c->nr] | (uint64_t)c->f->r[c->nr + 1] << 32; c->nr += 2; return v; } }
    c->nr = 4; c->ns = (c->ns + 1) & ~1u; uint64_t v = rd32(c->sp + 4 * c->ns) | (uint64_t)rd32(c->sp + 4 * c->ns + 4) << 32; c->ns += 2; return v;
}
static inline float acf(AC *c) { if (c->nf >= 16) die("NVN adapter: float argument on the stack"); float v; memcpy(&v, c->f->q[c->nf / 4] + 4 * (c->nf % 4), 4); c->nf++; return v; }
static inline void *gp(uint32_t a) { return a ? (void *)(g_mem + a) : NULL; }
static inline uint32_t hp(const void *p) {
    if (!p) return 0;
    uintptr_t d = (uintptr_t)p - (uintptr_t)g_mem;
    if (d >= 0x100000000ull) { LOG("[RED] NVN returned a host pointer outside the guest window: %p", p); return (uint32_t)(uintptr_t)p; }
    return (uint32_t)d;
}
'''
tail = '''
const A32NvnEnt a32_nvn_table[] = {
%s
    { NULL, NULL, NULL }
};
const char *nvn_names[NVN_NFUNC] = {
%s
};
void *nvn_thunk[NVN_NFUNC];
''' % ('\n'.join(table), '\n'.join('    "%s",' % n for n in names))
open(OUT, 'w').write(hdr + '\n'.join(body) + '\n' + tail)
print('%d adapters, %d by hand, %d without a signature: %s' % (len(table), len(HAND), len(skipped), skipped), file=sys.stderr)
