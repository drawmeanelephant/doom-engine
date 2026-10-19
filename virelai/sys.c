/*
 * virelai/sys.c — semantic wrappers over vl_sys().
 *
 * One copy, both backends: on the guest vl_sys is the svc stub
 * (sys_guest.S); in host tests the mock kernel answers the same slot
 * numbers (sys_mock.c), so these wrappers and everything layered on them
 * exercise identically on both sides.
 */

#include "sys.h"

int64_t vl_write(int fd, const void *buf, uint64_t len)
{
    return vl_sys(VL_SYS_WRITE, (uint64_t)fd, (uint64_t)(uintptr_t)buf,
                  len, 0, 0, 0);
}

void vl_exit(int status)
{
    vl_sys(VL_SYS_EXIT, (uint64_t)(int64_t)status, 0, 0, 0, 0, 0);
    for (;;) { /* the kernel never returns here */ }
}

int64_t vl_sleep_ticks(uint64_t ticks)
{
    return vl_sys(VL_SYS_SLEEP, ticks, 0, 0, 0, 0, 0);
}

int64_t vl_win_open(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    return vl_sys(VL_SYS_WIN_OPEN, x, y, w, h, 0, 0);
}

int64_t vl_win_present(uint32_t id)
{
    return vl_sys(VL_SYS_WIN_PRESENT, id, 0, 0, 0, 0, 0);
}

int64_t vl_win_resize(uint32_t id, uint32_t w, uint32_t h)
{
    return vl_sys(VL_SYS_WIN_RESIZE, id, w, h, 0, 0, 0);
}

int64_t vl_win_set_title(uint32_t id, const char *text, uint64_t len)
{
    return vl_sys(VL_SYS_WIN_SET_TITLE, id, (uint64_t)(uintptr_t)text,
                  len, 0, 0, 0);
}

int64_t vl_poll_event(vl_event_t *ev)
{
    return vl_sys(VL_SYS_POLL_EVENT, (uint64_t)(uintptr_t)ev, 0, 0, 0, 0, 0);
}

int64_t vl_wait_event(vl_event_t *ev)
{
    return vl_sys(VL_SYS_WAIT_EVENT, (uint64_t)(uintptr_t)ev, 0, 0, 0, 0, 0);
}

int64_t vl_file_open(const char *path, uint64_t len, uint32_t flags)
{
    return vl_sys(VL_SYS_FILE_OPEN, (uint64_t)(uintptr_t)path, len, flags,
                  0, 0, 0);
}

int64_t vl_file_read(uint32_t h, void *buf, uint64_t len)
{
    return vl_sys(VL_SYS_FILE_READ, h, (uint64_t)(uintptr_t)buf, len,
                  0, 0, 0);
}

int64_t vl_file_write(uint32_t h, const void *buf, uint64_t len)
{
    return vl_sys(VL_SYS_FILE_WRITE, h, (uint64_t)(uintptr_t)buf, len,
                  0, 0, 0);
}

int64_t vl_file_close(uint32_t h)
{
    return vl_sys(VL_SYS_FILE_CLOSE, h, 0, 0, 0, 0, 0);
}

int64_t vl_file_delete(const char *path, uint64_t len)
{
    return vl_sys(VL_SYS_FILE_DELETE, (uint64_t)(uintptr_t)path, len,
                  0, 0, 0, 0);
}

int64_t vl_file_rename(const char *old, uint64_t oldlen,
                       const char *newp, uint64_t newlen)
{
    return vl_sys(VL_SYS_FILE_RENAME, (uint64_t)(uintptr_t)old, oldlen,
                  (uint64_t)(uintptr_t)newp, newlen, 0, 0);
}

int64_t vl_timer_set(uint64_t delay_ticks)
{
    return vl_sys(VL_SYS_TIMER_SET, delay_ticks, 0, 0, 0, 0, 0);
}

int64_t vl_timer_cancel(void)
{
    return vl_sys(VL_SYS_TIMER_CANCEL, 0, 0, 0, 0, 0, 0);
}

int64_t vl_audio_info(vl_audio_info_t *out)
{
    return vl_sys(VL_SYS_AUDIO_INFO, (uint64_t)(uintptr_t)out, 0, 0, 0, 0, 0);
}

int64_t vl_audio_play(const void *pcm, uint64_t len)
{
    return vl_sys(VL_SYS_AUDIO_PLAY, (uint64_t)(uintptr_t)pcm, len,
                  0, 0, 0, 0);
}

void *vl_mmap(uint64_t addr_hint, uint64_t len, uint32_t prot, uint32_t flags)
{
    int64_t r = vl_sys(VL_SYS_MMAP, addr_hint, len, prot, flags, 0, 0);
    if (r < 0) return (void *)0;
    return (void *)(uintptr_t)r;
}

int64_t vl_munmap(void *addr, uint64_t len)
{
    return vl_sys(VL_SYS_MUNMAP, (uint64_t)(uintptr_t)addr, len, 0, 0, 0, 0);
}

int64_t vl_time(void)
{
    return vl_sys(VL_SYS_TIME, 0, 0, 0, 0, 0, 0);
}

int64_t vl_getrandom(void *buf, uint64_t len)
{
    return vl_sys(VL_SYS_GETRANDOM, (uint64_t)(uintptr_t)buf, len,
                  0, 0, 0, 0);
}

void *vl_mmap_window_surface(uint32_t id, uint64_t bytes)
{
    uint64_t n = (bytes + 4095) & ~4095ULL;
    return vl_mmap(VL_MMAP_WINDOW_TAG | id, n,
                   VL_PROT_READ | VL_PROT_WRITE,
                   VL_MAP_ANONYMOUS | VL_MAP_SHARED);
}
