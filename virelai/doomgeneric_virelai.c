/*
 * virelai/doomgeneric_virelai.c — the DG_* platform surface on the
 * VirelaiOS seam.
 *
 *   DG_Init            — open a 640x400 window, bind its shared-anon
 *                        surface, aim DG_ScreenBuffer at it
 *   DG_DrawFrame       — win_present (the surface IS the frame)
 *   DG_GetKey          — drain poll_event; HID usage -> doomkeys codes
 *   DG_GetTicksMs      — hardware counter (EL0PCTEN-guaranteed) on guest;
 *                        mock timebase on host
 *   DG_SleepMs         — counter busy-wait (sys_sleep ticks are 1 s)
 *   DG_SetWindowTitle  — win_set_title
 *   PCM submit         — vl_audio_play wrapper; the M130d sound module
 *                        (virelai/vl_sound.c) uses the slot-43 stream ABI
 *                        (vl_audio_stream_* in sys.h), not this legacy
 *                        blast path
 *
 * M130c: the in-guest run is choreographed by serial markers printed from
 * this file (vldoom: boot / win / level / key / f=N t=T), so a host-side
 * driver can anchor input injection and framebuffer snapshots without a
 * display round trip.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "sys.h"
#include "doomgeneric.h"
#include "doomkeys.h"
#include "doomstat.h"   /* gamestate, leveltime */

#define VL_WIN_W 640
#define VL_WIN_H 400

static uint32_t win_id;
static int      quit_requested;
static unsigned char held_code[256]; /* HID usage -> doom code emitted at key-down */

/* ------------------------------------------------------------------ */
/* timebase                                                            */
/* ------------------------------------------------------------------ */

#ifdef VIRELAI_GUEST

