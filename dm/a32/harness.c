// Test harness of dm/a32/difftest.py: runs each lifted AArch32 instruction (generated AArch64 test functions) natively on a
// register / memory state and writes the final state. Guest memory: a 4 GB reservation whose first 64 KB are real.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <zlib.h>

typedef struct { uint32_t r[15]; uint32_t nzcv; uint32_t pc_out; uint32_t memcrc; uint32_t status; uint32_t fpscr; uint8_t q[16][16]; } St;   // q at +80
typedef void (*TestFn)(St *, uint8_t *);
extern const TestFn tests[];
#define MEM 0x10000
#define ADDR 0x8000

static sigjmp_buf jb;
static void on_fault(int sig) { siglongjmp(jb, 1); }

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: harness tests.bin mem.bin out.bin count\n"); return 2; }
    int n = atoi(argv[4]);
    FILE *ft = fopen(argv[1], "rb"), *fm = fopen(argv[2], "rb"), *fo = fopen(argv[3], "wb");
    if (!ft || !fm || !fo) { perror("open"); return 2; }
    uint8_t *img = malloc(MEM);
    if (fread(img, 1, MEM, fm) != MEM) { fprintf(stderr, "short mem image\n"); return 2; }
    uint8_t *base = mmap(NULL, (1ULL << 32) + 0x4000, PROT_NONE, MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED || mprotect(base, MEM, PROT_READ | PROT_WRITE)) { perror("mmap"); return 2; }
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_fault; sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    for (int i = 0; i < n; i++) {
        St s; memset(&s, 0, sizeof s);
        uint32_t in[18]; uint8_t qin[256];
        if (fread(in, 4, 18, ft) != 18 || fread(qin, 1, 256, ft) != 256) { fprintf(stderr, "short tests file\n"); return 2; }
        memcpy(s.r, in, 60); s.nzcv = in[15]; s.pc_out = 0xdeadbeef; s.fpscr = in[17]; memcpy(s.q, qin, 256);
        memcpy(base, img, MEM); memcpy(base + ADDR, &in[16], 4);   // the instruction itself sits in guest memory (pc-relative loads)
        if (!sigsetjmp(jb, 1)) { tests[i](&s, base); s.status = 0; } else s.status = 1;
        s.memcrc = crc32(0, base, MEM);
        fwrite(&s, sizeof s, 1, fo);
    }
    fclose(fo);
    return 0;
}

// symbols the lifted code of some instruction words refers to (the runtime provides them in the game)
unsigned long long a32_tick_ctl[2] __attribute__((aligned(16))) = {1, 0};
void a32_svc_gate(void) {}
