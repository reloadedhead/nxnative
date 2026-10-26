// nn::audio AudioRenderer for the 32-bit guest, after dm/host/hle_audren.c: the game's SDK ships a complete renderer (nn::audio::server::AudioRenderSystem and the DSP's command list
// processor on the CPU, for the "CPU rendering + manual execution" mode). The host forces that mode, and a render thread of the guest calls ExecuteAudioRendererRendering every 5 ms;
// the DeviceSink command of each rendered frame is taken out of the command list, downmixed to stereo 16-bit and played by the host's output (dm_pcmout_*, dm/host/hle_audio.c:
// a CoreAudio unit, or a silent sink that consumes in real time). Without this the SDK would send the work to the audio service (audren:u), which the host does not have.
// This SDK (4.x: AudioRenderSystem at +0x30 of AudioRendererImpl, update lock nn::os::MutexType at +0x11c, command list header 0x18 bytes, DeviceSink command id 0xf of 0x128 bytes) was
// read off the 32-bit code; the layouts differ from the 64-bit hosts' in every pointer-sized field.
#define _DARWIN_C_SOURCE
#include "a32host.h"
#include "a32force.h"
#include "a32gfx.h"
#include <stdatomic.h>
#include <mach/mach_time.h>

extern uint32_t a32_callbackv(uint32_t pc, int n, const uint32_t *args);
extern uint32_t a32_callback(uint32_t pc, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
extern uint32_t a32_find_symbol(const char *name);
extern uint32_t a32_hook_add(const char *name, void (*fn)(Frame *));
extern void *dm_pcmout_open(int ch);
extern void dm_pcmout_close(void *k);
extern void dm_pcmout_start(void *k);
extern int dm_pcmout_append(void *k, void *b);
extern void *dm_pcmout_released(void *k);
extern void dm_pcmout_wait(void *k, int ms);
static uint64_t now_ns(void) { static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb); return mach_absolute_time() * tb.numer / tb.denom; }

enum { P_DEVICE = 0x2c, P_MODE = 0x30, P_SIZE = 0x40, WORK_PAGE = 0x1000, FRAME_MAX = 1024, NBUF = 4 };
static uint32_t gsym(const char *n) { uint32_t a = a32_find_symbol(n); if (!a) die("audren: missing SDK symbol %s", n); return a; }

// ---- guest threads of the SDK's own kind (a ThreadType, so nn::os locks know them), entered through a host entry point ----
typedef struct { void (*fn)(void *); void *arg; } OsThreadStart;
static OsThreadStart g_ots[16]; static int g_nots;
static void ots_entry(Frame *f) { OsThreadStart *s = &g_ots[f->r[0]]; s->fn(s->arg); f->r[0] = 0; }
// Creates and starts a guest thread running fn(arg) (a host function, entered like an import: its calls into guest code use a32_callback). Called from a guest thread.
uint32_t a32_os_thread(void (*fn)(void *), void *arg, uint32_t stack) {
    static uint32_t entry; if (!entry) entry = a32_hook_add("a32_os_thread_entry", ots_entry);
    if (g_nots >= 16) die("too many host threads");
    g_ots[g_nots] = (OsThreadStart){fn, arg};
    uint32_t tt = a32_guest_alloc(0x200, 16), stk = a32_guest_alloc(stack, 0x1000);
    uint32_t create = gsym("_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_ji"), start = gsym("_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE");
    uint32_t a[7] = {tt, entry, (uint32_t)g_nots++, stk, stack, 16, 0};
    uint32_t r = a32_callbackv(create, 6, a);
    if (r) LOG("[RED] audren: CreateThread failed: %#x", r);
    a32_callback(start, tt, 0, 0, 0);
    return tt;
}

