// Host symbols the 64-bit NVN-on-Metal implementation (dm/gfx/nvn) expects from the 64-bit host (dm/host), provided for the 32-bit host.
#import <Foundation/Foundation.h>
#import <QuartzCore/CAMetalLayer.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

uint64_t now_ns(void) { static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb); return mach_absolute_time() * tb.numer / tb.denom; }
uint64_t g_t0_ns;
_Atomic uint64_t dm_present_ns;
__attribute__((constructor)) static void t0_init(void) { g_t0_ns = now_ns(); }

void crash_set_wait_dump(void (*fn)(void)) { (void)fn; }
void dm_backtrace(uint64_t fp) { (void)fp; }
void dm_res_report_register(void (*fn)(uint32_t, const char *)) { (void)fn; }
void *module_of(const void *p) { (void)p; return NULL; }                 // no 64-bit guest modules
// the window (dm/host/front_mac.m, linked in) calls these: the controller state it collects is kept here for the hid service
uint64_t a32_pad_buttons; int32_t a32_pad_axes[4];
void dm_focus_set(int st) { (void)st; }
void dm_haptics_apple_init(void) {}
void dm_haptics_apple_controller(int player, void *controller) { (void)player; (void)controller; }
void dm_input_set(uint64_t b, int32_t lx, int32_t ly, int32_t rx, int32_t ry) { a32_pad_buttons = b; a32_pad_axes[0] = lx; a32_pad_axes[1] = ly; a32_pad_axes[2] = rx; a32_pad_axes[3] = ry; }
void dm_pad_set(uint64_t b, int32_t lx, int32_t ly, int32_t rx, int32_t ry) { dm_input_set(b, lx, ly, rx, ry); }
void dm_pad_set_player(int p, uint64_t b, int32_t lx, int32_t ly, int32_t rx, int32_t ry) { if (p == 0) dm_input_set(b, lx, ly, rx, ry); }
void dm_pad_players(uint32_t mask) { (void)mask; }
uint32_t dm_player_npad(int p) { return p == 0 ? 0x20 : (uint32_t)(p - 1); }
void dm_touch_set(const void *t, int n) { (void)t; (void)n; }
int g_swkbd_min_chars;

// page-fault hook (texture dirty tracking write-protects pages) and the "about to write guest memory from the host" hook
static int (*g_fault)(void *, int, void *);
static void (*g_prep[4])(void *, size_t); static int g_nprep;
void dm_fault_hook_register(int (*fn)(void *addr, int is_write, void *uc)) { g_fault = fn; }
void dm_guest_write_prep_register(void (*fn)(void *addr, size_t len)) { if (g_nprep < 4) g_prep[g_nprep++] = fn; }
void dm_guest_write_prepare(void *addr, size_t len) { for (int i = 0; i < g_nprep; i++) g_prep[i](addr, len); }
int a32_gfx_fault(void *addr, int is_write, void *uc) { return g_fault ? g_fault(addr, is_write, uc) : 0; }

// the entry-point thunks of the 64-bit host are not used: the 32-bit host calls the implementations through adapters
void nvn_big_entry(void) {}
void nvn_prof_entry(void) {}
void nvn_unimpl_entry(void) {}

// what dm/host/hle_audio.c (the host's PCM output, linked as it is) asks of the 64-bit host
#include <pthread.h>
static struct { const void *k; void *v; } g_objmap[64]; static pthread_mutex_t g_objmu = PTHREAD_MUTEX_INITIALIZER;
void *objmap_get(const void *g) { void *r = NULL; pthread_mutex_lock(&g_objmu); for (int i = 0; i < 64; i++) if (g_objmap[i].k == g) r = g_objmap[i].v; pthread_mutex_unlock(&g_objmu); return r; }
void objmap_put(const void *g, void *h) { pthread_mutex_lock(&g_objmu); for (int i = 0; i < 64; i++) if (!g_objmap[i].k) { g_objmap[i].k = g; g_objmap[i].v = h; break; } pthread_mutex_unlock(&g_objmu); }
void *objmap_del(const void *g) { void *r = NULL; pthread_mutex_lock(&g_objmu); for (int i = 0; i < 64; i++) if (g_objmap[i].k == g) { r = g_objmap[i].v; g_objmap[i].k = NULL; } pthread_mutex_unlock(&g_objmu); return r; }
void hle_bind_system_event(void *ev, int clear) { (void)ev; (void)clear; }
void hle_signal_system_event(void *ev) { (void)ev; }
void dm_host_helper_qos(int q) { (void)q; }


// Turbo: while the key is held the guest's system counter runs faster (debugging aid for slow stretches of the game). The counter is real * ctl[0] + ctl[1] and the pair is replaced in one store
// with ctl[1] adjusted by the real counter, so the guest sees no jump when the speed changes.
extern uint64_t a32_tick_ctl[2];
void dm_turbo_set(int on) {
    static int cur; if (on == cur) return; cur = on;
    uint64_t cnt, frq; __asm__ volatile("mrs %0, cntvct_el0" : "=r"(cnt)); __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
    uint64_t q = cnt / frq, r = cnt % frq, real = q * 19200000ull + r * 19200000ull / frq;
    uint64_t s0 = a32_tick_ctl[0], o0 = a32_tick_ctl[1], s1 = on ? (getenv("A32_TURBO") ? (uint64_t)atoll(getenv("A32_TURBO")) : 8) : 1, o1 = o0 + real * (s0 - s1);
    __asm__ volatile("stp %0, %1, [%2]" :: "r"(s1), "r"(o1), "r"(a32_tick_ctl) : "memory");
}
