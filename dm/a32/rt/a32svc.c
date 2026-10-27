// The 32-bit Horizon kernel interface for translated AArch32 guests: svc numbers and register conventions as the Nintendo SDK's
// 32-bit svc stubs use them (read off the sdk module: inputs in r0-r4, outputs from r1 up, 64-bit values in register pairs).
// Threads are pthreads running a32_enter; synchronization (address arbitration, process-wide keys, events) is implemented here.
#define _DARWIN_C_SOURCE
#include "a32host.h"
#include "a32ipc.h"
#include <errno.h>
#include <time.h>
#include <mach/mach_time.h>

// ---- results ------------------------------------------------------------------------------------------------------------------
enum { KR_OK = 0, KR_TIMEOUT = 0xea01, KR_INVALID_HANDLE = 0xe401, KR_INVALID_ADDR = 0xcc01, KR_INVALID_STATE = 0xd401, KR_NOT_FOUND = 0xf001, KR_PORT_UNKNOWN = 0xf201 };

// ---- handles ------------------------------------------------------------------------------------------------------------------
typedef enum { H_NONE, H_THREAD, H_EVENT_W, H_EVENT_R, H_SESSION, H_TRANSFER, H_SHMEM } HType;
typedef struct { int fd; uint32_t size; } ShMem;
typedef struct GThread GThread;
typedef struct { HType type; void *obj; int refs; } HEnt;
typedef struct { int signaled; int manual; } Event;
#define MAX_HANDLES 4096
static HEnt g_handles[MAX_HANDLES];
static pthread_mutex_t g_big = PTHREAD_MUTEX_INITIALIZER;     // one lock for all kernel state: the guest's own locking is what matters for speed
static pthread_cond_t g_big_cv = PTHREAD_COND_INITIALIZER;     // broadcast on every state change a waiter may care about (simple, correct, slow)

static uint32_t handle_new(HType t, void *obj) {
    for (uint32_t i = 1; i < MAX_HANDLES; i++) if (g_handles[i].type == H_NONE) { g_handles[i] = (HEnt){t, obj, 1}; return 0xd000 + i; }
    die("out of handles");
    return 0;
}
static HEnt *handle_get(uint32_t h) { uint32_t i = h - 0xd000; return (h >= 0xd001 && i < MAX_HANDLES && g_handles[i].type != H_NONE) ? &g_handles[i] : NULL; }

// ---- threads ------------------------------------------------------------------------------------------------------------------
struct GThread {
    pthread_t th; ThreadCtx ctx; uint32_t handle, id, entry, arg, stack_top, tls; int prio, core, started, exited;
    uint32_t wait_addr, wait_key; int waiting_lock, waiting_key, woken;     // address arbitration / process-wide key waits
    uint64_t mask; int cancel_sync; uint32_t cb_sp;                          // cb_sp: the stack of a host-made helper thread, below which the host may call guest code
    struct GThread *next;
};
static GThread *g_threads; static uint32_t g_next_tid = 1; static __thread GThread *t_self;
static GThread g_main_thread;

#define TLS_BASE 0x0f000000u
static uint32_t alloc_tls(uint32_t id) { uint32_t a = TLS_BASE + id * 0x200; memset(g_mem + a, 0, 0x200); return a; }

GThread *a32_main_thread_init(uint32_t stack_top) {
    GThread *t = &g_main_thread;
    t->id = g_next_tid++; t->handle = handle_new(H_THREAD, t); t->prio = 44; t->core = 0; t->started = 1; t->stack_top = stack_top;
    t->tls = alloc_tls(t->id); t->ctx.mem = g_mem; t->ctx.tls = t->tls; t->mask = 7; t_self = t; t->next = g_threads; g_threads = t;
    return t;
}
ThreadCtx *a32_main_ctx(void) { return &g_main_thread.ctx; }
ThreadCtx *a32_cur_ctx(void) { return &t_self->ctx; }
static GThread *thread_of(uint32_t h) {
    if (h == 0xffff8000 || h == 0) return t_self;           // 0: the SDK's main ThreadType keeps no handle of its own, the call means the current thread
    HEnt *e = handle_get(h); return e && e->type == H_THREAD ? e->obj : NULL;
}

