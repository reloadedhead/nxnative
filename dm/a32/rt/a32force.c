// Forced take-over of SDK functions by name: a reference to one of these symbols is bound to the host handler instead of the module's
// definition (the same idea as dm/host's HLE_FORCE). The handler gets the guest registers in the frame (r0-r3 = arguments, stack arguments at
// [sp]) and leaves the result in r0 (r0:r1 for 64-bit values).
#define _DARWIN_C_SOURCE
#include "a32host.h"
#include "a32force.h"
#include <time.h>
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
static void f_ngc_check(Frame *f) { if (f->r[0]) wr32(f->r[0], 0); f->r[0] = 0; }
static void f_ngc_mask(Frame *f) { if (f->r[0]) wr32(f->r[0], 0); f->r[0] = 0; }
// nn::diag::detail::AbortImpl(condition, function, file, line, ...): print what the SDK aborts on, then stop. The arguments after `line` are on the stack.
static void f_abort(Frame *f) {
    const char *c = f->r[0] ? gstr(f->r[0]) : "?", *fn = f->r[1] ? gstr(f->r[1]) : "?", *file = f->r[2] ? gstr(f->r[2]) : "?";
    fprintf(stderr, "[a32] SDK ABORT: %s\n   in %s at %s:%d\n", c, fn, file, (int)f->r[3]);
    for (int k = 0; k < 3; k++) { uint32_t v = rd32(f->r[13] + 4 * k); if (v > 0x100000 && v < 0x10000000 && !(v & 0xf0000000)) { const char *t = gstr(v); int ok = 1; for (int i = 0; i < 40 && t[i]; i++) if ((unsigned char)t[i] < 9) ok = 0; if (ok && t[0]) fprintf(stderr, "   arg: %.100s\n", t); } }
    dump_regs(f); fflush(stderr); _exit(6);
}
const ForceEnt a32_force[] = {
    // the inline software keyboard: the game initialises it at startup and never needs it to show anything (no library applet exists here)
    { "_ZN2nn5swkbd14InlineKeyboard10InitializeEPv", f_noop },
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
    { "_ZN2nn5swkbd14InlineKeyboard23SetDecidedEnterCallbackEPFvPKNS0_15DecidedEnterArgEE", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard24SetChangedStringCallbackEPFvPKNS0_16ChangedStringArgEE", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard24SetDecidedCancelCallbackEPFvvE", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard26IsUsedTouchPointByKeyboardEii", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard26SetMovedCursorCallbackUtf8EPFvPKNS0_18MovedCursorArgUtf8EE", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard27SetDecidedEnterCallbackUtf8EPFvPKNS0_19DecidedEnterArgUtf8EE", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard27SetFinishedKeyboardCallbackEPFvvE", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard28SetChangedStringCallbackUtf8EPFvPKNS0_20ChangedStringArgUtf8EE", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard29SetFinishedInitializeCallbackEPFvvE", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard31SetReleasedUserWordInfoCallbackEPFvvE", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard4CalcEv", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard6AppearERKNS0_9AppearArgE", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard6LaunchEv", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard8FinalizeEv", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard8GetImageEPvj", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard9DisappearEv", f_noop },
    { "_ZN2nn5swkbd14InlineKeyboard9SetVolumeEf", f_noop },
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
