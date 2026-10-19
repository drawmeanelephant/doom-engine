/*
 * virelai/vl_sound.c — M130d: the DG_* sound modules on the sys_audio seam.
 *
 * DG_sound_module decodes DMX PCM lumps out of the WAD (8-byte header:
 * u16 format, u16 rate, u32 count, then unsigned 8-bit mono samples),
 * mixes the active channels in software with left/right separation, and
 * streams the result to the kernel mixer as bounded periods through the
 * slot-43 bit-63 stream ABI in whatever tuple sys_audio_info negotiated
 * (FLOAT/stereo/48 kHz on the observed device; S16 also handled).
 *
 * Each Update() — called by the engine once per game tic — first drains
 * the small pending buffer into the seat's 4-period queue, then mixes and
 * submits new periods until the queue is saturated (EAGAIN) or the per-tic
 * budget is spent. Positions advance only with samples that enter a
 * period, so a full queue stalls the mix instead of dropping sound.
 *
 * DG_music_module refuses by name: there is no synthesizer on this
 * platform, so it logs one line at init and every op is an inert no-op.
 *
 * Serial markers ("vldoom:") are the class-B evidence channel:
 *   snd  — negotiated tuple + seat generation, or the named refusal
 *   sfx  — one decoded DMX lump bound to a channel
 *   pcm  — throttled byte counters (submitted/nonzero) + seat counters
 *   music unsupported — the card's named refusal
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "doomtype.h"
#include "deh_str.h"
#include "i_sound.h"
#include "m_misc.h"
#include "w_wad.h"
#include "z_zone.h"

#include "sys.h"

/* Config knobs i_sound.c/m_config.c reference under FEATURE_SOUND. The
 * libsamplerate pair is inert here — this module resamples on its own. */
int   use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;
char *timidity_cfg_path = "";

/* s_sound allocates snd_channels (default 8) slots and passes the slot
 * index as the channel argument; the handle we return is that index. */
#define VL_SND_CHANNELS 8

/* Pending periods staged ahead of the kernel queue: enough to ride out a
 * stalled Update without dropping sound, small enough to keep latency
 * sub-frame. */
#define VL_PEND_MAX   4
#define VL_PERIOD_MAX 8192

/* Per-tic generation budget (periods). At 48 kHz stereo one period is
 * ~10.7 ms and a tic is ~28.6 ms, so the drain between Updates frees
 * ~2.7 queue slots; the budget covers that plus a catch-up margin. */
#define VL_UPD_BUDGET 4

typedef struct {
    uint32_t rate;              /* lump's own sample rate */
    uint32_t len;               /* decoded sample count */
    uint8_t  data[];            /* unsigned 8-bit mono PCM */
} vl_sfx_t;

typedef struct {
    const uint8_t *data;
    uint32_t len;
    uint64_t step;              /* 16.16 source samples per output frame */
    uint64_t pos;               /* 16.16 cursor into data */
    uint16_t lvol, rvol;        /* 0..255 pan gains */
    int      active;
} vl_chan_t;

static boolean   use_sfx_prefix;
static int       snd_live;       /* seat open + stream started */
static uint64_t  snd_gen;        /* the kernel's seat generation */
static uint8_t   snd_fmt, snd_ch;
static uint32_t  snd_hz;         /* negotiated rate in Hz */
static uint32_t  snd_period;     /* period_bytes the seat accepts */
static uint32_t  snd_framelen;   /* bytes per output frame */

static vl_chan_t chans[VL_SND_CHANNELS];

static uint8_t   pend[VL_PEND_MAX][VL_PERIOD_MAX];
static int       pend_head, pend_n;
static uint8_t   scratch[VL_PERIOD_MAX];

static uint64_t  stat_submitted; /* bytes accepted by the seat */
static uint64_t  stat_nonzero;   /* bytes carrying mixed sfx */
static uint64_t  stat_refused;   /* EAGAIN submits (queue full) */
static uint32_t  stat_marker;    /* throttle: submissions since last marker */

