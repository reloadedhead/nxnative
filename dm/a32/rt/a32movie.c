// movie::Decoder for the 32-bit guest: the host decodes. The SDK's movie library (subsdk0, an Android MediaCodec wrapper) demuxes in guest code as usual; its Decoder would drive
// NVIDIA's hardware decoder through nvdrv, which is not here, so the Decoder is replaced the way dm/host/hle_movie.c replaces it for the 64-bit games: H.264 on VideoToolbox
// (dm/host/hle_movie_avc.c, linked as it is), AAC on AudioToolbox. The decoding core below is that file's, with the 32-bit guest's differences: guest addresses instead of pointers,
// the SDK's Buffer / MediaData / event functions called through a32_callback (guest code, on a guest thread), input buffers in guest memory, and the virtual table of the object
// patched, since the guest's virtual calls do not go through the import table. VP8 / VP9 / Vorbis are not here (this game's movies are MP4).
#define _DARWIN_C_SOURCE
#include "a32host.h"
#include "a32force.h"
#include "a32gfx.h"
#include <AudioToolbox/AudioToolbox.h>
#include <mach/mach_time.h>
#include "hle_movie_avc.h"

extern uint32_t a32_callback(uint32_t pc, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
extern uint32_t a32_callbackv(uint32_t pc, int n, const uint32_t *args);
extern uint32_t a32_find_symbol(const char *name);
extern uint32_t a32_hook_add(const char *name, void (*fn)(Frame *));
extern void *a32_spawn_guest_thread(void (*fn)(void *), void *arg, size_t stack);
extern int nvn_host_write_texture(const void *nvntex, const void *src, int w, int h, size_t row, int *tw, int *th);
extern int nvn_host_signal_sync(void *nvnsync);
static uint64_t now_ns(void) { static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb); return mach_absolute_time() * tb.numer / tb.denom; }

#define MD_NIN 4
#define MD_INCAP (2u << 20)      // 2 MB per input buffer
#define MD_NOUT 64
#define MD_NOUT_AAC 32
#define MD_NOUT_VIDEO 16
#define MD_NO_INPUT 0x1011
#define MD_NO_OUTPUT 0x100e
#define MD_STALE_NS 250000000ull
#define MD_FMT_NV12 4
#define MD_FMT_ABGR 3
#define MD_MODE_TEX 2
#define MD_TYPE_AVC 1
#define MD_TYPE_AAC 4
#define MD_AUDIO(d) ((d)->type == MD_TYPE_AAC)
#define MD_FMT_PCM16 1
#define MD_PCM_CAP (8192 * 8 * 2)

typedef struct {
    pthread_mutex_t mu;
    int type, fmt, mode, nout;
    uint32_t evc;                                // guest address of the client's DecoderEvents: four EventType pointers, read at signal time
    uint32_t ev[4];                              // the entries that were events at registration
    int w, h, fmt_sent, started, in_eos;
    uint32_t in_g[MD_NIN]; uint8_t *in[MD_NIN]; int infree[MD_NIN], ninfree;   // input buffers live in guest memory (in_g), in = the host's view
    int inown[MD_NIN]; uint64_t instale[MD_NIN];
    struct { int used, pic; int64_t pts; uint32_t flags; uint8_t *frm; size_t cap, bytes; } out[MD_NOUT];
    int outq[MD_NOUT], noutq;
    struct { int in, off, size; int64_t pts; uint32_t flags; } pend[MD_NIN]; int npend;
    pthread_mutex_t dmu, wmu; pthread_cond_t wcv; pthread_t wk;
    int job; const uint8_t *jp; int jn, jslot, jgot; int64_t jpts, jpts_out; int jflush;
    mavc *avc; int avc_drain;
    uint8_t *vhdr[2]; int vhdr_n[2]; int ch, rate, red_dec, red_img, red_vb;
    AudioConverterRef aac; const uint8_t *aac_pkt; UInt32 aac_pkt_n; AudioStreamPacketDescription aac_desc;
    uint64_t dec_n, dec_ns, dec_max;
    unsigned long ncall[24];
} mdec_t;

