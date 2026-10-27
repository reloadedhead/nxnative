// The services the host answers, described by tables: a service or interface object (a "spec") lists the commands that return an
// interface object, an event handle or a plain value. Every other command succeeds with zeroed output and is logged once, which is
// enough for most initialisation calls; a spec grows when the game needs more than that. Names are those of switchbrew's service pages.
#define _DARWIN_C_SOURCE
#include "a32ipc.h"
#include "a32svcs.h"
#include <time.h>
#include <pthread.h>

// "not implemented" notes are printed once per distinct message: a service the game polls every frame would otherwise flood the log
static int ni_seen(const char *msg) {
    static char tab[512][200]; static int n; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; int hit = 0;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < n; i++) if (!strcmp(tab[i], msg)) { hit = 1; break; }
    if (!hit && n < 512) snprintf(tab[n++], 200, "%s", msg);
    pthread_mutex_unlock(&mu); return hit;
}
#define LOGNI(...) do { char _b[200]; snprintf(_b, sizeof _b, __VA_ARGS__); if (!ni_seen(_b)) LOG("%s", _b); } while (0)
typedef struct { const char *name; Obj *(*make)(void); } ServiceDef;

typedef struct { uint32_t cmd; const char *obj; } ObjCmd;
typedef struct { uint32_t cmd; int ev; } EvCmd;                       // returns a copy handle of an event; `ev` identifies which one (shared per object)
typedef struct { uint32_t cmd; uint32_t nbytes; uint64_t val; uint32_t result; } ValCmd;   // returns `nbytes` bytes of val (0 = none), or just a result code
typedef struct Spec { const char *name; const ObjCmd *objs; const EvCmd *evs; const ValCmd *vals; } Spec;
static const Spec *spec_find(const char *name);