static uint32_t rate_code_hz(uint8_t code)
{
    switch (code) {
    case VL_ARATE_8000:  return 8000;
    case VL_ARATE_16000: return 16000;
    case VL_ARATE_22050: return 22050;
    case VL_ARATE_32000: return 32000;
    case VL_ARATE_44100: return 44100;
    case VL_ARATE_48000: return 48000;
    default: return 0;
    }
}

/* ------------------------------------------------------------------ */
/* DMX lump decode — cached on sfxinfo->driver_data                     */
/* ------------------------------------------------------------------ */

static vl_sfx_t *vl_sfx_decode(sfxinfo_t *sfx)
{
    if (sfx->driver_data) return sfx->driver_data;
    if (sfx->lumpnum < 0) return NULL;

    int lumpnum = sfx->lumpnum;
    uint8_t *data = W_CacheLumpNum(lumpnum, PU_STATIC);
    unsigned int lumplen = W_LumpLength(lumpnum);

    /* Header: u16 format (3), u16 rate, u32 count — then unsigned 8-bit
     * samples. DMX lumps shorter than ~49 samples and ones whose declared
     * length overruns the lump are refused, matching upstream behavior,
     * as is the first-16/last-16 byte skip. */
    if (lumplen < 8 || data[0] != 0x03 || data[1] != 0x00) {
        W_ReleaseLumpNum(lumpnum);
        return NULL;
    }
    uint32_t rate = (data[3] << 8) | data[2];
    uint32_t len  = (data[7] << 24) | (data[6] << 16) |
                    (data[5] << 8) | data[4];
    if (len > lumplen - 8 || len <= 48) {
        W_ReleaseLumpNum(lumpnum);
        return NULL;
    }
    data += 16;
    len -= 32;
    const uint8_t *samples = data + 8;

    vl_sfx_t *s = malloc(sizeof(vl_sfx_t) + len);
    if (!s) {
        W_ReleaseLumpNum(lumpnum);
        return NULL;
    }
    s->rate = rate;
    s->len = len;
    memcpy(s->data, samples, len);
    W_ReleaseLumpNum(lumpnum);
    sfx->driver_data = s;
    return s;
}

static void vl_sfx_lumpname(sfxinfo_t *sfx, char *buf, size_t buflen)
{
    if (sfx->link) sfx = sfx->link;
    if (use_sfx_prefix)
        M_snprintf(buf, buflen, "ds%s", DEH_String(sfx->name));
    else
        M_StringCopy(buf, DEH_String(sfx->name), buflen);
}

/* ------------------------------------------------------------------ */
/* mixing — sum the active channels into f32 L/R, encode to the        */
/* negotiated tuple                                                    */
/* ------------------------------------------------------------------ */

/* Mix one output frame into l/r; nonzero return means a channel
 * actually contributed (the period carries real sfx). */
static int mix_frame(float *l, float *r)
{
    float al = 0.0f, ar = 0.0f;
    int used = 0;
    for (int i = 0; i < VL_SND_CHANNELS; i++) {
        vl_chan_t *c = &chans[i];
        if (!c->active) continue;
        uint32_t idx = (uint32_t)(c->pos >> 16);
        if (idx >= c->len) {
            c->active = 0;
            continue;
        }
        /* linear interp between idx and idx+1 (clamped at the tail) */
        uint32_t frac = (uint32_t)(c->pos & 0xffff);
        float a = (float)c->data[idx] - 128.0f;
        float s = a;
        if (idx + 1 < c->len) {
            float b = (float)c->data[idx + 1] - 128.0f;
            s = a + (b - a) * ((float)frac / 65536.0f);
        }
        s /= 128.0f;
        al += s * ((float)c->lvol / 255.0f);
        ar += s * ((float)c->rvol / 255.0f);
        c->pos += c->step;
        used = 1;
    }
    if (al > 1.0f) al = 1.0f; else if (al < -1.0f) al = -1.0f;
    if (ar > 1.0f) ar = 1.0f; else if (ar < -1.0f) ar = -1.0f;
    *l = al;
    *r = ar;
    return used;
}

