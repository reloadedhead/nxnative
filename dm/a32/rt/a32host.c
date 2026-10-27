// Host for the AArch32 ahead-of-time recompiler (dm/a32): maps 32-bit NSO modules into a 4 GB guest address space, links them
// (ELF32 REL relocations, imports resolved against the loaded modules, the rest bound to host hook stubs), and runs the
// translated code. The guest sees Horizon's 32-bit process model: svc numbers and register conventions are the 32-bit ones.
// Usage: a32host <build dir> [-v]   (the build dir has layout.tsv and image/<module>.bin from translate.py)
#define _DARWIN_C_SOURCE
#include "a32host.h"
#include "a32force.h"
#include "a32gfx.h"
#include <sys/ucontext.h>
#define MAX_MOD 8
#define MAX_HOOK 16384

typedef struct {
    char name[16]; uint32_t base, text, image, text_off, ro_off, data_off, filesz;
    uint32_t symtab, strtab, nsym, init, init_array, init_arraysz, fini;
    int nrel_relative, nrel_sym, nrel_hook, nrel_other;
} Mod;
static Mod g_mod[MAX_MOD]; static int g_nmod;
uint8_t *g_mem;                 // guest address 0 = g_mem
const char *g_build_dir;
uint64_t a32_tick_ctl[2] __attribute__((aligned(16))) = {1, 0};     // guest counter = real * ctl[0] + ctl[1] (A32_TICK_SCALE, A32_TICK_OFFSET=<seconds>, the turbo key); read and written as a pair                                  // debugging: A32_TICK_SCALE=n makes the guest's system counter run n times fast
int g_trace; uint32_t g_heap_size; uint8_t g_unimpl_svc_seen[256];
static int g_verbose;
void die(const char *fmt, ...) { va_list ap; va_start(ap, fmt); fputs("a32host: ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr); va_end(ap); exit(1); }

// ---- symbols and hooks -------------------------------------------------------------------------------------------------------
typedef struct { const char *name; uint32_t addr; } Sym;
static Sym *g_sym; static uint32_t g_symcap, g_nsym_total;
static uint32_t hash_str(const char *s) { uint32_t h = 2166136261u; while (*s) h = (h ^ (uint8_t)*s++) * 16777619u; return h; }
static void sym_put(const char *name, uint32_t addr) {
    for (uint32_t j = hash_str(name) & (g_symcap - 1);; j = (j + 1) & (g_symcap - 1)) {
        if (!g_sym[j].name) { g_sym[j].name = name; g_sym[j].addr = addr; g_nsym_total++; return; }
        if (!strcmp(g_sym[j].name, name)) return;        // the first definition in load order wins
    }
}
static uint32_t sym_get(const char *name) {
    for (uint32_t j = hash_str(name) & (g_symcap - 1); g_sym[j].name; j = (j + 1) & (g_symcap - 1))
        if (!strcmp(g_sym[j].name, name)) return g_sym[j].addr;
    return 0;
}
static const char *g_hook_name[MAX_HOOK]; static void (*g_hook_fn[MAX_HOOK])(Frame *); static unsigned g_nhook; static uint32_t g_hook_seen[MAX_HOOK / 32];
static uint32_t hook_for(const char *name, void (*fn)(Frame *)) {
    for (unsigned i = 0; i < g_nhook; i++) if (!strcmp(g_hook_name[i], name)) return HOOK_BASE + 16 * (i + 1);
    if (g_nhook >= MAX_HOOK) die("too many hooks");
    g_hook_name[g_nhook] = name; g_hook_fn[g_nhook] = fn; return HOOK_BASE + 16 * (++g_nhook);
}
static const ForceEnt *force_find(const char *name) {
    for (unsigned i = 0; i < a32_nforce; i++) if (!strcmp(a32_force[i].name, name)) return &a32_force[i];
    for (unsigned i = 0; i < a32_nforce_gfx; i++) if (!strcmp(a32_force_gfx[i].name, name)) return &a32_force_gfx[i];
    for (unsigned i = 0; i < a32_nforce_audren; i++) if (!strcmp(a32_force_audren[i].name, name)) return &a32_force_audren[i];
    for (unsigned i = 0; i < a32_nforce_movie; i++) if (!strcmp(a32_force_movie[i].name, name)) return &a32_force_movie[i];
    return NULL;
}
// a host entry point created at run time (the NVN functions the game asks nvnBootstrapLoader / nvnDeviceGetProcAddress for)
uint32_t a32_hook_add(const char *name, void (*fn)(Frame *)) { return hook_for(name, fn); }
static __thread const char *t_hook_name;
static __thread Frame *t_frame;                      // the frame of the hook being served on this thread
extern ThreadCtx *a32_cur_ctx(void);
const char *a32_hook_current_name(void) { return t_hook_name ? t_hook_name : "?"; }

// ---- ELF32 dynamic section ---------------------------------------------------------------------------------------------------
enum { DT_NULL, DT_NEEDED, DT_PLTRELSZ, DT_PLTGOT, DT_HASH, DT_STRTAB, DT_SYMTAB, DT_RELA, DT_RELASZ, DT_RELAENT, DT_STRSZ, DT_SYMENT, DT_INIT, DT_FINI,
       DT_SONAME, DT_RPATH, DT_SYMBOLIC, DT_REL, DT_RELSZ, DT_RELENT, DT_PLTREL, DT_DEBUG, DT_TEXTREL, DT_JMPREL, DT_BIND_NOW, DT_INIT_ARRAY, DT_FINI_ARRAY, DT_INIT_ARRAYSZ, DT_FINI_ARRAYSZ };
typedef struct { uint32_t name, value, size; uint8_t info, other; uint16_t shndx; } Sym32;
typedef struct { uint32_t off, info; } Rel32;
typedef struct { uint32_t rel, relsz, jmprel, pltrelsz, hash; } DynInfo;

static DynInfo parse_dyn(Mod *m) {
    DynInfo d = {0};
    uint32_t mod0 = m->base + rd32(m->base + 4);
    if (memcmp(g_mem + mod0, "MOD0", 4)) die("%s: no MOD0", m->name);
    uint32_t dyn = mod0 + rd32(mod0 + 4);
    for (uint32_t p = dyn;; p += 8) {
        uint32_t tag = rd32(p), val = rd32(p + 4);
        if (!tag) break;
        switch (tag) {
        case DT_SYMTAB: m->symtab = m->base + val; break;
        case DT_STRTAB: m->strtab = m->base + val; break;
        case DT_HASH: d.hash = m->base + val; break;
        case DT_REL: d.rel = m->base + val; break;
        case DT_RELSZ: d.relsz = val; break;
        case DT_JMPREL: d.jmprel = m->base + val; break;
        case DT_PLTRELSZ: d.pltrelsz = val; break;
        case DT_INIT: m->init = val; break;
        case DT_FINI: m->fini = val; break;
        case DT_INIT_ARRAY: m->init_array = val; break;
        case DT_INIT_ARRAYSZ: m->init_arraysz = val; break;
        case DT_RELA: die("%s: DT_RELA in a 32-bit module", m->name);
        }
    }
    m->nsym = d.hash ? rd32(d.hash + 4) : (m->strtab - m->symtab) / 16;
    return d;
}

typedef struct { uint32_t addr; const char *name; } Near;
static Near *g_near; static unsigned g_nnear, g_nearcap;
static void near_add(uint32_t a, const char *n) { if (g_nnear == g_nearcap) { g_nearcap = g_nearcap ? g_nearcap * 2 : 1 << 16; g_near = realloc(g_near, g_nearcap * sizeof *g_near); } g_near[g_nnear++] = (Near){a, n}; }
static int near_cmp(const void *a, const void *b) { uint32_t x = ((const Near *)a)->addr, y = ((const Near *)b)->addr; return x < y ? -1 : x > y; }
static const char *near_name(uint32_t addr, uint32_t *off) {
    unsigned lo = 0, hi = g_nnear;
    while (lo < hi) { unsigned mid = (lo + hi) / 2; if (g_near[mid].addr <= addr) lo = mid + 1; else hi = mid; }
    if (!lo) return NULL;
    *off = addr - g_near[lo - 1].addr; return *off < 0x4000 ? g_near[lo - 1].name : NULL;
}
static void export_symbols(Mod *m) {
    for (uint32_t i = 1; i < m->nsym; i++) {
        Sym32 s; memcpy(&s, g_mem + m->symtab + i * 16, 16);
        unsigned bind = s.info >> 4, type = s.info & 15;
        if (!s.shndx || (bind != 1 && bind != 2) || (type != 0 && type != 1 && type != 2)) continue;
        sym_put(gstr(m->strtab + s.name), m->base + (s.value & ~1u)); near_add(m->base + (s.value & ~1u), gstr(m->strtab + s.name));
    }
}

static void relocate(Mod *m, DynInfo *d) {
    uint32_t tabs[2] = {d->rel, d->jmprel}, sizes[2] = {d->relsz, d->pltrelsz};
    for (int t = 0; t < 2; t++)
        for (uint32_t i = 0; i < sizes[t] / 8; i++) {
            Rel32 r; memcpy(&r, g_mem + tabs[t] + i * 8, 8);
            unsigned type = r.info & 0xff, si = r.info >> 8; uint32_t slot = m->base + r.off;
            if (type == 23) { wr32(slot, rd32(slot) + m->base); m->nrel_relative++; continue; }          // R_ARM_RELATIVE
            Sym32 s; memcpy(&s, g_mem + m->symtab + si * 16, 16);
            const char *nm = gstr(m->strtab + s.name);
            const ForceEnt *fe = force_find(nm);
            uint32_t v = fe ? hook_for(nm, fe->fn) : sym_get(nm);
            if (fe) m->nrel_hook++;
            if (!v && s.shndx) v = m->base + (s.value & ~1u);                                             // its own definition
            int weak = (s.info >> 4) == 2;
            if (!v && weak) { v = 0; }
            else if (!v) { v = hook_for(nm, NULL); m->nrel_hook++; if (g_verbose) LOG("%s: import %s -> host hook %#x", m->name, nm, v); }
            else m->nrel_sym++;
            { static const char *bind = NULL; static int init; if (!init) { init = 1; bind = getenv("A32_BIND"); } if (bind && strstr(nm, bind)) LOG("bind %s: %s slot %#x type %u -> %#x (%s)", m->name, nm, slot, type, v, where(v)); }
            if (type == 21 || type == 22) wr32(slot, v);                                                  // GLOB_DAT, JUMP_SLOT
            else if (type == 2) wr32(slot, v + rd32(slot));                                              // ABS32
            else { m->nrel_other++; if (m->nrel_other < 6) LOG("%s: relocation type %u (%s) not handled", m->name, type, nm); }
        }
}

static void load_modules(const char *dir) {
    char path[1024], line[512];
    snprintf(path, sizeof path, "%s/layout.tsv", dir);
    FILE *f = fopen(path, "r"); if (!f) die("cannot open %s", path);
    while (fgets(line, sizeof line, f)) {
        Mod *m = &g_mod[g_nmod];
        if (sscanf(line, "%15s %x %x %x %x %x %x %x", m->name, &m->base, &m->text, &m->image, &m->text_off, &m->ro_off, &m->data_off, &m->filesz) != 8) continue;
        snprintf(path, sizeof path, "%s/image/%s.bin", dir, m->name);
        FILE *im = fopen(path, "rb"); if (!im) die("cannot open %s", path);
        if (fread(g_mem + m->base, 1, m->filesz, im) != m->filesz) die("%s: short image", path);
        fclose(im); g_nmod++;
    }
    fclose(f);
}

// ---- guest threads and calls ---------------------------------------------------------------------------------------------------
static uint32_t g_main_sp;
static uint32_t a32_call(ThreadCtx *c, uint32_t pc, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t sp) {
    uint32_t regs[16] = {0};
    regs[0] = a0; regs[1] = a1; regs[2] = a2; regs[3] = a3; regs[13] = sp; regs[14] = HOOK_BASE;
    return a32_enter(c, regs, pc);
}
uint32_t a32_find_symbol(const char *name) { return sym_get(name); }
static uint32_t call_sym(const char *name, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    uint32_t a = sym_get(name); if (!a) die("required symbol %s not found", name);
    return a32_call(a32_main_ctx(), a, a0, a1, a2, a3, g_main_sp);
}

// ---- hooks and svcs ------------------------------------------------------------------------------------------------------------
const char *where(uint32_t addr) {
    static __thread char buf[200];
    for (int i = 0; i < g_nmod; i++) if (addr >= g_mod[i].base && addr < g_mod[i].base + g_mod[i].text) {
        uint32_t off; const char *n = near_name(addr, &off);
        if (n) snprintf(buf, sizeof buf, "%s+%#x [%s+%#x]", g_mod[i].name, addr - g_mod[i].base, n, off); else snprintf(buf, sizeof buf, "%s+%#x", g_mod[i].name, addr - g_mod[i].base);
        return buf; }
    snprintf(buf, sizeof buf, "%#x", addr); return buf;
}
void dump_regs(const Frame *f) {
    fprintf(stderr, "   r0-r3 %08x %08x %08x %08x  r4-r7 %08x %08x %08x %08x\n   r8-r11 %08x %08x %08x %08x  r12 %08x sp %08x lr %08x (%s)\n",
            f->r[0], f->r[1], f->r[2], f->r[3], f->r[4], f->r[5], f->r[6], f->r[7], f->r[8], f->r[9], f->r[10], f->r[11], f->r[12], f->r[13], f->r[14], where(f->r[14]));
    // backtrace through the frame-pointer chain (r11; the SDK builds with frame pointers), and the return addresses found on the stack
    fprintf(stderr, "   backtrace (fp chain):");
    uint32_t fp = f->r[11];
    for (int i = 0; i < 16 && fp >= 0x1000 && fp < 0xf0000000u && !(fp & 3); i++) { uint32_t lr = rd32(fp + 4); fprintf(stderr, " %s", where(lr)); fp = rd32(fp); }
    fprintf(stderr, "\n   code addresses on the stack:");
    int n = 0;
    for (uint32_t sp = f->r[13]; n < 14 && sp < f->r[13] + 0x400; sp += 4) { uint32_t v = rd32(sp); for (int k = 0; k < g_nmod; k++) if (v >= g_mod[k].base && v < g_mod[k].base + g_mod[k].text) { fprintf(stderr, " %s", where(v)); n++; break; } }
    fputc('\n', stderr);
}
uint32_t a32_ring[2 + 256 + 256];
static void ring_dump(void) {
    fprintf(stderr, "   last dispatches (target <- lr):");
    uint32_t pos = a32_ring[0];
    for (int k = 24; k >= 1; k--) { uint32_t i = (pos - (uint32_t)k) & 255; if (a32_ring[2 + i]) fprintf(stderr, "\n      %s <- %s", where(a32_ring[2 + i]), where(a32_ring[2 + 256 + i])); }
    fputc('\n', stderr);
}
// A32_TRACE_PEEK="reg:offset:size" (decimal or 0x): every traced instruction also prints that guest memory value (size 1, 2 or 4 bytes, signed 16 for 2)
static void trace_peek(const Frame *f) {
    static int on = -1; static unsigned reg, off, size; if (on < 0) { on = 0; const char *e = getenv("A32_TRACE_PEEK"); if (e && sscanf(e, "%u:%i:%u", &reg, (int *)&off, &size) == 3 && reg < 16) on = 1; }
    if (!on) return; uint32_t a = f->r[reg] + off; int32_t v = size == 1 ? (int32_t)g_mem[a] : size == 2 ? (int32_t)(int16_t)(g_mem[a] | g_mem[a + 1] << 8) : (int32_t)rd32(a);
    const char *nn = getenv("A32_TRACE_PEEKN"); int cnt = nn ? atoi(nn) : 0;
    if (cnt > 0) { fprintf(stderr, "   peek [r%u%+d] = %d |", reg, (int)off, v); uint32_t base = f->r[reg]; for (int i = 0; i < cnt; i++) fprintf(stderr, " %x", rd32(base + 4 * (unsigned)i)); fputc('\n', stderr); }
    else fprintf(stderr, "   peek [r%u%+d] = %d\n", reg, (int)off, v);
}
void a32_trace_c(uint32_t addr, Frame *f) {
    trace_peek(f);
    const char *mn = "?"; uint32_t mo = addr; for (int i = 0; i < g_nmod; i++) if (addr >= g_mod[i].base && addr < g_mod[i].base + g_mod[i].text) { mn = g_mod[i].name; mo = addr - g_mod[i].base; }
    fprintf(stderr, "T %s+%#x %08x | r0 %08x r1 %08x r2 %08x r3 %08x r4 %08x r5 %08x sp %08x lr %08x nzcv %x\n", mn, mo, rd32(addr), f->r[0], f->r[1], f->r[2], f->r[3], f->r[4], f->r[5], f->r[13], f->r[14], f->nzcv >> 28);
    for (int i = 0; i < 3; i++) if (f->r[i] >= 0x200000 && f->r[i] < 0xf0000000u - 256) { const char *t = gstr(f->r[i]); int n = 0; while (n < 120 && t[n] >= 32 && t[n] < 127) n++; if (n >= 3 && !t[n]) fprintf(stderr, "    r%d -> \"%s\"\n", i, t); }
}
void a32_unsup_c(uint32_t addr, Frame *f) {
    fprintf(stderr, "[a32] FATAL: executed a word the translator did not cover at %s: %08x\n", where(addr), rd32(addr)); dump_regs(f); ring_dump(); exit(3);
}
static int plausible_str(uint32_t a) { if (a < 0x200000 || a >= 0x7f000000) return 0; const char *t = (const char *)g_mem + a; int n = 0; while (n < 200 && t[n]) { if ((unsigned char)t[n] < 9 || (unsigned char)t[n] > 126) return 0; n++; } return n >= 3 && n < 200; }
// the strings reachable from the first few registers and from the words they point to (an abort's SdkAbortInfo carries the condition text, function and file)
void dump_strings(const Frame *f) {
    for (int i = 0; i < 4; i++) {
        uint32_t a = f->r[i];
        if (plausible_str(a)) fprintf(stderr, "   r%d -> \"%.160s\"\n", i, gstr(a));
        else if (a >= 0x1000 && a < 0xf0000000u - 64) for (int k = 0; k < 12; k++) { uint32_t v = rd32(a + 4 * k); if (plausible_str(v)) fprintf(stderr, "   [r%d+%#x] -> \"%.160s\"\n", i, 4 * k, gstr(v)); }
    }
}
void a32_bad_target_c(uint32_t addr, Frame *f) {
    fprintf(stderr, "[a32] FATAL: branch to %#x (not translated code%s)\n", addr, addr & 1 ? ", odd: Thumb" : ""); dump_regs(f); dump_strings(f); ring_dump(); exit(3);
}
void a32_hook_c(uint32_t addr, Frame *f) {
    unsigned i = (addr - HOOK_BASE) / 16 - 1;
    f->next = f->r[14];
    if (i >= g_nhook) { fprintf(stderr, "[a32] FATAL: call to unknown hook %#x\n", addr); dump_regs(f); exit(3); }
    const char *nm = g_hook_name[i];
    if (g_hook_fn[i]) { Frame *prev = t_frame; t_frame = f; t_hook_name = nm; g_hook_fn[i](f); t_frame = prev; return; }
    if (!(g_hook_seen[i / 32] & (1u << (i % 32)))) { g_hook_seen[i / 32] |= 1u << (i % 32); LOG("unimplemented import %s called from %s (returns 0)", nm, where(f->r[14])); }
    f->r[0] = 0;
}

// call a guest function from the host, on the calling guest thread: inside a hook below the hook's caller, on a helper guest thread below its own stack pointer.
// More than four arguments: the rest go on the guest stack (AAPCS)
extern uint32_t a32_cb_sp(void);
uint32_t a32_callbackv(uint32_t pc, int n, const uint32_t *args) {
    uint32_t base = t_frame ? t_frame->r[13] : a32_cb_sp();
    if (!base) die("a32_callback outside a guest thread");
    Frame *saved = t_frame, copy; if (saved) copy = *saved;
    uint32_t sp = (base - 256 - 4u * (uint32_t)(n > 4 ? n - 4 : 0)) & ~7u, regs[16] = {0};
    for (int i = 0; i < n && i < 4; i++) regs[i] = args[i];
    for (int i = 4; i < n; i++) wr32(sp + 4u * (uint32_t)(i - 4), args[i]);
    regs[13] = sp; regs[14] = HOOK_BASE;
    uint32_t r = a32_enter(a32_cur_ctx(), regs, pc);
    if (saved) { *saved = copy; t_frame = saved; }
    return r;
}
uint32_t a32_callback(uint32_t pc, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) { uint32_t a[4] = {a0, a1, a2, a3}; return a32_callbackv(pc, 4, a); }

// ---- faults ---------------------------------------------------------------------------------------------------------------------
extern int a32_gfx_fault(void *addr, int is_write, void *uc);

// A32_WWATCH=<guest address>: the 16 KB page holding that float is kept read-only; every first write after a short pause is caught, the guest code address of the writer is remembered and
// the page is opened again. A checker thread prints the writers seen whenever the watched float changes (debugging: who writes this value).
#include <sys/mman.h>
static uint64_t g_ww_page, g_ww_addr; static volatile int g_ww_on, g_ww_open; static struct { uint64_t pc; uint32_t r[8]; uint32_t lr, sp; } g_ww_ring[16]; static volatile unsigned g_ww_n;
static const char *ww_where(uint64_t pc, char *buf, size_t cap) {
    for (int i = 0; ; i++) {
        if (!a32_modtab[i].size) break;
        uint64_t s = (uint64_t)(uintptr_t)a32_modtab[i].slots, e = s + (uint64_t)(a32_modtab[i].size / 4) * 16;
        if (pc >= s && pc < e) { snprintf(buf, cap, "%s+%#x", g_mod[i].name, a32_modtab[i].lo + (uint32_t)((pc - s) / 16) * 4 - g_mod[i].base); return buf; }
    }
    snprintf(buf, cap, "host %#llx", (unsigned long long)pc); return buf;
}
// the page stays read-only: a plain "store register, register offset" is carried out here (the page is opened for that one store) and skipped, so no write is missed. Other forms (exclusive
// stores, ...) open the page for a moment as before.
static int ww_emulate(ucontext_t *uc, uint64_t fa) {
    uint64_t pc = uc->uc_mcontext->__ss.__pc; uint32_t insn = *(uint32_t *)(uintptr_t)pc;
    if ((insn & 0x3f200c00) != 0x38200800 && (insn & 0x3f200c00) != 0x3c200800) return 0;
    int size = (int)(insn >> 30), V = (int)(insn >> 26 & 1), opc = (int)(insn >> 22 & 3), Rt = (int)(insn & 31);
    if (opc != 0 || size > 3) return 0;
    uint64_t val;
    if (!V) val = Rt == 31 ? 0 : uc->uc_mcontext->__ss.__x[Rt];
    else { if (size > 3) return 0; memcpy(&val, &uc->uc_mcontext->__ns.__v[Rt], 8); }
    (void)fa; uint8_t *dst = (uint8_t *)(uintptr_t)fa;
    mprotect((void *)(uintptr_t)g_ww_page, 0x4000, PROT_READ | PROT_WRITE);
    memcpy(dst, &val, (size_t)1 << size);
    mprotect((void *)(uintptr_t)g_ww_page, 0x4000, PROT_READ);
    uc->uc_mcontext->__ss.__pc = pc + 4; return 1;
}
static int ww_fault(uint64_t fa, ucontext_t *uc) {
    if (!g_ww_on || (fa & ~0x3fffull) != g_ww_page) return 0;
    if (fa >= g_ww_addr && fa < g_ww_addr + 4) { unsigned k = g_ww_n++ & 15; g_ww_ring[k].pc = uc->uc_mcontext->__ss.__pc; for (int i = 0; i < 8; i++) g_ww_ring[k].r[i] = (uint32_t)uc->uc_mcontext->__ss.__x[i]; g_ww_ring[k].lr = (uint32_t)uc->uc_mcontext->__ss.__x[14]; g_ww_ring[k].sp = (uint32_t)uc->uc_mcontext->__ss.__x[13]; }
    if (ww_emulate(uc, fa)) return 1;
    mprotect((void *)(uintptr_t)g_ww_page, 0x4000, PROT_READ | PROT_WRITE); g_ww_open = 1; return 1;
}
static void *ww_thread(void *arg) {
    (void)arg; float last; memcpy(&last, (void *)(uintptr_t)g_ww_addr, 4); unsigned seen = 0;
    for (;;) {
        usleep(100);
        if (g_ww_open) { usleep(50); g_ww_open = 0; mprotect((void *)(uintptr_t)g_ww_page, 0x4000, PROT_READ); }
        float v; memcpy(&v, (void *)(uintptr_t)g_ww_addr, 4);
        if (memcmp(&v, &last, 4)) {
            fprintf(stderr, "[a32] wwatch %#x: %f -> %f; last writers:", (uint32_t)(g_ww_addr - (uint64_t)(uintptr_t)g_mem), last, v);
            unsigned n = g_ww_n; for (unsigned i = n > 6 ? n - 6 : 0; i < n; i++) { char b[96]; fprintf(stderr, " [%s r0-r3 %08x %08x %08x %08x lr %08x sp %08x]", ww_where(g_ww_ring[i & 15].pc, b, sizeof b), g_ww_ring[i & 15].r[0], g_ww_ring[i & 15].r[1], g_ww_ring[i & 15].r[2], g_ww_ring[i & 15].r[3], g_ww_ring[i & 15].lr, g_ww_ring[i & 15].sp); }
            fputc('\n', stderr); last = v; seen = n;
        }
        (void)seen;
    }
    return NULL;
}
static void on_fault(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_;
    if (ww_fault((uint64_t)(uintptr_t)si->si_addr, uc)) return;
    if (a32_gfx_fault(si->si_addr, (int)(uc->uc_mcontext->__es.__esr >> 6 & 1), uc)) return;
    uint64_t pc = uc->uc_mcontext->__ss.__pc, fa = (uint64_t)(uintptr_t)si->si_addr;
    fprintf(stderr, "[a32] FATAL: signal %d at host pc %#llx, address %#llx", sig, (unsigned long long)pc, (unsigned long long)fa);
    if (fa >= (uint64_t)(uintptr_t)g_mem && fa < (uint64_t)(uintptr_t)g_mem + (1ull << 32)) fprintf(stderr, " = guest address %#llx", (unsigned long long)(fa - (uint64_t)(uintptr_t)g_mem));
    for (int i = 0; ; i++) {
        if (!a32_modtab[i].size) break;
        uint64_t s = (uint64_t)(uintptr_t)a32_modtab[i].slots, e = s + (uint64_t)(a32_modtab[i].size / 4) * 16;
        if (pc >= s && pc < e) { fprintf(stderr, "; guest code %s (addr %#x)", g_mod[i].name, a32_modtab[i].lo + (uint32_t)((pc - s) / 16) * 4); break; }
    }
    fputc('\n', stderr);
    fprintf(stderr, "   r0-r7 ");
    for (int i = 0; i < 8; i++) fprintf(stderr, "%08x ", (uint32_t)uc->uc_mcontext->__ss.__x[i]);
    fprintf(stderr, "\n   r8-r14 ");
    for (int i = 8; i < 15; i++) fprintf(stderr, "%08x ", (uint32_t)uc->uc_mcontext->__ss.__x[i]);
    fputc('\n', stderr); _exit(4);
}

// ---- main -------------------------------------------------------------------------------------------------------------------------

static int g_argc; static char **g_argv;
extern void front_run(void (*guest)(void));
static void guest_main(void);
int main(int argc, char **argv) {
    if (argc < 2) die("usage: a32host <build dir> [-v]");
    g_argc = argc; g_argv = argv;
    front_run(guest_main);                                   // with DM_WINDOW the main thread runs the window and the guest gets its own thread
    return 0;
}

// A32_DUMP="s1,s2,...": the guest heap is written to $A32_DUMP_DIR/heap_<n>.bin at those seconds after start (debugging: find what changes with the input)
#include <pthread.h>
static void *dump_thread(void *arg) {
    (void)arg; const char *dir = getenv("A32_DUMP_DIR") ? getenv("A32_DUMP_DIR") : "/tmp"; char spec[256]; snprintf(spec, sizeof spec, "%s", getenv("A32_DUMP"));
    extern uint64_t a32_keys_t0_ms; while (!a32_keys_t0_ms) usleep(50000);                         // seconds are counted from the scripted key clock
    struct timespec t0 = { (time_t)(a32_keys_t0_ms / 1000), (long)(a32_keys_t0_ms % 1000) * 1000000 }; int n = 0;
    for (char *tok = strtok(spec, ","); tok; tok = strtok(NULL, ","), n++) {
        double at = atof(tok);
        for (;;) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); double e = (double)(t.tv_sec - t0.tv_sec) + (double)(t.tv_nsec - t0.tv_nsec) / 1e9; if (e >= at) break; usleep(20000); }
        char path[512]; snprintf(path, sizeof path, "%s/heap_%d.bin", dir, n); FILE *f = fopen(path, "wb");
        if (f) { fwrite(g_mem + HEAP_BASE, 1, g_heap_size, f); fclose(f); fprintf(stderr, "[a32] dumped the heap (%u bytes) to %s\n", g_heap_size, path); }
    }
    return NULL;
}

