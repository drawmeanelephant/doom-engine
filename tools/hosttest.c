/*
 * tools/hosttest.c — the host legs (built -nostdlib, linked to
 * sys_mock.c: the same vl_sys ABI a mock kernel answers).
 *
 *   leg 1  shim round-trips: DG_Init/DrawFrame/GetKey/GetTicksMs/
 *          SleepMs/SetWindowTitle + malloc/free + FILE + console write
 *   leg 2  WAD loads: fopen + doomgeneric's W_AddFile/W_CheckNumForName
 *   leg 3  TITLEPIC decodes: W_CacheLumpName -> patch_t -> 320x200,
 *          non-trivial post coverage, PLAYPAL 256x3 palette present
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "../virelai/sys.h"
#include "doomgeneric.h"
#include "doomkeys.h"
#include "i_sound.h"
#include "sounds.h"
#include "w_wad.h"
#include "v_patch.h"
#include "z_zone.h"
#include "doomstat.h"

/* mock test surface (sys_mock.c) */
int      mock_window_count(void);
uint64_t mock_window_presents(uint32_t id);
void    *mock_window_surface(uint32_t id, uint64_t *bytes);
const char *mock_window_title(uint32_t id);
void     mock_window_geometry(uint32_t id, uint32_t *x, uint32_t *y,
                              uint32_t *w, uint32_t *h);
void     mock_push_event(uint16_t kind, uint16_t flags, uint32_t arg0,
                         uint32_t arg1);
uint64_t mock_audio_bytes(void);
uint64_t mock_now_ms(void);
int      mock_exit_status_get(void);

extern pixel_t *DG_ScreenBuffer;
extern int myargc;
extern char **myargv;

static int fails;
static int checks;

#define CHECK(cond, msg) do { \
    checks++; \
    if (!(cond)) { fails++; printf("FAIL leg: %s\n", msg); } \
} while (0)

/* ---------------- leg 1: shim round-trips ------------------------- */