/* Mix one period's worth of frames into dst (snd_period bytes) in the
 * negotiated tuple. Returns nonzero if any channel contributed. */
static int mix_period(uint8_t *dst)
{
    uint32_t frames = snd_period / snd_framelen;
    int used = 0;
    uint8_t *p = dst;

    for (uint32_t i = 0; i < frames; i++) {
        float l, r;
        used |= mix_frame(&l, &r);
        switch (snd_fmt) {
        case VL_AFMT_FLOAT: {
            float mono = (l + r) * 0.5f;
            if (snd_ch == 1) { l = r = mono; }
            memcpy(p, &l, 4); p += 4;
            if (snd_ch == 2) { memcpy(p, &r, 4); p += 4; }
            break;
        }
        case VL_AFMT_S32: {
            int32_t sl = (int32_t)(l * 2147483647.0f);
            int32_t sr = (int32_t)(r * 2147483647.0f);
            if (snd_ch == 1) { sl = (int32_t)((l + r) * 0.5f * 2147483647.0f); sr = sl; }
            memcpy(p, &sl, 4); p += 4;
            if (snd_ch == 2) { memcpy(p, &sr, 4); p += 4; }
            break;
        }
        default: { /* VL_AFMT_S16 */
            int16_t sl = (int16_t)(l * 32767.0f);
            int16_t sr = (int16_t)(r * 32767.0f);
            if (snd_ch == 1) { sl = (int16_t)((l + r) * 0.5f * 32767.0f); sr = sl; }
            memcpy(p, &sl, 2); p += 2;
            if (snd_ch == 2) { memcpy(p, &sr, 2); p += 2; }
            break;
        }
        }
    }
    return used;
}

/* ------------------------------------------------------------------ */
/* submission pump                                                      */
/* ------------------------------------------------------------------ */

static void pcm_marker(void)
{
    vl_astream_status_t st;
    memset(&st, 0, sizeof(st));
    vl_audio_stream_status(snd_gen, &st);
    printf("vldoom: pcm submitted=%llu nonzero=%llu completed=%llu "
           "underruns=%llu refused=%llu\n",
           (unsigned long long)stat_submitted,
           (unsigned long long)stat_nonzero,
           (unsigned long long)st.completed,
           (unsigned long long)st.underruns,
           (unsigned long long)stat_refused);
}

/* Submit one full period to the seat's queue. Returns 1 consumed, 0 on
 * EAGAIN (queue full or stream between phases — retry later), -1 dead. */
static int submit_raw(const uint8_t *period)
{
    int64_t r = vl_audio_stream_submit(snd_gen, period, snd_period);
    if (r == (int64_t)snd_period) {
        stat_submitted += snd_period;
        return 1;
    }
    if (r == -VL_EAGAIN) {
        stat_refused++;
        return 0;
    }
    snd_live = 0;
    printf("vldoom: snd submit rc=%ld — seat dead\n", (long)r);
    return -1;
}

/* Freshly mixed periods: like submit_raw, but EAGAIN stages the period
 * at the pending tail (callers guarantee room) rather than dropping it.
 * Never call this on an already-pending period — see VL_Update's flush,
 * where the entry is already queued and must not be duplicated. */
