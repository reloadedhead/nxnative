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
        static int trace = -1; if (trace < 0) trace = getenv("A32_FS_TRACE") != NULL;
        if (trace && off >= g_rom.data_off) LOG("fs: IStorage::Read data offset %#llx length %#llx", (unsigned long long)off, (unsigned long long)len);
        romfs_read(off, g_mem + c->b[0].addr, len); return 0;
    }
    case 4: romfs_load(); ipc_put64(c, g_rom.total); return 0;
    case 2: return 0;
    default: LOG("fs: IStorage command %u (%u bytes in) not implemented", cmd, in_sz); return 0;
    }
}

// ---- save data: an IFileSystem over a host directory (A32_SAVE_DIR, default <build>/save) ----
#include <dirent.h>
typedef struct { char root[1024]; } HostFs;
static const Buf *path_buf(const Ipc *c, int k) { if (k < c->nx && c->x[k].addr) return &c->x[k]; if (k < c->na && c->a[k].addr) return &c->a[k]; return NULL; }
static int host_path(const HostFs *fs, const Ipc *c, int k, char *out, size_t cap) {
    const Buf *b = path_buf(c, k); if (!b) return 0x2ee;
    const char *p = (const char *)g_mem + b->addr; size_t n = strnlen(p, b->size < 0x301 ? b->size : 0x301);
    char tmp[0x302]; memcpy(tmp, p, n); tmp[n] = 0;
    if (strstr(tmp, "..")) return 0x2ee;
    snprintf(out, cap, "%s%s%s", fs->root, tmp[0] == '/' ? "" : "/", tmp);
    size_t l = strlen(out); while (l > strlen(fs->root) + 1 && out[l - 1] == '/') out[--l] = 0;
    return 0;
}
static int errno_result(void) { return errno == ENOENT ? 0x202 : errno == EEXIST ? 0x402 : errno == ENOTEMPTY ? 0x1002 : 0x2ee; }
typedef struct { int fd; } HFile;
static int hfile_cmd(Obj *o, uint32_t cmd, const uint8_t *in, uint32_t in_sz, Ipc *c) {
    HFile *f = o->data; (void)in_sz;
    switch (cmd) {
    case 0: {                                                                      // Read(option, offset, size) -> bytes read
        uint64_t off, sz; memcpy(&off, in + 8, 8); memcpy(&sz, in + 16, 8);
        Buf ob = c->nb && c->b[0].addr ? c->b[0] : c->nc ? c->rc[0] : (Buf){0, 0};
        if (!ob.addr) return 0x2ee;
        if (sz > ob.size) sz = ob.size;
        ssize_t r = pread(f->fd, g_mem + ob.addr, sz, (off_t)off); if (r < 0) r = 0; ipc_put64(c, (uint64_t)r); return 0;
    }
    case 1: {                                                                      // Write(option, offset, size) from an input buffer
        uint64_t off, sz; memcpy(&off, in + 8, 8); memcpy(&sz, in + 16, 8);
        const Buf *ib = path_buf(c, 0); if (!ib) return 0x2ee;
        if (sz > ib->size) sz = ib->size;
        if (pwrite(f->fd, g_mem + ib->addr, sz, (off_t)off) < 0) return errno_result();
        return 0;
    }
    case 2: fsync(f->fd); return 0;
    case 3: { uint64_t sz; memcpy(&sz, in, 8); return ftruncate(f->fd, (off_t)sz) ? errno_result() : 0; }
    case 4: { struct stat st; fstat(f->fd, &st); ipc_put64(c, (uint64_t)st.st_size); return 0; }
    case 5: ipc_pad(c, 0x10); return 0;                                            // OperateRange
    default: LOG("fs: IFile command %u not implemented: success", cmd); return 0;
    }
}
typedef struct { char path[1024]; } HDir;
static int hdir_cmd(Obj *o, uint32_t cmd, const uint8_t *in, uint32_t in_sz, Ipc *c) {
    HDir *d = o->data; (void)in; (void)in_sz;
    if (cmd == 0 || cmd == 1) {
        DIR *dp = opendir(d->path); if (!dp) return errno_result();
        uint64_t n = 0, cap = 0; Buf ob = {0, 0};
        if (cmd == 0) { ob = c->nb && c->b[0].addr ? c->b[0] : c->nc ? c->rc[0] : ob; cap = ob.size / 0x310; }
        static __thread unsigned skip; (void)skip;
        struct dirent *e;
        while ((e = readdir(dp))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            if (cmd == 0 && n < cap) {
                uint8_t *ent = g_mem + ob.addr + n * 0x310; memset(ent, 0, 0x310); strncpy((char *)ent, e->d_name, 0x300);
                char full[2200]; snprintf(full, sizeof full, "%s/%s", d->path, e->d_name); struct stat st; stat(full, &st);
                ent[0x304] = S_ISDIR(st.st_mode) ? 0 : 1; int64_t sz = S_ISDIR(st.st_mode) ? 0 : st.st_size; memcpy(ent + 0x308, &sz, 8);
            }
            n++;
        }
        closedir(dp);
        ipc_put64(c, cmd == 0 ? (n < cap ? n : cap) : n); return 0;                // (a single Read returns up to the buffer; the rest on the next call is not tracked: save dirs are small)
    }
    return 0;
}
static int hfs_cmd(Obj *o, uint32_t cmd, const uint8_t *in, uint32_t in_sz, Ipc *c) {
    HostFs *fs = o->data; char p[2200], q[2200]; int r; (void)in_sz;
    switch (cmd) {
    case 0: { uint64_t sz; memcpy(&sz, in + 8, 8); if ((r = host_path(fs, c, 0, p, sizeof p))) return r;
              int fd = open(p, O_CREAT | O_EXCL | O_WRONLY, 0644); if (fd < 0) return errno_result(); if (sz) ftruncate(fd, (off_t)sz); close(fd); return 0; }
    case 1: if ((r = host_path(fs, c, 0, p, sizeof p))) return r; return unlink(p) ? errno_result() : 0;
    case 2: if ((r = host_path(fs, c, 0, p, sizeof p))) return r; return mkdir(p, 0755) ? errno_result() : 0;
    case 3: if ((r = host_path(fs, c, 0, p, sizeof p))) return r; return rmdir(p) ? errno_result() : 0;
    case 4: case 13: if ((r = host_path(fs, c, 0, p, sizeof p))) return r; { char cmdline[2500]; snprintf(cmdline, sizeof cmdline, cmd == 4 ? "rm -rf '%s'" : "rm -rf '%s'/*", p); return system(cmdline) ? 0x2ee : 0; }
    case 5: case 6: if ((r = host_path(fs, c, 0, p, sizeof p)) || (r = host_path(fs, c, 1, q, sizeof q))) return r; return rename(p, q) ? errno_result() : 0;
    case 7: { if ((r = host_path(fs, c, 0, p, sizeof p))) return r; struct stat st; if (stat(p, &st)) return errno_result(); ipc_put32(c, S_ISDIR(st.st_mode) ? 0 : 1); return 0; }
    case 8: { if ((r = host_path(fs, c, 0, p, sizeof p))) return r; uint32_t mode; memcpy(&mode, in, 4);
              int fd = open(p, (mode & 2) ? O_RDWR : O_RDONLY); if (fd < 0 && (mode & 2) && errno != ENOENT) fd = open(p, O_RDONLY); if (fd < 0) return errno_result();
              HFile *f = calloc(1, sizeof *f); f->fd = fd; ipc_out_obj(c, obj_new("IFile", hfile_cmd, f)); return 0; }
    case 9: { if ((r = host_path(fs, c, 0, p, sizeof p))) return r; struct stat st; if (stat(p, &st) || !S_ISDIR(st.st_mode)) return errno_result();
              HDir *d = calloc(1, sizeof *d); snprintf(d->path, sizeof d->path, "%s", p); ipc_out_obj(c, obj_new("IDirectory", hdir_cmd, d)); return 0; }
    case 10: return 0;
    case 11: case 12: ipc_put64(c, 0x40000000ull); return 0;
    case 14: ipc_pad(c, 0x20); return 0;
    default: LOG("fs: IFileSystem command %u not implemented: success, zeroed output", cmd); return 0;
    }
}
static Obj *make_save_fs(void) {
    HostFs *fs = calloc(1, sizeof *fs); const char *d = getenv("A32_SAVE_DIR");
    if (d) snprintf(fs->root, sizeof fs->root, "%s", d); else snprintf(fs->root, sizeof fs->root, "%s/save", g_build_dir);
    mkdir(fs->root, 0755); return obj_new("IFileSystem", hfs_cmd, fs);
}

// fsp-srv (IFileSystemProxy): 1 SetCurrentProcess, 200 OpenDataStorageByCurrentProcess -> IStorage, 1005 GetGlobalAccessLogMode
static int fsp_cmd(Obj *o, uint32_t cmd, const uint8_t *in, uint32_t in_sz, Ipc *c) {
    (void)in;
    switch (cmd) {
    case 1: return 0;
    case 51: case 52: case 53: ipc_out_obj(c, make_save_fs()); return 0;                     // OpenSaveDataFileSystem (and by system id, read-only): one save directory
    case 200: ipc_out_obj(c, obj_new("IStorage", storage_cmd, NULL)); return 0;
    case 1005: ipc_put32(c, 0); return 0;
    case 202: LOG("fs: OpenDataStorageByDataId: no such system data (ResultTargetNotFound)"); return 0x202;
    default: LOG("fs: fsp-srv command %u (%u bytes in) not implemented: success, zeroed output", cmd, in_sz); return 0;
    }
}
Obj *a32_make_fsp(void) { return obj_new("fsp-srv", fsp_cmd, NULL); }
