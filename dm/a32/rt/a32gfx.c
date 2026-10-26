// Graphics glue for the 32-bit guest: the nv:: driver layer and nn::vi are replaced by static objects (as dm/gfx/hle_nv.c and hle_vi.c do for 64-bit
// guests), and nvnBootstrapLoader hands out host entry points for the NVN API. Guest-side objects the host creates live in a small guest arena.
#define _DARWIN_C_SOURCE
#include "a32host.h"
#include "a32force.h"
#include "a32gfx.h"
#include <sys/stat.h>

#define ARENA_BASE 0x04000000u                  // guest memory the host hands out (objects it makes, buffers the guest reads): 64 MB, registered as in use
#define ARENA_SIZE 0x04000000u
static uint32_t g_arena_pos = ARENA_BASE;
uint32_t a32_guest_alloc(uint32_t size, uint32_t align) {
    pthread_mutex_lock(&(pthread_mutex_t){PTHREAD_MUTEX_INITIALIZER});
    static int reg; if (!reg) { reg = 1; a32_region_add(ARENA_BASE, ARENA_SIZE, 0xb, 3); }
    uint32_t a = (g_arena_pos + align - 1) & ~(align - 1);
    if (a + size > ARENA_BASE + ARENA_SIZE) die("guest arena exhausted");
    g_arena_pos = a + size; memset(g_mem + a, 0, size); return a;
}

// ---- nv:: ----
uint32_t g_nv_alloc[4];                     // allocate, free, reallocate, user (guest function pointers)
static void f_nv_set_alloc(Frame *f) { for (int i = 0; i < 4; i++) g_nv_alloc[i] = f->r[i]; f->r[0] = 0; }
static void f_nv_nop(Frame *f) { f->r[0] = 0; }

// ---- nn::vi: one display, one layer, one native window ----
static uint32_t g_display, g_layer, g_native;
static void vi_init_objects(void) { if (!g_display) { g_display = a32_guest_alloc(16, 16); g_layer = a32_guest_alloc(16, 16); g_native = a32_guest_alloc(16, 16); wr32(g_display, 0x50534944); wr32(g_layer, 0x5259414c); wr32(g_native, 0x4e49574e); } }
static void f_vi_open_display(Frame *f) { vi_init_objects(); wr32(f->r[0], g_display); f->r[0] = 0; }
static void f_vi_create_layer(Frame *f) { vi_init_objects(); wr32(f->r[0], g_layer); f->r[0] = 0; }
static void f_vi_native_window(Frame *f) { vi_init_objects(); wr32(f->r[0], g_native); f->r[0] = 0; }
static void f_vi_list_displays(Frame *f) {            // DisplayInfo is 0x60 bytes: name[0x40], bool, u64 layer count limit, largest layer width and height
    uint32_t out = f->r[0]; int n = (int)f->r[1];
    if (n <= 0 || !out) { f->r[0] = 0; return; }
    memset(g_mem + out, 0, 0x60); strcpy((char *)g_mem + out, "Default"); g_mem[out + 0x40] = 1;
    uint64_t lim[3] = {1, 1920, 1080}; memcpy(g_mem + out + 0x48, lim, sizeof lim); f->r[0] = 1;
}