static int submit_period(const uint8_t *period)
{
    int rc = submit_raw(period);
    if (rc == 0) {
        memcpy(pend[(pend_head + pend_n) % VL_PEND_MAX], period, snd_period);
        pend_n++;
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* sound_module_t                                                      */
/* ------------------------------------------------------------------ */

static boolean VL_Init(boolean _use_sfx_prefix)
{
    use_sfx_prefix = _use_sfx_prefix;

    vl_audio_info_t info;
    memset(&info, 0, sizeof(info));
    if (vl_audio_info(&info) < 0 || !info.ready) {
        printf("vldoom: snd nodev\n");
        return false;
    }
    if ((info.format != VL_AFMT_FLOAT && info.format != VL_AFMT_S16 &&
         info.format != VL_AFMT_S32)
     || rate_code_hz(info.rate) == 0
     || info.channels < 1 || info.channels > 2
     || !info.period_bytes || info.period_bytes > VL_PERIOD_MAX) {
        printf("vldoom: snd refused fmt=%u rate=%u ch=%u\n",
               info.format, info.rate, info.channels);
        return false;
    }

    int64_t gen = vl_audio_stream_open(info.format, info.rate,
                                       info.channels);
    if (gen < 0) {
        printf("vldoom: snd open rc=%ld\n", (long)gen);
        return false;
    }

    snd_fmt = info.format;
    snd_ch = info.channels;
    snd_hz = rate_code_hz(info.rate);
    snd_period = info.period_bytes;
    snd_framelen = (snd_fmt == VL_AFMT_S16 ? 2 : 4) * snd_ch;
    snd_gen = (uint64_t)gen;
    memset(chans, 0, sizeof(chans));
    pend_head = pend_n = 0;
    stat_submitted = stat_nonzero = stat_refused = stat_marker = 0;

    /* START refuses an empty queue — stage one (silent) period first. */
    memset(scratch, 0, snd_period);
    if (submit_period(scratch) < 0) return false;
    if (vl_audio_stream_ctl(snd_gen, VL_ASTREAM_START) < 0) {
        printf("vldoom: snd start refused\n");
        return false;
    }
    snd_live = 1;
    printf("vldoom: snd fmt=%u rate=%u ch=%u hz=%u period=%u gen=%llu\n",
           info.format, info.rate, info.channels, snd_hz, snd_period,
           (unsigned long long)snd_gen);
    return true;
}

static void VL_Shutdown(void)
{
    if (snd_live) {
        vl_audio_stream_ctl(snd_gen, VL_ASTREAM_END);
        snd_live = 0;
    }
}

static int VL_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char namebuf[16];
    vl_sfx_lumpname(sfx, namebuf, sizeof(namebuf));
    return W_GetNumForName(namebuf);
}

static void VL_UpdateSoundParams(int handle, int vol, int sep)
{
    if (handle < 0 || handle >= VL_SND_CHANNELS) return;
    int left  = ((254 - sep) * vol) / 127;
    int right = (sep * vol) / 127;
    if (left < 0) left = 0; else if (left > 255) left = 255;
    if (right < 0) right = 0; else if (right > 255) right = 255;
    chans[handle].lvol = (uint16_t)left;
    chans[handle].rvol = (uint16_t)right;
}

static int VL_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep)
{
    if (!snd_live || channel < 0 || channel >= VL_SND_CHANNELS) {
        printf("vldoom: sfx-refuse ch=%d live=%d\n", channel, snd_live);
        return -1;
    }

    vl_sfx_t *s = vl_sfx_decode(sfxinfo);
    if (!s || !s->len) {
        printf("vldoom: sfx-refuse ch=%d decode lump=%d\n",
               channel, sfxinfo->lumpnum);
        return -1;
    }

    vl_chan_t *c = &chans[channel];
    c->data = s->data;
    c->len = s->len;
    c->step = ((uint64_t)s->rate << 16) / snd_hz;
    if (!c->step) c->step = 1;
    c->pos = 0;
    c->active = 1;
    VL_UpdateSoundParams(channel, vol, sep);

    char namebuf[16];
    vl_sfx_lumpname(sfxinfo, namebuf, sizeof(namebuf));
    printf("vldoom: sfx %s ch=%d n=%u rate=%u\n",
           namebuf, channel, s->len, s->rate);
    return channel;
}