// ---- the work buffer size the service wants (the SDK's local renderer needs more than the service budget; the game gets the service's number) ----
static size_t ar_align(size_t n, size_t alignment) { return (n + alignment - 1) & ~(alignment - 1); }
static size_t ar_service_work_size(const uint8_t *p) {
    uint32_t revision = *(const uint32_t *)(p + 0x34);
    if ((revision & 0xffffff) != 0x564552) revision = *(const uint32_t *)(p + 0x38);
    if ((revision & 0xffffff) != 0x564552 || revision < 0x31564552 || revision > 0x3f564552) return 0;
    unsigned r = (revision >> 24) - 0x30;
    size_t n[11];
    for (int i = 0; i < 11; i++) {
        if (i == 8) { n[i] = p[0x20]; continue; }
        int32_t v = ((const int32_t *)p)[i];
        if (v < 0 || v > 0x100000) return 0;
        n[i] = (size_t)v;
    }
    size_t mix = n[2], sub = n[3], voices = n[4], sinks = n[5], effects = n[6], perf = n[7], splitters = n[9], dest = n[10], mixes = sub + 1;
    size_t size = ar_align(((sinks + sub) * 0x3c0 + n[1] * 4) * (mix + 6), 64);
    size += ar_align(mix * 4, 64) + sub * 0x400 + mixes * 0x940 + ar_align(mixes * 8, 16);
    size += voices * 0x1d0 + ar_align(voices * (r >= 15 ? 0x238 : 0x220), 16) + ar_align(voices * 8, 16);
    if (r >= 2) {
        size_t graph = mixes * mixes * 4 + mixes * 12 + 2 * ar_align(mixes, 64) / 8 + ar_align(mixes * mixes, 64) / 8;
        size += ar_align(graph, 16);
        size += splitters * 0x20 + ar_align(dest * (r >= 12 ? (r >= 15 ? 0x130 : 8) : 0xe0), 16);
        if (r < 5) size += ar_align(dest * 4, 16);
    }
    size += (effects + voices * 4) * 0x20 + 0x50;
    if (r >= 12 && splitters && dest) size += ar_align(dest * (r >= 14 ? 0xe0 : 0x80), 64);
    size = ar_align(size + (r >= 9 ? effects * 0x80 : 0), 64);
    size += (sinks + sub) * 0x280 + effects * (r >= 9 ? 0x530 : 0x4b0) + sinks * 0x170 + voices * 0x100 + 0x40;
    if (perf) {
        size_t frame = (sub + voices + sinks + effects + 1) * (r >= 5 ? 0x18 : 0x10) + (r >= 5 ? 0x990 : 0x658);
        size += ar_align(frame * (perf + 1), 64) + 0xc0;
    }
    size_t commands = 0x18000;
    if (r >= 5) commands = effects * 0x540 + sub * 0x3f24 + sinks * 0x130 + dest * 0x360
        + voices * (splitters * 0x50 + (r >= 8 ? 0x284 : 0x250)) + (voices + sub + effects + sinks + 0x65) * 0x28 + 0x2c4;
    return ar_align(size + commands + 0x7e, WORK_PAGE);
}
// a copy of the game's parameter in guest memory, forced to CPU rendering and manual execution
static uint32_t cpu_manual(const uint8_t *src) {
    static uint32_t p; if (!p) p = a32_guest_alloc(P_SIZE, 16);
    memcpy(g_mem + p, src, P_SIZE); wr32(p + P_DEVICE, 1); wr32(p + P_MODE, 1); return p;
}
static void h_ar_worksize(Frame *f) {                  // GetAudioRendererWorkBufferSize(const AudioRendererParameter &)
    const uint8_t *param = g_mem + f->r[0];
    static uint32_t orig; if (!orig) orig = gsym("_ZN2nn5audio30GetAudioRendererWorkBufferSizeERKNS0_22AudioRendererParameterE");
    uint32_t p = cpu_manual(param); uint32_t n = a32_callback(orig, p, 0, 0, 0);
    size_t service = rd32(f->r[0] + P_DEVICE) == 0 ? ar_service_work_size(g_mem + p) : 0;
    uint32_t guest_n = service ? (uint32_t)service : n;
    { static int once; if (!once++) LOG("audren: work buffer guest %#x, local CPU/manual renderer %#x", guest_n, n); }
    f->r[0] = guest_n;
}

