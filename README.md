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
| **GAME** hub (`MAIN_G.EXE`): Play with the vib-ribbon disc / Play with my own choice of CD / Options / Back | **Works**: all four bubbles navigate, one step per tap |
| Difficulty menu (Bronze / Silver / Gold / High scores) | **Works** (Bronze verified) |
| **Gameplay** (Bronze level) | **Runs**: Vibri runs, CD audio plays, and the ribbon (loops, zig-zags, terrain) is generated from the music. Playing it by hand is untested |
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

## Fixed: GAME stuck on LOADING, then no ribbon in the level

3. **SPU IRQ (IRQ9).** After `MAIN_G.EXE` loads, the game programs an SPU IRQ address and waits for the interrupt.
   The SPU now raises IRQ9 (SPUSTAT bit 6 + I_STAT bit 9) when a voice block fetch, a FIFO/DMA write, or the
   capture-buffer write pointer touches that address; writing SPUCNT with bit 6 = 0 acknowledges it.
4. **Capture buffers + SPU->RAM DMA.** Vib-Ribbon builds the ribbon by reading the CD audio *back out of the SPU*:
   the capture buffers (CD-L @0x000, CD-R @0x400, voice 1 @0x800, voice 3 @0xC00, 512 samples each) are read by DMA
   channel 4 (SPU -> RAM) whenever the SPU IRQ fires. DMA4 reads are implemented. The capture buffers receive the CD
   signal regardless of the SPU's CD volume / enable bit (the game turns both off and still reads the audio).
5. **44.1 kHz tick in the CPU loop.** The CD-DA position, capture buffers and SPU IRQ now advance every 768 CPU
   cycles inside `Cpu::step()` (`spu_cd_tick`), so the IRQ cadence matches hardware (about 3.4 per video frame) and
   headless tools no longer need `audio_frame()` for CD-DA or the IRQ. SPU **voice envelopes** still advance in
   `audio_frame()`, so tools should still call it once per video frame.

`tests/script_run.cpp` drives any menu path with a tiny script, for example
`script_run bios.bin game.cue out "w1500 X w400 X w1500 X w150 X w300 p"` (wN = wait N frames, X/O/T/Q/U/D/L/R/S =
6-frame taps through the tap filter, p = screenshot + state line).

## Music in levels: the SPU reverb unit

Vib-Ribbon plays a level's music through the **SPU reverb unit, used as a delayed PCM player**:

- The game captures the CD audio through the SPU capture buffers (SPU IRQ + DMA channel 4 read-back, see fixes 3-5) and
  analyses it to build the ribbon. It keeps that audio for **~8 s** (the lookahead), then writes it back, 256 bytes per
  SPU IRQ, into the **reverb work area at the very top of SPU RAM (0x7FE00-0x7FFFF)** and sets the reverb output volume
  (vLOUT/vROUT, 0x1F801D84/86) to 0x7FFF. The SPU's CD input stays off the whole time.
- The reverb volume is the game's **music on/off switch**: it is 0 until the lookahead has filled (~8 s after the level
  loads, so no music during the intro), and the game zeroes it on pause (START opens "Go on, try again! / Quit").
  "Go on, try again!" restarts the level and the ~8 s lookahead.
- `spu.h` now implements the reverb unit (psx-spx "SPU Reverb Formula", 22.05 kHz, linear upsampling) inside the
  CPU-timed 44.1 kHz tick, so it stays in step with the game's DMA writes into the tiny ring.
- Verified with `tests/music_timeline.cpp`: the emulator's output correlates with the real CD track 2 audio at **0.99 /
  0.98** with a constant **8.0 s delay**, starts ~8 s after the level loads and goes silent on pause.
  `tests/spu_reverb_test.cpp` unit-tests the reverb as a PCM player.
- Not implemented: reverb *input* from voices (EON) and from the CD, so sound effects keyed with reverb play dry; the
  hardware's FIR resampling is approximated with linear interpolation.
- An earlier "music" workaround (mixing the live CD) was removed: it started the music during the intro and kept
  it playing in the pause menu, which is exactly what the real mechanism does not do.

Diagnostic tool: `tests/cdvol_watch.cpp` logs every change of the SPU CD volume / enable bit and CD play/mute state.

## Launcher and upscaling

Starting the app (without a disc on the command line) opens the **launcher**, drawn with the same smooth vector lines:

1. **Disc**: drop `vibribbon.cue` on the window (checked on the spot: it must be the Vib-Ribbon EU disc).
2. **Mods** (optional): drop a `.zip`. It is checked and remembered, but mods are **not supported yet**; this is the placeholder.
3. **Upscale**: 1x = the original PS1 pixels (512x256), 2x, 3x, 4x (2048x1024, about 1080p). The game's line commands are
   redrawn at N times the resolution with anti-aliasing (`Gpu::set_scale`, `vec.h`); screen clears, frame copies and
   uploads are mirrored, and the normal VRAM the game reads is untouched. 1x output is unchanged and uses no extra memory.
4. **START**. Settings are saved to `vibribbon-settings.ini` next to the exe and restored next time.