// ---- the SDK's functions (guest code) ----
static struct { uint32_t signal, set_data, set_range, data, cap, set_i32, set_rect, find_i32, find_buf, size, nvn_sync, del; } G;
static uint32_t msym(const char *n) { uint32_t a = a32_find_symbol(n); if (!a) die("movie: missing SDK symbol %s", n); return a; }
static void md_syms(void) {
    if (G.signal) return;
    G.set_data = msym("_ZN5movie6Buffer18SetDataAndCapacityEPvj");
    G.set_range = msym("_ZN5movie6Buffer8SetRangeEii");
    G.data = msym("_ZNK5movie6Buffer4DataEv");
    G.cap = msym("_ZNK5movie6Buffer8CapacityEv");
    G.set_i32 = msym("_ZN5movie9MediaData8SetInt32EPKci");
    G.set_rect = msym("_ZN5movie9MediaData7SetRectEPKciiii");
    G.find_i32 = msym("_ZNK5movie9MediaData9FindInt32EPKcPi");
    G.find_buf = msym("_ZNK5movie9MediaData10FindBufferEPKcPPNS_6BufferE");
    G.size = msym("_ZNK5movie6Buffer4SizeEv");
    G.signal = msym("_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
    G.del = a32_find_symbol("_ZdlPv");
    G.nvn_sync = a32_find_symbol("_ZNK5movie6Buffer10GetNvnSyncEv");
}
// Strings the SDK functions take must be in guest memory: a small scratch area per call (one per host thread)
static uint32_t gstring(const char *s) {
    static __thread uint32_t buf; if (!buf) buf = a32_guest_alloc(64, 4);
    strncpy((char *)g_mem + buf, s, 63); return buf;
}
static void g_set_i32(uint32_t md, const char *k, int v) { a32_callback(G.set_i32, md, gstring(k), (uint32_t)v, 0); }
static int g_find_i32(uint32_t md, const char *k, int *v) {
    static __thread uint32_t out; if (!out) out = a32_guest_alloc(8, 4);
    uint32_t r = a32_callback(G.find_i32, md, gstring(k), out, 0); if (r & 0xff) *v = (int)rd32(out); return r & 0xff;
}
static int g_find_buf(uint32_t md, const char *k, uint32_t *buf) {
    static __thread uint32_t out; if (!out) out = a32_guest_alloc(8, 4);
    uint32_t r = a32_callback(G.find_buf, md, gstring(k), out, 0); if (r & 0xff) *buf = rd32(out); return r & 0xff;
}
static uint32_t g_data(uint32_t buf) { return a32_callback(G.data, buf, 0, 0, 0); }
static uint32_t g_cap(uint32_t buf) { return a32_callback(G.cap, buf, 0, 0, 0); }
static uint32_t g_size(uint32_t buf) { return a32_callback(G.size, buf, 0, 0, 0); }
static void g_set_range(uint32_t buf, int off, int size) { a32_callback(G.set_range, buf, (uint32_t)off, (uint32_t)size, 0); }

static int md_trace_all(void) { static int on = -1; if (on < 0) on = getenv("A32_MOVIE_TRACE") != NULL; return on; }
// ---- decoders: the guest object holds an index into this table at +4 ----
#define MAXDEC 64
static mdec_t *g_dec[MAXDEC]; static pthread_mutex_t g_decmu = PTHREAD_MUTEX_INITIALIZER;
static mdec_t *md_of(uint32_t self) { uint32_t i = self ? rd32(self + 4) : 0; return i >= 1 && i <= MAXDEC ? g_dec[i - 1] : NULL; }

static int ev_ok(uint32_t p) { return p >= 0x200000 && p < 0xf0000000u && !(p & 3); }
static void md_event(mdec_t *d, int i) {   // 0 input 1 output 2 format 3 error; called on a guest thread
    uint32_t p = d->evc ? rd32(d->evc + 4 * (uint32_t)i) : 0;
    if (!ev_ok(p)) p = d->ev[i];
    if (md_trace_all()) LOG("[movie] signal event %d (%#x)", i, p);
    if (p) a32_callback(G.signal, p, 0, 0, 0);
}
// "input available" after Start / Flush is signalled a little later from a guest thread of its own (the SDK's codec thread does the same)
static pthread_mutex_t g_dfmu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_dfcv = PTHREAD_COND_INITIALIZER;
static struct { mdec_t *d; int ev; uint64_t due; } g_dfq[64]; static int g_ndfq, g_dfthr;
static void md_defer_body(void *arg);
static void md_defer_at(mdec_t *d, int ev, uint64_t due) {
    pthread_mutex_lock(&g_dfmu);
    if (!g_dfthr) { g_dfthr = 1; a32_spawn_guest_thread(md_defer_body, NULL, 64 << 10); }
    for (int i = 0; i < g_ndfq; i++) if (g_dfq[i].d == d && g_dfq[i].ev == ev && g_dfq[i].due <= due) { pthread_mutex_unlock(&g_dfmu); return; }
    if (g_ndfq < 64) g_dfq[g_ndfq++] = (__typeof__(g_dfq[0])){d, ev, due};
    pthread_cond_signal(&g_dfcv); pthread_mutex_unlock(&g_dfmu);
}
static void md_defer(mdec_t *d, int ev) { md_defer_at(d, ev, now_ns() + 100000000); }
static void md_defer_drop(mdec_t *d) {
    pthread_mutex_lock(&g_dfmu);
    int n = 0; for (int i = 0; i < g_ndfq; i++) if (g_dfq[i].d != d) g_dfq[n++] = g_dfq[i];
    g_ndfq = n; pthread_mutex_unlock(&g_dfmu);
}
static void md_defer_body(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_dfmu);
    for (;;) {
        uint64_t now = now_ns(), next = 0; int k = -1;
        for (int i = 0; i < g_ndfq && k < 0; i++) { if (g_dfq[i].due <= now) k = i; else if (!next || g_dfq[i].due < next) next = g_dfq[i].due; }
        if (k >= 0) {
            mdec_t *d = g_dfq[k].d; int e = g_dfq[k].ev;
            memmove(g_dfq + k, g_dfq + k + 1, (size_t)(--g_ndfq - k) * sizeof *g_dfq);
            if (d->started) md_event(d, e);
            continue;
        }
        if (!next) pthread_cond_wait(&g_dfcv, &g_dfmu);
        else { uint64_t w = next - now; struct timespec ts = { (time_t)(w / 1000000000u), (long)(w % 1000000000u) }; pthread_cond_timedwait_relative_np(&g_dfcv, &g_dfmu, &ts); }
    }
}
#define MD_TRACE(d, slot, fmt, ...) do { unsigned long n_ = ++(d)->ncall[slot]; if (md_trace_all() || n_ <= 12 || !(n_ & (n_ - 1))) LOG("[movie] " fmt, ##__VA_ARGS__); } while (0)

static void md_dims(const mdec_t *d, int *w, int *h, int *stride, int *slice) { *w = d->w > 0 ? d->w : 1920; *h = d->h > 0 ? d->h : 1080; *stride = *w; *slice = *h; }

