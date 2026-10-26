// fsp-srv (the file system proxy) for the 32-bit guest: the game's RomFS as an IStorage in the real RomFS binary format, so the SDK's own RomFS parser
// (nn::fs::RomFsFileSystem) runs unchanged. The header and tables come from dm/a32/mkromfs.py; file data is read from the extracted files.
#define _DARWIN_C_SOURCE
#include "a32ipc.h"
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

typedef struct { uint64_t off, size; char *path; } RFile;
typedef struct { uint8_t *tables; uint64_t tsize, data_off, total; RFile *files; int nfiles; char root[1024]; int fd; int fd_idx; } Romfs;
static Romfs g_rom; static int g_rom_loaded;
extern const char *g_build_dir;

static void romfs_load(void) {
    if (g_rom_loaded) return;
    g_rom_loaded = 1;
    char p[1200], line[2048];
    snprintf(p, sizeof p, "%s/romfs/romfs.tables", g_build_dir);
    FILE *f = fopen(p, "rb"); if (!f) die("cannot open %s (run mkromfs.py)", p);
    fseek(f, 0, SEEK_END); g_rom.tsize = (uint64_t)ftell(f); fseek(f, 0, SEEK_SET);
    g_rom.tables = malloc(g_rom.tsize); if (fread(g_rom.tables, 1, g_rom.tsize, f) != g_rom.tsize) die("short read of romfs.tables"); fclose(f);
    const char *root = getenv("A32_ROMFS_DIR");
    if (!root) die("set A32_ROMFS_DIR to the extracted RomFS directory");
    snprintf(g_rom.root, sizeof g_rom.root, "%s", root);
    snprintf(p, sizeof p, "%s/romfs/romfs_files.tsv", g_build_dir);
    f = fopen(p, "r"); if (!f) die("cannot open %s", p);
    unsigned long long a, b; if (!fgets(line, sizeof line, f) || sscanf(line, "%llu %llu", &a, &b) != 2) die("bad romfs_files.tsv");
    g_rom.data_off = a; g_rom.total = a + b;
    int cap = 1024; g_rom.files = malloc(cap * sizeof *g_rom.files);
    while (fgets(line, sizeof line, f)) {
        unsigned long long o, s; int n = 0; if (sscanf(line, "%llu\t%llu\t%n", &o, &s, &n) < 2 || !n) continue;
        char *path = line + n; path[strcspn(path, "\n")] = 0;
        if (g_rom.nfiles == cap) { cap *= 2; g_rom.files = realloc(g_rom.files, cap * sizeof *g_rom.files); }
        g_rom.files[g_rom.nfiles++] = (RFile){o, s, strdup(path)};
    }
    fclose(f); g_rom.fd = -1; g_rom.fd_idx = -1;
    LOG("fs: RomFS image: %d files, %llu bytes of data, tables %llu bytes", g_rom.nfiles, (unsigned long long)(g_rom.total - g_rom.data_off), (unsigned long long)g_rom.tsize);
}

static pthread_mutex_t g_rom_mu = PTHREAD_MUTEX_INITIALIZER;
static void romfs_read(uint64_t off, uint8_t *dst, uint64_t size) {
    romfs_load();
    pthread_mutex_lock(&g_rom_mu);
    memset(dst, 0, size);
    uint64_t pos = off, end = off + size;
    if (pos < g_rom.data_off) {                                         // header and tables
        uint64_t n = (g_rom.data_off < end ? g_rom.data_off : end) - pos;
        if (pos < g_rom.tsize) { uint64_t m = g_rom.tsize - pos < n ? g_rom.tsize - pos : n; memcpy(dst, g_rom.tables + pos, m); }
        pos += n;
    }
    while (pos < end) {                                                 // file data: find the file holding pos (files are in ascending offset order)
        int lo = 0, hi = g_rom.nfiles - 1, hit = -1;
        while (lo <= hi) { int mid = (lo + hi) / 2; if (g_rom.files[mid].off <= pos) { hit = mid; lo = mid + 1; } else hi = mid - 1; }
        if (hit < 0) break;
        RFile *fl = &g_rom.files[hit];
        uint64_t fend = fl->off + fl->size, nxt = hit + 1 < g_rom.nfiles ? g_rom.files[hit + 1].off : g_rom.total;
        uint64_t n = (nxt < end ? nxt : end) - pos;
        if (pos < fend) {
            uint64_t m = (fend < end ? fend : end) - pos;
            if (g_rom.fd_idx != hit) {
                if (g_rom.fd >= 0) close(g_rom.fd);
                char p[2400]; snprintf(p, sizeof p, "%s/%s", g_rom.root, fl->path);
                g_rom.fd = open(p, O_RDONLY); g_rom.fd_idx = hit;
                if (g_rom.fd < 0) { LOG("fs: cannot open %s: %s", p, strerror(errno)); }
            }
            if (g_rom.fd >= 0) { ssize_t r = pread(g_rom.fd, dst + (pos - off), m, (off_t)(pos - fl->off)); (void)r; }
        }
        pos += n;
    }
    pthread_mutex_unlock(&g_rom_mu);
}

// IStorage: 0 Read(offset, length) -> buffer, 1 Write, 2 Flush, 3 SetSize, 4 GetSize -> u64
static int storage_cmd(Obj *o, uint32_t cmd, const uint8_t *in, uint32_t in_sz, Ipc *c) {
    (void)o;
    switch (cmd) {
    case 0: {
        uint64_t off, len; memcpy(&off, in, 8); memcpy(&len, in + 8, 8);
        if (c->nb < 1) { LOG("fs: IStorage::Read without an output buffer"); return 0x2ea; }
        if (len > c->b[0].size) len = c->b[0].size;
        romfs_read(off, g_mem + c->b[0].addr, len); return 0;
    }
    case 4: romfs_load(); ipc_put64(c, g_rom.total); return 0;
    case 2: return 0;
    default: LOG("fs: IStorage command %u (%u bytes in) not implemented", cmd, in_sz); return 0;
    }
}

// fsp-srv (IFileSystemProxy): 1 SetCurrentProcess, 200 OpenDataStorageByCurrentProcess -> IStorage, 1005 GetGlobalAccessLogMode
static int fsp_cmd(Obj *o, uint32_t cmd, const uint8_t *in, uint32_t in_sz, Ipc *c) {
    (void)in;
    switch (cmd) {
    case 1: return 0;
    case 200: ipc_out_obj(c, obj_new("IStorage", storage_cmd, NULL)); return 0;
    case 1005: ipc_put32(c, 0); return 0;
    case 202: LOG("fs: OpenDataStorageByDataId: no such system data (ResultTargetNotFound)"); return 0x202;
    default: LOG("fs: fsp-srv command %u (%u bytes in) not implemented: success, zeroed output", cmd, in_sz); return 0;
    }
}
Obj *a32_make_fsp(void) { return obj_new("fsp-srv", fsp_cmd, NULL); }