Controls: mouse, or arrows/WASD + Enter, or controller D-pad + CROSS (START button = start). Drop `bios.bin` on the launcher
to set the BIOS. In a game, **Esc or F1 returns to the launcher** (Esc in the launcher quits); F11 fullscreen; F10 tap filter.
Giving a `.cue` on the command line skips the launcher.

Cost per frame in my sandbox (software renderer): 1x about 4.7 ms, 2x about 6.6 ms, 4x about 13 ms of emulation + picture
building, so 4x is the heavy setting; if it runs below 50 fps on your machine use 2x or 3x (the log's heartbeat shows fps).
Not yet optimized (the picture is rebuilt every frame).

Developer hook: `VIBRIBBON_EVENTS="drop=PATH;key=up;key=right;key=return;wait=300;key=escape;quit"` injects events (one per 10
frames) so the whole app can be driven headlessly; `tests/launcher_test.cpp` renders the launcher to PPM files.

## Launcher and upscaling

Starting the app with no arguments opens the **launcher** (smooth vector line art, same style as the game):

1. **Disc**: drop your `vibribbon.cue` on the window. It is checked straight away (must be the Vib-Ribbon EU disc).
2. **Mods (optional)**: drop a `.zip` here. It is only validated and remembered; **mods are not supported yet**.
3. **Upscale**: 1x = the original PS1 pixels, 2x / 3x / 4x = the game's lines redrawn smoothly at that multiple
   (4x = 2048 x 1024, about 1080p). Then press **START**.

Controls: mouse (click, hover), arrow keys / WASD + Enter (Tab cycles, Delete clears the mods zip), or a controller
(D-pad + A, Start button). Drop `bios.bin` on the window if it is missing. **Esc** quits from the launcher and returns
to the launcher from the game (F1 does the same). F11 = fullscreen.

Your choices are saved in `vibribbon-settings.ini` next to the exe. `vibribbon.exe path\to\game.cue` still starts
straight away (using the saved upscale).

How the upscaling works (`gpu.h`, `vec.h`): the game's picture is made almost entirely of flat line segments (GP0
`0x40`), a screen clear (`0x02`) and a frame copy (`0x80`). At 2x-4x the GPU keeps a second, N-times larger picture:
every line is drawn into it again with smooth anti-aliased edges, and clears, copies and uploads are mirrored. The
normal 1024x512 VRAM is still maintained exactly as before (the game reads from it), and 1x output is unchanged.
Cost per frame in my sandbox: about 4.7 ms at 1x, 6.6 ms at 2x, 12.9 ms at 4x (budget 20 ms). If your machine cannot
hold 50 fps at 4x, pick 2x or 3x.

Developer hook: `VIBRIBBON_EVENTS="drop=PATH;key=right;key=return;wait=300;quit"` injects scripted events (one per
10 frames) so the whole app can be driven headlessly. New tests: `gpu_hires_test`, `launcher_test`; tools
`hires_shots` (screenshots + timing at a given scale) and `cmd_census` (which GPU commands the game uses).

## Run log (`vibribbon-log.txt`)

The app writes a plain-text log next to the exe (`src/runlog.h`). It is saved every 5 seconds and again when you
close the window or press Esc, so a crash still leaves one behind. The previous run is kept as
`vibribbon-log-previous.txt`. It records:

- startup: SDL/video/audio/controller details, command-line arguments, which disc and BIOS were used, boot result;
- events: files dropped on the window, F10 (tap filter) / F11 (fullscreen), controller connect/disconnect, CPU stops;
- button presses sent to the game (first 400 only, with names such as `DOWN+CROSS`);
- a **heartbeat every 5 s**: fps, game frames advanced, CPU position, CD/SPU activity, buttons held;
- warnings: running slow (< 45 fps), game not advancing, **picture unchanged for 15 s / 60 s / 5 min** (likely stuck);
- a summary at the end: run time, average/min/max fps, slow frames, button changes, last CPU position, exit reason.

If something gets stuck, send `vibribbon-log.txt` and I can usually tell where.

## Still to do

- The launcher has only been exercised with scripted events and screenshots (no real mouse/controller/GPU window yet).
- Mods: the zip is accepted and remembered but not used.
- Upscaled textures/polygons are mirrored block by block (only the language screen uses them); lines are the smooth part.
- Upscale speed on real hardware is untested: 4x ran at ~39-44 fps in my software-rendered sandbox.
- Optimize the upscaled path (dirty-rectangle picture building, SIMD fills) so 4x is cheap everywhere.
- Mods: define what a mods zip contains and apply it.

- `main.cpp` compiles cleanly with `-Wall -Wextra` against SDL 2.30 and was run headlessly with the dummy video/audio
  drivers (50 fps, log written, clean close); on-screen display and real keyboard/controller input are still unverified.
- Play a level by hand (button input during gameplay, scoring, level end, return to the menus).
- Silver / Gold, High scores, Options.
- "Play with my own choice of CD": needs a way to swap in a different CD image.
- The CD-audio sector data path (`ReadS` data) is still never consumed; the game only needs the SPU capture path.
- Reverb input from voices (EON) so sound effects get their reverb; hardware-accurate reverb resampling.
- Find why the menu double-steps on 3+ frame presses (the tap filter is a workaround).
