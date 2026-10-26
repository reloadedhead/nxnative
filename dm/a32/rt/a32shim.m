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
void *front_metal_layer(void) { return NULL; }                           // headless for now

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