static void *thread_main(void *p) {
    GThread *t = p; t_self = t;
    uint32_t regs[16] = {0};
    regs[0] = t->arg; regs[13] = t->stack_top; regs[14] = HOOK_BASE;
    if (g_trace) LOG("thread %u starts at %s (arg %#x, sp %#x)", t->id, where(t->entry), t->arg, t->stack_top);
    a32_enter(&t->ctx, regs, t->entry);
    pthread_mutex_lock(&g_big); t->exited = 1; pthread_cond_broadcast(&g_big_cv); pthread_mutex_unlock(&g_big);
    return NULL;
}

// ---- time ---------------------------------------------------------------------------------------------------------------------
static uint64_t now_ns(void) { static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb); return mach_absolute_time() * tb.numer / tb.denom; }
uint64_t a32_system_tick(void) { return now_ns() * 12 / 625; }      // the Switch counter runs at 19.2 MHz
static struct timespec deadline_after(uint64_t ns) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t t = (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec + ns; ts.tv_sec = t / 1000000000ull; ts.tv_nsec = t % 1000000000ull; return ts;
}
// wait on the big condition variable until pred-state changes or the timeout (ns, -1 = forever) expires; returns 1 on timeout. Caller holds g_big.
static int wait_big(int64_t ns) {
    if (ns < 0) { pthread_cond_wait(&g_big_cv, &g_big); return 0; }
    struct timespec ts = deadline_after((uint64_t)ns);
    return pthread_cond_timedwait(&g_big_cv, &g_big, &ts) == ETIMEDOUT;
}

// ---- address arbitration (mutexes) and process-wide keys (condition variables) --------------------------------------------------------
#define WAIT_MASK 0x40000000u
static volatile uint32_t *gptr(uint32_t a) { return (volatile uint32_t *)(g_mem + a); }