static void VL_StopSound(int handle)
{
    if (handle < 0 || handle >= VL_SND_CHANNELS) return;
    chans[handle].active = 0;
}

static boolean VL_SoundIsPlaying(int handle)
{
    if (!snd_live || handle < 0 || handle >= VL_SND_CHANNELS) return false;
    return chans[handle].active ? true : false;
}

static void VL_Update(void)
{
    if (!snd_live) return;

    /* drain pending into whatever queue space opened up — raw submit:
     * these entries are already pending, never re-stage them */
    while (pend_n) {
        int rc = submit_raw(pend[pend_head]);
        if (rc <= 0) break;
        pend_head = (pend_head + 1) % VL_PEND_MAX;
        pend_n--;
        stat_marker++;
    }
    if (!snd_live) return;

    /* mix + submit fresh periods while the queue takes them; on EAGAIN
     * the period is staged pending and the mix stalls until next tic */
    for (int i = 0; i < VL_UPD_BUDGET && pend_n < VL_PEND_MAX; i++) {
        int used = mix_period(scratch);
        int rc = submit_period(scratch);
        if (rc < 0) return;
        if (rc == 1) {
            if (used) stat_nonzero += snd_period;
            stat_marker++;
            continue;
        }
        if (used) stat_nonzero += snd_period;
        break;  /* staged — queue is full */
    }

    if (stat_marker >= 64) {
        stat_marker = 0;
        pcm_marker();
    }
}

static snddevice_t vl_sound_devices[] = {
    SNDDEVICE_SB,
    SNDDEVICE_PAS,
    SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS,
    SNDDEVICE_AWE32,
};

sound_module_t DG_sound_module = {
    vl_sound_devices,
    sizeof(vl_sound_devices) / sizeof(vl_sound_devices[0]),
    VL_Init,
    VL_Shutdown,
    VL_GetSfxLumpNum,
    VL_Update,
    VL_UpdateSoundParams,
    VL_StartSound,
    VL_StopSound,
    VL_SoundIsPlaying,
    NULL,   /* CacheSounds — decode is lazy, on first StartSound */
};

/* ------------------------------------------------------------------ */
/* music_module_t — refused by name: no synthesizer on this platform    */
/* ------------------------------------------------------------------ */

static boolean VL_MusicInit(void)
{
    printf("vldoom: music unsupported — no synthesizer, refused\n");
    return false;
}

/* i_sound.c calls every op through music_module without NULL-checking the
 * function pointer (only Poll is guarded), so the refusal module needs
 * inert stubs, not NULLs. */
static void     VL_MusicNoop(void) { }
static void     VL_MusicSetVol(int volume) { (void)volume; }
static void    *VL_MusicReg(void *data, int len) { (void)data; (void)len; return NULL; }
static void     VL_MusicUnreg(void *handle) { (void)handle; }
static void     VL_MusicPlay(void *handle, boolean looping) { (void)handle; (void)looping; }
static boolean  VL_MusicPlaying(void) { return false; }

static snddevice_t vl_music_devices[] = {
    SNDDEVICE_ADLIB,
    SNDDEVICE_SB,
    SNDDEVICE_GENMIDI,
};

music_module_t DG_music_module = {
    vl_music_devices,
    sizeof(vl_music_devices) / sizeof(vl_music_devices[0]),
    VL_MusicInit,
    VL_MusicNoop,       /* Shutdown */
    VL_MusicSetVol,
    VL_MusicNoop,       /* PauseMusic */
    VL_MusicNoop,       /* ResumeMusic */
    VL_MusicReg,
    VL_MusicUnreg,
    VL_MusicPlay,
    VL_MusicNoop,       /* StopSong */
    VL_MusicPlaying,
    NULL,               /* Poll — the one NULL-guarded op */
};