// ICommonStateGetter keeps the applet message queue: a freshly started application gets the focus-state-changed message (15) first, as on a console
typedef struct { uint32_t ev; int q[8]; int n; } CsgState;
static int csg_cmd(Obj *o, uint32_t cmd, const uint8_t *in, uint32_t in_sz, Ipc *c) {
    CsgState *st = o->data; (void)in;
    switch (cmd) {
    case 0: if (!st->ev) { st->ev = a32_new_event_handle(); st->q[st->n++] = 15; a32_signal_event(st->ev); } c->copy[c->ncopy++] = st->ev; return 0;     // GetEventHandle
    case 1:                                                                                                                                         // ReceiveMessage
        if (!st->n) { a32_clear_event(st->ev); return 0x680; }
        ipc_put32(c, (uint32_t)st->q[0]); memmove(st->q, st->q + 1, sizeof(int) * (size_t)--st->n); if (!st->n) a32_clear_event(st->ev); return 0;
    case 5: c->out[c->out_sz++] = 1; ipc_pad(c, 3); return 0;                                                                                      // GetOperationMode: docked
    case 6: c->out[c->out_sz++] = 0; ipc_pad(c, 3); return 0;                                                                                      // GetPerformanceMode: normal
    case 9: c->out[c->out_sz++] = 1; ipc_pad(c, 3); return 0;                                                                                      // GetCurrentFocusState: in focus
    default: LOGNI("ipc: ICommonStateGetter command %u (%u bytes in) not implemented: success, zeroed output", cmd, in_sz); return 0;
    }
}
// ---- IStorage (applet storage): a byte array the game opens and reads (launch parameters) ----
typedef struct { uint8_t *data; uint32_t size; } AStorage;
static int astorage_accessor_cmd(Obj *o, uint32_t cmd, const uint8_t *in, uint32_t in_sz, Ipc *c) {
    AStorage *st = o->data; (void)in_sz;
    if (cmd == 0) { ipc_put64(c, st->size); return 0; }                                                // GetSize
    if (cmd == 11) {                                                                                   // Read(offset) -> out buffer
        uint64_t off; memcpy(&off, in, 8);
        Buf ob = c->nb && c->b[0].addr ? c->b[0] : c->nc ? c->rc[0] : (Buf){0, 0};
        if (ob.addr) { uint32_t n = ob.size; memset(g_mem + ob.addr, 0, n); if (off < st->size) memcpy(g_mem + ob.addr, st->data + off, off + n > st->size ? st->size - (uint32_t)off : n); }
        return 0;
    }
    LOGNI("ipc: IStorageAccessor command %u not implemented: success, zeroed output", cmd); return 0;
}
static int astorage_cmd(Obj *o, uint32_t cmd, const uint8_t *in, uint32_t in_sz, Ipc *c) {
    (void)in; (void)in_sz;
    if (cmd == 0) { ipc_out_obj(c, obj_new("IStorageAccessor", astorage_accessor_cmd, o->data)); return 0; }   // Open
    LOGNI("ipc: IStorage (applet) command %u not implemented: success, zeroed output", cmd); return 0;
}
static Obj *make_astorage(const void *bytes, uint32_t size) { AStorage *st = calloc(1, sizeof *st); st->data = calloc(1, size); memcpy(st->data, bytes, size); st->size = size; return obj_new("IStorage", astorage_cmd, st); }
static int appfn_cmd(Obj *o, uint32_t cmd, const uint8_t *in, uint32_t in_sz, Ipc *c) {
    extern int spec_cmd(Obj *, uint32_t, const uint8_t *, uint32_t, Ipc *);
    if (cmd == 1 && in_sz >= 4) {                                                                      // PopLaunchParameter(kind): 2 = the preselected user
        uint32_t kind; memcpy(&kind, in, 4);
        static int popped;
        if (kind == 2 && !popped) {
            popped = 1; uint8_t buf[0x88]; memset(buf, 0, sizeof buf); uint32_t magic = 0xc79497ca; memcpy(buf, &magic, 4); buf[4] = 1;
            static const uint64_t uid[2] = {0x1111111111111111ull, 0x2222222222222222ull}; memcpy(buf + 8, uid, 16);
            ipc_out_obj(c, make_astorage(buf, sizeof buf)); return 0;
        }
    }
    return spec_cmd(o, cmd, in, in_sz, c);
}
static Obj *obj_of(const char *name) {
    if (!strcmp(name, "IApplicationFunctions")) return obj_new("IApplicationFunctions", appfn_cmd, (void *)spec_find(name));
    if (!strcmp(name, "ICommonStateGetter")) return obj_new("ICommonStateGetter", csg_cmd, calloc(1, sizeof(CsgState))); const Spec *s = spec_find(name); extern int spec_cmd(Obj *, uint32_t, const uint8_t *, uint32_t, Ipc *); return obj_new(s ? s->name : name, spec_cmd, (void *)s); }
int spec_cmd(Obj *o, uint32_t cmd, const uint8_t *in, uint32_t in_sz, Ipc *c) {
    const Spec *s = o->data; (void)in;
    if (s) {
        for (const ObjCmd *x = s->objs; x && x->obj; x++) if (x->cmd == cmd) { ipc_out_obj(c, obj_of(x->obj)); return 0; }
        for (const EvCmd *x = s->evs; x && x->cmd != 0xffffffffu; x++) if (x->cmd == cmd) {
            uint32_t h = a32_event_handle(o, x->ev); c->copy[c->ncopy++] = h;
            if (!strcmp(o->iface, "ISelfController") && cmd == 9) a32_signal_event(h);          // the library-applet-launchable event is signaled for an application
            return 0;
        }
        for (const ValCmd *x = s->vals; x && x->cmd != 0xffffffffu; x++) if (x->cmd == cmd) {
            if (x->nbytes == 16) { memcpy(c->out + c->out_sz, &x->val, 8); c->out_sz += 16; }
            else if (x->nbytes == 8) ipc_put64(c, x->val); else if (x->nbytes == 4) ipc_put32(c, (uint32_t)x->val); else if (x->nbytes == 1) { c->out[c->out_sz++] = (uint8_t)x->val; ipc_pad(c, 3); }
            return (int)x->result;
        }
    }
    LOGNI("ipc: %s command %u (%u bytes in) not implemented: success, zeroed output", o->iface, cmd, in_sz);
    return 0;
}
#define NOOBJ {0, 0}
#define NOEV {0xffffffffu, 0}
#define NOVAL {0xffffffffu, 0, 0, 0}

