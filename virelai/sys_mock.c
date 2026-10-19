/*
 * virelai/sys_mock.c — host-test mock kernel (never linked into DOOM.ELF).
 *
 * Answers the same slot numbers vl_sys() carries on the guest, so the
 * shared wrappers (sys.c), the C library, and the DG_* surface exercise
 * identically on host. Backing store: raw Darwin BSD syscalls
 * (darwin_syscall.S) for files/console/mmap; fake registries for windows,
 * the event queue, and audio.
 */

#include "sys.h"

#include <string.h>
#include <stdio.h>

int64_t darwin_sys6(int64_t num, uint64_t a0, uint64_t a1, uint64_t a2,
                    uint64_t a3, uint64_t a4, uint64_t a5);

/* Darwin BSD syscall numbers used by the mock. */
enum {
    D_EXIT    = 1,
    D_READ    = 3,
    D_WRITE   = 4,
    D_OPEN    = 5,
    D_CLOSE   = 6,
    D_UNLINK  = 10,
    D_ACCESS  = 33,
    D_MUNMAP  = 73,
    D_GETTIMEOFDAY = 116,
    D_RENAME  = 128,
    D_MKDIR   = 136,
    D_MMAP    = 197,
    D_LSEEK   = 199,
};

/* Darwin flag values (distinct namespace from vl_*). */
enum {
    D_O_WRONLY = 1, D_O_RDWR = 2, D_O_CREAT = 0x200, D_O_APPEND = 8,
    D_PROT_READ = 1, D_PROT_WRITE = 2, D_PROT_EXEC = 4,
    D_MAP_PRIVATE = 0x02, D_MAP_ANON = 0x1000,
};

/* ------------------------------------------------------------------ */
/* Mock-visible state                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    int      used;
    uint32_t x, y, w, h;
    void    *surface;
    uint64_t surface_bytes;
    char     title[128];
    uint64_t presents;
} mock_win_t;

#define MOCK_MAX_WINDOWS 8

static mock_win_t mock_windows[MOCK_MAX_WINDOWS];
static uint32_t   mock_next_wid = 2;   /* kernel user windows start at 2 */

/* scripted event queue */
#define MOCK_MAX_EVENTS 64
static vl_event_t mock_events[MOCK_MAX_EVENTS];
static int mock_ev_head, mock_ev_count;

static uint64_t mock_audio_played;
static uint64_t mock_rand_seq;
static int      mock_exit_status = -1;

/* Test surface — the harness drives these. */
int      mock_window_count(void) { int n = 0; for (int i = 0; i < MOCK_MAX_WINDOWS; i++) n += mock_windows[i].used; return n; }
uint64_t mock_window_presents(uint32_t id) { return id - 2 < MOCK_MAX_WINDOWS ? mock_windows[id - 2].presents : 0; }
void    *mock_window_surface(uint32_t id, uint64_t *bytes) { *bytes = mock_windows[id - 2].surface_bytes; return mock_windows[id - 2].surface; }
const char *mock_window_title(uint32_t id) { return mock_windows[id - 2].title; }
void     mock_window_geometry(uint32_t id, uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h)
         { mock_win_t *m = &mock_windows[id - 2]; *x = m->x; *y = m->y; *w = m->w; *h = m->h; }
void     mock_push_event(uint16_t kind, uint16_t flags, uint32_t arg0, uint32_t arg1)
         { if (mock_ev_count < MOCK_MAX_EVENTS) {
               vl_event_t *e = &mock_events[(mock_ev_head + mock_ev_count) % MOCK_MAX_EVENTS];
               e->kind = kind; e->flags = flags; e->seq = (uint32_t)(mock_ev_head + mock_ev_count + 1);
               e->arg0 = arg0; e->arg1 = arg1;
               mock_ev_count++; } }
uint64_t mock_audio_bytes(void) { return mock_audio_played; }
int      mock_exit_status_get(void) { return mock_exit_status; }

/* ------------------------------------------------------------------ */
/* File layer — whole-path open through Darwin, capped like the kernel */
/* ------------------------------------------------------------------ */

static int64_t mock_file_open(uint64_t ptr, uint64_t len, uint64_t flags)
{
    char path[256];
    if (!len || len >= sizeof(path)) return -VL_EINVAL;
    memcpy(path, (const void *)(uintptr_t)ptr, len);
    path[len] = 0;
    int dflags;
    if (flags & VL_O_WRITE)
        dflags = (flags & VL_O_APPEND) ? D_O_RDWR | D_O_APPEND
                                       : D_O_RDWR;
    else
        dflags = 0; /* O_RDONLY */
    if (flags & VL_O_CREATE) dflags |= D_O_CREAT;
    return darwin_sys6(D_OPEN, (uint64_t)path, (uint64_t)dflags, 0644, 0, 0, 0);
}