// ---- AAC ----
static uint8_t *esds_descr(uint8_t *p, int tag, unsigned n) { *p++ = (uint8_t)tag; for (int i = 3; i > 0; i--) *p++ = (uint8_t)(n >> (7 * i) | 0x80); *p++ = (uint8_t)(n & 0x7f); return p; }
static OSStatus aac_feed(AudioConverterRef c, UInt32 *np, AudioBufferList *bl, AudioStreamPacketDescription **pd, void *ud) {
    (void)c; mdec_t *d = ud;
    if (!d->aac_pkt) { *np = 0; return 1; }
    bl->mNumberBuffers = 1; bl->mBuffers[0].mData = (void *)d->aac_pkt; bl->mBuffers[0].mDataByteSize = d->aac_pkt_n; bl->mBuffers[0].mNumberChannels = (UInt32)d->ch;
    d->aac_desc = (AudioStreamPacketDescription){0, 0, d->aac_pkt_n}; if (pd) *pd = &d->aac_desc;
    *np = 1; d->aac_pkt = NULL; return 0;
}
static int md_aac_open(mdec_t *d) {
    static const int rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    uint8_t asc[64]; int an = d->vhdr_n[0] > 0 && d->vhdr_n[0] <= (int)sizeof asc ? d->vhdr_n[0] : 0;
    if (an) memcpy(asc, d->vhdr[0], (size_t)an);
    if (an >= 2) {
        int fi = (asc[0] & 7) << 1 | asc[1] >> 7, cc = asc[1] >> 3 & 15;
        if (fi < 13 && d->rate <= 0) d->rate = rates[fi]; if (cc && cc < 8 && d->ch <= 0) d->ch = cc == 7 ? 8 : cc;
    }
    if (d->rate <= 0) d->rate = 48000; if (d->ch <= 0) d->ch = 2;
    if (!an) { int fi = 3; for (int i = 0; i < 13; i++) if (rates[i] == d->rate) fi = i; asc[0] = (uint8_t)(2 << 3 | fi >> 1); asc[1] = (uint8_t)((fi & 1) << 7 | (d->ch & 15) << 3); an = 2; }
    AudioStreamBasicDescription in = {.mSampleRate = d->rate, .mFormatID = kAudioFormatMPEG4AAC, .mFramesPerPacket = 1024, .mChannelsPerFrame = (UInt32)d->ch};
    AudioStreamBasicDescription out = {.mSampleRate = d->rate, .mFormatID = kAudioFormatLinearPCM, .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
                                       .mBytesPerPacket = 2u * d->ch, .mFramesPerPacket = 1, .mBytesPerFrame = 2u * d->ch, .mChannelsPerFrame = (UInt32)d->ch, .mBitsPerChannel = 16};
    OSStatus st = AudioConverterNew(&in, &out, &d->aac);
    if (st) { if (!d->red_vb++) LOG("[RED] movie: AudioToolbox cannot create an AAC decoder (%d; %d channels %d Hz), outputting silence", (int)st, d->ch, d->rate); d->aac = NULL; return 0; }
    uint8_t ck[128], *p = ck;
    p = esds_descr(p, 3, 3 + 5 + 13 + 5 + (unsigned)an); *p++ = 0; *p++ = 0; *p++ = 0;
    p = esds_descr(p, 4, 13 + 5 + (unsigned)an); *p++ = 0x40; *p++ = 0x15; memset(p, 0, 11); p += 11;
    p = esds_descr(p, 5, (unsigned)an); memcpy(p, asc, (size_t)an); p += an;
    st = AudioConverterSetProperty(d->aac, kAudioConverterDecompressionMagicCookie, (UInt32)(p - ck), ck);
    if (st && !d->red_vb++) LOG("[RED] movie: AAC configuration not accepted by AudioToolbox (%d)", (int)st);
    LOG("[movie] AAC: %d channels %d Hz, AudioSpecificConfig %d bytes (object type %d)", d->ch, d->rate, an, asc[0] >> 3);
    return 1;
}
static int md_aac(mdec_t *d, const uint8_t *p, int n, int slot) {
    if (!d->aac && !md_aac_open(d)) return 0;
    if (n <= 0) return 0;
    if (d->out[slot].cap < MD_PCM_CAP) { free(d->out[slot].frm); d->out[slot].frm = malloc(MD_PCM_CAP); d->out[slot].cap = MD_PCM_CAP; }
    d->aac_pkt = p; d->aac_pkt_n = (UInt32)n;
    UInt32 frames = (UInt32)(MD_PCM_CAP / (2 * d->ch));
    AudioBufferList ob = {1, {{(UInt32)d->ch, frames * 2u * d->ch, d->out[slot].frm}}};
    OSStatus st = AudioConverterFillComplexBuffer(d->aac, aac_feed, d, &frames, &ob, NULL);
    d->aac_pkt = NULL;
    if (st && st != 1 && !d->red_dec++) LOG("[RED] movie: AAC decode error (AudioToolbox %d, first case)", (int)st);
    d->out[slot].bytes = (size_t)frames * 2 * d->ch;
    return frames > 0;
}
// ---- H.264 ----
static void md_avc_open(mdec_t *d) { if (!d->avc) { d->avc = mavc_new(); if (d->fmt == MD_FMT_ABGR) mavc_set_rgba(d->avc); } }
static int md_avc(mdec_t *d, const uint8_t *p, int n, int slot, int take_only) {
    md_avc_open(d);
    if (!take_only && n > 0) mavc_decode(d->avc, p, n, d->jpts);
    if (d->w <= 0 || d->h <= 0) return 0;
    size_t need = (size_t)d->w * d->h * (d->fmt == MD_FMT_ABGR ? 4 : 3) / (d->fmt == MD_FMT_ABGR ? 1 : 2);
    if (d->out[slot].cap < need) { free(d->out[slot].frm); d->out[slot].frm = malloc(need); d->out[slot].cap = need; }
    return mavc_take(d->avc, d->jflush, d->out[slot].frm, d->w, d->h, &d->jpts_out);
}
static void md_close(mdec_t *d) { if (d->aac) { AudioConverterDispose(d->aac); d->aac = NULL; } if (d->avc) { mavc_free(d->avc); d->avc = NULL; } }