// ---- lm (log manager) ----
static const ObjCmd lm_objs[] = { {0, "ILogger"}, NOOBJ };
// ---- appletOE ----
static const ObjCmd oe_objs[] = { {0, "IApplicationProxy"}, NOOBJ };
static const ObjCmd proxy_objs[] = { {0, "ICommonStateGetter"}, {1, "ISelfController"}, {2, "IWindowController"}, {3, "IAudioController"}, {4, "IDisplayController"},
                                     {10, "IProcessWindingController"}, {11, "ILibraryAppletCreator"}, {20, "IApplicationFunctions"}, {1000, "IDebugFunctions"}, NOOBJ };
static const EvCmd csg_evs[] = { {0, 0}, NOEV };                                  // GetEventHandle
static const ValCmd csg_vals[] = { {1, 0, 0, 0x680}, {5, 1, 0, 0}, {6, 1, 0, 0}, {8, 4, 1, 0}, NOVAL };           // ReceiveMessage: no message; GetOperationMode/GetPerformanceMode: handheld/normal; GetCurrentFocusState: in focus
static const EvCmd sc_evs[] = { {9, 1}, {91, 2}, NOEV };                           // GetLibraryAppletLaunchableEvent, GetAccumulatedSuspendedTickChangedEvent
static const ValCmd sc_vals[] = { {40, 8, 1, 0}, {50, 0, 0, 0}, NOVAL };           // CreateManagedDisplayLayer -> layer id 1
static const ValCmd af_vals[] = { {1, 0, 0, 0x480}, {20, 8, 0, 0}, {21, 8, 0x53552d6e65ull, 0}, {40, 1, 1, 0}, {120, 4, 0, 0}, NOVAL };   // PopLaunchParameter: none; EnsureSaveData; GetDesiredLanguage = "en-US"; NotifyRunning
static const ValCmd wc_vals[] = { {1, 0, 0, 0}, NOVAL };
static const Spec g_specs[] = {
    { "lm", lm_objs, NULL, NULL }, { "ILogger", NULL, NULL, NULL },
    { "appletOE", oe_objs, NULL, NULL }, { "IApplicationProxy", proxy_objs, NULL, NULL },
    { "ICommonStateGetter", NULL, csg_evs, csg_vals }, { "ISelfController", NULL, sc_evs, sc_vals },
    { "IWindowController", NULL, NULL, wc_vals }, { "IApplicationFunctions", NULL, NULL, af_vals },
};
static const Spec *spec_find(const char *name) { for (unsigned i = 0; i < sizeof g_specs / sizeof *g_specs; i++) if (!strcmp(g_specs[i].name, name)) return &g_specs[i]; return NULL; }

// ---- events shared per object ----
uint32_t a32_event_handle(Obj *o, int id) {
    static struct { Obj *o; int id; uint32_t h; } tab[256]; static int n;
    for (int i = 0; i < n; i++) if (tab[i].o == o && tab[i].id == id) return tab[i].h;
    uint32_t h = a32_new_event_handle(); if (n < 256) { tab[n].o = o; tab[n].id = id; tab[n].h = h; n++; }
    return h;
}

