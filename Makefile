# doom-engine — DOOM (doomgeneric, GPLv2) as a freestanding VirelaiOS EL0
# payload plus its host-test harness. See README.md.

ZIG      ?= zig
CC       ?= clang
PYTHON   ?= python3

BUILD    := build
DGDIR    := doomgeneric
VIR      := virelai

# Upstream object list (Makefile's SRC_DOOM minus the platform files),
# plus the VirelaiOS platform unit.
ENGINE_SRC := \
    dummy.c am_map.c doomdef.c doomstat.c dstrings.c d_event.c \
    d_items.c d_iwad.c d_loop.c d_main.c d_mode.c d_net.c \
    f_finale.c f_wipe.c g_game.c hu_lib.c hu_stuff.c info.c \
    i_cdmus.c i_endoom.c i_joystick.c i_scale.c i_sound.c \
    i_system.c i_timer.c memio.c m_argv.c m_bbox.c m_cheat.c \
    m_config.c m_controls.c m_fixed.c m_menu.c m_misc.c m_random.c \
    p_ceilng.c p_doors.c p_enemy.c p_floor.c p_inter.c p_lights.c \
    p_map.c p_maputl.c p_mobj.c p_plats.c p_pspr.c p_saveg.c \
    p_setup.c p_sight.c p_spec.c p_switch.c p_telept.c p_tick.c \
    p_user.c r_bsp.c r_data.c r_draw.c r_main.c r_plane.c r_segs.c \
    r_sky.c r_things.c sha1.c sounds.c statdump.c st_lib.c \
    st_stuff.c s_sound.c tables.c v_video.c wi_stuff.c w_checksum.c \
    w_file.c w_main.c w_wad.c z_zone.c w_file_stdc.c i_input.c \
    i_video.c doomgeneric.c

ENGINE_OBJS := $(ENGINE_SRC:%.c=$(BUILD)/guest/%.o)
LIBC_SRC    := string.c ctype.c stdlib.c stdio.c misc.c
LIBC_OBJS   := $(LIBC_SRC:%.c=$(BUILD)/guest/libc_%.o)
GUEST_EXTRA := $(BUILD)/guest/doomgeneric_virelai.o \
               $(BUILD)/guest/vl_sound.o \
               $(BUILD)/guest/sys.o \
               $(BUILD)/guest/sys_guest.o \
               $(BUILD)/guest/start.o

CFLAGS_COMMON := -O2 -ffreestanding -fno-stack-protector \
    -I$(VIR)/include -I$(DGDIR) -I$(VIR) \
    -Wall -Wno-unused-parameter -Wno-sign-compare -Wno-pointer-sign \
    -Wno-macro-redefined

# M130d: compile the sound modules in (DG_sound_module/DG_music_module
# live in virelai/vl_sound.c; the SDL_mixer.h the engine includes under
# FEATURE_SOUND is the virelai/include stub).
CFLAGS_COMMON += -DFEATURE_SOUND

# ---------------------------------------------------------------------
# guest payload
# ---------------------------------------------------------------------

$(BUILD)/guest/%.o: $(DGDIR)/%.c
	@mkdir -p $(dir $@)
	$(ZIG) cc -target aarch64-freestanding-none -nostdlib -DVIRELAI_GUEST \
	    $(CFLAGS_COMMON) -c $< -o $@

$(BUILD)/guest/libc_%.o: $(VIR)/libc/%.c
	@mkdir -p $(dir $@)
	$(ZIG) cc -target aarch64-freestanding-none -nostdlib -DVIRELAI_GUEST \
	    $(CFLAGS_COMMON) -c $< -o $@

$(BUILD)/guest/doomgeneric_virelai.o: $(VIR)/doomgeneric_virelai.c
	@mkdir -p $(dir $@)
	$(ZIG) cc -target aarch64-freestanding-none -nostdlib -DVIRELAI_GUEST \
	    $(CFLAGS_COMMON) -c $< -o $@

$(BUILD)/guest/vl_sound.o: $(VIR)/vl_sound.c
	@mkdir -p $(dir $@)
	$(ZIG) cc -target aarch64-freestanding-none -nostdlib -DVIRELAI_GUEST \
	    $(CFLAGS_COMMON) -c $< -o $@

$(BUILD)/guest/sys.o: $(VIR)/sys.c
	@mkdir -p $(dir $@)
	$(ZIG) cc -target aarch64-freestanding-none -nostdlib -DVIRELAI_GUEST \
	    $(CFLAGS_COMMON) -c $< -o $@

