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
2. Unzip, then put your PS1 **BIOS** (a 512 KB `.bin`, any name) in the same folder as `vibribbon.exe`.
   You can also just **drag the BIOS file onto the window** once; it is copied next to the exe as `bios.bin`.
   The BIOS is required for the controller/keyboard to work (see Status).
3. Run `vibribbon.exe` and **drag `vibribbon.cue` onto the window** (or drag the .cue onto the .exe).
   The title screen comes up in a few seconds. Without a BIOS it still boots, but input is dead and the title bar says so.

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

Keyboard: arrows or WASD = D-pad, Z/Space = Cross, X = Circle, C = Square, V = Triangle, Enter = Start, Right Shift/Backspace = Select, Q/E = L1/R1, 1/3 = L2/R2. Every change in pad state is logged to stderr as `pad: XXXX` (active-high bits, see `pad_map.h`).

## Status (honest)

| Piece | State |
|---|---|
| Loader boots, reads the disc, starts the title (`MAIN_T.EXE`) | Works (tested headlessly on the EU disc) |
| Title screen: animated vector logo, language menu, copyright line | Renders correctly (checked via screenshots) |
| Speed | ~60 fps headless in the dev sandbox (target 50); Windows speed not yet measured |
| **HOW TO PLAY** (loads `MAIN_K.EXE` + `KIOSK/*`, plays CD-DA track 8) | **Works**: loading screen then the full tutorial runs (`tests/howto_e2e.cpp`) |
| `MAIN_G.EXE` (GAME on the title menu) | Not tested yet |
| Sound (SPU: 24 ADPCM voices, ADSR) and CD-DA audio tracks | Implemented; title-screen sound verified headlessly, CD-DA verified against the disc's audio tracks. Not implemented: reverb, noise, pitch modulation, volume sweeps |
| Real BIOS boot | Wired into `main.cpp`: the app finds/accepts your BIOS, boots it to the shell (~0.1 s), then side-loads the disc's boot EXE under the real kernel. Headless demos: `tests/bios_boot.cpp`, `tests/bios_title.cpp`, `tests/menu_nav.cpp` |
| CD timing | Sector rate slowed to 90000 cycles/sector so the real kernel's IRQ path can keep up; pad port ACKs the last byte too (the game's pad driver waits for it) |
| CPU timing | 2 cycles per instruction (was 1). At 1, the game's `VSync` timeout loops expired early and it printed `VSync: timeout` every frame; now none |
| Controller / keyboard | **Works with a BIOS**: verified headlessly with scripted button taps (CROSS confirms the language, DOWN/UP move through GAME / HOW TO PLAY / LANGUAGE). Without a BIOS the game never starts its pad driver, so input is dead. The game's menus step twice for a D-pad press of 3+ frames (cause not found), so the app passes each new D-pad press for 2 frames only (`pad::TapFilter`, F10 toggles it) |
| Windows build (`src/main.cpp`, CMake, CI) | Written and type-checked, **never compiled with real SDL** -- first CI run may need a small fix |
| Gouraud/dither/exact fill rules, semi-transparency edge cases | Approximate |

## Layout

- `src/cpu.h` - R3000 interpreter, memory map, real-BIOS boot (`load_bios`/`sideload_exe`) and a fallback high-level BIOS
- `src/gpu.h`, `src/gte.h`, `src/cdrom.h`, `src/disc.h` - hardware and disc reading
- `src/main.cpp` - SDL window, drag-and-drop, controller
- `tests/` - unit tests (`cpu_test`, `pad_map_test`, run in CI) and headless tools that need your disc:
  - `disc_info game.cue` - list the disc's boot files
  - `boot_trace game.cue [EXE] [max_instr] [out.ppm]` - run and report BIOS calls, hardware access, GPU commands; optional screenshot
  - `title_shot game.cue prefix` - boot to the title, press some buttons, save PPM screenshots
- `tools/extract_disc.py` - extract the files from a BIN/CUE
- `config/` - early splat configs from the decompilation approach (not used by the build)

## Fixed: the "stuck after choosing a menu item" bug

Two emulation bugs, both found by tracing what the game was waiting on:

1. **SPU/CD-DA clock.** SPU voice envelopes and the CD-audio read position only advance inside `Cpu::audio_frame()`.
   The game waits for a sound effect to finish (it polls the voice's ADSR volume), so with that clock frozen the
   scribble transition never ended. `main.cpp` now calls `audio_frame()` every video frame, even with no audio
   device. **Any headless tool must call `audio_frame(882, buf)` after each `run_frame()`.**
2. **CD-DA Mute froze the read position.** The game sends Mute, plays track 8 from 5 sectors before the track
   start, and only Demutes once `GetlocP` reaches the track start. `cdrom.h` stopped advancing the position while
   muted, so it deadlocked on the LOADING screen. Mute now only silences output.

The garbled line art on the first transition is the game's own scribble effect, not a GTE bug.

## Still to do

- Try GAME (`MAIN_G.EXE`) with the same harness.
- Find why the menu double-steps on 3+ frame presses (the tap filter is a workaround).
- `src/main.cpp` has not been compiled against SDL since the edits above.