enum { MDJ_NONE, MDJ_DECODE, MDJ_CLOSE, MDJ_QUIT, MDJ_TAKE };
static void *md_worker(void *arg) {
    mdec_t *d = arg;
    pthread_setname_np("a32 movie decoder");
    pthread_mutex_lock(&d->wmu);
    for (;;) {
        while (d->job == MDJ_NONE) pthread_cond_wait(&d->wcv, &d->wmu);
        int job = d->job;
        pthread_mutex_unlock(&d->wmu);
        if (job == MDJ_DECODE || job == MDJ_TAKE) {
            uint64_t t0 = now_ns();
            d->jgot = d->type == MD_TYPE_AVC ? md_avc(d, d->jp, d->jn, d->jslot, job == MDJ_TAKE) : md_aac(d, d->jp, d->jn, d->jslot);
            uint64_t dt = now_ns() - t0; d->dec_n++; d->dec_ns += dt; if (dt > d->dec_max) d->dec_max = dt;
        }
        else md_close(d);
        pthread_mutex_lock(&d->wmu);
        d->job = MDJ_NONE; pthread_cond_broadcast(&d->wcv);
        if (job == MDJ_QUIT) break;
    }
    pthread_mutex_unlock(&d->wmu);
    return NULL;
}
static void md_job(mdec_t *d, int job) {
    pthread_mutex_lock(&d->wmu);
    d->job = job; pthread_cond_broadcast(&d->wcv);
    while (d->job != MDJ_NONE) pthread_cond_wait(&d->wcv, &d->wmu);
    pthread_mutex_unlock(&d->wmu);
}
static void md_pump(mdec_t *d) {
    pthread_mutex_lock(&d->dmu);
    for (;;) {
        pthread_mutex_lock(&d->mu);
        if (d->avc_drain) {
            int s = -1; for (int k = 0; k < d->nout; k++) if (!d->out[k].used) { s = k; break; }
            if (s < 0) { pthread_mutex_unlock(&d->mu); break; }
            d->out[s].used = 1; pthread_mutex_unlock(&d->mu);
            d->jslot = s; d->jflush = 1; md_job(d, MDJ_TAKE); int got = d->jgot, left = d->avc ? mavc_queued(d->avc) : 0;
            pthread_mutex_lock(&d->mu);
            d->out[s].pic = got; d->out[s].pts = got ? d->jpts_out : 0; d->out[s].flags = left ? 0 : 4; d->outq[d->noutq++] = s;
            if (!left) d->avc_drain = 0;
            pthread_mutex_unlock(&d->mu);
            md_event(d, 1);
            continue;
        }
        int slot = -1;
        if (d->npend) for (int k = 0; k < d->nout; k++) if (!d->out[k].used) { slot = k; break; }
        if (slot < 0) { pthread_mutex_unlock(&d->mu); break; }
        __typeof__(d->pend[0]) pk = d->pend[0];
        memmove(d->pend, d->pend + 1, --d->npend * sizeof *d->pend);
        d->out[slot].used = 1;
        pthread_mutex_unlock(&d->mu);
        int got = 0, avc = d->type == MD_TYPE_AVC;
        int eos = (pk.flags & 4) != 0;
        if (eos && (pk.pts < 0 || (uint32_t)pk.size >= MD_INCAP)) pk.size = 0;
        if ((pk.size > 0 && (MD_AUDIO(d) || avc)) || (avc && eos)) {
            d->jp = d->in[pk.in] + pk.off; d->jn = pk.size; d->jslot = slot; d->jpts = pk.pts; d->jflush = avc && eos; md_job(d, MDJ_DECODE); got = d->jgot; }
        int more = avc && eos && d->avc && mavc_queued(d->avc) > 0;
        pthread_mutex_lock(&d->mu);
        d->infree[d->ninfree++] = pk.in;
        int ready = got || eos;
        if (ready) { d->out[slot].pic = got; d->out[slot].pts = got && avc ? d->jpts_out : pk.pts; d->out[slot].flags = more ? pk.flags & ~4u : pk.flags; d->outq[d->noutq++] = slot; }
        else d->out[slot].used = 0;
        if (more) d->avc_drain = 1;
        int ineos = d->in_eos;
        pthread_mutex_unlock(&d->mu);
        if (!ineos) md_event(d, 0);
        if (ready) md_event(d, 1);
    }
    pthread_mutex_unlock(&d->dmu);
}