$(BUILD)/guest/sys_guest.o: $(VIR)/sys_guest.S
	@mkdir -p $(dir $@)
	$(ZIG) cc -target aarch64-freestanding-none -nostdlib -c $< -o $@

$(BUILD)/guest/start.o: $(VIR)/start.S
	@mkdir -p $(dir $@)
	$(ZIG) cc -target aarch64-freestanding-none -nostdlib -c $< -o $@

$(BUILD)/DOOM.ELF: $(BUILD)/guest/start.o $(ENGINE_OBJS) $(LIBC_OBJS) \
                   $(BUILD)/guest/doomgeneric_virelai.o \
                   $(BUILD)/guest/vl_sound.o \
                   $(BUILD)/guest/sys.o $(BUILD)/guest/sys_guest.o
	$(ZIG) cc -target aarch64-freestanding-none -nostdlib \
	    -Wl,-T,$(VIR)/virelai.ld -Wl,--gc-sections \
	    $^ -o $@

doom.elf: $(BUILD)/DOOM.ELF

# ---------------------------------------------------------------------
# host legs
# ---------------------------------------------------------------------

HOST_SRC   := $(ENGINE_SRC:%.c=$(BUILD)/host/%.o)
HOST_LIBC  := $(LIBC_SRC:%.c=$(BUILD)/host/libc_%.o)
HOST_EXTRA := $(BUILD)/host/doomgeneric_virelai.o \
              $(BUILD)/host/vl_sound.o \
              $(BUILD)/host/sys.o \
              $(BUILD)/host/sys_mock.o \
              $(BUILD)/host/darwin_syscall.o \
              $(BUILD)/host/hosttest.o

$(BUILD)/host/%.o: $(DGDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -target arm64-apple-macos11 -nostdlib -ffreestanding \
	    $(CFLAGS_COMMON) -c $< -o $@

$(BUILD)/host/libc_%.o: $(VIR)/libc/%.c
	@mkdir -p $(dir $@)
	$(CC) -target arm64-apple-macos11 -nostdlib -ffreestanding \
	    $(CFLAGS_COMMON) -c $< -o $@

$(BUILD)/host/sys.o: $(VIR)/sys.c
	@mkdir -p $(dir $@)
	$(CC) -target arm64-apple-macos11 -nostdlib -ffreestanding \
	    $(CFLAGS_COMMON) -c $< -o $@

$(BUILD)/host/sys_mock.o: $(VIR)/sys_mock.c
	@mkdir -p $(dir $@)
	$(CC) -target arm64-apple-macos11 -nostdlib -ffreestanding \
	    $(CFLAGS_COMMON) -c $< -o $@

$(BUILD)/host/doomgeneric_virelai.o: $(VIR)/doomgeneric_virelai.c
	@mkdir -p $(dir $@)
	$(CC) -target arm64-apple-macos11 -nostdlib -ffreestanding \
	    $(CFLAGS_COMMON) -c $< -o $@

$(BUILD)/host/vl_sound.o: $(VIR)/vl_sound.c
	@mkdir -p $(dir $@)
	$(CC) -target arm64-apple-macos11 -nostdlib -ffreestanding \
	    $(CFLAGS_COMMON) -c $< -o $@

$(BUILD)/host/hosttest.o: tools/hosttest.c
	@mkdir -p $(dir $@)
	$(CC) -target arm64-apple-macos11 -nostdlib -ffreestanding \
	    $(CFLAGS_COMMON) -c $< -o $@

$(BUILD)/host/darwin_syscall.o: $(VIR)/darwin_syscall.S
	@mkdir -p $(dir $@)
	$(CC) -target arm64-apple-macos11 -c $< -o $@

$(BUILD)/hosttest: $(BUILD)/host/darwin_syscall.o $(HOST_SRC) \
                   $(HOST_LIBC) $(BUILD)/host/doomgeneric_virelai.o \
                   $(BUILD)/host/vl_sound.o \
                   $(BUILD)/host/sys.o $(BUILD)/host/sys_mock.o \
                   $(BUILD)/host/hosttest.o
	$(CC) -target arm64-apple-macos11 -nostdlib \
	    -Wl,-e,_start $^ -lSystem -o $@

host-test: $(BUILD)/hosttest
	cd . && ./$(BUILD)/hosttest

# ---------------------------------------------------------------------
# smoke: contract check
# ---------------------------------------------------------------------

check-elf: $(BUILD)/DOOM.ELF
	$(PYTHON) tools/check-elf.py $(BUILD)/DOOM.ELF

test: doom.elf check-elf host-test

clean:
	rm -rf $(BUILD)

.PHONY: doom.elf host-test check-elf test clean
