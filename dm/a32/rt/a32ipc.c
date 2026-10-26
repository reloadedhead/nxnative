// HIPC / CMIF inter-process communication of the 32-bit guest, served in the host: ConnectToNamedPort and SendSyncRequest on host sessions.
// The guest's message sits in its thread-local region (offset 0, 0x100 bytes). Message layout (Atmosphère / libnx): word 0 = type (low 16 bits)
// and the counts of X / A / B / W buffer descriptors; word 1 = raw data size in words, receive-list flags, the special-header flag (bit 31);
// then the special header (process id flag, copy and move handle counts) with its handles, the descriptors, and the raw data, which starts 16-byte
// aligned from the start of the message. Raw data of a request = {"SFCI", version, command, token} and the arguments; of a response = {"SFCO", 0, result, 0}
// and the results. A domain session has a 16-byte domain header in front. Interfaces are tables of command handlers; a service the host has no
// handler for answers every command with success and zeroed output (the log names each command once).
#define _DARWIN_C_SOURCE
#include "a32host.h"
#include "a32ipc.h"

static pthread_mutex_t g_ipc_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_ipc_trace = -1;
__thread const char *a32_cur_iface;                   // the interface of the request being served (for logging)

// ---- interface objects and sessions -----------------------------------------------------------------------------------------------
typedef struct Domain { Obj *obj[64]; } Domain;                       // shared by a session and its clones
typedef struct Session { Obj *root; Domain *domain; } Session;
#define MAX_SESS 512
static Session g_sess[MAX_SESS];
#define SESS_H0 0xe000u
static uint32_t sess_new(Obj *root) {
    pthread_mutex_lock(&g_ipc_mu);
    for (uint32_t i = 0; i < MAX_SESS; i++) if (!g_sess[i].root) { g_sess[i] = (Session){root, NULL}; pthread_mutex_unlock(&g_ipc_mu); return SESS_H0 + i; }
    pthread_mutex_unlock(&g_ipc_mu); die("out of sessions"); return 0;
}
static Session *sess_get(uint32_t h) { uint32_t i = h - SESS_H0; return h >= SESS_H0 && i < MAX_SESS && g_sess[i].root ? &g_sess[i] : NULL; }
int a32_is_session(uint32_t h) { return sess_get(h) != NULL; }
void a32_session_close(uint32_t h) { Session *s = sess_get(h); if (s) s->root = NULL; }
Obj *obj_new(const char *iface, CmdFn fn, void *data) { Obj *o = calloc(1, sizeof *o); o->iface = iface; o->fn = fn; o->data = data; return o; }

// ---- services -----------------------------------------------------------------------------------------------------------------------
typedef struct { const char *name; Obj *(*make)(void); } ServiceDef;
static int generic_cmd(Obj *o, uint32_t cmd, const uint8_t *in, uint32_t in_sz, Ipc *c) {
    (void)in; (void)in_sz; (void)c;
    LOG("ipc: %s command %u (%u bytes in): success with zeroed output (not implemented)", o->iface, cmd, in_sz);
    return 0;
}
extern const ServiceDef a32_services[];            // a32svcs.c
extern const unsigned a32_nservices;

static Obj *service_by_name(const char *name) {
    for (unsigned i = 0; i < a32_nservices; i++) if (!strncmp(a32_services[i].name, name, 8) && strlen(a32_services[i].name) == strnlen(name, 8)) return a32_services[i].make();
    char *n = malloc(32); snprintf(n, 32, "%.8s", name);
    LOG("ipc: service \"%s\" has no host implementation (generic: every command succeeds)", n);
    return obj_new(n, generic_cmd, NULL);
}

static int sm_cmd(Obj *o, uint32_t cmd, const uint8_t *in, uint32_t in_sz, Ipc *c) {
    (void)o;
    switch (cmd) {
    case 0: return 0;                                         // Initialize(pid)
    case 1: {                                                 // GetService(name[8]) -> object
        char name[9] = {0}; memcpy(name, in, in_sz < 8 ? in_sz : 8);
        LOG("ipc: sm GetService \"%s\"", name);
        Obj *s = service_by_name(name); ipc_out_obj(c, s); return 0;
    }
    case 2: return 0;                                         // RegisterService
    case 3: return 0;                                         // UnregisterService
    default: return generic_cmd(o, cmd, in, in_sz, c);
    }
}