// ---- the methods, as host entry points taking the guest's registers ----
static void md_free(uint32_t self) {
    mdec_t *d = md_of(self); if (!d) return;
    md_defer_drop(d);
    pthread_mutex_lock(&g_decmu); g_dec[rd32(self + 4) - 1] = NULL; pthread_mutex_unlock(&g_decmu);
    wr32(self + 4, 0);
    LOG("[movie] decoder destroyed: %lu packets in, %lu frames out; decoded %llu packets, average %.2f ms per packet, longest %.2f ms", d->ncall[7], d->ncall[8], (unsigned long long)d->dec_n, d->dec_n ? d->dec_ns / 1e6 / d->dec_n : 0.0, d->dec_max / 1e6);
    pthread_mutex_lock(&d->dmu); md_job(d, MDJ_QUIT); pthread_mutex_unlock(&d->dmu);
    pthread_join(d->wk, NULL);
    for (int k = 0; k < MD_NOUT; k++) free(d->out[k].frm);
    for (int k = 0; k < MD_NIN; k++) a32_guest_free(d->in_g[k], MD_INCAP);
    free(d->vhdr[0]); free(d->vhdr[1]);
    pthread_mutex_destroy(&d->mu); pthread_mutex_destroy(&d->dmu); pthread_mutex_destroy(&d->wmu); pthread_cond_destroy(&d->wcv); free(d);
}
static uint32_t g_vt;       // the patched virtual table (address of the first virtual function, as an object's vptr points)
static void h_ctor(Frame *f) {                         // Decoder(type, format, mode [, priority])
    uint32_t self = f->r[0]; int type = (int)f->r[1], fmt = (int)f->r[2], mode = (int)f->r[3];
    md_syms();
    wr32(self, g_vt); wr32(self + 4, 0);
    int audio = type == MD_TYPE_AAC && fmt == MD_FMT_PCM16;
    if (!audio && (type != MD_TYPE_AVC || (fmt != MD_FMT_NV12 && !(fmt == MD_FMT_ABGR && mode == MD_MODE_TEX)))) { LOG("[RED] movie: decoder type %d output %d mode %d not supported by the host, every method returns failure", type, fmt, mode); return; }
    mdec_t *d = calloc(1, sizeof *d);
    pthread_mutex_init(&d->mu, NULL); pthread_mutex_init(&d->dmu, NULL); pthread_mutex_init(&d->wmu, NULL); pthread_cond_init(&d->wcv, NULL);
    d->type = type; d->fmt = fmt; d->mode = mode;
    d->nout = !audio ? MD_NOUT_VIDEO : MD_NOUT_AAC;
    for (int i = 0; i < MD_NIN; i++) { d->in_g[i] = a32_guest_alloc(MD_INCAP, 16); d->in[i] = g_mem + d->in_g[i]; d->infree[i] = i; }
    d->ninfree = MD_NIN;
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 1 << 20);
    if (pthread_create(&d->wk, &a, md_worker, d)) die("movie: failed to create the decoder thread");
    pthread_attr_destroy(&a);
    pthread_mutex_lock(&g_decmu);
    int k = 0; while (k < MAXDEC && g_dec[k]) k++;
    if (k == MAXDEC) die("movie: too many decoders");
    g_dec[k] = d; wr32(self + 4, (uint32_t)k + 1); pthread_mutex_unlock(&g_decmu);
    LOG("[movie] decoder %#x: type %d output %d mode %d", self, type, fmt, mode);
}
static void h_dtor(Frame *f) { md_free(f->r[0]); }
static void h_dtor_deleting(Frame *f) {
    uint32_t self = f->r[0]; md_free(self);
    if (G.del) a32_callback(G.del, self, 0, 0, 0); else LOG("[RED] movie: no operator delete in the game; the decoder object is not freed");
}
static void h_configure(Frame *f) {
    mdec_t *d = md_of(f->r[0]); uint32_t md = f->r[1]; f->r[0] = 0;
    if (!d) { f->r[0] = 0x1000; return; }
    if (MD_AUDIO(d)) {
        for (int k = 0; k < 2 && md; k++) { uint32_t b = 0; char key[8]; snprintf(key, sizeof key, "csd-%d", k);
            if (g_find_buf(md, key, &b) && b && g_data(b) && g_size(b)) { size_t n = g_size(b); free(d->vhdr[k]); d->vhdr[k] = malloc(n); memcpy(d->vhdr[k], g_mem + g_data(b), n); d->vhdr_n[k] = (int)n; } }
        int ch = 0, rate = 0; if (md) { g_find_i32(md, "channel-count", &ch); g_find_i32(md, "sample-rate", &rate); }
        if (ch > 0) d->ch = ch; if (rate > 0) d->rate = rate;
        MD_TRACE(d, 0, "Configure (AAC): %d channels %d Hz, config %d bytes", ch, rate, d->vhdr_n[0]);
        return;
    }
    if (d->type == MD_TYPE_AVC && md) {
        md_avc_open(d);
        uint32_t b = 0;
        for (int k = 0; k < 2; k++) { char key[8]; snprintf(key, sizeof key, "csd-%d", k); if (g_find_buf(md, key, &b) && b && g_data(b) && g_size(b)) mavc_param_sets(d->avc, g_mem + g_data(b), g_size(b)); }
        if (g_find_buf(md, "avcc", &b) && b && g_data(b) && g_size(b)) mavc_avcc(d->avc, g_mem + g_data(b), g_size(b));
    }
    int w = 0, h = 0; if (md) { g_find_i32(md, "width", &w); g_find_i32(md, "height", &h); }
    pthread_mutex_lock(&d->mu); if (w > 0 && h > 0 && !d->fmt_sent) { d->w = w; d->h = h; } pthread_mutex_unlock(&d->mu);
    MD_TRACE(d, 0, "Configure: MediaData width %d height %d", w, h);
}
static void h_regev(Frame *f) {
    mdec_t *d = md_of(f->r[0]); uint32_t ev = f->r[1]; f->r[0] = 0;
    if (!d) { f->r[0] = 0x1000; return; }
    for (int i = 0; i < 4; i++) { uint32_t p = ev ? rd32(ev + 4 * (uint32_t)i) : 0; d->ev[i] = ev_ok(p) ? p : 0; }
    d->evc = ev;
    MD_TRACE(d, 1, "RegisterClientEvents: %#x %#x %#x %#x", ev ? rd32(ev) : 0, ev ? rd32(ev + 4) : 0, ev ? rd32(ev + 8) : 0, ev ? rd32(ev + 12) : 0);
}
static void h_start(Frame *f) {
    mdec_t *d = md_of(f->r[0]); f->r[0] = 0;
    if (!d) { f->r[0] = 0x2002; return; }
    pthread_mutex_lock(&d->mu); d->started = 1; pthread_mutex_unlock(&d->mu);
    MD_TRACE(d, 2, "Start");
    md_defer(d, 0);
}
static void md_reset(mdec_t *d, int close) {
    pthread_mutex_lock(&d->dmu);
    if (close) md_job(d, MDJ_CLOSE);
    if (d->avc) mavc_drop(d->avc);
    pthread_mutex_lock(&d->mu);
    uint64_t now = now_ns();
    for (int i = 0; i < MD_NIN; i++) { d->infree[i] = i; d->instale[i] = d->inown[i] ? now : 0; d->inown[i] = 0; }
    d->ninfree = MD_NIN; d->noutq = 0; d->npend = 0; d->avc_drain = 0; d->in_eos = 0;
    for (int i = 0; i < MD_NOUT; i++) { d->out[i].used = 0; if (close) d->out[i].pic = 0; }
    pthread_mutex_unlock(&d->mu);
    pthread_mutex_unlock(&d->dmu);
}
static void h_stop(Frame *f) {
    mdec_t *d = md_of(f->r[0]); f->r[0] = 0;
    if (!d) { f->r[0] = 0x2003; return; }
    MD_TRACE(d, 3, "Stop");
    pthread_mutex_lock(&g_dfmu); pthread_mutex_lock(&d->mu); d->started = 0; pthread_mutex_unlock(&d->mu); pthread_mutex_unlock(&g_dfmu);
    md_defer_drop(d); md_reset(d, 1);
}
static void h_reset(Frame *f) { mdec_t *d = md_of(f->r[0]); f->r[0] = 0; if (!d) { f->r[0] = 0x1000; return; } MD_TRACE(d, 4, "Reset"); md_reset(d, 1); d->fmt_sent = 0; }
static void h_flush(Frame *f) { mdec_t *d = md_of(f->r[0]); f->r[0] = 0; if (!d) { f->r[0] = 0x1000; return; } MD_TRACE(d, 5, "Flush"); md_defer_drop(d); md_reset(d, 0); }
static void h_acqin(Frame *f) {                         // (int *index, int *left)
    mdec_t *d = md_of(f->r[0]); uint32_t pidx = f->r[1], pleft = f->r[2]; f->r[0] = 0;
    if (!d) { f->r[0] = 0x1000; return; }
    if (!pidx || !pleft) { f->r[0] = 0x100b; return; }
    pthread_mutex_lock(&d->mu);
    int r = MD_NO_INPUT, idx = -1, left = 0;
    uint64_t now = now_ns(), wake = 0; int k = -1;
    for (int j = 0; j < d->ninfree && k < 0; j++) { int x = d->infree[j]; if (!d->instale[x] || now - d->instale[x] >= MD_STALE_NS) k = j; }
    if (d->in_eos) k = -1;
    if (k >= 0 && d->started) {
        idx = d->infree[k]; memmove(d->infree + k, d->infree + k + 1, (size_t)(--d->ninfree - k) * sizeof *d->infree); left = d->ninfree; r = 0;
        d->inown[idx] = 1; d->instale[idx] = 0;
    } else if (d->ninfree && d->started && !d->in_eos) {
        wake = UINT64_MAX; for (int j = 0; j < d->ninfree; j++) { uint64_t t = d->instale[d->infree[j]] + MD_STALE_NS; if (t < wake) wake = t; }
    }
    pthread_mutex_unlock(&d->mu);
    if (wake) md_defer_at(d, 0, wake);
    wr32(pidx, (uint32_t)idx); wr32(pleft, (uint32_t)left);
    MD_TRACE(d, 6, "AcquireInputBufferIndex -> %#x index %d left %d", r, idx, left);
    f->r[0] = (uint32_t)r;
}
static void h_getin(Frame *f) {                         // (int index, Buffer *)
    mdec_t *d = md_of(f->r[0]); int i = (int)f->r[1]; uint32_t buf = f->r[2]; f->r[0] = 0;
    if (!d) { f->r[0] = 0x1000; return; }
    if (!buf || i < 0 || i >= MD_NIN) { f->r[0] = 0x100b; return; }
    a32_callback(G.set_data, buf, d->in_g[i], MD_INCAP, 0); g_set_range(buf, 0, (int)MD_INCAP);
    MD_TRACE(d, 14, "GetInputBuffer index %d", i);
}
static void h_sendin(Frame *f) {                        // (int index, int offset, int size, long long time, unsigned flags): the time and the flags are on the stack
    mdec_t *d = md_of(f->r[0]); int i = (int)f->r[1], off = (int)f->r[2], size = (int)f->r[3];
    int64_t pts; memcpy(&pts, g_mem + f->r[13], 8); uint32_t flags = rd32(f->r[13] + 8); f->r[0] = 0;
    if (!d) { f->r[0] = 0x1000; return; }
    if (i < 0 || i >= MD_NIN || off < 0 || size < 0 || (uint32_t)off + (uint32_t)size > MD_INCAP) { f->r[0] = 0x100b; return; }
    pthread_mutex_lock(&d->mu);
    for (int k = 0; k < d->ninfree; k++) if (d->infree[k] == i) { memmove(d->infree + k, d->infree + k + 1, (size_t)(--d->ninfree - k) * sizeof *d->infree); break; }
    d->inown[i] = d->instale[i] = 0;
    if (flags & 4) d->in_eos = 1;
    for (int k = 0; k < d->npend; k++) if (d->pend[k].in == i) { pthread_mutex_unlock(&d->mu); f->r[0] = 0x100b; return; }
    if (d->npend == MD_NIN) { pthread_mutex_unlock(&d->mu); f->r[0] = 0x100b; return; }
    int fmt_now = 0;
    if (!d->fmt_sent) { d->fmt_sent = 1; fmt_now = 1; if (!d->w && !MD_AUDIO(d)) { d->w = 1920; d->h = 1080; } }
    d->pend[d->npend].in = i; d->pend[d->npend].off = off; d->pend[d->npend].size = size; d->pend[d->npend].pts = pts; d->pend[d->npend].flags = flags; d->npend++;
    int queued = d->npend;
    pthread_mutex_unlock(&d->mu);
    MD_TRACE(d, 7, "SendInputBufferForDecode index %d offset %d length %d time %lld us flags %#x (%d packets queued)", i, off, size, (long long)pts, flags, queued);
    if (fmt_now) md_event(d, 2);
    md_pump(d);
}
static void h_acqout(Frame *f) {                        // (int *index, long long *time, unsigned *flags, int *left): left is on the stack
    mdec_t *d = md_of(f->r[0]); uint32_t pidx = f->r[1], ppts = f->r[2], pfl = f->r[3], pleft = rd32(f->r[13]); f->r[0] = 0;
    if (!d) { f->r[0] = 0x1000; return; }
    if (!pidx || !ppts || !pfl || !pleft) { f->r[0] = 0x100b; return; }
    pthread_mutex_lock(&d->mu);
    int r = MD_NO_OUTPUT, idx = -1, left = 0; int64_t pts = 0; uint32_t flags = 0;
    if (d->noutq && d->started) {
        int s = d->outq[0]; memmove(d->outq, d->outq + 1, --d->noutq * sizeof *d->outq);
        idx = s; pts = d->out[s].pts; flags = d->out[s].flags; left = d->noutq; r = 0;
    }
    pthread_mutex_unlock(&d->mu);
    wr32(pidx, (uint32_t)idx); memcpy(g_mem + ppts, &pts, 8); wr32(pfl, flags); wr32(pleft, (uint32_t)left);
    if (!r) MD_TRACE(d, 8, "AcquireOutputBufferIndex -> index %d time %lld flags %#x left %d", idx, (long long)pts, flags, left);
    f->r[0] = (uint32_t)r;
}
static void md_format(mdec_t *d, uint32_t md) {
    if (MD_AUDIO(d)) { g_set_i32(md, "channel-count", d->ch > 0 ? d->ch : 2); g_set_i32(md, "sample-rate", d->rate > 0 ? d->rate : 48000); return; }
    int w, h, stride, slice; md_dims(d, &w, &h, &stride, &slice);
    g_set_i32(md, "width", w); g_set_i32(md, "height", h);
    { uint32_t a[6] = {md, gstring("crop"), 0, 0, (uint32_t)(w - 1), (uint32_t)(h - 1)}; a32_callbackv(G.set_rect, 6, a); }       // SetRect(this, key, l, t, r, b): the last two go on the stack
    g_set_i32(md, "stride", stride); g_set_i32(md, "slice-height", slice);
    g_set_i32(md, "nv12-y-stride", stride); g_set_i32(md, "nv12-y-offset", 0); g_set_i32(md, "nv12-uv-offset", stride * slice);
    g_set_i32(md, "nv12-uv-stride", stride); g_set_i32(md, "nv12-width", w); g_set_i32(md, "nv12-height", h);
}
static void h_outfmt(Frame *f) { mdec_t *d = md_of(f->r[0]); uint32_t md = f->r[1]; f->r[0] = 0; if (!d) { f->r[0] = 0x1000; return; } if (!md) { f->r[0] = 0x100b; return; } md_format(d, md); MD_TRACE(d, 9, "GetOutputFormat"); }
static void h_buffmt(Frame *f) { mdec_t *d = md_of(f->r[0]); uint32_t md = f->r[2]; int i = (int)f->r[1]; f->r[0] = 0; if (!d) { f->r[0] = 0x1000; return; } if (!md) { f->r[0] = 0x100b; return; } md_format(d, md); MD_TRACE(d, 10, "GetOutputBufferFormat index %d", i); }
static void h_outsize(Frame *f) {                       // (int *size, int *mode)
    mdec_t *d = md_of(f->r[0]); uint32_t psize = f->r[1], pmode = f->r[2]; f->r[0] = 0;
    if (!d) { f->r[0] = 0x1000; return; }
    if (!psize || !pmode) { f->r[0] = 0x100b; return; }
    int size;
    if (MD_AUDIO(d)) {
        int sbr = d->vhdr_n[0] > 0 && ((d->vhdr[0][0] >> 3) == 5 || (d->vhdr[0][0] >> 3) == 29);
        size = d->ch > 0 ? 1024 * (sbr ? 2 : 1) * d->ch * 2 : MD_PCM_CAP;
    } else { int w, h, stride, slice; md_dims(d, &w, &h, &stride, &slice); size = stride * slice * 3 / 2; }
    wr32(psize, (uint32_t)size); wr32(pmode, (uint32_t)d->mode);
    MD_TRACE(d, 11, "GetOutputBufferSize -> %d", size);
}
static int md_out_tex(mdec_t *d, int i, uint32_t buf) {
    uint32_t p = g_data(buf), cap = g_cap(buf);
    if (!p || cap < 8) { LOG("[RED] movie: client buffer in native-texture mode is %u bytes, needs 8 (a texture object)", cap); return 0x100b; }
    void *tex = g_mem + p; int tw = 0, th = 0, fm = nvn_host_write_texture(tex, NULL, 0, 0, 0, &tw, &th);
    if (fm < 0) { LOG("[RED] movie: buffer data %#x in native-texture mode is not a texture object", p); return 0x100b; }
    int w = d->w < tw ? d->w : tw, h = d->h < th ? d->h : th;
    if (d->out[i].pic && d->out[i].frm) nvn_host_write_texture(tex, d->out[i].frm, w, h, (size_t)d->w * 4, NULL, NULL);
    uint32_t sy = G.nvn_sync ? a32_callback(G.nvn_sync, buf, 0, 0, 0) : 0; if (sy) nvn_host_signal_sync(g_mem + sy);
    MD_TRACE(d, 12, "GetOutputBuffer index %d -> texture %p (%d x %d format %#x)", i, tex, tw, th, fm);
    return 0;
}
static int md_getout(mdec_t *d, int i, uint32_t buf) {
    if (!buf || i < 0 || i >= d->nout) return 0x100b;
    if (d->mode == MD_MODE_TEX && !MD_AUDIO(d)) return md_out_tex(d, i, buf);
    g_set_range(buf, 0, 0);
    if (MD_AUDIO(d)) {
        size_t n = d->out[i].pic ? d->out[i].bytes : 0, cap = g_cap(buf); uint32_t p = g_data(buf);
        if (!p || cap < n) { LOG("[RED] movie: client audio output buffer is %zu bytes, this packet needs %zu", cap, n); return 0x100b; }
        if (n) memcpy(g_mem + p, d->out[i].frm, n);
        g_set_range(buf, 0, (int)n);
        MD_TRACE(d, 12, "GetOutputBuffer index %d -> PCM %zu bytes", i, n);
        return 0;
    }
    int w, h, stride, slice; md_dims(d, &w, &h, &stride, &slice);
    size_t need = (size_t)stride * slice * 3 / 2, cap = g_cap(buf); uint32_t p = g_data(buf);
    if (!p || cap < need) { LOG("[RED] movie: client output buffer is %zu bytes, a frame needs %zu", cap, need); return 0x100b; }
    if (d->out[i].pic && d->out[i].frm) memcpy(g_mem + p, d->out[i].frm, need);
    else { memset(g_mem + p, 16, (size_t)stride * slice); memset(g_mem + p + (size_t)stride * slice, 128, need - (size_t)stride * slice); }
    g_set_range(buf, 0, (int)need);
    MD_TRACE(d, 12, "GetOutputBuffer index %d -> %zu bytes%s", i, need, d->out[i].pic ? "" : " (black frame)");
    return 0;
}
static void h_getout(Frame *f) { mdec_t *d = md_of(f->r[0]); int i = (int)f->r[1]; uint32_t buf = f->r[2]; f->r[0] = d ? (uint32_t)md_getout(d, i, buf) : 0x1000; }
static void h_getout_fmt(Frame *f) { mdec_t *d = md_of(f->r[0]); int i = (int)f->r[1]; uint32_t buf = f->r[2], md = f->r[3]; if (!d) { f->r[0] = 0x1000; return; } if (md) md_format(d, md); f->r[0] = (uint32_t)md_getout(d, i, buf); }
static void h_relout(Frame *f) {
    mdec_t *d = md_of(f->r[0]); int i = (int)f->r[1]; f->r[0] = 0;
    if (!d) { f->r[0] = 0x1000; return; }
    if (i < 0 || i >= d->nout) { f->r[0] = 0x100b; return; }
    pthread_mutex_lock(&d->mu);
    d->out[i].used = 0; d->out[i].pic = 0;
    int queued = d->npend, drain = d->avc_drain;
    pthread_mutex_unlock(&d->mu);
    MD_TRACE(d, 13, "ReleaseOutputBufferIndex index %d (%d packets queued%s)", i, queued, drain ? ", H.264 pictures draining" : "");
    if (queued || drain) md_pump(d);
}
static void h_lasterr(Frame *f) { f->r[0] = 0; }