// ---- hid: the shared memory is mapped by the game and read for controller input (zeroed: no controller connected) ----
uint32_t a32_new_shmem_handle(uint32_t size, uint8_t **host);
static uint32_t g_hid_shm; static uint8_t *g_hid_mem;
static int hid_res_cmd(Obj *o, uint32_t cmd, const uint8_t *in, uint32_t in_sz, Ipc *c) {
    (void)o; (void)in;
    if (cmd == 0) { if (!g_hid_shm) g_hid_shm = a32_new_shmem_handle(0x40000, &g_hid_mem); c->copy[c->ncopy++] = g_hid_shm; return 0; }       // GetSharedMemoryHandle
    LOGNI("ipc: IAppletResource command %u (%u bytes in) not implemented: success, zeroed output", cmd, in_sz); return 0;
}
static int hid_generic(Obj *o, uint32_t cmd, const uint8_t *in, uint32_t in_sz, Ipc *c) { (void)in; (void)c; LOGNI("ipc: %s command %u (%u bytes in) not implemented: success, zeroed output", o->iface, cmd, in_sz); return 0; }
static int hid_cmd(Obj *o, uint32_t cmd, const uint8_t *in, uint32_t in_sz, Ipc *c) {
    switch (cmd) {
    case 0: ipc_out_obj(c, obj_new("IAppletResource", hid_res_cmd, NULL)); return 0;                                                    // CreateAppletResource
    case 203: ipc_out_obj(c, obj_new("IActiveVibrationDeviceList", hid_generic, NULL)); return 0;                                       // CreateActiveVibrationDeviceList
    case 106: case 124: case 131: case 142: case 211: c->copy[c->ncopy++] = a32_event_handle(o, (int)cmd); return 0;                  // Acquire...EventHandle
    default: LOGNI("ipc: IHidServer command %u (%u bytes in) not implemented: success, zeroed output", cmd, in_sz); return 0;
    }
}
static Obj *mk_hid(void) { return obj_new("IHidServer", hid_cmd, NULL); }
// ---- acc:u0: one user, always open ----
static const uint64_t k_uid[2] = {0x1111111111111111ull, 0x2222222222222222ull};
static int acc_profile_cmd(Obj *o, uint32_t cmd, const uint8_t *in, uint32_t in_sz, Ipc *c) {
    (void)o; (void)in;
    if (cmd == 0 || cmd == 1) {                                                                 // Get (+ user data buffer) / GetBase: ProfileBase = uid, last edit time, nickname[0x20]
        memcpy(c->out + c->out_sz, k_uid, 16); memset(c->out + c->out_sz + 16, 0, 0x38 - 16); strcpy((char *)c->out + c->out_sz + 24, "Hunter"); c->out_sz += 0x38;
        if (cmd == 0 && c->nb && c->b[0].addr) memset(g_mem + c->b[0].addr, 0, c->b[0].size);
        return 0;
    }
    LOGNI("ipc: IProfile command %u (%u bytes in) not implemented: success, zeroed output", cmd, in_sz); return 0;
}
static int acc_generic(Obj *o, uint32_t cmd, const uint8_t *in, uint32_t in_sz, Ipc *c) { (void)in; (void)c; LOGNI("ipc: %s command %u (%u bytes in) not implemented: success, zeroed output", o->iface, cmd, in_sz); return 0; }
static int acc_cmd(Obj *o, uint32_t cmd, const uint8_t *in, uint32_t in_sz, Ipc *c) {
    (void)o;
    switch (cmd) {
    case 0: ipc_put32(c, 1); return 0;                                                                  // GetUserCount
    case 1: c->out[c->out_sz++] = 1; ipc_pad(c, 3); return 0;                                           // GetUserExistence
    case 2: case 3: case 140: if (c->nb && c->b[0].addr && c->b[0].size >= 16) { memset(g_mem + c->b[0].addr, 0, c->b[0].size); memcpy(g_mem + c->b[0].addr, k_uid, 16); } return 0;   // ListAllUsers / ListOpenUsers / ListQualifiedUsers
    case 4: memcpy(c->out + c->out_sz, k_uid, 16); c->out_sz += 16; return 0;                           // GetLastOpenedUser
    case 5: ipc_out_obj(c, obj_new("IProfile", acc_profile_cmd, NULL)); return 0;                       // GetProfile
    case 101: ipc_out_obj(c, obj_new("IManagerForApplication", acc_generic, NULL)); return 0;           // GetBaasAccountManagerForApplication
    default: LOGNI("ipc: IAccountServiceForApplication command %u (%u bytes in) not implemented: success, zeroed output", cmd, in_sz); return 0;
    }
}
static Obj *mk_acc(void) { return obj_new("IAccountServiceForApplication", acc_cmd, NULL); }
// ---- audren:u: the SDK's audio renderer talks to this; nothing is mixed yet, the renderer just keeps time (a system event every 5 ms) and answers updates with an empty report ----
typedef struct { uint32_t ev; int started; } ArState;
static void *ar_clock(void *p) {
    ArState *st = p;
    for (;;) { struct timespec ts = {0, 5000000}; nanosleep(&ts, NULL); if (st->started) a32_signal_event(st->ev); }
    return NULL;
}
static int ar_cmd(Obj *o, uint32_t cmd, const uint8_t *in, uint32_t in_sz, Ipc *c) {
    ArState *st = o->data; (void)in;
    switch (cmd) {
    case 0: ipc_put32(c, 48000); return 0;                                                        // GetSampleRate
    case 1: ipc_put32(c, 240); return 0;                                                          // GetSampleCount
    case 2: ipc_put32(c, 24); return 0;                                                           // GetMixBufferCount
    case 3: ipc_put32(c, st->started ? 0 : 1); return 0;                                          // GetState (0 = started)
    case 4: case 10: {                                                                            // RequestUpdate: the report is an UpdateDataHeader (revision echoed) without entries
        if (c->nb && c->b[0].addr && c->b[0].size >= 0x40) {
            memset(g_mem + c->b[0].addr, 0, c->b[0].size);
            if (c->na && c->a[0].addr) memcpy(g_mem + c->b[0].addr, g_mem + c->a[0].addr, 4);           // revision
            wr32(c->b[0].addr + 0x3c, 0x40);                                                           // total size
        }
        return 0;
    }
    case 5: st->started = 1; return 0;                                                            // Start
    case 6: st->started = 0; return 0;                                                            // Stop
    case 7: c->copy[c->ncopy++] = st->ev; return 0;                                               // QuerySystemEvent
    case 8: return 0;                                                                             // SetRenderingTimeLimit
    case 9: ipc_put32(c, 100); return 0;                                                          // GetRenderingTimeLimit
    default: LOGNI("ipc: IAudioRenderer command %u (%u bytes in) not implemented: success, zeroed output", cmd, in_sz); return 0;
    }
}
static int ar_generic(Obj *o, uint32_t cmd, const uint8_t *in, uint32_t in_sz, Ipc *c) { (void)in; (void)c; LOGNI("ipc: %s command %u (%u bytes in) not implemented: success, zeroed output", o->iface, cmd, in_sz); return 0; }
static int audren_cmd(Obj *o, uint32_t cmd, const uint8_t *in, uint32_t in_sz, Ipc *c) {
    (void)o;
    switch (cmd) {
    case 0: case 3: {                                                                             // OpenAudioRenderer
        ArState *st = calloc(1, sizeof *st); st->ev = a32_new_event_handle();
        pthread_t th; pthread_create(&th, NULL, ar_clock, st); pthread_detach(th);
        ipc_out_obj(c, obj_new("IAudioRenderer", ar_cmd, st)); return 0;
    }
    case 1: ipc_put64(c, 0x600000); return 0;                                                     // GetWorkBufferSize
    case 2: case 4: ipc_out_obj(c, obj_new("IAudioDevice", ar_generic, NULL)); return 0;           // GetAudioDeviceService
    default: LOGNI("ipc: IAudioRendererManager command %u (%u bytes in) not implemented: success, zeroed output", cmd, in_sz); return 0;
    }
}
static Obj *mk_audren(void) { return obj_new("IAudioRendererManager", audren_cmd, NULL); }
// ---- friend:u: no friends ----
static int friend_generic(Obj *o, uint32_t cmd, const uint8_t *in, uint32_t in_sz, Ipc *c) { (void)in; (void)c; LOGNI("ipc: %s command %u (%u bytes in) not implemented: success, zeroed output", o->iface, cmd, in_sz); return 0; }
static int friend_cmd(Obj *o, uint32_t cmd, const uint8_t *in, uint32_t in_sz, Ipc *c) {
    if (cmd == 0) { ipc_out_obj(c, obj_new("IFriendService", friend_generic, NULL)); return 0; }                // CreateFriendService
    if (cmd == 1) { ipc_out_obj(c, obj_new("INotificationService", friend_generic, NULL)); return 0; }          // CreateNotificationService
    return friend_generic(o, cmd, in, in_sz, c);
}
static Obj *mk_friend(void) { return obj_new("friend:u", friend_cmd, NULL); }
// ---- apm and set ----
static int misc_generic(Obj *o, uint32_t cmd, const uint8_t *in, uint32_t in_sz, Ipc *c) { (void)in; (void)c; LOGNI("ipc: %s command %u (%u bytes in) not implemented: success, zeroed output", o->iface, cmd, in_sz); return 0; }
static int apm_cmd(Obj *o, uint32_t cmd, const uint8_t *in, uint32_t in_sz, Ipc *c) {
    if (cmd == 0) { ipc_out_obj(c, obj_new("ISession", misc_generic, NULL)); return 0; }               // OpenSession
    if (cmd == 1) { ipc_put32(c, 0); return 0; }                                                        // GetPerformanceMode: handheld
    return misc_generic(o, cmd, in, in_sz, c);
}
static Obj *mk_apm(void) { return obj_new("apm", apm_cmd, NULL); }
static const char *const k_langs[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant"};
static uint64_t lang_code(int i) { uint64_t v = 0; const char *n = k_langs[i < 0 || i > 16 ? 1 : i]; memcpy(&v, n, strlen(n)); return v; }
static int set_cmd(Obj *o, uint32_t cmd, const uint8_t *in, uint32_t in_sz, Ipc *c) {
    switch (cmd) {
    case 0: ipc_put64(c, lang_code(1)); return 0;                                                       // GetLanguageCode
    case 1: if (c->nb && c->b[0].addr) { uint64_t v = lang_code(1); memcpy(g_mem + c->b[0].addr, &v, 8); } ipc_put32(c, 1); return 0;   // GetAvailableLanguageCodes
    case 2: { int32_t i; memcpy(&i, in, 4); ipc_put64(c, lang_code(i)); return 0; }                      // MakeLanguageCode
    case 5: ipc_put32(c, 1); return 0;                                                                  // GetAvailableLanguageCodeCount
    default: return misc_generic(o, cmd, in, in_sz, c);
    }
}
static Obj *mk_set(void) { return obj_new("set", set_cmd, NULL); }
// ---- pctl: parental controls are not restricting anything ----
static int pctl_cmd(Obj *o, uint32_t cmd, const uint8_t *in, uint32_t in_sz, Ipc *c) {
    if (cmd == 0 || cmd == 1) { ipc_out_obj(c, obj_new("IParentalControlService", misc_generic, NULL)); return 0; }      // CreateService / CreateServiceWithoutInitialize
    return misc_generic(o, cmd, in, in_sz, c);
}
static Obj *mk_pctl(void) { return obj_new("pctl", pctl_cmd, NULL); }
Obj *a32_make_fsp(void);
static Obj *make_named(const char *n) { return obj_of(n); }
#define SVC(n) { n, NULL }
static Obj *mk_lm(void) { return make_named("lm"); }
static Obj *mk_oe(void) { return make_named("appletOE"); }
const ServiceDef a32_services[] = { { "lm", mk_lm }, { "appletOE", mk_oe }, { "fsp-srv", a32_make_fsp }, { "hid", mk_hid }, { "acc:u0", mk_acc }, { "audren:u", mk_audren }, { "friend:u", mk_friend }, { "pctl", mk_pctl }, { "pctl:s", mk_pctl }, { "pctl:r", mk_pctl }, { "pctl:a", mk_pctl }, { "apm", mk_apm }, { "set", mk_set } };
const unsigned a32_nservices = sizeof a32_services / sizeof a32_services[0];
