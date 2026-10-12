# Vib-Ribbon native (PS1, European disc `SCES-028.73`)

Runs Vib-Ribbon on Windows from **your own disc image**. No game data is included
and none should ever be committed (`assets/`, `*.bin`, `*.cue` are gitignored).

The game's original code runs on a MIPS R3000 interpreter. The PS1 hardware it
talks to (GPU, GTE, CD-ROM, DMA, timers, interrupts, controller port) is emulated
in this repo, and the picture is drawn by a software rasterizer.

## Use it

1. Push this repo to GitHub. The **build** workflow runs automatically and
   publishes a `vibribbon-windows` artifact (Actions tab -> latest run).
   Push a tag like `v0.1` to also get a downloadable Release.
2. Unzip, run `vibribbon.exe`, and **drag `vibribbon.cue` onto the window**
   (or drag the .cue onto the .exe). The title screen comes up in a few seconds.

Keys: Esc quits, F11 toggles fullscreen.

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

## Status (honest)

| Piece | State |
|---|---|
| Loader boots, reads the disc, starts the title (`MAIN_T.EXE`) | Works (tested headlessly on the EU disc) |
| Title screen: animated vector logo, language menu, copyright line | Renders correctly (checked via screenshots) |
| Speed | ~60 fps headless in the dev sandbox (target 50); Windows speed not yet measured |
| `MAIN_G.EXE` (the actual game), `MAIN_K.EXE` (kiosk) | Not tested yet |
| Sound (SPU: 24 ADPCM voices, ADSR) and CD-DA audio tracks | Implemented; title-screen sound verified headlessly, CD-DA verified against the disc's audio tracks. Not implemented: reverb, noise, pitch modulation, volume sweeps |
| Controller | Controller port is emulated, but the title code never reads it, so button input is **unverified** |
| Windows build (`src/main.cpp`, CMake, CI) | Written and type-checked, **never compiled with real SDL** -- first CI run may need a small fix |
| Gouraud/dither/exact fill rules, semi-transparency edge cases | Approximate |

## Layout

- `src/cpu.h` - R3000 interpreter, memory map, high-level BIOS (BIOS ROM is not needed)
- `src/gpu.h`, `src/gte.h`, `src/cdrom.h`, `src/disc.h` - hardware and disc reading
- `src/main.cpp` - SDL window, drag-and-drop, controller
- `tests/` - unit tests (`cpu_test`, `pad_map_test`, run in CI) and headless tools that need your disc:
  - `disc_info game.cue` - list the disc's boot files
  - `boot_trace game.cue [EXE] [max_instr] [out.ppm]` - run and report BIOS calls, hardware access, GPU commands; optional screenshot
  - `title_shot game.cue prefix` - boot to the title, press some buttons, save PPM screenshots
- `tools/extract_disc.py` - extract the files from a BIN/CUE
- `config/` - early splat configs from the decompilation approach (not used by the build)