static int64_t mock_file_rw(uint64_t h, uint64_t ptr, uint64_t len, int is_read)
{
    uint64_t take = len > VL_FILE_IO_MAX ? VL_FILE_IO_MAX : len;
    return darwin_sys6(is_read ? D_READ : D_WRITE, h, ptr, take, 0, 0, 0);
}

/* ------------------------------------------------------------------ */
/* mmap — window-surface tag allocates a fake surface; everything else  */
/* is a private anonymous mapping                                       */
/* ------------------------------------------------------------------ */

static int64_t mock_mmap(uint64_t addr, uint64_t len, uint64_t prot,
                         uint64_t flags)
{
    if ((addr & VL_MMAP_WINDOW_TAG) == VL_MMAP_WINDOW_TAG) {
        uint32_t id = addr & 0xff;
        if (!id || id - 2 >= MOCK_MAX_WINDOWS || !mock_windows[id - 2].used)
            return -VL_EINVAL;
        uint64_t want = (uint64_t)mock_windows[id - 2].w *
                        mock_windows[id - 2].h * 4;
        if (len < want) return -VL_EINVAL;
        if (!mock_windows[id - 2].surface) {
            void *p = (void *)(uintptr_t)darwin_sys6(
                D_MMAP, 0, len, D_PROT_READ | D_PROT_WRITE,
                D_MAP_PRIVATE | D_MAP_ANON, (uint64_t)-1, 0);
            if ((int64_t)p < 0) return (int64_t)p;
            mock_windows[id - 2].surface = p;
            mock_windows[id - 2].surface_bytes = len;
        }
        return (int64_t)(uintptr_t)mock_windows[id - 2].surface;
    }
    int dprot = 0;
    if (prot & VL_PROT_READ)  dprot |= D_PROT_READ;
    if (prot & VL_PROT_WRITE) dprot |= D_PROT_WRITE;
    if (prot & VL_PROT_EXEC)  dprot |= D_PROT_EXEC;
    return darwin_sys6(D_MMAP, addr, len, (uint64_t)dprot,
                       D_MAP_PRIVATE | D_MAP_ANON, (uint64_t)-1, 0);
}

/* ------------------------------------------------------------------ */
/* The dispatch                                                        */
/* ------------------------------------------------------------------ */