// A32_WATCHF=<guest address>[,<address>...]: those floats are printed once a second (debugging)
static void *watchf_thread(void *arg) {
    (void)arg; char spec[256]; snprintf(spec, sizeof spec, "%s", getenv("A32_WATCHF")); uint32_t a[8]; int n = 0;
    for (char *t = strtok(spec, ","); t && n < 8; t = strtok(NULL, ",")) a[n++] = (uint32_t)strtoul(t, NULL, 0);
    int ms = getenv("A32_WATCHF_MS") ? atoi(getenv("A32_WATCHF_MS")) : 1000; char last[256] = ""; struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        usleep((useconds_t)ms * 1000); char line[256]; int o = 0;
        for (int i = 0; i < n; i++) { float v; memcpy(&v, g_mem + a[i], 4); o += snprintf(line + o, sizeof line - (size_t)o, " %#x=%.3f", a[i], v); }
        struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); double e = (double)(t.tv_sec - t0.tv_sec) + (double)(t.tv_nsec - t0.tv_nsec) / 1e9;
        if (ms >= 1000 || strcmp(line, last)) { struct timespec rt; clock_gettime(CLOCK_REALTIME, &rt); fprintf(stderr, "[a32] watch %.2f @%.3f:%s\n", e, (double)rt.tv_sec + (double)rt.tv_nsec / 1e9, line); snprintf(last, sizeof last, "%s", line); }
    }
    return NULL;
}