// the guest's mutex word: owner handle | WAIT_MASK when somebody waits. ArbitrateLock sleeps while the word still says "owned by that thread, with waiters".
static uint32_t g_watch;
#define WATCH(addr, ...) do { if (g_watch && (addr) == g_watch) { fprintf(stderr, "[a32] watch %#x: ", addr); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
static uint32_t arbitrate_lock(uint32_t owner, uint32_t addr, uint32_t tag) {
    if (!g_watch && getenv("A32_WATCH")) g_watch = (uint32_t)strtoul(getenv("A32_WATCH"), NULL, 16);
    WATCH(addr, "[t%u] ArbitrateLock owner %#x tag %#x word %#x", t_self->id, owner, tag, rd32(addr));
    pthread_mutex_lock(&g_big);
    GThread *me = t_self;
    for (;;) {
        if (__atomic_load_n(gptr(addr), __ATOMIC_ACQUIRE) != (owner | WAIT_MASK)) break;
        me->waiting_lock = 1; me->wait_addr = addr; me->woken = 0;
        while (!me->woken) wait_big(-1);
        me->waiting_lock = 0;
        break;
    }
    pthread_mutex_unlock(&g_big);
    (void)tag; return KR_OK;
}
static uint32_t arbitrate_unlock(uint32_t addr) {
    WATCH(addr, "[t%u] ArbitrateUnlock word %#x", t_self->id, rd32(addr));
    pthread_mutex_lock(&g_big);
    GThread *best = NULL; int more = 0;
    for (GThread *t = g_threads; t; t = t->next) if (t->waiting_lock && t->wait_addr == addr && !t->woken) { if (!best || t->prio < best->prio) { if (best) more = 1; best = t; } else more = 1; }
    uint32_t nv = 0;
    if (best) { nv = best->handle | (more ? WAIT_MASK : 0); best->woken = 1; }
    __atomic_store_n(gptr(addr), nv, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&g_big_cv);
    pthread_mutex_unlock(&g_big);
    return KR_OK;
}
static uint32_t wait_key_atomic(uint32_t addr, uint32_t key, uint32_t tag, int64_t timeout) {
    WATCH(addr, "[t%u] WaitKeyAtomic key %#x tag %#x word %#x timeout %lld", t_self->id, key, tag, rd32(addr), (long long)timeout);
    pthread_mutex_lock(&g_big);
    GThread *me = t_self;
    // release the mutex at addr as ArbitrateUnlock does
    GThread *best = NULL; int more = 0;
    for (GThread *t = g_threads; t; t = t->next) if (t->waiting_lock && t->wait_addr == addr && !t->woken) { if (!best || t->prio < best->prio) { if (best) more = 1; best = t; } else more = 1; }
    uint32_t nv = 0; if (best) { nv = best->handle | (more ? WAIT_MASK : 0); best->woken = 1; }
    __atomic_store_n(gptr(addr), nv, __ATOMIC_RELEASE);
    me->waiting_key = 1; me->wait_key = key; me->woken = 0;
    int to = 0;
    pthread_cond_broadcast(&g_big_cv);
    struct timespec ts; if (timeout >= 0) ts = deadline_after((uint64_t)timeout);
    while (!me->woken && !to) {
        if (timeout < 0) pthread_cond_wait(&g_big_cv, &g_big);
        else if (pthread_cond_timedwait(&g_big_cv, &g_big, &ts) == ETIMEDOUT) to = 1;
    }
    me->waiting_key = 0;
    if (to) { pthread_mutex_unlock(&g_big); return KR_TIMEOUT; }       // a timeout leaves the mutex unlocked: the caller (libnx, nn::os) locks it again itself
    // re-acquire the mutex word: take it if free, otherwise mark it contended and sleep as a lock waiter
    for (;;) {
        uint32_t cur = __atomic_load_n(gptr(addr), __ATOMIC_ACQUIRE);
        if (cur == 0) { uint32_t z = 0; if (__atomic_compare_exchange_n(gptr(addr), &z, tag, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) break; continue; }     // the guest's own atomics race with these: compare-and-swap, never a blind store
        uint32_t owner = cur & ~WAIT_MASK;
        if (!(cur & WAIT_MASK) && !__atomic_compare_exchange_n(gptr(addr), &cur, owner | WAIT_MASK, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) continue;
        me->waiting_lock = 1; me->wait_addr = addr; me->woken = 0;
        while (!me->woken) wait_big(-1);
        me->waiting_lock = 0;
        if (__atomic_load_n(gptr(addr), __ATOMIC_ACQUIRE) == (tag | 0) || __atomic_load_n(gptr(addr), __ATOMIC_ACQUIRE) == (tag | WAIT_MASK)) break;   // handed to us
    }
    pthread_mutex_unlock(&g_big);
    return to ? KR_TIMEOUT : KR_OK;
}
static void signal_key(uint32_t key, int32_t count) {
    pthread_mutex_lock(&g_big);
    int n = 0;
    for (GThread *t = g_threads; t && (count <= 0 || n < count); t = t->next) if (t->waiting_key && t->wait_key == key && !t->woken) { t->woken = 1; n++; }
    pthread_cond_broadcast(&g_big_cv);
    pthread_mutex_unlock(&g_big);
}

// ---- memory map ---------------------------------------------------------------------------------------------------------------
typedef struct { uint32_t start, size, state, perm, attr; } Region;
static Region g_reg[256]; static int g_nreg;
void a32_region_add(uint32_t start, uint32_t size, uint32_t state, uint32_t perm) {
    if (g_nreg < 256) g_reg[g_nreg++] = (Region){start, size, state, perm, 0};
}
static void query_memory(uint32_t out, uint32_t addr) {
    uint64_t base = addr & ~0xfffu, size = 0, state = 0, perm = 0, attr = 0;
    int hit = 0;
    for (int i = 0; i < g_nreg; i++) if (addr >= g_reg[i].start && addr < (uint64_t)g_reg[i].start + g_reg[i].size) { base = g_reg[i].start; size = g_reg[i].size; state = g_reg[i].state; perm = g_reg[i].perm; hit = 1; break; }
    if (!hit) {                                   // a free gap up to the next region
        uint64_t lo = 0, hi = 1ull << 32;
        for (int i = 0; i < g_nreg; i++) {
            uint64_t s = g_reg[i].start, e = s + g_reg[i].size;
            if (e <= addr && e > lo) lo = e;
            if (s > addr && s < hi) hi = s;
        }
        base = lo; size = hi - lo;
    }
    uint8_t *m = g_mem + out; memset(m, 0, 0x28);
    memcpy(m, &base, 8); memcpy(m + 8, &size, 8);
    uint32_t w[3] = {(uint32_t)state, (uint32_t)attr, (uint32_t)perm}; memcpy(m + 16, w, 12);
}

// ---- GetInfo ----------------------------------------------------------------------------------------------------------------------
static int g_nthreads_alive(void) { int n = 0; for (GThread *t = g_threads; t; t = t->next) if (!t->exited) n++; return n; }
static uint64_t total_memory(void) { static uint64_t v; if (!v) v = getenv("A32_TOTALMEM") ? strtoull(getenv("A32_TOTALMEM"), NULL, 0) : TOTAL_MEMORY; return v; }      // A32_TOTALMEM=<bytes> (debugging: what the game believes it has)
static uint64_t get_info(uint32_t type, uint32_t handle, uint64_t sub, uint32_t *res) {
    *res = KR_OK; (void)handle;
    switch (type) {
    case 0: return 7;                              // CoreMask
    case 1: return 0xffffffffffffffffull;          // PriorityMask
    case 2: return ALIAS_BASE;  case 3: return ALIAS_SIZE;
    case 4: return HEAP_BASE;   case 5: return HEAP_REGION_SIZE;
    case 6: return total_memory(); case 7: return g_heap_size + 0x2000000;
    case 8: return 0;                              // DebuggerAttached
    case 9: return 0;                              // ResourceLimit
    case 10: return 0;                             // IdleTickCount
    case 11: { static uint64_t e[4] = {0x1234567890abcdefull, 0x0fedcba098765432ull, 0x1111222233334444ull, 0x5555666677778888ull}; if (sub > 3) { *res = 0xe801; return 0; } return e[sub]; }
    case 12: return ASLR_BASE;  case 13: return ASLR_SIZE;
    case 14: return STACK_REGION_BASE; case 15: return STACK_REGION_SIZE;
    case 16: return 0; case 17: return 0;          // system resource size total / used
    case 18: return 0x0100770008DD8000ull;         // ProgramId
    case 20: return 0;                             // UserExceptionContextAddress
    case 21: return total_memory(); case 22: return g_heap_size + 0x2000000;
    case 23: return 1;                             // IsApplication
    case 24: return 96 - g_nthreads_alive();       // FreeThreadCount
    case 25: return 0;                             // ThreadTickCount
    default: *res = 0xf001; return 0;
    }
}

// ---- the svc dispatcher -----------------------------------------------------------------------------------------------------------------
static void set64(Frame *f, int i, uint64_t v) { f->r[i] = (uint32_t)v; f->r[i + 1] = (uint32_t)(v >> 32); }
void a32_svc_c(uint32_t num, Frame *f) {
    uint32_t *r = f->r;
    if (g_trace) {
        static uint32_t seen[8];
        if (g_trace > 1 || !(seen[(num >> 5) & 7] & (1u << (num & 31)))) {
            seen[(num >> 5) & 7] |= 1u << (num & 31);
            LOG("[t%u] svc %#x from %s: r0-r5 %08x %08x %08x %08x %08x %08x", t_self ? t_self->id : 0, num, where(r[14]), r[0], r[1], r[2], r[3], r[4], r[5]);
        }
    }
    switch (num) {
    case 0x01: {                                   // SetHeapSize(r1 = size) -> r1 = base
        uint32_t size = r[1];
        pthread_mutex_lock(&g_big);
        if (size > HEAP_REGION_SIZE) { pthread_mutex_unlock(&g_big); r[0] = 0xd001; return; }
        g_heap_size = size; pthread_mutex_unlock(&g_big);
        a32_region_update_heap();
        r[0] = KR_OK; r[1] = HEAP_BASE; return;
    }
    case 0x02: case 0x03: r[0] = KR_OK; return;     // SetMemoryPermission / SetMemoryAttribute: the guest never executes memory, so permissions need no change
    case 0x04: {                                   // MapMemory(dst, src, size): move the contents (the SDK moves thread stacks into the stack region)
        uint32_t dst = r[0], src = r[1], size = r[2];
        memcpy(g_mem + dst, g_mem + src, size); a32_region_add(dst, size, 0xb, 3); r[0] = KR_OK; return;
    }
    case 0x05: {                                   // UnmapMemory(dst, src, size): copy back
        uint32_t dst = r[0], src = r[1], size = r[2];
        memcpy(g_mem + src, g_mem + dst, size); r[0] = KR_OK; return;
    }
    case 0x06: query_memory(r[0], r[2]); r[0] = KR_OK; r[1] = 0; return;
    case 0x07: LOG("ExitProcess(%u)", r[0]); dump_regs(f); dump_strings(f);
        for (uint32_t a = r[13]; a < r[13] + 0x1800; ) { uint32_t n = 0; while (n < 300 && g_mem[a + n] >= 32 && g_mem[a + n] < 127) n++; if (n >= 14) { LOG("stack text @%#x: \"%.*s\"", a, (int)n, g_mem + a); a += n; } else a++; }
        fflush(stderr); _exit((int)r[0]);
    case 0x08: {                                   // CreateThread(r0 = priority, r1 = entry, r2 = arg, r3 = stack top, r4 = core) -> r1 = handle
        GThread *t = calloc(1, sizeof *t);
        pthread_mutex_lock(&g_big);
        t->id = g_next_tid++; t->entry = r[1]; t->arg = r[2]; t->stack_top = r[3]; t->prio = (int)r[0]; t->core = (int)r[4]; t->mask = 1ull << (r[4] & 3);
        t->tls = alloc_tls(t->id); t->ctx.mem = g_mem; t->ctx.tls = t->tls;
        t->handle = handle_new(H_THREAD, t); t->next = g_threads; g_threads = t;
        pthread_mutex_unlock(&g_big);
        r[0] = KR_OK; r[1] = t->handle; return;
    }
    case 0x09: {                                   // StartThread(r0 = handle)
        GThread *t = thread_of(r[0]);
        if (!t) { r[0] = KR_INVALID_HANDLE; return; }
        if (!t->started) {
            t->started = 1;
            pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 8u << 20);
            if (pthread_create(&t->th, &a, thread_main, t)) die("pthread_create");
            pthread_detach(t->th);
        }
        r[0] = KR_OK; return;
    }
    case 0x0a: {                                   // ExitThread
        GThread *t = t_self;
        pthread_mutex_lock(&g_big); t->exited = 1; pthread_cond_broadcast(&g_big_cv); pthread_mutex_unlock(&g_big);
        if (t == &g_main_thread) { LOG("main thread exited"); fflush(stderr); _exit(0); }
        pthread_exit(NULL);
    }
    case 0x0b: {                                   // SleepThread(r0:r1 = ns)
        if (g_trace >= 4) { static __thread int shown; if (!shown++) { LOG("thread %u sleeps", t_self->id); dump_regs(f); } }
        int64_t ns = (int64_t)(((uint64_t)r[1] << 32) | r[0]);
        if (ns <= 0) { sched_yield(); } else { struct timespec ts = {(time_t)(ns / 1000000000), (long)(ns % 1000000000)}; nanosleep(&ts, NULL); }
        r[0] = KR_OK; return;
    }
    case 0x0c: { GThread *t = thread_of(r[1]); if (!t) { r[0] = KR_INVALID_HANDLE; return; } r[0] = KR_OK; r[1] = (uint32_t)t->prio; return; }
    case 0x0d: { GThread *t = thread_of(r[0]); if (!t) { r[0] = KR_INVALID_HANDLE; return; } t->prio = (int)r[1]; r[0] = KR_OK; return; }
    case 0x0e: { GThread *t = thread_of(r[2]); if (!t) { r[0] = KR_INVALID_HANDLE; return; } r[0] = KR_OK; r[1] = (uint32_t)t->core; set64(f, 2, t->mask); return; }
    case 0x0f: { GThread *t = thread_of(r[0]); if (!t) { r[0] = KR_INVALID_HANDLE; return; } if ((int32_t)r[1] >= 0) t->core = (int)r[1]; t->mask = ((uint64_t)r[3] << 32) | r[2]; r[0] = KR_OK; return; }
    case 0x10: r[0] = (uint32_t)(t_self ? t_self->core : 0); return;
    case 0x11: case 0x12: case 0x17: {             // SignalEvent / ClearEvent / ResetSignal
        HEnt *e = handle_get(r[0]); if (!e || (e->type != H_EVENT_W && e->type != H_EVENT_R)) { r[0] = KR_INVALID_HANDLE; return; }
        Event *ev = e->obj;
        pthread_mutex_lock(&g_big);
        if (num == 0x11) ev->signaled = 1; else if (num == 0x12) ev->signaled = 0; else { if (!ev->signaled) { pthread_mutex_unlock(&g_big); r[0] = KR_INVALID_STATE; return; } ev->signaled = 0; }
        pthread_cond_broadcast(&g_big_cv); pthread_mutex_unlock(&g_big);
        r[0] = KR_OK; return;
    }
    case 0x16: {                                   // CloseHandle
        if (r[0] >= 0xe000 && r[0] < 0xe200) { a32_session_close(r[0]); r[0] = KR_OK; return; }      // a host session (closed by the guest's close message already, or not)
        HEnt *e = handle_get(r[0]); if (!e) { r[0] = KR_INVALID_HANDLE; return; }
        if (e->type != H_THREAD && e->type != H_EVENT_W && e->type != H_EVENT_R) { e->type = H_NONE; }
        r[0] = KR_OK; return;
    }
    case 0x18: {                                   // WaitSynchronization(r0:r3 = timeout ns, r1 = handles*, r2 = count) -> r1 = index
        int64_t to = (int64_t)(((uint64_t)r[3] << 32) | r[0]); uint32_t hp = r[1]; int n = (int)r[2];
        if (g_trace >= 3) for (int i = 0; i < n; i++) { uint32_t h = rd32(hp + 4 * i); HEnt *e = handle_get(h); LOG("  [t%u] wait handle %#x type %d timeout %lld", t_self->id, h, e ? e->type : -1, (long long)to); }
        pthread_mutex_lock(&g_big);
        struct timespec ts; if (to > 0) ts = deadline_after((uint64_t)to);
        for (;;) {
            if (t_self->cancel_sync) { t_self->cancel_sync = 0; pthread_mutex_unlock(&g_big); r[0] = 0xec01; return; }
            for (int i = 0; i < n; i++) {
                uint32_t h = rd32(hp + 4 * i); HEnt *e = handle_get(h);
                if (!e) { pthread_mutex_unlock(&g_big); r[0] = KR_INVALID_HANDLE; return; }
                int sig = 0;
                if (e->type == H_EVENT_R || e->type == H_EVENT_W) { Event *ev = e->obj; sig = ev->signaled; }          // a readable event stays signalled when a wait on it succeeds; the SDK clears it (ClearEvent / ResetSignal) when its mode asks
                else if (e->type == H_THREAD) { sig = ((GThread *)e->obj)->exited; }
                if (sig) { pthread_mutex_unlock(&g_big); r[0] = KR_OK; r[1] = (uint32_t)i; return; }
            }
            if (to == 0) { pthread_mutex_unlock(&g_big); r[0] = KR_TIMEOUT; return; }
            if (to < 0) pthread_cond_wait(&g_big_cv, &g_big);
            else if (pthread_cond_timedwait(&g_big_cv, &g_big, &ts) == ETIMEDOUT) { pthread_mutex_unlock(&g_big); r[0] = KR_TIMEOUT; return; }
        }
    }
    case 0x19: {                                   // CancelSynchronization(thread): its WaitSynchronization (now or the next one) returns Cancelled
        GThread *t = thread_of(r[0]); if (!t) { r[0] = KR_INVALID_HANDLE; return; }
        pthread_mutex_lock(&g_big); t->cancel_sync = 1; pthread_cond_broadcast(&g_big_cv); pthread_mutex_unlock(&g_big);
        r[0] = KR_OK; return;
    }
    case 0x1a: if ((r[0] & ~WAIT_MASK) == r[2]) { LOG("recursive lock of %#x by thread %u", r[1], t_self->id); dump_regs(f); } r[0] = arbitrate_lock(r[0], r[1], r[2]); return;
    case 0x1b: r[0] = arbitrate_unlock(r[0]); return;
    case 0x1c: r[0] = wait_key_atomic(r[0], r[1], r[2], (int64_t)(((uint64_t)r[4] << 32) | r[3])); return;
    case 0x1d: signal_key(r[0], (int32_t)r[1]); r[0] = KR_OK; return;
    case 0x1f: LOG("ConnectToNamedPort \"%s\"", gstr(r[1])); r[0] = a32_connect_port(gstr(r[1]), &r[1]); return;
    case 0x21: r[0] = a32_send_sync(r[0]); return;
    case 0x25: { GThread *t = thread_of(r[1]); if (!t) { r[0] = KR_INVALID_HANDLE; return; } r[0] = KR_OK; set64(f, 1, t->id); return; }
    case 0x26: LOG("Break(reason %#x, addr %#x, size %#x) from %s", r[0], r[1], r[2], where(r[14])); dump_regs(f); fflush(stderr); _exit(5);
    case 0x27: fprintf(stderr, "[guest] %.*s\n", (int)r[1], gstr(r[0])); r[0] = KR_OK; return;
    case 0x29: { uint32_t res; uint64_t v = get_info(r[1], r[2], ((uint64_t)r[3] << 32) | r[0], &res); r[0] = res; set64(f, 1, v); return; }
    case 0x2a: r[0] = KR_OK; return;                // FlushEntireDataCache
    case 0x32: r[0] = KR_OK; return;                // SetThreadActivity
    case 0x34: {                                   // WaitForAddress(r0 = addr, r1 = type, r2 = value, r3:r4 = timeout): types 0 = wait if equal, 1 = decrement-and-wait, 2 = modify
        uint32_t addr = r[0], type = r[1], val = r[2]; int64_t to = (int64_t)(((uint64_t)r[4] << 32) | r[3]);
        pthread_mutex_lock(&g_big);
        uint32_t cur = __atomic_load_n(gptr(addr), __ATOMIC_ACQUIRE);
        if (type == 1) { if ((int32_t)cur < (int32_t)val) { pthread_mutex_unlock(&g_big); r[0] = 0xd001; return; } }
        if (cur != val && type == 0) { pthread_mutex_unlock(&g_big); r[0] = KR_INVALID_STATE; return; }
        GThread *me = t_self; me->wait_addr = addr; me->waiting_lock = 2; me->woken = 0;
        struct timespec ts; if (to > 0) ts = deadline_after((uint64_t)to);
        int tmo = 0;
        while (!me->woken && !tmo) {
            if (to == 0) { tmo = 1; break; }
            if (to < 0) pthread_cond_wait(&g_big_cv, &g_big); else if (pthread_cond_timedwait(&g_big_cv, &g_big, &ts) == ETIMEDOUT) tmo = 1;
        }
        me->waiting_lock = 0; pthread_mutex_unlock(&g_big);
        r[0] = tmo ? KR_TIMEOUT : KR_OK; return;
    }
    case 0x35: {                                   // SignalToAddress(r0 = addr, r1 = type, r2 = value, r3 = count)
        uint32_t addr = r[0]; int32_t count = (int32_t)r[3];
        pthread_mutex_lock(&g_big);
        if (r[1] == 1) __atomic_add_fetch(gptr(addr), 1, __ATOMIC_SEQ_CST);
        int n = 0;
        for (GThread *t = g_threads; t && (count <= 0 || n < count); t = t->next) if (t->waiting_lock == 2 && t->wait_addr == addr && !t->woken) { t->woken = 1; n++; }
        pthread_cond_broadcast(&g_big_cv); pthread_mutex_unlock(&g_big);
        r[0] = KR_OK; return;
    }
    case 0x13: {                                   // MapSharedMemory(handle, addr, size, perm): the guest range becomes a view of the same pages the host writes
        HEnt *e = handle_get(r[0]); if (!e || e->type != H_SHMEM) { r[0] = KR_INVALID_HANDLE; return; }
        ShMem *sm = e->obj; if (r[2] > sm->size || mmap(g_mem + r[1], r[2], PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, sm->fd, 0) == MAP_FAILED) { r[0] = 0xcc01; return; }
        r[0] = KR_OK; return;
    }
    case 0x14: {                                   // UnmapSharedMemory(handle, addr, size): back to plain anonymous memory
        mmap(g_mem + r[1], r[2], PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0); r[0] = KR_OK; return;
    }
    case 0x15: {                                   // CreateTransferMemory(r1 = addr, r2 = size, r3 = perm) -> r1 = handle (the memory stays where it is: the host reads it in place)
        ShMem *tm = calloc(1, sizeof *tm); tm->fd = -1; tm->size = r[2];
        pthread_mutex_lock(&g_big); uint32_t h = handle_new(H_TRANSFER, tm); pthread_mutex_unlock(&g_big);
        r[0] = KR_OK; r[1] = h; return;
    }
    case 0x45: {                                   // CreateEvent -> r1 = writable, r2 = readable
        Event *ev = calloc(1, sizeof *ev);
        pthread_mutex_lock(&g_big); uint32_t w = handle_new(H_EVENT_W, ev), rd = handle_new(H_EVENT_R, ev); pthread_mutex_unlock(&g_big);
        r[0] = KR_OK; r[1] = w; r[2] = rd; return;
    }
    default:
        if (!g_unimpl_svc_seen[num & 0xff]) { g_unimpl_svc_seen[num & 0xff] = 1; LOG("[unimplemented svc %#x] from %s (returns success)", num, where(r[14])); }
        r[0] = KR_OK; return;
    }
}

void a32_region_update_heap(void) {
    for (int i = 0; i < g_nreg; i++) if (g_reg[i].start == HEAP_BASE) { g_reg[i].size = g_heap_size; return; }
    a32_region_add(HEAP_BASE, g_heap_size, 5, 3);
}
uint32_t a32_cur_tls(void) { return t_self->tls; }

extern __thread const char *a32_cur_iface;
uint32_t a32_new_event_handle(void) {
    Event *ev = calloc(1, sizeof *ev);
    pthread_mutex_lock(&g_big); uint32_t h = handle_new(H_EVENT_R, ev); pthread_mutex_unlock(&g_big);
    if (g_trace >= 3) LOG("event handle %#x created for %s", h, a32_cur_iface ? a32_cur_iface : "?");
    return h;
}
void a32_signal_event(uint32_t handle) {
    HEnt *e = handle_get(handle); if (!e) return; Event *ev = e->obj;
    pthread_mutex_lock(&g_big); ev->signaled = 1; pthread_cond_broadcast(&g_big_cv); pthread_mutex_unlock(&g_big);
}
void a32_clear_event(uint32_t handle) {
    HEnt *e = handle_get(handle); if (!e) return; Event *ev = e->obj;
    pthread_mutex_lock(&g_big); ev->signaled = 0; pthread_mutex_unlock(&g_big);
}
uint32_t a32_main_handle(void) { return g_main_thread.handle; }

// a shared-memory object the host also maps (hid's shared memory, ...): returns the kernel handle, *host gets the host's view
uint32_t a32_new_shmem_handle(uint32_t size, uint8_t **host) {
    char path[] = "/tmp/a32shm.XXXXXX"; int fd = mkstemp(path); if (fd < 0) die("mkstemp"); unlink(path);
    if (ftruncate(fd, size)) die("ftruncate"); 
    ShMem *sm = calloc(1, sizeof *sm); sm->fd = fd; sm->size = size;
    *host = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); if (*host == MAP_FAILED) die("mmap shared memory");
    pthread_mutex_lock(&g_big); uint32_t h = handle_new(H_SHMEM, sm); pthread_mutex_unlock(&g_big); return h;
}