int64_t vl_sys(int64_t num, uint64_t a0, uint64_t a1, uint64_t a2,
               uint64_t a3, uint64_t a4, uint64_t a5)
{
    switch (num) {
    case VL_SYS_WRITE: {
        /* enforce the kernel's 256-byte staging bound for fidelity */
        uint64_t take = a2 > VL_WRITE_MAX ? VL_WRITE_MAX : a2;
        return darwin_sys6(D_WRITE, a0, a1, take, 0, 0, 0);
    }
    case VL_SYS_EXIT:
        mock_exit_status = (int)a0;
        return 0;
    case VL_SYS_SLEEP:
        return 0; /* tests never park on the 1 s tick */
    case VL_SYS_WIN_OPEN: {
        int i;
        for (i = 0; i < MOCK_MAX_WINDOWS; i++)
            if (!mock_windows[i].used) break;
        if (i == MOCK_MAX_WINDOWS) return -VL_ENOSPC;
        mock_win_t *m = &mock_windows[i];
        memset(m, 0, sizeof(*m));
        m->used = 1;
        m->x = (uint32_t)a0; m->y = (uint32_t)a1;
        m->w = (uint32_t)a2; m->h = (uint32_t)a3;
        return mock_next_wid + i;
    }
    case VL_SYS_WIN_PRESENT: {
        uint32_t i = (uint32_t)a0 - 2;
        if (i >= MOCK_MAX_WINDOWS || !mock_windows[i].used) return -VL_EINVAL;
        mock_windows[i].presents++;
        return 0;
    }
    case VL_SYS_WIN_RESIZE: {
        uint32_t i = (uint32_t)a0 - 2;
        if (i >= MOCK_MAX_WINDOWS || !mock_windows[i].used) return -VL_EINVAL;
        mock_windows[i].w = (uint32_t)a1;
        mock_windows[i].h = (uint32_t)a2;
        return 0;
    }
    case VL_SYS_WIN_SET_TITLE: {
        uint32_t i = (uint32_t)a0 - 2;
        if (i >= MOCK_MAX_WINDOWS || !mock_windows[i].used) return -VL_EINVAL;
        uint64_t n = a2 < sizeof(mock_windows[i].title) - 1
                   ? a2 : sizeof(mock_windows[i].title) - 1;
        memcpy(mock_windows[i].title, (const void *)(uintptr_t)a1, n);
        mock_windows[i].title[n] = 0;
        return 0;
    }
    case VL_SYS_POLL_EVENT:
    case VL_SYS_WAIT_EVENT: {
        if (mock_ev_count == 0) return 0;
        *(vl_event_t *)(uintptr_t)a0 = mock_events[mock_ev_head];
        mock_ev_head = (mock_ev_head + 1) % MOCK_MAX_EVENTS;
        mock_ev_count--;
        return 1;
    }
    case VL_SYS_FILE_OPEN:   return mock_file_open(a0, a1, (uint32_t)a2);
    case VL_SYS_FILE_READ:   return mock_file_rw(a0, a1, a2, 1);
    case VL_SYS_FILE_WRITE:  return mock_file_rw(a0, a1, a2, 0);
    case VL_SYS_FILE_CLOSE:  return darwin_sys6(D_CLOSE, a0, 0, 0, 0, 0, 0);
    case VL_SYS_FILE_DELETE: {
        char path[256];
        if (!a1 || a1 >= sizeof(path)) return -VL_EINVAL;
        memcpy(path, (const void *)(uintptr_t)a0, a1);
        path[a1] = 0;
        return darwin_sys6(D_UNLINK, (uint64_t)path, 0, 0, 0, 0, 0);
    }
    case VL_SYS_FILE_RENAME: {
        char o[256], n[256];
        uint64_t ol = a1, nl = a3;
        if (!ol || ol >= sizeof(o) || !nl || nl >= sizeof(n)) return -VL_EINVAL;
        memcpy(o, (const void *)(uintptr_t)a0, ol); o[ol] = 0;
        memcpy(n, (const void *)(uintptr_t)a2, nl); n[nl] = 0;
        return darwin_sys6(D_RENAME, (uint64_t)o, (uint64_t)n, 0, 0, 0, 0);
    }
    case VL_SYS_TIMER_SET:    return 0;
    case VL_SYS_TIMER_CANCEL: return 0;
    case VL_SYS_AUDIO_INFO: {
        vl_audio_info_t *o = (vl_audio_info_t *)(uintptr_t)a0;
        memset(o, 0, sizeof(*o));
        o->ready = 1;
        o->format = 19;   /* observed negotiated values on VZ */
        o->rate = 7;
        o->channels = 2;
        o->period_bytes = 4096;
        o->max_len = VL_AUDIO_PLAY_MAX;
        return 0;
    }
    case VL_SYS_AUDIO_PLAY: {
        if (a1 & VL_ASTREAM_FLAG) {
            uint64_t len = a1 & ~VL_ASTREAM_FLAG;
            switch (a2) {
            case VL_ASTREAM_OPEN: {
                if (len != sizeof(vl_astream_params_t) || a3) return -VL_EINVAL;
                return 1;   /* one fake seat, generation 1 */
            }
            case VL_ASTREAM_SUBMIT: {
                if (!len || len > 4096) return -VL_EINVAL;
                mock_audio_played += len;
                return (int64_t)len;
            }
            case VL_ASTREAM_START:
            case VL_ASTREAM_END:
            case VL_ASTREAM_ABORT:
                return 0;
            case VL_ASTREAM_STATUS: {
                if (len != sizeof(vl_astream_status_t)) return -VL_EINVAL;
                vl_astream_status_t *s =
                    (vl_astream_status_t *)(uintptr_t)a0;
                memset(s, 0, sizeof(*s));
                s->generation = a3;
                s->accepted = mock_audio_played;
                s->submitted = mock_audio_played;
                s->completed = mock_audio_played;
                s->slots = 4;
                s->sources = 1;
                s->peak_sources = 1;
                return 0;
            }
            default:
                return -VL_EINVAL;
            }
        }
        if (!a1 || a1 > VL_AUDIO_PLAY_MAX) return -VL_EINVAL;
        mock_audio_played += a1;
        return (int64_t)a1;
    }
    case VL_SYS_MMAP:   return mock_mmap(a0, a1, a2, a3);
    case VL_SYS_MUNMAP: return darwin_sys6(D_MUNMAP, a0, a1, 0, 0, 0, 0);
    case VL_SYS_TIME: {
        int64_t tv[2] = {0, 0};
        darwin_sys6(D_GETTIMEOFDAY, (uint64_t)tv, 0, 0, 0, 0, 0);
        return tv[0];
    }
    case VL_SYS_GETRANDOM: {
        uint64_t take = a1 > VL_GETRANDOM_MAX ? VL_GETRANDOM_MAX : a1;
        uint8_t *p = (uint8_t *)(uintptr_t)a0;
        for (uint64_t i = 0; i < take; i++)
            p[i] = (uint8_t)(mock_rand_seq++ * 2654435761u >> 24);
        return (int64_t)take;
    }
    default:
        return -VL_ENOSYS;
    }
}

/* host-side millisecond timebase for DG_GetTicksMs/DG_SleepMs */
uint64_t mock_now_ms(void)
{
    int64_t tv[2] = {0, 0};
    darwin_sys6(D_GETTIMEOFDAY, (uint64_t)tv, 0, 0, 0, 0, 0);
    return (uint64_t)tv[0] * 1000 + (uint64_t)tv[1] / 1000;
}