// A32_POKEF="addr:value:seconds": at that many seconds on the scripted key clock the float is written into guest memory (debugging experiments)
static void *pokef_thread(void *arg) {
    (void)arg; char spec[1024]; snprintf(spec, sizeof spec, "%s", getenv("A32_POKEF")); char *a = strtok(spec, ":"), *v = strtok(NULL, ":"), *t = strtok(NULL, ":");   // addr[,addr...]:value:seconds
    if (!a || !v || !t) return NULL;
    extern uint64_t a32_keys_t0_ms; while (!a32_keys_t0_ms) usleep(50000);
    struct timespec t0 = { (time_t)(a32_keys_t0_ms / 1000), (long)(a32_keys_t0_ms % 1000) * 1000000 };
    for (;;) { struct timespec n; clock_gettime(CLOCK_MONOTONIC, &n); double e = (double)(n.tv_sec - t0.tv_sec) + (double)(n.tv_nsec - t0.tv_nsec) / 1e9; if (e >= atof(t)) break; usleep(20000); }
    float f = (float)atof(v); for (char *x = strtok(a, ","); x; x = strtok(NULL, ",")) { memcpy(g_mem + (uint32_t)strtoul(x, NULL, 0), &f, 4); fprintf(stderr, "[a32] poked %s = %f\n", x, f); }
    return NULL;
}
static void on_usr1(int sig) { (void)sig; g_trace = g_trace ? 0 : 3; }
static void on_usr2(int sig) { (void)sig; ring_dump(); }
static void guest_main(void) {
    int argc = g_argc; char **argv = g_argv;
    g_build_dir = argv[1];
    if (getenv("A32_POKEF")) { pthread_t th; pthread_create(&th, NULL, pokef_thread, NULL); pthread_detach(th); }
    if (getenv("A32_WATCHF")) { pthread_t th; pthread_create(&th, NULL, watchf_thread, NULL); pthread_detach(th); }
    if (getenv("A32_DUMP")) { pthread_t th; pthread_create(&th, NULL, dump_thread, NULL); pthread_detach(th); }
    if (getenv("A32_TICK_OFFSET")) a32_tick_ctl[1] = (uint64_t)(atof(getenv("A32_TICK_OFFSET")) * 19200000.0);
    if (getenv("A32_TICK_SCALE")) a32_tick_ctl[0] = (uint64_t)atoll(getenv("A32_TICK_SCALE"));
    g_verbose = argc > 2 && !strcmp(argv[2], "-v"); g_trace = getenv("A32_SVC") ? atoi(getenv("A32_SVC")) : 1;
    setvbuf(stderr, NULL, _IOLBF, 0);
    g_mem = mmap(NULL, (1ull << 32) + (1ull << 20), PROT_NONE, MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
    if (g_mem == MAP_FAILED) die("cannot reserve the 4 GB guest address space");
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_fault; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    signal(SIGUSR1, on_usr1); signal(SIGUSR2, on_usr2);          // debugging: USR1 toggles the svc trace, USR2 prints the last dispatches
    g_symcap = 1 << 18; g_sym = calloc(g_symcap, sizeof *g_sym);
    // make the whole low part (modules, thread areas, main stack) readable and writable; the guest never executes memory
    if (mprotect(g_mem, 0xf0000000u, PROT_READ | PROT_WRITE)) die("mprotect the guest address space");      // all of it, lazily committed: the 16 KB host pages are coarser than the guest's 4 KB ones
    load_modules(argv[1]);
    LOG("%d modules loaded", g_nmod);
    if (getenv("A32_WWATCH")) { g_ww_addr = (uint64_t)(uintptr_t)g_mem + strtoul(getenv("A32_WWATCH"), NULL, 0); g_ww_page = g_ww_addr & ~0x3fffull; g_ww_on = 1; mprotect((void *)(uintptr_t)g_ww_page, 0x4000, PROT_READ); pthread_t th; pthread_create(&th, NULL, ww_thread, NULL); pthread_detach(th); }
    { extern const void *const *const a32_chunk_tab[]; extern const unsigned a32_chunk_n[], a32_chunk_size;      // the translated slots must be exactly where the dispatcher computes them
      for (int i = 0; i < g_nmod; i++) for (unsigned k = 1; k < a32_chunk_n[i]; k++)
          if ((const char *)a32_chunk_tab[i][k] - (const char *)a32_chunk_tab[i][0] != (long)k * a32_chunk_size) die("%s: translated chunk %u is not at the expected offset: the linker inserted code between the slot chunks", g_mod[i].name, k); }
    DynInfo di[MAX_MOD];
    for (int i = 0; i < g_nmod; i++) { di[i] = parse_dyn(&g_mod[i]); export_symbols(&g_mod[i]); }
    qsort(g_near, g_nnear, sizeof *g_near, near_cmp);
    LOG("%u exported symbols", g_nsym_total);
    for (int i = 0; i < g_nmod; i++) {
        relocate(&g_mod[i], &di[i]);
        LOG("%s: base %#x text %#x image %#x: %d relative, %d bound to modules, %d to host hooks, %d other", g_mod[i].name, g_mod[i].base, g_mod[i].text, g_mod[i].image,
            g_mod[i].nrel_relative, g_mod[i].nrel_sym, g_mod[i].nrel_hook, g_mod[i].nrel_other);
    }
    g_main_sp = 0x10400000u;
    a32_main_thread_init(g_main_sp);
    for (int i = 0; i < g_nmod; i++) {
        Mod *m = &g_mod[i];
        a32_region_add(m->base, m->ro_off, 3, 5); a32_region_add(m->base + m->ro_off, m->data_off - m->ro_off, 3, 1);
        a32_region_add(m->base + m->data_off, (m->image - m->data_off + 0xfff) & ~0xfffu, 4, 3);
    }
    a32_region_add(0x0f000000, 0x100000, 0xc, 3);
    a32_region_add(0x10000000, 0x400000, 0xb, 3);
    // Horizon's start sequence (see dm/host/main.c guest_main for the 64-bit host): libc and os init, SDK init, startup, module constructors, nnMain
    call_sym("_init_libc0", 0, 0, 0, 0);
    LOG(">>> nnosInitialize"); call_sym("nnosInitialize", a32_main_handle(), 0, 0, 0);
    call_sym("_init_libc1", 0, 0, 0, 0);
    LOG(">>> nninitInitializeSdkModule"); call_sym("nninitInitializeSdkModule", 0, 0, 0, 0);
    LOG(">>> nninitStartup"); call_sym("nninitStartup", 0, 0, 0, 0);
    call_sym("_init_libc2", 0, 0, 0, 0);
    for (int i = 0; i < g_nmod; i++) {                       // rtld's InitModules: each module's DT_INIT (its global constructors), in load order
        Mod *m = &g_mod[i];
        if (!m->init || !strcmp(m->name, "rtld")) continue;
        LOG("%s: DT_INIT +%#x (init_array %u entries)", m->name, m->init, m->init_arraysz / 4);
        a32_call(a32_main_ctx(), m->base + m->init, 0, 0, 0, 0, g_main_sp);
    }
    LOG(">>> nnMain"); call_sym("nnMain", 0, 0, 0, 0);
    LOG("nnMain returned");
    exit(0);
}