uint32_t a32_cb_sp(void) { return t_self ? t_self->cb_sp : 0; }
// a thread of the host that can call guest code (a32_callback): its own TLS block and guest stack
extern uint32_t a32_guest_alloc(uint32_t size, uint32_t align);
typedef struct { void (*fn)(void *); void *arg; GThread *t; } HelperStart;
static void *helper_main(void *p) { HelperStart *h = p; t_self = h->t; h->fn(h->arg); return NULL; }
void *a32_spawn_guest_thread(void (*fn)(void *), void *arg, size_t stack) {
    GThread *t = calloc(1, sizeof *t);
    pthread_mutex_lock(&g_big); t->id = g_next_tid++; t->next = g_threads; g_threads = t; pthread_mutex_unlock(&g_big);
    t->tls = alloc_tls(t->id); t->ctx.mem = g_mem; t->ctx.tls = t->tls; t->started = 1;
    uint32_t base = a32_guest_alloc((uint32_t)stack, 16); t->stack_top = base + (uint32_t)stack; t->cb_sp = t->stack_top;
    HelperStart *h = malloc(sizeof *h); h->fn = fn; h->arg = arg; h->t = t;
    pthread_t th; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 1u << 20);
    if (pthread_create(&th, &a, helper_main, h)) die("cannot create a helper thread");
    pthread_detach(th); return t;
}
