// Shared declarations of the AArch32 recompiler host (a32host.c, a32svc.c) and the runtime glue (a32rt.S).
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>

#define HOOK_BASE 0xF0000000u
// guest address space map (the 4 GB window)
#define HEAP_BASE 0x40000000u
#define HEAP_REGION_SIZE 0x80000000u
#define ALIAS_BASE 0xc0000000u
#define ALIAS_SIZE 0x10000000u
#define STACK_REGION_BASE 0xd0000000u
#define STACK_REGION_SIZE 0x10000000u
#define ASLR_BASE 0x00200000u
#define ASLR_SIZE 0x3fe00000u
#define TOTAL_MEMORY 0x82000000u

typedef struct { uint32_t r[15], nzcv, next, pad[3]; uint8_t q[16][16]; uint64_t x29, x30; } Frame;
_Static_assert(sizeof(Frame) == 352, "frame layout is shared with a32rt.S");
typedef struct { uint64_t host_sp; uint32_t r0, r1; uint8_t *mem; uint32_t tls, tpidrrw; } ThreadCtx;
typedef struct { uint32_t lo, size; const void *slots; } ModEnt;
extern const ModEnt a32_modtab[];
uint32_t a32_enter(ThreadCtx *ctx, const uint32_t *regs, uint32_t pc);

extern uint8_t *g_mem; extern int g_trace; extern uint32_t g_heap_size;
extern uint8_t g_unimpl_svc_seen[256];
void die(const char *fmt, ...) __attribute__((noreturn));
#define LOG(...) do { fprintf(stderr, "[a32] " __VA_ARGS__); fputc('\n', stderr); } while (0)
static inline uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, g_mem + a, 4); return v; }
static inline void wr32(uint32_t a, uint32_t v) { memcpy(g_mem + a, &v, 4); }
static inline const char *gstr(uint32_t a) { return (const char *)(g_mem + a); }
const char *where(uint32_t addr);
void dump_regs(const Frame *f); void dump_strings(const Frame *f);

void a32_svc_c(uint32_t num, Frame *f);
struct GThread; typedef struct GThread GThread;
GThread *a32_main_thread_init(uint32_t stack_top);
ThreadCtx *a32_main_ctx(void);
uint32_t a32_main_handle(void);
void a32_region_add(uint32_t start, uint32_t size, uint32_t state, uint32_t perm);
void a32_region_update_heap(void);
uint32_t a32_connect_port(const char *name, uint32_t *handle_out);
uint32_t a32_send_sync(uint32_t handle);
uint64_t a32_system_tick(void);