static void leg_shim(void)
{
    DG_ScreenBuffer = malloc(640 * 400 * 4); /* doomgeneric_Create's shape */
    CHECK(DG_ScreenBuffer != NULL, "DG_ScreenBuffer malloc");

    DG_Init();
    CHECK(mock_window_count() == 1, "DG_Init opened one window");

    /* window 2 is the first user id */
    uint32_t x, y, w, h;
    mock_window_geometry(2, &x, &y, &w, &h);
    CHECK(w == 640 && h == 400, "window geometry 640x400");
    CHECK(strcmp(mock_window_title(2), "DOOM") == 0, "title DOOM");

    uint64_t bytes;
    void *surface = mock_window_surface(2, &bytes);
    CHECK(surface != NULL, "surface bound");
    CHECK(bytes >= 640 * 400 * 4, "surface sized");
    CHECK((void *)DG_ScreenBuffer == surface, "DG_ScreenBuffer is surface");

    /* draw a pattern and present */
    memset(DG_ScreenBuffer, 0x5a, 640 * 400 * 4);
    DG_DrawFrame();
    CHECK(mock_window_presents(2) == 1, "win_present observed");
    CHECK(((uint8_t *)surface)[0] == 0x5a, "frame bytes landed");

    DG_SetWindowTitle("DOOM — VirelaiOS");
    CHECK(strcmp(mock_window_title(2), "DOOM — VirelaiOS") == 0,
          "win_set_title round-trip");

    /* keys: HID usage 0x08 = 'e' down then up */
    mock_push_event(VL_EV_KEY_DOWN, 0, 0x08, 'e');
    mock_push_event(VL_EV_KEY_UP, 0, 0x08, 'e');
    int pressed = -1;
    unsigned char key = 0;
    CHECK(DG_GetKey(&pressed, &key) == 1 && pressed == 1 && key == 'e',
          "key down delivered");
    CHECK(DG_GetKey(&pressed, &key) == 1 && pressed == 0 && key == 'e',
          "key up delivered");
    CHECK(DG_GetKey(&pressed, &key) == 0, "queue drained");

    /* M130c control mappings: WASD are movement aliases, space = use,
     * ctrl-modified stroke = fire (modifier-only presses never arrive
     * as usages on the guest channel). */
    mock_push_event(VL_EV_KEY_DOWN, 0, 0x1a, 'w');
    mock_push_event(VL_EV_KEY_DOWN, 0, 0x2c, ' ');
    mock_push_event(VL_EV_KEY_DOWN, VL_MOD_CTRL, 0x1b, 'x');
    mock_push_event(VL_EV_KEY_UP,   VL_MOD_CTRL, 0x1b, 'x');
    CHECK(DG_GetKey(&pressed, &key) == 1 && pressed == 1 &&
          key == KEY_UPARROW, "w is KEY_UPARROW");
    CHECK(DG_GetKey(&pressed, &key) == 1 && pressed == 1 &&
          key == KEY_USE, "space is KEY_USE");
    CHECK(DG_GetKey(&pressed, &key) == 1 && pressed == 1 &&
          key == KEY_FIRE, "ctrl-x is KEY_FIRE");
    CHECK(DG_GetKey(&pressed, &key) == 1 && pressed == 0 &&
          key == KEY_FIRE, "ctrl-x up is KEY_FIRE");
    /* M130d: held 'f' is the second fire binding (a gate's --input-key
     * can hold a letter usage; it cannot hold a ctrl chord) */
    mock_push_event(VL_EV_KEY_DOWN, 0, 0x09, 'f');
    CHECK(DG_GetKey(&pressed, &key) == 1 && pressed == 1 &&
          key == KEY_FIRE, "f is KEY_FIRE");
    CHECK(DG_GetKey(&pressed, &key) == 0, "queue drained");

    /* ticks monotonic; sleep actually waits */
    uint32_t t0 = DG_GetTicksMs();
    DG_SleepMs(5);
    uint32_t t1 = DG_GetTicksMs();
    CHECK(t1 >= t0 + 5, "DG_SleepMs waits");
    CHECK(t1 - t0 < 5000, "tick clock sane");

    /* malloc/free round-trips through mmap-backed heap */
    void *p = malloc(1234);
    CHECK(p != NULL, "malloc");
    memset(p, 0xa5, 1234);
    void *q = realloc(p, 65536);
    CHECK(q != NULL, "realloc grows");
    CHECK(((uint8_t *)q)[0] == 0xa5, "realloc preserves");
    free(q);

    /* FILE over the mock file layer */
    FILE *wf = fopen("build/hosttest-write.txt", "w");
    CHECK(wf != NULL, "fopen for write");
    CHECK(fwrite("doom on virelai", 1, 15, wf) == 15, "fwrite");
    CHECK(fclose(wf) == 0, "fclose writes back");

    FILE *rf = fopen("build/hosttest-write.txt", "r");
    CHECK(rf != NULL, "fopen read back");
    char buf[32] = {0};
    CHECK(fread(buf, 1, 15, rf) == 15 &&
          memcmp(buf, "doom on virelai", 15) == 0, "read matches");
    fclose(rf);
    CHECK(remove("build/hosttest-write.txt") == 0, "remove");

    CHECK(fopen("no/such/file.bin", "r") == NULL, "missing file fails");

    /* console write through slot 1 */
    puts("leg1 shim round-trips: OK");
}

/* ---------------- leg 2: WAD loads --------------------------------- */

static void leg_wad(void)
{
    myargc = 1;
    myargv = (char *[]) { "doom", NULL };

    printf("leg2: fopen probe\n");
    FILE *probe = fopen("assets/doom1.wad", "rb");
    printf("leg2: probe %p\n", (void *)probe);
    CHECK(probe != NULL, "wad opens");
    if (probe) {
        unsigned char hdr[12];
        size_t n = fread(hdr, 1, 12, probe);
        printf("leg2: fread %zu bytes: %02x %02x %02x %02x\n",
               n, hdr[0], hdr[1], hdr[2], hdr[3]);
        fclose(probe);
    }

    printf("leg2: W_AddFile\n");
    Z_Init();
    wad_file_t *wf = W_AddFile("assets/doom1.wad");
    printf("leg2: wf %p\n", (void *)wf);
    CHECK(wf != NULL, "W_AddFile doom1.wad");
    if (!wf) return;
    CHECK(W_CheckNumForName("TITLEPIC") >= 0, "TITLEPIC lump present");
    CHECK(W_CheckNumForName("PLAYPAL") >= 0, "PLAYPAL lump present");
    CHECK(W_CheckNumForName("E1M1") >= 0, "E1M1 marker present");
    CHECK(W_CheckNumForName("NO_SUCH_LUMP") == -1, "bogus lump absent");
    printf("leg2 WAD loads: %s, %d lumps\n", "assets/doom1.wad", numlumps);
}

