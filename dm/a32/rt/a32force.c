// Forced take-over of SDK functions by name: a reference to one of these symbols is bound to the host handler instead of the module's
// definition (the same idea as dm/host's HLE_FORCE). The handler gets the guest registers in the frame (r0-r3 = arguments, stack arguments at
// [sp]) and leaves the result in r0 (r0:r1 for 64-bit values).
#define _DARWIN_C_SOURCE
#include "a32host.h"
#include "a32force.h"
#include "a32gfx.h"
#include <time.h>
extern const char *a32_hook_current_name(void);
static void f_noop(Frame *f) { f->r[0] = 0; }
// ---- nn::time: the host answers in UTC from the Mac's clock ----
static void wr64(uint32_t a, int64_t v) { memcpy(g_mem + a, &v, 8); }
static void time_calendar(uint32_t pct, uint32_t pinfo, int64_t t) {                    // CalendarTime {s16 year; s8 month, day, hour, minute, second}; CalendarAdditionalInfo {s32 dayOfWeek, dayOfYear; TimeZone {char name[8]; bool dst; s32 utcOffset}}
    time_t tt = (time_t)t; struct tm tm; gmtime_r(&tt, &tm);
    uint8_t *c = g_mem + pct; c[0] = (uint8_t)(tm.tm_year + 1900); c[1] = (uint8_t)((tm.tm_year + 1900) >> 8); c[2] = (uint8_t)(tm.tm_mon + 1); c[3] = (uint8_t)tm.tm_mday; c[4] = (uint8_t)tm.tm_hour; c[5] = (uint8_t)tm.tm_min; c[6] = (uint8_t)tm.tm_sec;
    if (pinfo) { memset(g_mem + pinfo, 0, 24); wr32(pinfo, (uint32_t)tm.tm_wday); wr32(pinfo + 4, (uint32_t)tm.tm_yday); memcpy(g_mem + pinfo + 8, "UTC", 4); }
}
static int64_t time_from_calendar(uint32_t pct) {
    const uint8_t *c = g_mem + pct; struct tm tm; memset(&tm, 0, sizeof tm);
    tm.tm_year = (int16_t)(c[0] | c[1] << 8) - 1900; tm.tm_mon = c[2] - 1; tm.tm_mday = c[3]; tm.tm_hour = c[4]; tm.tm_min = c[5]; tm.tm_sec = c[6];
    return (int64_t)timegm(&tm);
}
static void f_time_loc(Frame *f) { memset(g_mem + f->r[0], 0, 36); memcpy(g_mem + f->r[0], "UTC", 4); f->r[0] = 0; }
static void f_time_now(Frame *f) { wr64(f->r[0], (int64_t)time(NULL)); f->r[0] = 0; }
static void f_time_steady(Frame *f) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); memset(g_mem + f->r[0], 0, 24); wr64(f->r[0], ts.tv_sec); f->r[0] = 0; }
static void f_tocal(Frame *f) { int64_t t; memcpy(&t, g_mem + f->r[2], 8); time_calendar(f->r[0], f->r[1], t); f->r[0] = 0; }
static void f_tocal_utc(Frame *f) { int64_t t; memcpy(&t, g_mem + f->r[1], 8); time_calendar(f->r[0], 0, t); }                      // sret: r0 = result, r1 = PosixTime const&
static void f_toposix_utc(Frame *f) { wr64(f->r[0], time_from_calendar(f->r[1])); }                                             // sret
static void f_toposix(Frame *f) { wr32(f->r[0], 1); wr64(f->r[1], time_from_calendar(f->r[3])); f->r[0] = 0; }                 // (int *count, PosixTime *out, int outcount, CalendarTime const &)
static void f_true(Frame *f) { f->r[0] = 1; }
// nn::ngc::ProfanityFilter (system data the host does not have): the filter initialises and finds nothing to mask
static void f_ngc_check(Frame *f) { if (f->r[1]) wr32(f->r[1], 0); f->r[0] = 0; }          // members: r0 is the filter, r1 the output (CheckProfanityWords(uint *flags, ...))
static void f_ngc_mask(Frame *f) { if (f->r[1]) wr32(f->r[1], 0); f->r[0] = 0; }           // MaskProfanityWordsInText(int *count, text, length): nothing masked
// nn::diag::detail::AbortImpl(condition, function, file, line, ...): print what the SDK aborts on, then stop. The arguments after `line` are on the stack.
static void f_abort(Frame *f) {
    const char *c = f->r[0] ? gstr(f->r[0]) : "?", *fn = f->r[1] ? gstr(f->r[1]) : "?", *file = f->r[2] ? gstr(f->r[2]) : "?";
    fprintf(stderr, "[a32] SDK ABORT: %s\n   in %s at %s:%d\n", c, fn, file, (int)f->r[3]);
    for (int k = 0; k < 3; k++) { uint32_t v = rd32(f->r[13] + 4 * k); if (v > 0x100000 && v < 0x10000000 && !(v & 0xf0000000)) { const char *t = gstr(v); int ok = 1; for (int i = 0; i < 40 && t[i]; i++) if ((unsigned char)t[i] < 9) ok = 0; if (ok && t[0]) fprintf(stderr, "   arg: %.100s\n", t); } }
    dump_regs(f); fflush(stderr); _exit(6);
}