// ---- rendering and output ----
static uint32_t g_handle[2]; static _Atomic int g_open; static uint32_t g_render_tt;
static int16_t g_frame[FRAME_MAX * 2]; static int g_frame_n;
static _Atomic uint64_t g_frames, g_nonsilent, g_updates, g_renders, g_catchups;
enum { CQ = 4 }; static int16_t g_cq[CQ][FRAME_MAX * 2]; static int g_cq_n[CQ]; static _Atomic uint32_t g_cq_head, g_cq_tail;
static pthread_mutex_t g_cq_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_cq_cv = PTHREAD_COND_INITIALIZER;
static void cq_popped(void) { pthread_mutex_lock(&g_cq_mu); pthread_cond_broadcast(&g_cq_cv); pthread_mutex_unlock(&g_cq_mu); }
typedef struct { void *ptr; uint64_t cap, size; } gbuf;
static void render_main(void *arg) {
    (void)arg;
    uint32_t exec = gsym("_ZN2nn5audio29ExecuteAudioRendererRenderingENS0_19AudioRendererHandleE");
    void *out = dm_pcmout_open(2);
    static int16_t data[NBUF][FRAME_MAX * 2]; static gbuf bufs[NBUF]; gbuf *free_list[NBUF]; int nfree = NBUF;
    for (int i = 0; i < NBUF; i++) { bufs[i] = (gbuf){data[i], sizeof data[i], 0}; free_list[i] = &bufs[i]; }
    dm_pcmout_start(out);
    LOG("audren: render thread started (%d buffers in flight, paced by the output)", NBUF);
    while (atomic_load(&g_open)) {
        while (!nfree) {
            gbuf *b = dm_pcmout_released(out);
            if (b) { free_list[nfree++] = b; break; }
            dm_pcmout_wait(out, 10);
            if (!atomic_load(&g_open)) break;
        }
        if (!nfree) break;
        const int16_t *src; int got;
        uint32_t h = atomic_load(&g_cq_head);
        if (h != atomic_load(&g_cq_tail)) { src = g_cq[h % CQ]; got = g_cq_n[h % CQ]; atomic_store(&g_cq_head, h + 1); cq_popped(); }
        else { g_frame_n = 0; a32_callback(exec, g_handle[0], g_handle[1], 0, 0); src = g_frame; got = g_frame_n; }
        int n = got ? got : 240;
        gbuf *b = free_list[--nfree];
        if (got) memcpy(b->ptr, src, (size_t)n * 4); else memset(b->ptr, 0, (size_t)n * 4);
        b->size = (uint64_t)n * 4;
        if (!dm_pcmout_append(out, b)) free_list[nfree++] = b;
    }
    dm_pcmout_close(out);
    cq_popped();
}
static __thread int t_in_open;
static uint32_t ar_open(uint32_t handle, uint32_t ev, uint32_t param, uint32_t work, uint32_t sz) {
    static uint32_t orig_ev, orig;
    if (!orig_ev) { orig_ev = gsym("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvj");
                    orig = gsym("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleERKNS0_22AudioRendererParameterEPvj"); }
    if (t_in_open) { uint32_t a[4] = {handle, param, work, sz}; return a32_callbackv(orig, 4, a); }
    uint32_t p = cpu_manual(g_mem + param);
    LOG("audren: OpenAudioRenderer%s rate %d samples per frame %d mix buffers %d submixes %d voices %d sinks %d effects %d (original device %d mode %d: CPU / manual) work buffer %#x+%#x", ev ? "" : " (no event)",
        (int)rd32(param), (int)rd32(param + 4), (int)rd32(param + 8), (int)rd32(param + 0xc), (int)rd32(param + 0x10), (int)rd32(param + 0x14), (int)rd32(param + 0x18),
        (int)rd32(param + P_DEVICE), (int)rd32(param + P_MODE), work, sz);
    if (rd32(param + P_DEVICE) == 0 && work && !(work & (WORK_PAGE - 1)) && sz >= WORK_PAGE && !(sz & (WORK_PAGE - 1))) {      // the work buffer follows the service's contract; the local renderer may need more
        uint32_t size = gsym("_ZN2nn5audio30GetAudioRendererWorkBufferSizeERKNS0_22AudioRendererParameterE");
        uint32_t need = a32_callback(size, p, 0, 0, 0);
        size_t service = ar_service_work_size(g_mem + p), guest_need = service ? service : need;
        if (sz < guest_need) { LOG("[RED] audren: work buffer %#x is smaller than the service requirement %#zx", sz, guest_need); return 0x1; }
        if (need > sz) { work = a32_guest_alloc(need, WORK_PAGE); sz = need; LOG("audren: host backs the local renderer work buffer %#x+%#x", work, sz); }
    }
    t_in_open = 1; uint32_t r;
    if (ev) { uint32_t a[5] = {handle, ev, p, work, sz}; r = a32_callbackv(orig_ev, 5, a); } else { uint32_t a[4] = {handle, p, work, sz}; r = a32_callbackv(orig, 4, a); }
    t_in_open = 0;
    if (r) { LOG("[RED] audren: SDK OpenAudioRenderer failed %#x", r); return r; }
    if (atomic_load(&g_open)) { LOG("[RED] audren: a second renderer (the host drives only one)"); return r; }
    g_handle[0] = rd32(handle); g_handle[1] = rd32(handle + 4); atomic_store(&g_open, 1);
    g_render_tt = a32_os_thread(render_main, NULL, 0x40000);
    return r;
}
static void h_ar_open_ev(Frame *f) { uint32_t sz = rd32(f->r[13]); f->r[0] = ar_open(f->r[0], f->r[1], f->r[2], f->r[3], sz); }
static void h_ar_open(Frame *f) { f->r[0] = ar_open(f->r[0], 0, f->r[1], f->r[2], f->r[3]); }
static void h_ar_close(Frame *f) {                      // CloseAudioRenderer(AudioRendererHandle): the handle is in r0:r1
    static uint32_t orig; if (!orig) orig = gsym("_ZN2nn5audio18CloseAudioRendererENS0_19AudioRendererHandleE");
    if (f->r[0] == g_handle[0] && g_render_tt) {
        atomic_store(&g_open, 0);
        a32_callback(gsym("_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE"), g_render_tt, 0, 0, 0);
        a32_callback(gsym("_ZN2nn2os13DestroyThreadEPNS0_10ThreadTypeE"), g_render_tt, 0, 0, 0); g_render_tt = 0;
    }
    a32_callback(orig, f->r[0], f->r[1], 0, 0);
}