/* ---------------- leg 3: TITLEPIC decode ---------------------------- */

static void leg_titlepic(void)
{
    int n = W_CheckNumForName("TITLEPIC");
    CHECK(n >= 0, "TITLEPIC lump number");
    if (n < 0) return;

    patch_t *pic = W_CacheLumpNum(n, PU_CACHE);
    CHECK(pic != NULL, "TITLEPIC cached");
    if (!pic) return;
    CHECK(pic->width == 320 && pic->height == 200,
          "TITLEPIC 320x200");

    /* decode every column post; count covered pixels */
    uint64_t covered = 0;
    for (int x = 0; x < pic->width && x < 2048; x++) {
        const uint8_t *col = (const uint8_t *)pic + pic->columnofs[x];
        for (;;) {
            uint8_t top = col[0];
            if (top == 0xff) break;
            uint8_t len = col[1];
            covered += len;
            col += 4 + len;
        }
    }
    CHECK(pic->width >= 1 && pic->width <= 2048, "width sane");
    CHECK(covered > 20000, "TITLEPIC posts cover real pixels");

    /* PLAYPAL: 256 x RGB triples in lump 0 region */
    int pn = W_CheckNumForName("PLAYPAL");
    patch_t *pal = NULL;
    const uint8_t *palbytes = NULL;
    if (pn >= 0) {
        palbytes = W_CacheLumpNum(pn, PU_CACHE);
        (void)pal;
    }
    CHECK(palbytes != NULL, "PLAYPAL cached");
    if (palbytes) {
        uint64_t nonzero = 0;
        for (int i = 0; i < 768; i++) nonzero += palbytes[i];
        CHECK(nonzero > 0, "PLAYPAL non-zero palette");
    }

    printf("leg3 TITLEPIC decodes: %dx%d, %llu post pixels, palette %s\n",
           pic->width, pic->height, (unsigned long long)covered,
           palbytes ? "present" : "missing");
}

/* ---------------- leg 4: M130d sound module on the mock ------------- */

static void leg_sound(void)
{
    /* leg_wad already Z_Init'd and W_AddFile'd doom1.wad. */
    CHECK(DG_sound_module.Init(true) == true, "sound module init");
    if (!DG_sound_module.Init) return;

    sfxinfo_t *sfx = &S_sfx[sfx_pistol];
    sfx->lumpnum = -1;
    sfx->lumpnum = DG_sound_module.GetSfxLumpNum(sfx);
    CHECK(sfx->lumpnum >= 0, "dspistol lump resolves");

    uint64_t before = mock_audio_bytes();
    int h = DG_sound_module.StartSound(sfx, 0, 127, 127);
    CHECK(h >= 0, "StartSound accepted the channel");
    CHECK(DG_sound_module.SoundIsPlaying(h) == true, "channel playing");

    /* a few tics of Update() must submit bounded periods to the mock */
    for (int i = 0; i < 8; i++) DG_sound_module.Update();
    CHECK(mock_audio_bytes() > before, "pcm submitted through stream ABI");
    CHECK((mock_audio_bytes() - before) % 4096 == 0,
          "submissions are whole 4096-B periods");

    DG_sound_module.StopSound(h);
    CHECK(DG_sound_module.SoundIsPlaying(h) == false, "channel stopped");
    DG_sound_module.Shutdown();
    printf("leg4 sound module: OK\n");
}