// ---- nn::hid: the controller state comes from the window's front end (keyboard / game controller, see a32shim.m), as in the 64-bit host (dm/host/hle_hid.c):
// the SDK's npad calls are taken over by name instead of emulating the hid shared memory. Player 1 is a full-key controller on No1; A32_KEYS="seconds:key[+key],..." presses keys for a moment (A B X Y L R ZL ZR Plus Minus Left Up Right Down)
extern uint64_t a32_pad_buttons; extern int32_t a32_pad_axes[4];
typedef struct { int64_t sampling; uint64_t buttons; int32_t lx, ly, rx, ry; uint32_t attributes, pad; } NpadState;
static uint64_t scripted_keys(void) {
    static const char *spec; static int init; static uint64_t t0;
    if (!init) { init = 1; spec = getenv("A32_KEYS"); struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); t0 = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000; }
    if (!spec) return 0;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); double now = ((uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000 - t0) / 1000.0;
    static const char *names[] = {"A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR", "Plus", "Minus", "Left", "Up", "Right", "Down"};
    uint64_t b = 0; const char *p = spec;
    while (*p) {
        double at = atof(p); const char *c = strchr(p, ':'); if (!c) break; c++;
        const char *e = c + strcspn(c, ",");
        if (now >= at && now < at + 0.25) for (const char *k = c; k < e; ) {
            const char *pl = k + strcspn(k, "+,"); size_t n = (size_t)(pl - k);
            for (int i = 0; i < 16; i++) if (strlen(names[i]) == n && !strncmp(k, names[i], n)) b |= 1ull << i;
            k = *pl == '+' ? pl + 1 : pl;
        }
        p = *e ? e + 1 : e;
    }
    return b;
}
extern uint32_t a32_kb_mgr;
static void kb_watch(void) {
    static int on = -1; if (on < 0) on = getenv("A32_KB_DEBUG") != NULL;
    if (!on || !a32_kb_mgr) return;
    static uint32_t last[6]; uint32_t m = a32_kb_mgr, v[6] = { rd32(m + 0x2ec8), rd32(m + 0x10), rd32(m + 0x36ac), g_mem[m + 0x2ecc], g_mem[m + 0x2ecd], g_mem[m + 0x2ece] };
    if (memcmp(v, last, sizeof v)) { memcpy(last, v, sizeof v); LOG("swkbd-watch: state %u [0x10] %u text-state %u flags %u %u %u", v[0], v[1], v[2], v[3], v[4], v[5]); }
}
static void hid_fill(NpadState *s, uint32_t id) {
    kb_watch();
    static int64_t sampling = 1 << 16; memset(s, 0, sizeof *s); s->sampling = ++sampling;
    if (id != 0 && id != 0x20) return;                                          // No1 and the handheld npad report the controller
    uint64_t b = (a32_pad_buttons | scripted_keys()) & 0xffffull; int32_t a[4]; memcpy(a, a32_pad_axes, sizeof a);
    s->lx = a[0]; s->ly = a[1]; s->rx = a[2]; s->ry = a[3];
    if (s->lx < -0x4000) b |= 1u << 16; if (s->ly > 0x4000) b |= 1u << 17; if (s->lx > 0x4000) b |= 1u << 18; if (s->ly < -0x4000) b |= 1u << 19;
    if (s->rx < -0x4000) b |= 1u << 20; if (s->ry > 0x4000) b |= 1u << 21; if (s->rx > 0x4000) b |= 1u << 22; if (s->ry < -0x4000) b |= 1u << 23;
    s->buttons = b; s->attributes = 1;                                          // connected
    { static uint64_t last = ~0ull; static unsigned calls; calls++; if (b != last) { last = b; LOG("hid: npad %u buttons %#llx (call %u)", id, (unsigned long long)b, calls); } }
}
static void f_hid_state(Frame *f) { static int n; if (n++ < 3) LOG("hid: GetNpadState id %#x from %s", rd32(f->r[1]), where(f->r[14])); NpadState s; hid_fill(&s, rd32(f->r[1])); memcpy(g_mem + f->r[0], &s, sizeof s); }
static void f_hid_state_none(Frame *f) { NpadState s; hid_fill(&s, 0xffffffffu); memcpy(g_mem + f->r[0], &s, sizeof s); }      // styles the host has no controller of
static void f_hid_style(Frame *f) { static int n; if (n++ < 3) LOG("hid: GetNpadStyleSet id %#x from %s", rd32(f->r[0]), where(f->r[14])); uint32_t id = rd32(f->r[0]); f->r[0] = id == 0 ? 1 : id == 0x20 ? 2 : 0; }                                                  // GetNpadStyleSet: No1 is a full-key controller
// BindNpadStyleSetUpdateEvent(id, SystemEventType *, clear mode): the game builds its pad object when this event fires, so it is created and signaled at once (a controller is attached from the start)
extern uint32_t a32_find_symbol(const char *name); extern uint32_t a32_callback(uint32_t pc, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
// nn::swkbd::ShowKeyboard(String *out, const ShowKeyboardArg &): the full-screen keyboard applet
static void f_swkbd_show(Frame *f) {
    LOG("swkbd: ShowKeyboard out %#x arg %#x from %s", f->r[0], f->r[1], where(f->r[14]));
    for (int k = 0; k < 8; k++) LOG("swkbd:   out[%d] = %#x", k, rd32(f->r[0] + 4 * k));
    for (int k = 0; k < 16; k++) LOG("swkbd:   arg[%#x] = %#x", 4 * k, rd32(f->r[1] + 4 * k));
    for (int k = 0; k < 4; k++) LOG("swkbd:   arg[%#x] = %#x", 0x3f0 + 4 * k, rd32(f->r[1] + 0x3f0 + 4 * k));
    f->r[0] = 0x29f;                                                    // cancelled
}
// nn::swkbd::InlineKeyboard: the applet is not there; callbacks the game registers are recorded and fired from Calc (where the SDK fires them) once the game has asked for the keyboard
static uint32_t kb_cb[8];       // 0 finished-initialize, 1 changed-string, 2 decided-enter, 3 decided-cancel, 4 finished-keyboard, 5 changed-string utf8, 6 decided-enter utf8
static int kb_state, kb_wait;   // 1 launched (finished-initialize due), 2 shown (the name is due)
static void kb_set(Frame *f, int i) { kb_cb[i] = f->r[1]; LOG("swkbd: callback %d = %#x", i, f->r[1]); f->r[0] = 0; }
static void f_kb_fin_init(Frame *f) { kb_set(f, 0); }
static void f_kb_changed(Frame *f) { kb_set(f, 1); }
static void f_kb_enter(Frame *f) { kb_set(f, 2); }
static void f_kb_cancel(Frame *f) { kb_set(f, 3); }
static void f_kb_finished(Frame *f) { kb_set(f, 4); }
static void f_kb_changed8(Frame *f) { kb_set(f, 5); }
static void f_kb_enter8(Frame *f) { kb_set(f, 6); }
uint32_t a32_kb_mgr;
#define kb_mgr a32_kb_mgr
static void kb_dump(const char *w) {
    if (!kb_mgr) return;
    LOG("swkbd: %s: ng-result %#x mgr %#x state %u cbuser %#x text-state %u len %u [0x10] %u text \"%s\"", w, rd32(kb_mgr + 0x36c0), kb_mgr, rd32(kb_mgr + 0x2ec8), rd32(kb_mgr + 0x2ed0), rd32(kb_mgr + 0x36ac), rd32(kb_mgr + 0x36b0), rd32(kb_mgr + 0x10), (char *)g_mem + kb_mgr + 0x2ed8);
}
static void f_kb_init(Frame *f) { kb_mgr = f->r[0] - 0x14; LOG("swkbd: Initialize"); kb_state = 1; kb_wait = 3; f->r[0] = 1; }
static void f_kb_imgreq(Frame *f) { wr32(f->r[1], 0x400000); wr32(f->r[2], 0x1000); f->r[0] = 0; }          // image memory (the keyboard is not drawn: the game gets a blank image)
static void f_kb_launch(Frame *f) { LOG("swkbd: Launch"); kb_state = 1; kb_wait = 2; f->r[0] = 0; }
static void f_kb_appear(Frame *f) { LOG("swkbd: Appear"); kb_state = 2; kb_wait = 30; f->r[0] = 0; }
static void f_kb_disappear(Frame *f) { LOG("swkbd: Disappear"); kb_state = 4; kb_wait = 10; f->r[0] = 0; }
static void f_kb_finalize(Frame *f) { LOG("swkbd: Finalize"); kb_dump("in Finalize"); kb_state = 0; f->r[0] = 0; }
// the Utf8 callback arguments: a text buffer (2001 bytes) followed by words from +0x7d4 (the length first; ChangedStringArgUtf8 has three more)
static uint32_t kb_arg(const char *name) {
    static uint32_t a; if (!a) a = a32_guest_alloc(0x800, 16);
    memset(g_mem + a, 0, 0x800); strncpy((char *)g_mem + a, name, 0x7d0);
    wr32(a + 0x7d4, (uint32_t)strlen(name)); wr32(a + 0x7d8, (uint32_t)strlen(name)); return a;
}
extern int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap);
// the player's text: A32_NAME when set (scripted runs), otherwise the front end's input box; no window: "Hunter"
static int kb_ask(char *out, int cap) {
    const char *n = getenv("A32_NAME"); if (n && *n) { snprintf(out, (size_t)cap, "%s", n); return 1; }
    int r = front_text_input("Hunter name", "Choose a name for your hunter.", "Names must be 10 characters or less.", "", 10, out, cap);
    if (r < 0) { snprintf(out, (size_t)cap, "Hunter"); return 1; }
    return r;
}
static void f_kb_calc(Frame *f) {
    f->r[0] = 0;
    if (!kb_state || --kb_wait > 0) return;
    int st = kb_state; kb_state = 0;
    if (st == 1) { if (kb_cb[0]) { LOG("swkbd: finished initialize"); a32_callback(kb_cb[0], 0, 0, 0, 0); } }
    else if (st == 2) {                                                 // shown: the player types the name and confirms (or cancels) it
        char text[256]; int ok = kb_ask(text, sizeof text);
        if (ok) {
            size_t n = 0; for (int chars = 0; text[n] && chars < 10; chars++) { n++; while ((text[n] & 0xc0) == 0x80) n++; } text[n] = 0;      // at most 10 characters
            uint32_t a = kb_arg(text); LOG("swkbd: name \"%s\"", text);
            if (kb_cb[5]) a32_callback(kb_cb[5], a, 0, 0, 0);
            if (kb_cb[6]) a32_callback(kb_cb[6], a, 0, 0, 0);
            kb_dump("after enter");
        } else { LOG("swkbd: cancelled"); if (kb_cb[3]) a32_callback(kb_cb[3], 0, 0, 0, 0); }
        kb_state = 4; kb_wait = getenv("A32_KB_FINISH") ? atoi(getenv("A32_KB_FINISH")) : 20;                                    // confirming or cancelling hides the keyboard by itself
    } else if (st == 4) { kb_dump("before finished"); if (kb_cb[4]) { LOG("swkbd: finished keyboard"); a32_callback(kb_cb[4], 0, 0, 0, 0); } kb_dump("after finished"); }
}

