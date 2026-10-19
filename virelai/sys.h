/*
 * virelai/sys.h — the VirelaiOS EL0 syscall seam, restated for this repo.
 *
 * Per VirelaiOS ADR 0058 D3 the ENTIRE contract between this engine and the
 * kernel is the syscall ABI: a trap instruction (`svc #0`) carrying a slot
 * number in x8 and up to six word arguments in x0..x5, whose result lands in
 * x0 with a negative value naming a kernel errno. These numbers and the
 * register convention are interface facts; this header is this repo's own
 * declaration of them — no VirelaiOS source is copied, translated, or linked.
 *
 * vl_sys() is the single trap. On the guest it is the svc stub
 * (sys_guest.S); in host tests a mock kernel answers it (sys_mock.c), so the
 * same wrappers and the same C library exercise both sides.
 */

#ifndef VIRELAI_SYS_H
#define VIRELAI_SYS_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* The trap.                                                           */
/* ------------------------------------------------------------------ */

int64_t vl_sys(int64_t num,
               uint64_t a0, uint64_t a1, uint64_t a2,
               uint64_t a3, uint64_t a4, uint64_t a5);

/* Slot numbers. */
enum {
    VL_SYS_WRITE        = 1,   /* write(fd, buf, len) — ≤256 B per call    */
    VL_SYS_EXIT         = 3,   /* exit(status)                             */
    VL_SYS_SLEEP        = 4,   /* sleep(scheduler ticks; 1 s each on VZ)   */
    VL_SYS_WIN_OPEN     = 12,  /* win_open(x, y, w, h) → window id         */
    VL_SYS_WIN_PRESENT  = 14,  /* win_present(id)                          */
    VL_SYS_POLL_EVENT   = 21,  /* poll_event(*Event) → 1/0                 */
    VL_SYS_WAIT_EVENT   = 22,  /* wait_event(*Event) → 1 or blocks         */
    VL_SYS_FILE_OPEN    = 23,  /* file_open(path, len, flags) → handle     */
    VL_SYS_FILE_READ    = 24,  /* file_read(h, buf, len) → ≤2048           */
    VL_SYS_FILE_WRITE   = 25,  /* file_write(h, buf, len) → ≤2048          */
    VL_SYS_FILE_CLOSE   = 26,  /* file_close(h)                            */
    VL_SYS_FILE_DELETE  = 34,  /* file_delete(path, len)                   */
    VL_SYS_FILE_RENAME  = 35,  /* file_rename(old, len, new, len)          */
    VL_SYS_TIMER_SET    = 40,  /* timer_set(delay_ticks) → one TIMER event */
    VL_SYS_TIMER_CANCEL = 41,  /* timer_cancel()                           */
    VL_SYS_AUDIO_INFO   = 42,  /* audio_info(*AudioInfo, 16 B)             */
    VL_SYS_AUDIO_PLAY   = 43,  /* audio_play(pcm, len) → bytes, ≤64 KiB    */
    VL_SYS_WIN_RESIZE   = 47,  /* win_resize(id, w, h)                     */
    VL_SYS_WIN_SET_TITLE = 61, /* win_set_title(id, ptr, len)              */
    VL_SYS_MMAP         = 63,  /* mmap(addr_hint, len, prot, flags) → VA   */
    VL_SYS_MUNMAP       = 64,  /* munmap(addr, len)                        */
    VL_SYS_TIME         = 66,  /* time() → unix seconds                    */
    VL_SYS_GETRANDOM    = 72,  /* getrandom(buf, len) → ≤256               */
};

/* Error magnitudes the ABI reports (x0 = -errno). */
enum {
    VL_EPERM    = 1,
    VL_ENOENT   = 2,
    VL_EFAULT   = 3,
    VL_EBADF    = 4,
    VL_ENOSPC   = 5,
    VL_EACCES   = 7,
    VL_EEXIST   = 9,
    VL_ENOMEM   = 10,
    VL_EINVAL   = 11,
    VL_ENAMETOOLONG = 14,
    VL_EAGAIN   = 16,
    VL_ENXIO    = 19,
    VL_ENOSYS   = 38,
};

/* mmap protection bits. */
enum {
    VL_PROT_READ  = 1,
    VL_PROT_WRITE = 2,
    VL_PROT_EXEC  = 4,
};