/* ---------------- leg 5: live game — TITLEPIC -> demo melt ---------- */
/*
 * Guest bug: DOOM.ELF took a data abort inside wipe_initMelt's melt
 * transform during the TITLEPIC->demo1 wipe (the -warp path never
 * runs the melt). Drive the real game loop through the transition on
 * the mock: TITLEPIC pages for 170 tics (~5 s), demo1 spawns, and the
 * wipe melts between the two captured screens.
 */
static void leg_game(void)
{
    extern int leveltime;
    extern gamestate_t gamestate;

    char *argv[] = { "doom", "-iwad", "assets/doom1.wad", NULL };
    doomgeneric_Create(3, argv);

    int saw_level = 0;
    /* 2000 ticks x ~5 ms of work+sleep comfortably clears 170 tics of
     * TITLEPIC, the ~2 s melt, and the first demo tics. */
    for (int i = 0; i < 2000 && !saw_level; i++) {
        doomgeneric_Tick();
        DG_SleepMs(1);
        if (gamestate == GS_LEVEL && leveltime > 0) saw_level = 1;
    }
    CHECK(saw_level, "demo1 reached GS_LEVEL past the melt");
    printf("leg5 TITLEPIC->demo melt: leveltime=%d\n", leveltime);
}

/* ---------------- leg 6: -playdemo <file> through demo end ---------- */
/*
 * Guest bug (store run 02/03, issue #2623): -playdemo names its demo by
 * the lump the file adds; G_DeferedPlayDemo stored that name in
 * defdemoname as a POINTER into D_DoomMain's demolumpname[9] — a local.
 * doomgeneric's split D_DoomLoop returns from D_DoomMain after the first
 * tick, so the pointer dangled into a dead frame: at demo end
 * G_CheckDemoStatus -> W_ReleaseLumpName(defdemoname) handed
 * W_GetNumForName whatever bytes later frames left there, I_Error'd, and
 * the recursive I_Error (guard warned but re-ran the body) unwound the
 * stack: fault far=stack_base-0x80 pc=I_Error+0x40 (stp q0,q1,[sp]).
 *
 * Drive the same path on the mock: a synthesized v109 demo, -playdemo,
 * then ticks until the run-on-error/singledemo path exits cleanly via
 * I_Quit -> exit(0). Pre-fix this leg died inside I_Error; post-fix the
 * mock records exit status 0.
 */
static void leg_playdemo(void)
{
    extern char *defdemoname;

    /* Minimal v109 (shorttics) demo: 13-byte header, sixteen no-op
     * 4-byte commands, DEMOMARKER. */
    unsigned char demo[13 + 4 * 16 + 1] = {
        109, 1, 1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0,
    };
    demo[sizeof(demo) - 1] = 0x80;   /* DEMOMARKER */

    FILE *f = fopen("build/walk.lmp", "wb");
    CHECK(f != NULL, "walk.lmp opens for write");
    if (!f) return;
    CHECK(fwrite(demo, 1, sizeof(demo), f) == sizeof(demo), "walk.lmp written");
    CHECK(fclose(f) == 0, "walk.lmp flushed");

    char *argv[] = { "doom", "-iwad", "assets/doom1.wad",
                     "-playdemo", "build/walk.lmp", NULL };
    doomgeneric_Create(5, argv);

    CHECK(defdemoname != NULL && strcmp(defdemoname, "WALK") == 0,
          "defdemoname is the added lump's name and stays valid");

    /* demo end -> G_CheckDemoStatus -> W_ReleaseLumpName(defdemoname)
     * -> singledemo -> I_Quit -> exit(0) -> the mock records status */
    int i;
    for (i = 0; i < 4000 && mock_exit_status_get() < 0; i++) {
        doomgeneric_Tick();
        DG_SleepMs(1);
    }
    CHECK(mock_exit_status_get() == 0, "walk.lmp ends in clean I_Quit");
    printf("leg6 -playdemo demo end: exit=%d ticks=%d\n",
           mock_exit_status_get(), i);
}

/* ---------------- entry --------------------------------------------- */

int hosttest_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    leg_shim();
    leg_wad();
    leg_titlepic();
    leg_sound();
    leg_game();
    leg_playdemo();

    printf("hosttest: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
