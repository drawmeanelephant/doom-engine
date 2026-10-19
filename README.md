# doom-engine — DOOM for VirelaiOS (M130b)

The GPL-family side of VirelaiOS milestone M130. Per ADR 0058 in
[VirelaiOS](https://github.com/drawmeanelephant/VirelaiOS), **all**
GPL-derived material lives in this repository: engine source, headers,
patches, license texts, test assets, and built binaries. The boundary is
the raw VirelaiOS syscall ABI — nothing here links to VirelaiOS code,
and nothing GPL crosses into the system repo or its build.

The engine is [ozkl/doomgeneric](https://github.com/ozkl/doomgeneric)
(GPLv2; see `doomgeneric/COPYING`), pinned per `UPSTREAM.md`.

## What this card (M130b) lands

- `virelai/` — the platform shim: raw `svc #0` wrappers (`sys.h`,
  `sys.c`, `sys_guest.S`), the `DG_*` surface upstream's
  `doomgeneric.c` calls (`doomgeneric_virelai.c`), a minimal freestanding
  libc under `virelai/libc/` + `virelai/include/`, `start.S`, and
  `virelai.ld` shaped to the loader contract.
- `build/DOOM.ELF` — `zig cc -target aarch64-freestanding-none
  -nostdlib` statically linked, ≤3 PT_LOAD (R+X @ 0x400000, R, RW),
  entry inside segment 0's file bytes, all segments below 0x1000_0000.
- `tools/hosttest.c` + `virelai/sys_mock.c` — host legs: shim
  round-trips, WAD load, TITLEPIC decode.
- `tools/check-elf.py` — the contract check stated engine-side.
- `assets/doom1.wad` — shareware DOOM 1.9 asset (see
  `assets/doom1.wad.NOTICE`), loaded by the host legs.

## What M130c adds (playable slice)

- `vldoom:` serial markers (`boot`, `win`, `level`, `key`, `f=N t=T`) so
  a host-side driver can anchor scripted input and framebuffer snapshots
  without a display round trip.
- Control mapping fixed for DOOM's abstract codes: space → `KEY_USE`,
  any ctrl-modified stroke → `KEY_FIRE` (modifier presses never arrive
  as HID usages on the guest channel), WASD as movement aliases.
- Game loop runs at tic rate in-guest: `doomgeneric_Tick` presents one
  frame per real tic; `f=N t=T` heartbeats carry `leveltime` so the gate
  can see tics advancing.
- `live-doom` spec leg (VirelaiOS side) execs `DOOM.ELF` off the virtio
  share with `-iwad doom1.wad -warp 1 1`, holds `w` (forward) and sends
  `space`/`ctrl-x`/`left` chords, and pixel-asserts the HUD + a scene
  delta on raw scanout snapshots.

Still stubbed: audio is a no-op until M130d (`FEATURE_SOUND` off),
store packaging is M130e.

## Build

```
make doom.elf      # build/DOOM.ELF (zig cc required)
make check-elf     # contract check
make host-test     # host legs (clang + Darwin; builds build/hosttest)
make test          # everything
```

## Layout

| path                     | contents                                        |
|--------------------------|-------------------------------------------------|
| `doomgeneric/`           | upstream engine, unmodified (GPLv2)             |
| `virelai/`               | shim: `sys.*`, `sys_guest.S`, `doomgeneric_virelai.c`, `start.S`, `virelai.ld`, `libc/`, `include/` |
| `virelai/sys_mock.c`, `virelai/darwin_syscall.S` | mock kernel for host legs — never in DOOM.ELF   |
| `tools/`                 | `hosttest.c`, `check-elf.py`                    |
| `assets/`                | WAD test assets + notices                       |