uint32_t a32_connect_port(const char *name, uint32_t *handle_out) {
    if (!strcmp(name, "sm:")) { *handle_out = sess_new(obj_new("sm:", sm_cmd, NULL)); return 0; }
    *handle_out = 0; return 0xf201;
}

// ---- message handling ---------------------------------------------------------------------------------------------------------------
static void dump_words(const char *tag, const uint32_t *m, int n) {
    char buf[1024]; int k = 0;
    for (int i = 0; i < n && k < 1000; i++) k += snprintf(buf + k, sizeof buf - (size_t)k, "%08x ", m[i]);
    LOG("ipc: %s %s", tag, buf);
}

uint32_t a32_send_sync(uint32_t handle) {
    if (g_ipc_trace < 0) g_ipc_trace = getenv("A32_IPC") ? atoi(getenv("A32_IPC")) : 0;
    Session *s = sess_get(handle);
    if (!s) { LOG("ipc: SendSyncRequest on %#x: not a host session", handle); return 0xe401; }
    uint32_t *m = (uint32_t *)(g_mem + a32_cur_tls());
    if (g_ipc_trace) dump_words("request", m, 24);
    unsigned type = m[0] & 0xffff, nx = m[0] >> 16 & 15, na = m[0] >> 20 & 15, nb = m[0] >> 24 & 15, nw = m[0] >> 28 & 15;
    unsigned rawwords = m[1] & 0x3ff, recvflags = m[1] >> 10 & 15;
    uint32_t *p = m + 2; int send_pid = 0;
    if (m[1] >> 31) { uint32_t sh = *p++; send_pid = sh & 1; p += send_pid ? 2 : 0; p += (sh >> 1 & 15) + (sh >> 5 & 15); }
    Ipc c; memset(&c, 0, sizeof c);
    c.nx = (int)nx; c.na = (int)na; c.nb = (int)nb; c.nw = (int)nw;
    for (unsigned i = 0; i < nx; i++, p += 2) { c.x[i].size = p[0] >> 16; c.x[i].addr = p[1]; }
    for (unsigned i = 0; i < na; i++, p += 3) { c.a[i].size = p[0]; c.a[i].addr = p[1]; }
    for (unsigned i = 0; i < nb; i++, p += 3) { c.b[i].size = p[0]; c.b[i].addr = p[1]; }
    for (unsigned i = 0; i < nw; i++, p += 3) { c.w[i].size = p[0]; c.w[i].addr = p[1]; }
    (void)recvflags;
    uint8_t *base = (uint8_t *)m;
    size_t off = (size_t)((uint8_t *)p - base), raw = (off + 15) & ~(size_t)15;
    uint32_t *r = (uint32_t *)(base + raw);
    // receive list (C descriptors): {addr, size << 16 | addr_hi} pairs at the end of the raw area; found by shape (a guest address, then a size in the high half)
    if (recvflags >= 2) {
        unsigned want = recvflags == 2 ? 1 : recvflags - 2, end = (unsigned)(raw / 4) + rawwords;
        for (unsigned k = (unsigned)(raw / 4) + 4; k + 1 < end + 2 && k + 1 < 64 && (unsigned)c.nc < want; k++)
            if (m[k] >= 0x1000 && m[k] < 0xf0000000u && (m[k + 1] & 0xff80) == 0 && (m[k + 1] >> 16) > 0 && k + 1 > (unsigned)(raw / 4) + 4) { c.rc[c.nc].addr = m[k]; c.rc[c.nc].size = m[k + 1] >> 16; c.nc++; k++; }
    }
    if (type == 2) { s->root = NULL; return 0; }              // Close
    int dom = s->domain != NULL && (type == 4 || type == 6);
    unsigned dtype = 1, objid = 0, dsize = 0;
    if (dom) { dtype = base[raw]; objid = r[1]; dsize = (uint16_t)(base[raw + 2] | base[raw + 3] << 8); r += 4; }
    Obj *target = s->root;
    if (dom) { target = objid >= 1 && objid <= 64 ? s->domain->obj[objid - 1] : NULL; if (dtype == 2) { if (target) s->domain->obj[objid - 1] = NULL; memset(m, 0, 16); m[1] = 4; return 0; } }
    int result = 0; c.domain = dom; c.session = s;
    if (type == 5 || type == 7) {                              // control messages: cmd at r[2]
        uint32_t cmd = r[2];
        switch (cmd) {
        case 0: if (!s->domain) { s->domain = calloc(1, sizeof(Domain)); s->domain->obj[0] = s->root; } c.out32[0] = 1; c.out_sz = 4; break;      // ConvertCurrentObjectToDomain -> object id 1
        case 2: case 4: { uint32_t nh = sess_new(s->root); Session *ns = sess_get(nh); ns->domain = s->domain; c.move[c.nmove++] = nh; break; }              // CloneCurrentObject: the clone shares the domain
        case 3: c.out32[0] = 0x1000; c.out_sz = 4; break;                                                                  // QueryPointerBufferSize: 16-bit (hid and others size arrays by it)
        default: LOG("ipc: control command %u on %s", cmd, s->root->iface); break;
        }
    } else if (type == 4 || type == 6) {
        if (!target) { LOG("ipc: request on a closed domain object %u", objid); result = 0x1a0b; }
        else if (r[0] != 0x49434653) { LOG("ipc: request without SFCI (%08x)", r[0]); result = 0xe401; }
        else {
            uint32_t cmd = r[2]; const uint8_t *in = (const uint8_t *)(r + 4);
            unsigned inlen = dom ? (dsize >= 16 ? dsize - 16 : 0) : (rawwords * 4 > (raw - off) + 16 ? (unsigned)(rawwords * 4 - (raw - off) - 16) : 0);
            if (g_ipc_trace) LOG("ipc: %s%s command %u in %u bytes", target->iface, dom ? " (domain)" : "", cmd, inlen);
            a32_cur_iface = target->iface; result = target->fn(target, cmd, in, inlen, &c);
        }
    } else { LOG("ipc: unsupported message type %u", type); }
    // ---- build the response in the same buffer ----
    uint8_t save_out[0x200]; memcpy(save_out, c.out, sizeof c.out);
    uint32_t out_bytes = c.out_sz, nmove = (uint32_t)c.nmove, ncopy = (uint32_t)c.ncopy;
    memset(m, 0, 0x100);
    uint32_t *q = m + 2; uint32_t hdr1 = 0;
    if (nmove || ncopy) { hdr1 |= 1u << 31; *q++ = (ncopy << 1) | (nmove << 5); for (uint32_t i = 0; i < ncopy; i++) *q++ = c.copy[i]; for (uint32_t i = 0; i < nmove; i++) *q++ = c.move[i]; }
    size_t roff = (size_t)((uint8_t *)q - base), rstart = (roff + 15) & ~(size_t)15;
    uint32_t *o = (uint32_t *)(base + rstart);
    size_t body = 0;
    uint32_t dom_objs = 0;
    if (dom && (type == 4 || type == 6)) { o += 4; body += 16; dom_objs = (uint32_t)c.nobj_out; }
    if (type == 5 || type == 7 || type == 4 || type == 6) {
        o[0] = 0x4f434653; o[1] = 0; o[2] = (uint32_t)result; o[3] = 0;
        memcpy(o + 4, save_out, out_bytes); body += 16 + out_bytes;
        if (dom && (type == 4 || type == 6)) {
            uint32_t *d = (uint32_t *)(base + rstart); d[0] = dom_objs; d[1] = d[2] = d[3] = 0;
            for (uint32_t i = 0; i < dom_objs; i++) ((uint32_t *)(base + rstart + body))[i] = c.obj_ids[i];
            body += 4 * dom_objs;
        }
    }
    hdr1 |= (uint32_t)(((rstart - ((size_t)((uint8_t *)(m + 2) - base) + (nmove || ncopy ? 4 * (1 + ncopy + nmove) : 0))) + body + 3) / 4) & 0x3ff;
    m[0] = 0; m[1] = hdr1;
    if (g_ipc_trace) dump_words("response", m, 16);
    return 0;
}

void ipc_out_obj(Ipc *c, Obj *o) {
    if (c->domain) {
        Session *s = c->session; int id = 0;
        for (int i = 0; i < 64; i++) if (!s->domain->obj[i]) { s->domain->obj[i] = o; id = i + 1; break; }
        c->obj_ids[c->nobj_out++] = (uint32_t)id;
    } else c->move[c->nmove++] = sess_new(o);
}