// ---- the command list: header (0x18 bytes: +4 command count, +8 mix buffer pointer, +0x0c buffer count, +0x10 samples per frame, +0x14 sample rate), then commands
//   (+0 magic 0xcafebabe, +4 size (s16), +6 id (s16), +0x10 enabled). DeviceSink: id 0xf, 0x128 bytes: +0x118 upsample buffer, +0x11c input count, +0x120 input mix buffer indices (bytes)
enum { SINK_ID = 0xf, CMD0 = 0x18 };
static int capture(const uint8_t *cl, size_t len, int16_t *dst) {
    uint32_t ncmd = *(const uint32_t *)(cl + 4), samples = *(const uint32_t *)(cl + 0x10), rate = *(const uint32_t *)(cl + 0x14);
    const int32_t *mix = (const int32_t *)(g_mem + *(const uint32_t *)(cl + 8)); const uint8_t *c = cl + CMD0, *cend = cl + len;
    static int logged;
    for (uint32_t i = 0; i < ncmd; i++) {
        int size = *(const int16_t *)(c + 4);
        if (c + 8 > cend || *(const uint32_t *)c != 0xcafebabeu || size <= 0) { if (!logged++) LOG("[RED] audren: command %u in the command list is malformed (first word %#x size %d)", i, c + 4 <= cend ? *(const uint32_t *)c : 0, size); return 0; }
        if (*(const int16_t *)(c + 6) == SINK_ID && c[0x10]) {
            uint32_t nin = *(const uint32_t *)(c + 0x11c); int8_t in[6];
            for (uint32_t j = 0; j < 6; j++) in[j] = ((const int8_t *)(c + 0x120))[j];
            if (!logged++) LOG("audren: DeviceSink inputs %u (%d %d ...) rate %u samples per frame %u", nin, nin ? in[0] : -1, nin > 1 ? in[1] : -1, rate, samples);
            if (!nin || samples > FRAME_MAX) return 0;
            if (rate != 48000) { static int once; if (!once++) LOG("[RED] audren: renderer sample rate %u (only 48000 is handled): no sound", rate); return 0; }
            const int32_t *ch[6]; uint32_t k = nin > 6 ? 6 : nin;
            for (uint32_t j = 0; j < k; j++) ch[j] = mix + (size_t)in[j] * samples;
            enum { CF = 32768, CC = 23167, CL = 8224, CB = 23167 };       // 6 -> 2 with the system's default downmix (front 1.0, center 0.707, LFE 0.251, back 0.707, Q15)
            uint64_t loud = 0;
            for (uint32_t s = 0; s < samples; s++) {
                int32_t l, r;
                if (k >= 6) { int64_t cc = (int64_t)ch[2][s] * CC + (int64_t)ch[3][s] * CL;
                              l = (int32_t)(((int64_t)ch[0][s] * CF + cc + (int64_t)ch[4][s] * CB) >> 15); r = (int32_t)(((int64_t)ch[1][s] * CF + cc + (int64_t)ch[5][s] * CB) >> 15); }
                else { l = ch[0][s]; r = k > 1 ? ch[1][s] : l; }
                l = l > 32767 ? 32767 : l < -32768 ? -32768 : l; r = r > 32767 ? 32767 : r < -32768 ? -32768 : r;
                dst[2 * s] = (int16_t)l; dst[2 * s + 1] = (int16_t)r; loud |= (uint64_t)(l | r);
            }
            atomic_fetch_add(&g_frames, 1); if (loud) atomic_fetch_add(&g_nonsilent, 1);
            uint64_t fr = atomic_load(&g_frames); if (fr == 200 || fr % 12000 == 0) LOG("audren: rendered %llu frames, %llu of them not silent", (unsigned long long)fr, (unsigned long long)atomic_load(&g_nonsilent));
            return (int)samples;
        }
        c += size;
    }
    return 0;
}
// one frame: generate the commands (under the update lock, as the game thread's Update holds it), process them on the CPU, take the DeviceSink's output. The render thread and the catch-up renders share one command buffer
static pthread_mutex_t g_render_mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t render_once(uint32_t sys, int16_t *dst, int *nout) {
    static uint32_t getmode, getdev, cbuf, cbsz, gen, lock, unlock, tmax, setup, process, proc;
    if (!process) {
        getmode = gsym("_ZNK2nn5audio6server17AudioRenderSystem16GetExecutionModeEv"); getdev = gsym("_ZNK2nn5audio6server17AudioRenderSystem18GetRenderingDeviceEv");
        cbuf = gsym("_ZNK2nn5audio6server17AudioRenderSystem26GetCommandBufferWorkBufferEv"); cbsz = gsym("_ZNK2nn5audio6server17AudioRenderSystem30GetCommandBufferWorkBufferSizeEv");
        gen = gsym("_ZN2nn5audio6server17AudioRenderSystem15GenerateCommandEPvj");
        lock = gsym("_ZN2nn2os9LockMutexEPNS0_9MutexTypeE"); unlock = gsym("_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE");
        tmax = gsym("_ZN2nn5audio3dsp20CommandListProcessor17SetProcessTimeMaxEj"); setup = gsym("_ZN2nn5audio3dsp20CommandListProcessor5SetupEPKvj");
        process = gsym("_ZN2nn5audio3dsp20CommandListProcessor7ProcessEv"); proc = a32_guest_alloc(0x100, 16);
    }
    if (a32_callback(getmode, sys, 0, 0, 0) != 1 || a32_callback(getdev, sys, 0, 0, 0) != 1) return 0x40299;
    pthread_mutex_lock(&g_render_mu);
    uint64_t t0 = now_ns();
    a32_callback(lock, sys + 0x11c, 0, 0, 0);
    uint32_t cl = a32_callback(cbuf, sys, 0, 0, 0), n = a32_callback(gen, sys, cl, a32_callback(cbsz, sys, 0, 0, 0), 0);
    a32_callback(unlock, sys + 0x11c, 0, 0, 0);
    atomic_fetch_add(&g_renders, 1);
    memset(g_mem + proc, 0, 0x100);
    a32_callback(tmax, proc, 0xffffffffu, 0, 0); a32_callback(setup, proc, cl, n, 0); a32_callback(process, proc, 0, 0, 0);
    if (dst) *nout = capture(g_mem + cl, n, dst);
    static uint64_t sum, mx, cnt; uint64_t d = now_ns() - t0; sum += d; if (d > mx) mx = d;
    if (++cnt == 2000) { LOG("audren: render time mean %.2f ms / max %.2f ms (5 ms budget per frame, last 2000 frames); catch-up renders so far %llu", sum / 2000 / 1e6, mx / 1e6, (unsigned long long)atomic_load(&g_catchups)); sum = mx = cnt = 0; }
    pthread_mutex_unlock(&g_render_mu);
    return 0;
}
static void h_impl_exec(Frame *f) { f->r[0] = render_once(f->r[0] + 0x30, g_frame, &g_frame_n); }       // the AudioRenderSystem sits at +0x30 inside AudioRendererImpl
// At least one rendered frame between two RequestUpdates (an effect's initialization reaches the DSP only through a generated frame), the catch-up frame's output queued for the render thread
static void h_update(Frame *f) {                       // AudioRenderSystem::Update(sys, void *, size_t, void *, size_t, const void *, size_t)
    static uint32_t orig; if (!orig) orig = gsym("_ZN2nn5audio6server17AudioRenderSystem6UpdateEPvjS3_jPKvj");
    uint32_t sys = f->r[0], args[7] = {f->r[0], f->r[1], f->r[2], f->r[3], rd32(f->r[13]), rd32(f->r[13] + 4), rd32(f->r[13] + 8)};
    static uint64_t last_r = ~0ull;
    uint64_t r = atomic_load(&g_renders); atomic_fetch_add(&g_updates, 1);
    if (last_r != ~0ull && r == last_r) {
        atomic_fetch_add(&g_catchups, 1);
        uint32_t t = atomic_load(&g_cq_tail);
        if (t - atomic_load(&g_cq_head) >= CQ) {
            struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_nsec += 50000000; if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
            pthread_mutex_lock(&g_cq_mu);
            while (atomic_load(&g_open) && t - atomic_load(&g_cq_head) >= CQ && !pthread_cond_timedwait(&g_cq_cv, &g_cq_mu, &dl)) {}
            pthread_mutex_unlock(&g_cq_mu);
        }
        if (t - atomic_load(&g_cq_head) < CQ) { render_once(sys, g_cq[t % CQ], &g_cq_n[t % CQ]); atomic_store(&g_cq_tail, t + 1); }
        else render_once(sys, NULL, NULL);
    }
    f->r[0] = a32_callbackv(orig, 7, args);
    last_r = atomic_load(&g_renders);
}

#define M(name, fn) { name, fn }
const ForceEnt a32_force_audren[] = {
    M("_ZN2nn5audio30GetAudioRendererWorkBufferSizeERKNS0_22AudioRendererParameterE", h_ar_worksize),
    M("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvj", h_ar_open_ev),
    M("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleERKNS0_22AudioRendererParameterEPvj", h_ar_open),
    M("_ZN2nn5audio18CloseAudioRendererENS0_19AudioRendererHandleE", h_ar_close),
    M("_ZN2nn5audio6server24AudioRendererManagerImpl17AudioRendererImpl29ExecuteAudioRendererRenderingEv", h_impl_exec),
    M("_ZN2nn5audio6server17AudioRenderSystem6UpdateEPvjS3_jPKvj", h_update),
};
const unsigned a32_nforce_audren = sizeof a32_force_audren / sizeof *a32_force_audren;
