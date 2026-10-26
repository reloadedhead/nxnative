// Host-side HIPC/CMIF service objects for the 32-bit guest (a32ipc.c, a32svcs.c).
#pragma once
#include "a32host.h"
typedef struct Obj Obj; typedef struct Ipc Ipc; struct Session;
typedef int (*CmdFn)(Obj *o, uint32_t cmd, const uint8_t *in, uint32_t in_sz, Ipc *c);   // returns the 32-bit result code; outputs go through ipc_put*, ipc_out_obj
struct Obj { const char *iface; CmdFn fn; void *data; };
typedef struct { uint32_t addr, size; } Buf;
struct Ipc {
    int nx, na, nb, nw, nc; Buf x[16], a[16], b[16], w[16], rc[4];   // rc: receive-list (C) buffers, pointer-style outputs
               // buffer descriptors of the request (A = guest-to-host, B = host-to-guest)
    union { uint8_t out[0x200]; uint32_t out32[0x80]; };
    uint32_t out_sz;
    uint32_t move[8], copy[8]; int nmove, ncopy;                   // handles returned in the response
    uint32_t obj_ids[8]; int nobj_out; int domain; void *session;
};
Obj *obj_new(const char *iface, CmdFn fn, void *data);
void ipc_out_obj(Ipc *c, Obj *o);                                  // return an interface object (a moved session handle, or a domain object id)
static inline void ipc_put32(Ipc *c, uint32_t v) { memcpy(c->out + c->out_sz, &v, 4); c->out_sz += 4; }
static inline void ipc_put64(Ipc *c, uint64_t v) { c->out_sz = (c->out_sz + 7) & ~7u; memcpy(c->out + c->out_sz, &v, 8); c->out_sz += 8; }
static inline void ipc_pad(Ipc *c, uint32_t n) { memset(c->out + c->out_sz, 0, n); c->out_sz += n; }
uint32_t a32_cur_tls(void);
int a32_is_session(uint32_t h);
void a32_session_close(uint32_t h);