// ---- binding: imports by name (a32_force), virtual calls by patching a copy of the class's table ----
static const struct { const char *name; void (*fn)(Frame *); } k_methods[] = {
    { "_ZN5movie7DecoderD2Ev", h_dtor }, { "_ZN5movie7DecoderD1Ev", h_dtor }, { "_ZN5movie7DecoderD0Ev", h_dtor_deleting },
    { "_ZN5movie7Decoder9ConfigureEPNS_9MediaDataE", h_configure }, { "_ZN5movie7Decoder20RegisterClientEventsEPNS_13DecoderEventsE", h_regev },
    { "_ZN5movie7Decoder5StartEv", h_start }, { "_ZN5movie7Decoder4StopEv", h_stop }, { "_ZN5movie7Decoder5ResetEv", h_reset }, { "_ZN5movie7Decoder5FlushEv", h_flush },
    { "_ZN5movie7Decoder23AcquireInputBufferIndexEPiS1_", h_acqin }, { "_ZN5movie7Decoder14GetInputBufferEiPNS_6BufferE", h_getin },
    { "_ZN5movie7Decoder24SendInputBufferForDecodeEiiixj", h_sendin }, { "_ZN5movie7Decoder24AcquireOutputBufferIndexEPiPxPjS1_", h_acqout },
    { "_ZN5movie7Decoder15GetOutputFormatEPNS_9MediaDataE", h_outfmt }, { "_ZN5movie7Decoder21GetOutputBufferFormatEiPNS_9MediaDataE", h_buffmt },
    { "_ZN5movie7Decoder19GetOutputBufferSizeEPiS1_", h_outsize }, { "_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferE", h_getout },
    { "_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferEPNS_9MediaDataE", h_getout_fmt }, { "_ZN5movie7Decoder24ReleaseOutputBufferIndexEi", h_relout },
    { "_ZN5movie7Decoder12GetLastErrorEv", h_lasterr },
};
static void h_ctor_first(Frame *f) {
    if (!g_vt) {                                          // the table is built at the first construction: when every symbol is known
        uint32_t vt0 = a32_find_symbol("_ZTVN5movie7DecoderE");
        if (!vt0) die("movie: the game has no vtable of movie::Decoder");
        uint32_t copy = a32_guest_alloc(4 * 96, 4); memcpy(g_mem + copy, g_mem + vt0, 4 * 96);
        int patched = 0;
        for (unsigned k = 0; k < sizeof k_methods / sizeof *k_methods; k++) {
            uint32_t addr = a32_find_symbol(k_methods[k].name); if (!addr) continue;
            uint32_t h = a32_hook_add(k_methods[k].name, k_methods[k].fn);
            for (int w = 0; w < 96; w++) if (rd32(copy + 4 * (uint32_t)w) == addr) { wr32(copy + 4 * (uint32_t)w, h); patched++; }
        }
        g_vt = copy + 8; LOG("[movie] Decoder vtable: %d entries patched", patched);
        for (int w = 2; w < 40; w++) { uint32_t v = rd32(copy + 4 * (uint32_t)w); LOG("[movie] vtable slot %d: %s", w - 2, v >= HOOK_BASE ? "host" : where(v)); }
    }
    h_ctor(f);
}
#define M(name, fn) { name, fn }
const ForceEnt a32_force_movie[] = {
    M("_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", h_ctor_first), M("_ZN5movie7DecoderC2ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", h_ctor_first),
    M("_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeE", h_ctor_first), M("_ZN5movie7DecoderC2ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeE", h_ctor_first),
    M("_ZN5movie7DecoderD2Ev", h_dtor), M("_ZN5movie7DecoderD1Ev", h_dtor), M("_ZN5movie7DecoderD0Ev", h_dtor_deleting),
    M("_ZN5movie7Decoder9ConfigureEPNS_9MediaDataE", h_configure), M("_ZN5movie7Decoder20RegisterClientEventsEPNS_13DecoderEventsE", h_regev),
    M("_ZN5movie7Decoder5StartEv", h_start), M("_ZN5movie7Decoder4StopEv", h_stop), M("_ZN5movie7Decoder5ResetEv", h_reset), M("_ZN5movie7Decoder5FlushEv", h_flush),
    M("_ZN5movie7Decoder23AcquireInputBufferIndexEPiS1_", h_acqin), M("_ZN5movie7Decoder14GetInputBufferEiPNS_6BufferE", h_getin),
    M("_ZN5movie7Decoder24SendInputBufferForDecodeEiiixj", h_sendin), M("_ZN5movie7Decoder24AcquireOutputBufferIndexEPiPxPjS1_", h_acqout),
    M("_ZN5movie7Decoder15GetOutputFormatEPNS_9MediaDataE", h_outfmt), M("_ZN5movie7Decoder21GetOutputBufferFormatEiPNS_9MediaDataE", h_buffmt),
    M("_ZN5movie7Decoder19GetOutputBufferSizeEPiS1_", h_outsize), M("_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferE", h_getout),
    M("_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferEPNS_9MediaDataE", h_getout_fmt), M("_ZN5movie7Decoder24ReleaseOutputBufferIndexEi", h_relout),
    M("_ZN5movie7Decoder12GetLastErrorEv", h_lasterr),
};
const unsigned a32_nforce_movie = sizeof a32_force_movie / sizeof *a32_force_movie;