static uint64_t vl_cntpct(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

static uint64_t vl_cntfrq(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static uint64_t vl_now_ms(void)
{
    uint64_t f = vl_cntfrq();
    if (!f) return 0;
    return vl_cntpct() * 1000 / f;
}

#else

extern uint64_t mock_now_ms(void);   /* sys_mock.c */

static uint64_t vl_now_ms(void) { return mock_now_ms(); }

#endif

uint32_t DG_GetTicksMs(void)
{
    return (uint32_t)vl_now_ms();
}

void DG_SleepMs(uint32_t ms)
{
    uint64_t end = vl_now_ms() + ms;
    while (vl_now_ms() < end) { }
}

/* ------------------------------------------------------------------ */
/* keys                                                                */
/* ------------------------------------------------------------------ */

/* HID boot-protocol usage + modifier flags -> doomkeys.h code.
 * 0 = unmapped.
 *
 * VirelaiOS keyboard events carry the HID boot-report modifier byte as
 * event flags and report usages in arg0 — a bare Ctrl press never appears
 * as a usage, so DOOM's fire key (bound to KEY_FIRE by default) can only
 * be reached through a ctrl-modified stroke. Upstream ports map a real
 * Ctrl keypress to KEY_FIRE at this same layer (doomgeneric_sdl.c); here
 * any ctrl-flagged event is the fire key. Likewise Space is the use key
 * (KEY_USE), matching upstream — a literal ' ' reaches DOOM nowhere.
 *
 * WASD are bound as movement aliases for the arrows: scripted input can
 * hold a letter key across tics, which an arrow chord cannot do on the
 * custom-virtio input channel. */
static unsigned char vl_hid_to_doom(unsigned int usage, unsigned int flags)
{
    if (flags & VL_MOD_CTRL) return KEY_FIRE;
    if (flags & VL_MOD_ALT)  return KEY_RALT;   /* strafe modifier */
    switch (usage) {
    case 0x28: return KEY_ENTER;
    case 0x29: return KEY_ESCAPE;
    case 0x2a: return KEY_BACKSPACE;
    case 0x2b: return KEY_TAB;
    case 0x2c: return KEY_USE;              /* Space = use */
    case 0x2d: return KEY_MINUS;
    case 0x2e: return KEY_EQUALS;
    case 0x2f: return '[';
    case 0x30: return ']';
    case 0x31: return '\\';
    case 0x33: return ';';
    case 0x34: return '\'';
    case 0x35: return '`';
    case 0x36: return ',';
    case 0x37: return '.';
    case 0x38: return '/';
    case 0x39: return KEY_CAPSLOCK;
    case 0x3a: return KEY_F1;
    case 0x3b: return KEY_F2;
    case 0x3c: return KEY_F3;
    case 0x3d: return KEY_F4;
    case 0x3e: return KEY_F5;
    case 0x3f: return KEY_F6;
    case 0x40: return KEY_F7;
    case 0x41: return KEY_F8;
    case 0x42: return KEY_F9;
    case 0x43: return KEY_F10;
    case 0x44: return KEY_F11;
    case 0x45: return KEY_F12;
    case 0x46: return KEY_PRTSCR;
    case 0x47: return KEY_SCRLCK;
    case 0x48: return KEY_PAUSE;
    case 0x49: return KEY_INS;
    case 0x4a: return KEY_HOME;
    case 0x4b: return KEY_PGUP;
    case 0x4c: return 0x7f;               /* DEL forward -> backspace   */
    case 0x4d: return KEY_END;
    case 0x4e: return KEY_PGDN;
    case 0x4f: return KEY_RIGHTARROW;
    case 0x50: return KEY_LEFTARROW;
    case 0x51: return KEY_DOWNARROW;
    case 0x52: return KEY_UPARROW;
    case 0x53: return KEY_NUMLOCK;
    case 0xe0: case 0xe4: return KEY_FIRE;  /* bare Ctrl if it ever lands */
    case 0xe1: case 0xe5: return KEY_RSHIFT;
    case 0xe2: case 0xe6: return KEY_RALT;
    case 0xe3: case 0xe7: return 0x80 + 0x5b;  /* GUI/CMD — unbound  */
    default: break;
    }
    /* 'f' is a second fire binding: DOOM attacks while the fire key is
     * held, and the gate's --input-key can hold a letter usage down (it
     * cannot hold a ctrl chord), so this gives scripted input a
     * deterministic held fire. 'f' is unbound in vanilla gameplay. */
    if (usage == 0x09) return KEY_FIRE;

    /* WASD movement aliases (ahead of the letter fallthrough). */
    if (usage == 0x1a) return KEY_UPARROW;        /* w */
    if (usage == 0x16) return KEY_DOWNARROW;      /* s */
    if (usage == 0x04) return KEY_LEFTARROW;      /* a */
    if (usage == 0x07) return KEY_RIGHTARROW;     /* d */
    if (usage >= 0x04 && usage <= 0x1d) return (unsigned char)('a' + usage - 0x04);
    if (usage >= 0x1e && usage <= 0x26) return (unsigned char)('1' + usage - 0x1e);
    if (usage == 0x27) return '0';
    return 0;
}

/* small pending-key ring fed by drained events */
#define KEYQ 64
static unsigned short keyq[KEYQ];   /* bit15 = pressed, low byte = key  */
static int keyq_head, keyq_count;

static void keyq_push(int pressed, unsigned char key)
{
    if (keyq_count >= KEYQ) return;
    keyq[(keyq_head + keyq_count) % KEYQ] =
        (unsigned short)((pressed ? 0x8000 : 0) | key);
    keyq_count++;
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    /* drain every queued event first so a burst cannot starve frames */
    vl_event_t ev;
    while (vl_poll_event(&ev) == 1) {
        switch (ev.kind) {
        case VL_EV_KEY_DOWN:
        case VL_EV_KEY_UP: {
            /* A release report carries no modifiers (all-zero HID
             * report), so the key-up must echo whatever code the
             * matching key-down emitted — otherwise a ctrl-chord's
             * release would unbind the bare letter and leave the fire
             * key latched. */
            unsigned char k;
            if (ev.kind == VL_EV_KEY_DOWN) {
                k = vl_hid_to_doom(ev.arg0, ev.flags);
                held_code[ev.arg0 & 0xff] = k;
            } else {
                k = held_code[ev.arg0 & 0xff];
                held_code[ev.arg0 & 0xff] = 0;
            }
            if (k) {
                keyq_push(ev.kind == VL_EV_KEY_DOWN, k);
                printf("vldoom: key %s hid=0x%02x doom=0x%02x\n",
                       ev.kind == VL_EV_KEY_DOWN ? "dn" : "up",
                       ev.arg0, k);
            }
            break;
        }
        case VL_EV_WIN_CLOSE:
            quit_requested = 1;
            break;
        default:
            break;   /* TIMER / WIN_RESIZE / mouse — M130c's surface   */
        }
    }
    if (keyq_count == 0) return 0;
    unsigned short v = keyq[keyq_head];
    keyq_head = (keyq_head + 1) % KEYQ;
    keyq_count--;
    *pressed = (v & 0x8000) != 0;
    *key = (unsigned char)(v & 0xff);
    return 1;
}

/* ------------------------------------------------------------------ */
/* frame                                                               */
/* ------------------------------------------------------------------ */

void DG_Init(void)
{
    /* center the 640x400 frame on the 1280x720 scanout */
    int64_t wid = vl_win_open((1280 - VL_WIN_W) / 2, (720 - VL_WIN_H) / 2,
                              VL_WIN_W, VL_WIN_H);
    if (wid < 0) {
        fprintf(stderr, "doomgeneric: win_open failed: %ld\n", (long)wid);
        exit(1);
    }
    win_id = (uint32_t)wid;

    void *surface =
        vl_mmap_window_surface(win_id, (uint64_t)VL_WIN_W * VL_WIN_H * 4);
    if (!surface) {
        fprintf(stderr, "doomgeneric: window surface bind failed\n");
        exit(1);
    }

    /* DG_ScreenBuffer IS the window surface — zero-copy present. */
    free(DG_ScreenBuffer);
    DG_ScreenBuffer = (pixel_t *)surface;

    vl_win_set_title(win_id, "DOOM", 4);
    printf("vldoom: win id=%u %ux%u\n", win_id, VL_WIN_W, VL_WIN_H);
}

static uint64_t frame_count;
static int      level_seen;

void DG_DrawFrame(void)
{
    vl_win_present(win_id);
    frame_count++;
    if (!level_seen && gamestate == GS_LEVEL) {
        level_seen = 1;
        printf("vldoom: level\n");
    }
    /* One heartbeat per presented 35-frame second — `t=` carries leveltime
     * (in-level tics), so a driver can see the game loop holding its tic
     * rate, not just frames landing. */
    if (frame_count % 35 == 0)
        printf("vldoom: f=%llu t=%d\n",
               (unsigned long long)frame_count, leveltime);
}

void DG_SetWindowTitle(const char *title)
{
    vl_win_set_title(win_id, title, strlen(title));
}

/* ------------------------------------------------------------------ */
/* PCM seam — legacy submit wrappers; the sound module lives in         */
/* virelai/vl_sound.c on the stream ABI                                 */
/* ------------------------------------------------------------------ */

/* Query the negotiated playback shape; returns 0 or negative errno. */
int64_t vl_audio_query(vl_audio_info_t *out)
{
    return vl_audio_info(out);
}

/* Submit one bounded PCM blast; returns bytes accepted or <0. The M130d
 * module streams through vl_audio_stream_submit instead. */
int64_t vl_audio_submit(const void *pcm, uint64_t len)
{
    return vl_audio_play(pcm, len);
}

/* ------------------------------------------------------------------ */
/* guest entry                                                         */
/* ------------------------------------------------------------------ */

/* _start lands here with x0 = argc, x1 = the argv block (8 slots of
 * 256 NUL-terminated bytes). */
void vl_entry(uint64_t argc, uint64_t argv_block)
{
    char *argv[9];
    uint64_t i;
    uint64_t n = argc > 8 ? 8 : argc;

    for (i = 0; i < n; i++)
        argv[i] = (char *)(uintptr_t)(argv_block + i * 256);
    argv[n] = NULL;

    printf("vldoom: boot argc=%llu\n", (unsigned long long)argc);

    doomgeneric_Create((int)n, argv);

    for (;;) {
        doomgeneric_Tick();
        if (quit_requested) vl_exit(0);
        DG_SleepMs(1);
    }
}
