# Upstream pins

| input               | source                                                  | pin / identity                                   | license      |
|---------------------|---------------------------------------------------------|--------------------------------------------------|--------------|
| engine source       | https://github.com/ozkl/doomgeneric (`doomgeneric/`)    | `e257a09527062bfb986b6e5e0780839b6cbd5b71` (2026-10-11, `master`) | GPLv2 — `doomgeneric/COPYING` |
| `assets/doom1.wad`  | idgames `doom19s.zip` self-extractor → `DOOMS_19.1`+`DOOMS_19.2` → `DOOM1.WAD` | md5 `f0cefca49926d00903cf57551d901abe`, 4,196,020 B | id shareware — `assets/doom1.wad.NOTICE` |

Extraction recipe for `doom1.wad`: unzip `doom19s.zip`, concatenate
`DOOMS_19.1` + `DOOMS_19.2` into a zip, unzip again, copy `DOOM1.WAD`
to `assets/doom1.wad`.