// ---- NVN: nvnBootstrapLoader(name) -> function pointer; every name becomes a host entry point ----
// The host implementation (dm/gfx/nvn) is linked in; a generated adapter per function (a32nvn_gen.c) turns the guest's call into a call of it.
#include "nvn.h"
#include "nvn_protos.h"
#include "nvn_count.h"
extern uint32_t a32_hook_add(const char *name, void (*fn)(Frame *));
extern const char *a32_hook_current_name(void);
extern uint32_t a32_callback(uint32_t pc, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
extern const A32NvnEnt a32_nvn_table[];
extern const char *nvn_names[NVN_NFUNC];
extern struct { _Atomic uint64_t cnt; void *fn; } nvn_slot[NVN_NFUNC];
extern void *nvn_big_real[NVN_NFUNC];
extern void nvn_unimpl_entry(void);

// the host implementation of a function, as hle_nvn.c / nvn_mtl.m registered it (NULL: not implemented)
static void *nvn_impl_of(const char *name) {
    for (unsigned i = 0; i < NVN_NFUNC; i++) if (!strcmp(nvn_names[i], name)) {
        void *fn = nvn_big_real[i] ? nvn_big_real[i] : nvn_slot[i].fn;
        return fn == (void *)nvn_unimpl_entry ? NULL : fn;
    }
    return NULL;
}
int g_nvn_trace = -1;
void a32_nvn_trace(Frame *f) {
    extern const char *a32_hook_current_name(void);
    LOG("nvn call %s from %s: %08x %08x %08x %08x", a32_hook_current_name(), where(f->r[14]), f->r[0], f->r[1], f->r[2], f->r[3]);
}
static void nvn_unimplemented(Frame *f) {
    static const char *seen[NVN_NFUNC + 8]; static unsigned n;
    const char *nm = a32_hook_current_name();
    for (unsigned i = 0; i < n; i++) if (!strcmp(seen[i], nm)) { f->r[0] = 0; return; }
    if (n < NVN_NFUNC + 8) seen[n++] = nm;
    LOG("[RED] NVN unimplemented function called: %s (from %s)", nm, where(f->r[14])); f->r[0] = 0;
}

// ---- adapters written by hand (callbacks, structures that hold pointers) ----
static void (*nvn_find(const char *name))(Frame *);
static void ad_get_proc(Frame *f) {                    // nvnDeviceGetProcAddress(device, name)
    const char *name = gstr(f->r[1]);
    f->r[0] = a32_hook_add(strdup(name), nvn_find(name) ? nvn_find(name) : nvn_unimplemented);
}
static void ad_nop(Frame *f) { f->r[0] = 0; }          // debug callbacks are not called on the host side
// A32_SHADER_DUMP=<dir>: every distinct shader the game hands to nvnProgramSetShaders is saved as <fnv>.shdr = 'BTCDBTCD' + 0x30 header (control length at +0x28, code length at +0x2c) + code area + control section
static void dump_shader(const uint8_t *code, const uint8_t *ctl, char *name_out) {
    static const char *dir; static int init;
    if (!init) { init = 1; dir = getenv("A32_SHADER_DUMP"); if (dir) mkdir(dir, 0755); }
    if (!dir) return;
    name_out[0] = 0;
    if (*(const uint32_t *)ctl != 0x98761234u) { LOG("[RED] shader control section without its magic (%08x)", *(const uint32_t *)ctl); return; }
    uint32_t nctl = *(const uint32_t *)(ctl + 0x20), ncode = *(const uint32_t *)(ctl + 0x704);
    if (nctl < 0x700 || nctl > 0x2000 || ncode < 0x40 || ncode > (16u << 20)) { LOG("[RED] shader control lengths %#x / %#x", nctl, ncode); return; }
    uint64_t h = 0xcbf29ce484222325ull; for (uint32_t i = 0; i < nctl; i++) h = (h ^ ctl[i]) * 0x100000001b3ull; for (uint32_t i = 0; i < ncode; i++) h = (h ^ code[i]) * 0x100000001b3ull;
    char path[1024]; snprintf(path, sizeof path, "%s/%016llx.shdr", dir, (unsigned long long)h);
    snprintf(name_out, 64, "%016llx.shdr", (unsigned long long)h);
    struct stat st; if (!stat(path, &st)) return;
    uint8_t hdr[0x38]; memset(hdr, 0, sizeof hdr); memcpy(hdr, "BTCDBTCD", 8); memcpy(hdr + 0x28, &nctl, 4); memcpy(hdr + 0x2c, &ncode, 4);
    FILE *fp = fopen(path, "wb"); if (!fp) return; fwrite(hdr, 1, 0x38, fp); fwrite(code, 1, ncode, fp); fwrite(ctl, 1, nctl, fp); fclose(fp);
}
static void ad_set_shaders(Frame *f) {                 // nvnProgramSetShaders(program, n, const NVNshaderData *): the guest's has a 32-bit control pointer
    static NVNT_nvnProgramSetShaders impl; if (!impl) impl = nvn_impl_of("nvnProgramSetShaders");
    int n = (int)f->r[1]; if (n < 0 || n > 6) die("nvnProgramSetShaders: %d shaders", n);
    NVNshaderData h[6]; memset(h, 0, sizeof h);
    for (int i = 0; i < n; i++) { uint64_t data; memcpy(&data, g_mem + f->r[2] + 16 * i, 8); uint32_t ctl = rd32(f->r[2] + 16 * i + 8); h[i].data = (NVNbufferAddress)data; h[i].control = ctl ? g_mem + ctl : NULL; }
    char nm[2][64] = {{0}, {0}};
    for (int i = 0; i < n; i++) if (h[i].data && h[i].control) dump_shader((const uint8_t *)(uintptr_t)h[i].data, (const uint8_t *)h[i].control, nm[i & 1]);
    if (n == 2 && nm[0][0] && nm[1][0] && getenv("A32_SHADER_DUMP")) {                         // the program's vertex / pixel pair, for the pair table
        static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; char pl[1024]; snprintf(pl, sizeof pl, "%s/programs.txt", getenv("A32_SHADER_DUMP"));
        pthread_mutex_lock(&mu); FILE *fp = fopen(pl, "a"); if (fp) { fprintf(fp, "%s %s\n", nm[0], nm[1]); fclose(fp); } pthread_mutex_unlock(&mu);
    }
    f->r[0] = impl((NVNprogram *)(g_mem + f->r[0]), n, h) & 0xff;
}
static void ad_vertex_attrib_get_format(Frame *f) {    // (state, NVNformat *format, ptrdiff_t *offset)
    static NVNT_nvnVertexAttribStateGetFormat impl; if (!impl) impl = nvn_impl_of("nvnVertexAttribStateGetFormat");
    NVNformat fmt = 0; ptrdiff_t off = 0; impl((const NVNvertexAttribState *)(g_mem + f->r[0]), &fmt, &off);
    if (f->r[1]) wr32(f->r[1], (uint32_t)fmt);
    if (f->r[2]) wr32(f->r[2], (uint32_t)off);
}
// command memory callback: the host calls it when a command buffer runs out of memory; the guest function runs on this thread, nested in the hook
static struct { NVNcommandBuffer *cb; uint32_t fn; } g_mcb[256];
static void mcb_tramp(NVNcommandBuffer *cb, NVNcommandBufferMemoryEvent ev, size_t min, void *data) {
    uint32_t fn = 0; for (unsigned i = 0; i < 256; i++) if (g_mcb[i].cb == cb) fn = g_mcb[i].fn;
    if (!fn) return;
    a32_callback(fn, (uint32_t)((uint8_t *)cb - g_mem), (uint32_t)ev, (uint32_t)min, data ? (uint32_t)((uint8_t *)data - g_mem) : 0);
}
static void ad_set_mem_callback(Frame *f) {            // nvnCommandBufferSetMemoryCallback(cb, fn)
    static NVNT_nvnCommandBufferSetMemoryCallback impl; if (!impl) impl = nvn_impl_of("nvnCommandBufferSetMemoryCallback");
    NVNcommandBuffer *cb = (NVNcommandBuffer *)(g_mem + f->r[0]); unsigned slot = 256;
    for (unsigned i = 0; i < 256; i++) if (g_mcb[i].cb == cb) { slot = i; break; }
    for (unsigned i = 0; slot == 256 && i < 256; i++) if (!g_mcb[i].cb) slot = i;
    if (slot == 256) die("too many command buffers with a memory callback");
    g_mcb[slot].cb = cb; g_mcb[slot].fn = f->r[1];
    impl(cb, f->r[1] ? mcb_tramp : NULL);
}
static void ad_get_mem_callback(Frame *f) {
    NVNcommandBuffer *cb = (NVNcommandBuffer *)(g_mem + f->r[0]); f->r[0] = 0;
    for (unsigned i = 0; i < 256; i++) if (g_mcb[i].cb == cb) f->r[0] = g_mcb[i].fn;
}
static const struct { const char *name; void (*fn)(Frame *); } g_hand[] = {
    { "nvnDeviceGetProcAddress", ad_get_proc }, { "nvnDeviceInstallDebugCallback", ad_nop }, { "nvnDeviceWalkDebugDatabase", ad_nop },
    { "nvnProgramSetShaders", ad_set_shaders }, { "nvnVertexAttribStateGetFormat", ad_vertex_attrib_get_format },
    { "nvnCommandBufferSetMemoryCallback", ad_set_mem_callback }, { "nvnCommandBufferGetMemoryCallback", ad_get_mem_callback },
};
static void (*nvn_find(const char *name))(Frame *) {
    for (unsigned i = 0; i < sizeof g_hand / sizeof *g_hand; i++) if (!strcmp(g_hand[i].name, name)) return nvn_impl_of(name) || !strcmp(name, "nvnDeviceGetProcAddress") || g_hand[i].fn == ad_nop ? g_hand[i].fn : NULL;
    for (const A32NvnEnt *e = a32_nvn_table; e->name; e++) if (!strcmp(e->name, name)) {
        if (!*e->impl) *e->impl = nvn_impl_of(name);
        return *e->impl ? e->fn : NULL;
    }
    return NULL;
}
static void f_nvn_bootstrap(Frame *f) {
    if (g_nvn_trace < 0) g_nvn_trace = getenv("A32_NVN") ? atoi(getenv("A32_NVN")) : 0;
    const char *name = gstr(f->r[0]);
    static int first = 1;
    if (first) { first = 0; LOG("nvn: nvnBootstrapLoader(\"%s\")", name); }
    f->r[0] = a32_hook_add(strdup(name), nvn_find(name) ? nvn_find(name) : nvn_unimplemented);
}

const ForceEnt a32_force_gfx[] = {
    { "_ZN2nv20SetGraphicsAllocatorEPFPvjjS0_EPFvS0_S0_EPFS0_S0_jS0_ES0_", f_nv_set_alloc },
    { "_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvjjS0_EPFvS0_S0_EPFS0_S0_jS0_ES0_", f_nv_nop },
    { "_ZN2nv18InitializeGraphicsEPvj", f_nv_nop }, { "_ZN2nv16FinalizeGraphicsEv", f_nv_nop }, { "_ZN2nv26InitializeGraphicsDevtoolsEPvj", f_nv_nop },
    { "_ZN2nv22SetGraphicsServiceNameEPKc", f_nv_nop },
    { "_ZN2nn2vi10InitializeEv", f_nv_nop }, { "_ZN2nn2vi8FinalizeEv", f_nv_nop },
    { "_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", f_vi_open_display },
    { "_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", f_vi_create_layer }, { "_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayEii", f_vi_create_layer },
    { "_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", f_vi_native_window },
    { "_ZN2nn2vi12SetLayerCropEPNS0_5LayerEiiii", f_nv_nop }, { "_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE", f_nv_nop },
    { "_ZN2nn2vi12DestroyLayerEPNS0_5LayerE", f_nv_nop }, { "_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE", f_nv_nop },
    { "_ZN2nn2vi12ListDisplaysEPNS0_11DisplayInfoEi", f_vi_list_displays },
    { "nvnBootstrapLoader", f_nvn_bootstrap },
};
const unsigned a32_nforce_gfx = sizeof a32_force_gfx / sizeof *a32_force_gfx;
