# Vib-Ribbon native (static recompilation WIP)

A work-in-progress effort to run Vib-Ribbon (PS1, European release, `SCES_028.73`)
natively on PC, built from your own disc image. **No game data is included or
should ever be committed.** You supply your own legally obtained disc.

## Status (read this first)

| Piece | State |
|---|---|
| Disc extraction (`tools/extract_disc.py`) | Works, tested on the EU BIN/CUE |
| Windows build + CI (`.github/workflows/build.yml`) | Written, not yet run on GitHub |
| SDL2 window + gamepad layer | Written, not yet compiled with SDL |
| Xbox 360 -> PS1 pad mapping (`src/pad_map.h`) | Done, unit tested |
| splat configs (`config/`) | Untested starting points |
| Game code (disassembly -> C++/recompiled) | **Not started** |
| GPU / SPU / CD / pad HLE | **Not started** |

Right now `vibribbon.exe` opens a window and reads your controller. It does not
run the game yet.

## Controls

Xbox 360 controller (any SDL-supported pad works):

| Xbox | PS1 |
|---|---|
| A / B / X / Y | Cross / Circle / Square / Triangle |
| D-pad or left stick | D-pad |
| LB / RB | L1 / R1 |
| LT / RT | L2 / R2 |
| Start / Back | Start / Select |

Keyboard fallback: arrows, Z=Cross, X=Circle, A=Square, S=Triangle, Enter=Start, Right Shift=Select.

## Build

Push to GitHub and the **build** workflow produces a `vibribbon-windows`
artifact. Locally: `cmake -S . -B build && cmake --build build --config Release`.

## Using your disc

    python tools/extract_disc.py path/to/vibribbon.cue

This writes `assets/disc/` (gitignored) plus `manifest.json` with SHA-1s.
Executables: `SCES_028.73` (loader, loads at 0x80010000), `MAIN_G.EXE` (game,
0x80019000), `MAIN_T.EXE` (title), `MAIN_K.EXE` (kiosk).

## Roadmap

1. Run splat on the loader and `MAIN_G.EXE`; get a byte-identical rebuild from asm.
2. Identify compiler/PsyQ versions (the game is C++; sources like `VideoSys.cpp`
   are named in its strings).
3. Reimplement the Sony library calls (GPU, SPU, CD, pad, timers) over SDL2.
4. Recompile or decompile game code to run against that layer.
