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

Later cards (per the M130 arc): M130c playable slice, M130d audio,
M130e/M130f store packaging.

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