static void f_hid_bind_event(Frame *f) {
    uint32_t ev = f->r[1], clear = f->r[2], create = a32_find_symbol("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb"), sig = a32_find_symbol("_ZN2nn2os17SignalSystemEventEPNS0_15SystemEventTypeE");
    if (!create || !sig) { LOG("hid: nn::os system event functions not found"); return; }
    a32_callback(create, ev, clear, 1, 0); a32_callback(sig, ev, 0, 0, 0);
}
const ForceEnt a32_force[] = {
    { "_ZN2nn3hid27BindNpadStyleSetUpdateEventERKjPNS_2os15SystemEventTypeENS3_14EventClearModeE", f_hid_bind_event },
    { "_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", f_hid_state }, { "_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", f_hid_state },
    { "_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", f_hid_state_none }, { "_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj", f_hid_state_none },
    { "_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj", f_hid_state_none }, { "_ZN2nn3hid15GetNpadStyleSetERKj", f_hid_style },
    // the inline software keyboard: the game initialises it at startup and never needs it to show anything (no library applet exists here)
    { "_ZN2nn5swkbd14InlineKeyboard10InitializeEPv", f_kb_init },
    { "_ZN2nn5swkbd14InlineKeyboard11SetUtf8ModeEb", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard12GetMaxHeightEv", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard12SetCursorPosEi", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard12SetInputTextERKNS0_9InputTextE", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard13SetWindowModeENS1_10WindowModeE", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard14SetFooterAlphaEf", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard14SetKeytopAlphaEf", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard14SetKeytopScaleEf", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard15SetCustomizeDicEPKvjRKNS0_16CustomizeDicInfoE", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard15SetTouchEnabledEb", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard15SetUserWordInfoERKNS0_12UserWordInfoE", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard17UnsetCustomizeDicEv", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard17UnsetUserWordInfoEv", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard18SetKeytopTranslateEff", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard21SetUsbKeyboardEnabledEb", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard22SetMovedCursorCallbackEPFvPKNS0_14MovedCursorArgEE", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard23SetDecidedEnterCallbackEPFvPKNS0_15DecidedEnterArgEE", f_kb_enter },
    { "_ZN2nn5swkbd14InlineKeyboard24SetChangedStringCallbackEPFvPKNS0_16ChangedStringArgEE", f_kb_changed },
    { "_ZN2nn5swkbd14InlineKeyboard24SetDecidedCancelCallbackEPFvvE", f_kb_cancel },
    { "_ZN2nn5swkbd14InlineKeyboard26IsUsedTouchPointByKeyboardEii", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard26SetMovedCursorCallbackUtf8EPFvPKNS0_18MovedCursorArgUtf8EE", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard27SetDecidedEnterCallbackUtf8EPFvPKNS0_19DecidedEnterArgUtf8EE", f_kb_enter8 },
    { "_ZN2nn5swkbd14InlineKeyboard27SetFinishedKeyboardCallbackEPFvvE", f_kb_finished },
    { "_ZN2nn5swkbd14InlineKeyboard28SetChangedStringCallbackUtf8EPFvPKNS0_20ChangedStringArgUtf8EE", f_kb_changed8 },
    { "_ZN2nn5swkbd14InlineKeyboard29SetFinishedInitializeCallbackEPFvvE", f_kb_fin_init },
    { "_ZN2nn5swkbd14InlineKeyboard31SetReleasedUserWordInfoCallbackEPFvvE", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard4CalcEv", f_kb_calc },
    { "_ZN2nn5swkbd14InlineKeyboard6AppearERKNS0_9AppearArgE", f_kb_appear },
    { "_ZN2nn5swkbd14InlineKeyboard6LaunchEv", f_kb_launch },
    { "_ZN2nn5swkbd14InlineKeyboard8FinalizeEv", f_kb_finalize },
    { "_ZN2nn5swkbd14InlineKeyboard8GetImageEPvj", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard9DisappearEv", f_kb_disappear },
    { "_ZN2nn5swkbd14InlineKeyboard9SetVolumeEf", f_noop },
    { "_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE", f_swkbd_show },
    { "_ZN2nn5swkbd14InlineKeyboard25GetImageMemoryRequirementEPjS2_", f_kb_imgreq },
    { "nndiagStartup", f_noop },
    { "_ZN2nn4time21GetDeviceLocationNameEPNS0_12LocationNameE", f_time_loc }, { "_ZN2nn4time16LoadTimeZoneRuleEPNS0_12TimeZoneRuleERKNS0_12LocationNameE", f_noop },
    { "_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", f_time_now }, { "_ZN2nn4time26StandardNetworkSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", f_time_now },
    { "_ZN2nn4time25AdjustableUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", f_time_now },
    { "_ZN2nn4time19StandardSteadyClock19GetCurrentTimePointEPNS0_20SteadyClockTimePointE", f_time_steady }, { "_ZN2nn4time38GetStandardSteadyClockCurrentTimePointEPNS0_20SteadyClockTimePointE", f_time_steady },
    { "_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE", f_tocal },
    { "_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeERKNS0_12TimeZoneRuleE", f_tocal },
    { "_ZN2nn4time19ToCalendarTimeInUtcERKNS0_9PosixTimeE", f_tocal_utc }, { "_ZN2nn4time18ToPosixTimeFromUtcERKNS0_12CalendarTimeE", f_toposix_utc },
    { "_ZN2nn4time11ToPosixTimeEPiPNS0_9PosixTimeEiRKNS0_12CalendarTimeE", f_toposix }, { "_ZN2nn4time11ToPosixTimeEPiPNS0_9PosixTimeEiRKNS0_12CalendarTimeERKNS0_12TimeZoneRuleE", f_toposix },
    { "_ZN2nn4time26StandardNetworkSystemClock20IsAccuracySufficientEv", f_true },
    { "_ZN2nn3ngc15ProfanityFilter10InitializeEPvj", f_noop }, { "_ZN2nn3ngc15ProfanityFilter10InitializeEPvjb", f_noop },
    { "_ZN2nn3ngc15ProfanityFilter11SetMaskModeENS1_8MaskModeE", f_noop }, { "_ZN2nn3ngc15ProfanityFilter17GetContentVersionEv", f_noop },
    { "_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKDsj", f_ngc_check }, { "_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKcj", f_ngc_check },
    { "_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPDsj", f_ngc_mask }, { "_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPcj", f_ngc_mask },
    { "_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPtj", f_ngc_mask }, { "_ZN2nn3ngc15ProfanityFilter8FinalizeEv", f_noop },
    { "_ZN2nn4time10InitializeEv", f_noop },            // the time service: clocks are answered by the host's own handlers
    { "_ZN2nn4time13IsInitializedEv", f_true },
    { "_ZN2nn4time8FinalizeEv", f_noop },
    { "_ZN2nn4diag6detail9AbortImplEPKcS3_S3_iS3_z", f_abort },
    { "_ZN2nn4diag6detail9AbortImplEPKcS3_S3_iPKNS_6ResultEPKNS_2os17UserExceptionInfoES3_z", f_abort },
    { "_ZN2nn4pctl10InitializeEv", f_noop },            // parental controls: a single-player game needs none
    { "_ZN2nn3aoc10InitializeEv", f_noop },             // add-on content: no session; the owned list comes from the aoc handlers below
};
const unsigned a32_nforce = sizeof a32_force / sizeof a32_force[0];