/* mmap flag bits. */
enum {
    VL_MAP_PRIVATE   = 0x00002,
    VL_MAP_ANONYMOUS = 0x00020,
    VL_MAP_POPULATE  = 0x08000,
    VL_MAP_SHARED    = 0x10000,
};

/*
 * Binding a window's backing surface is a tagged mmap: the address word is
 * not an address at all but this tag OR'd with the window id (low 8 bits);
 * the kernel picks the VA and returns it. The surface is shared-anon and
 * the compositor presents exactly what the owner writes into it.
 */
#define VL_MMAP_WINDOW_TAG 0x8000000000000000ULL

/* File-open mode flags. */
enum {
    VL_O_READ   = 0x01,
    VL_O_WRITE  = 0x02,
    VL_O_CREATE = 0x04,
    VL_O_APPEND = 0x08,
    VL_O_DIR    = 0x10,
};

/* The kernel's per-call staging bounds. */
enum {
    VL_WRITE_MAX      = 256,
    VL_FILE_IO_MAX    = 2048,
    VL_GETRANDOM_MAX  = 256,
    VL_AUDIO_PLAY_MAX = 65536,
};

/* The normative 16-byte application event. */
typedef struct vl_event {
    uint16_t kind;
    uint16_t flags;
    uint32_t seq;
    uint32_t arg0;
    uint32_t arg1;
} vl_event_t;

enum {
    VL_EV_KEY_DOWN     = 1,   /* arg0 = HID usage, arg1 = codepoint       */
    VL_EV_KEY_UP       = 2,
    VL_EV_WIN_CLOSE    = 8,
    VL_EV_TIMER        = 9,
    VL_EV_WIN_RESIZE   = 10,  /* arg0 = new width, arg1 = new height      */
};

/* Modifier bits in event flags. */
enum {
    VL_MOD_SHIFT = 0x0001,
    VL_MOD_CTRL  = 0x0002,
    VL_MOD_ALT   = 0x0004,
    VL_MOD_CMD   = 0x0008,
};

/* sys_audio_info's 16-byte negotiated playback state. */
typedef struct vl_audio_info {
    uint32_t ready;
    uint8_t  format;
    uint8_t  rate;
    uint8_t  channels;
    uint8_t  padding;
    uint32_t period_bytes;
    uint32_t max_len;
} vl_audio_info_t;

/* ------------------------------------------------------------------ */
/* Semantic wrappers — one C translation unit, both backends.          */
/* ------------------------------------------------------------------ */

int64_t  vl_write(int fd, const void *buf, uint64_t len);
void     vl_exit(int status);
int64_t  vl_sleep_ticks(uint64_t ticks);
int64_t  vl_win_open(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
int64_t  vl_win_present(uint32_t id);
int64_t  vl_win_resize(uint32_t id, uint32_t w, uint32_t h);
int64_t  vl_win_set_title(uint32_t id, const char *text, uint64_t len);
int64_t  vl_poll_event(vl_event_t *ev);
int64_t  vl_wait_event(vl_event_t *ev);
int64_t  vl_file_open(const char *path, uint64_t len, uint32_t flags);
int64_t  vl_file_read(uint32_t h, void *buf, uint64_t len);
int64_t  vl_file_write(uint32_t h, const void *buf, uint64_t len);
int64_t  vl_file_close(uint32_t h);
int64_t  vl_file_delete(const char *path, uint64_t len);
int64_t  vl_file_rename(const char *old, uint64_t oldlen,
                        const char *newp, uint64_t newlen);
int64_t  vl_timer_set(uint64_t delay_ticks);
int64_t  vl_timer_cancel(void);
int64_t  vl_audio_info(vl_audio_info_t *out);
int64_t  vl_audio_play(const void *pcm, uint64_t len);
void    *vl_mmap(uint64_t addr_hint, uint64_t len, uint32_t prot,
                 uint32_t flags);
int64_t  vl_munmap(void *addr, uint64_t len);
int64_t  vl_time(void);
int64_t  vl_getrandom(void *buf, uint64_t len);

/* Map a window's backing surface; returns the kernel-chosen VA or NULL. */
void    *vl_mmap_window_surface(uint32_t id, uint64_t bytes);

#endif /* VIRELAI_SYS_H */
