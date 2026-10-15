// Vib-Ribbon native host: drop (or pass on the command line) vibribbon.cue and it boots.
//
// The game's own code runs on a MIPS interpreter (src/cpu.h) with the PS1 GPU, GTE,
// CD-ROM, SPU (24 ADPCM voices + ADSR), CD-DA audio, DMA and interrupts emulated.
#include <SDL.h>

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "cpu.h"
#include "disc.h"
#include <initializer_list>
#include "pad_map.h"
#include "runlog.h"

namespace {

SDL_GameController* g_pad = nullptr;

void open_first_controller() {
    if (g_pad) return;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            g_pad = SDL_GameControllerOpen(i);
            if (g_pad) { std::printf("Controller: %s\n", SDL_GameControllerName(g_pad)); return; }
        }
    }
}

pad::XboxState read_controller() {
    pad::XboxState s{};
    if (!g_pad) return s;
    auto b = [](SDL_GameControllerButton btn) { return SDL_GameControllerGetButton(g_pad, btn) != 0; };
    auto ax = [](SDL_GameControllerAxis a) { return SDL_GameControllerGetAxis(g_pad, a); };
    s.a = b(SDL_CONTROLLER_BUTTON_A);
    s.b = b(SDL_CONTROLLER_BUTTON_B);
    s.x = b(SDL_CONTROLLER_BUTTON_X);
    s.y = b(SDL_CONTROLLER_BUTTON_Y);
    s.lb = b(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    s.rb = b(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    s.back = b(SDL_CONTROLLER_BUTTON_BACK);
    s.start = b(SDL_CONTROLLER_BUTTON_START);
    s.dpad_up = b(SDL_CONTROLLER_BUTTON_DPAD_UP);
    s.dpad_down = b(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    s.dpad_left = b(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    s.dpad_right = b(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    s.lx = ax(SDL_CONTROLLER_AXIS_LEFTX);
    s.ly = ax(SDL_CONTROLLER_AXIS_LEFTY);
    s.lt = ax(SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    s.rt = ax(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    return s;
}

// Keyboard -> PS1 pad. Several keys per button so any common layout works.
//   D-pad: arrows or W/A/S/D     Cross: Z, Space      Circle: X
//   Square: C                    Triangle: V          Start: Enter / Keypad Enter
//   Select: Right Shift / Backspace     L1/R1: Q/E     L2/R2: 1/3
uint16_t read_keyboard() {
    const Uint8* k = SDL_GetKeyboardState(nullptr);
    auto any = [&](std::initializer_list<SDL_Scancode> l) { for (auto sc : l) if (k[sc]) return true; return false; };
    uint16_t w = 0;
    if (any({SDL_SCANCODE_UP, SDL_SCANCODE_W})) w |= pad::UP;
    if (any({SDL_SCANCODE_DOWN, SDL_SCANCODE_S})) w |= pad::DOWN;
    if (any({SDL_SCANCODE_LEFT, SDL_SCANCODE_A})) w |= pad::LEFT;
    if (any({SDL_SCANCODE_RIGHT, SDL_SCANCODE_D})) w |= pad::RIGHT;
    if (any({SDL_SCANCODE_Z, SDL_SCANCODE_SPACE})) w |= pad::CROSS;
    if (any({SDL_SCANCODE_X})) w |= pad::CIRCLE;
    if (any({SDL_SCANCODE_C})) w |= pad::SQUARE;
    if (any({SDL_SCANCODE_V})) w |= pad::TRIANGLE;
    if (any({SDL_SCANCODE_RETURN, SDL_SCANCODE_KP_ENTER})) w |= pad::START;
    if (any({SDL_SCANCODE_RSHIFT, SDL_SCANCODE_BACKSPACE})) w |= pad::SELECT;
    if (any({SDL_SCANCODE_Q})) w |= pad::L1;
    if (any({SDL_SCANCODE_E})) w |= pad::R1;
    if (any({SDL_SCANCODE_1})) w |= pad::L2;
    if (any({SDL_SCANCODE_3})) w |= pad::R2;
    return w;
}

pad::TapFilter g_tap;   // see pad_map.h; F10 toggles it

runlog::Log g_log;      // saved as vibribbon-log.txt next to the exe (see runlog.h)

void crash_handler(int sig) {   // best effort: note the crash and save what we have
    g_log.note("CRASH: the app received signal %d", sig);
    g_log.save();
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

std::string pad_names(uint16_t w) {
    if (!w) return "(all released)";
    static const struct { uint16_t bit; const char* name; } kNames[] = {
        {pad::UP, "UP"}, {pad::DOWN, "DOWN"}, {pad::LEFT, "LEFT"}, {pad::RIGHT, "RIGHT"}, {pad::CROSS, "CROSS"},
        {pad::CIRCLE, "CIRCLE"}, {pad::SQUARE, "SQUARE"}, {pad::TRIANGLE, "TRIANGLE"}, {pad::L1, "L1"}, {pad::R1, "R1"},
        {pad::L2, "L2"}, {pad::R2, "R2"}, {pad::START, "START"}, {pad::SELECT, "SELECT"}};
    std::string r;
    for (auto& n : kNames) if (w & n.bit) { if (!r.empty()) r += "+"; r += n.name; }
    return r;
}

uint64_t hash_pixels(const std::vector<uint32_t>& p) {   // cheap fingerprint of the picture, to spot a frozen screen
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < p.size(); i += 13) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// One running game. The disc image must outlive the CPU that reads from it.
struct Emulator {
    std::unique_ptr<disc::Image> img;
    std::unique_ptr<cpu::Cpu> cpu;
};

// A PS1 BIOS image is exactly 512 KB and contains the Sony copyright string.
bool looks_like_bios(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec) || std::filesystem::file_size(p, ec) != 512 * 1024) return false;
    std::ifstream f(p, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return data.find("Sony Computer Entertainment") != std::string::npos;
}

// Look for a BIOS next to the exe, in the working directory, and next to the disc image.
std::string find_bios(const std::string& cue) {
    std::vector<std::filesystem::path> dirs;
    if (char* base = SDL_GetBasePath()) { dirs.push_back(base); SDL_free(base); }
    dirs.push_back(std::filesystem::current_path());
    if (!cue.empty()) dirs.push_back(std::filesystem::path(cue).parent_path());
    for (auto& d : dirs) {
        std::error_code ec;
        if (d.empty() || !std::filesystem::is_directory(d, ec)) continue;
        for (auto& e : std::filesystem::directory_iterator(d, ec)) {
            std::string ext = e.path().extension().string();
            for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
            if (ext == ".bin" && looks_like_bios(e.path())) return e.path().string();
        }
    }
    return "";
}

std::string g_bios;   // path of the BIOS in use ("" = none found)

// Returns an empty string on success, otherwise a message for the window title.
std::string boot_disc(const std::string& cue, Emulator& emu) {
    auto img = std::make_unique<disc::Image>();
    std::string err = img->open(cue);
    if (!err.empty()) return "Error: " + err;
    std::string boot = disc::boot_file(*img);
    std::vector<uint8_t> f;
    disc::PsxExe exe;
    if (boot.empty() || !img->read_file(boot, f) || !disc::parse_psx_exe(f, exe))
        return "Error: no bootable PS1 executable on disc";
    if (boot != "SCES_028.73")
        return "This is " + boot + ", not the Vib-Ribbon EU disc (SCES_028.73); not supported";
    if (g_bios.empty()) g_bios = find_bios(cue);

    auto cpu = std::make_unique<cpu::Cpu>();
    cpu->trace.enabled = false;   // statistics are for the headless tools; they cost a lot of speed
    cpu->attach_disc(img.get());
    std::string note;
    if (!g_bios.empty() && cpu->load_bios(g_bios)) {
        // Real BIOS: boot to the shell (~5 million instructions, a fraction of a second), then start the
        // disc's executable under the real kernel exactly as the shell would. The kernel is what makes the
        // game's controller driver run, so buttons only work this way.
        while (!cpu->halted && cpu->pc != 0x80030000u && cpu->icount < 100000000ull) cpu->step();
        if (cpu->halted || cpu->pc != 0x80030000u)
            return "BIOS did not reach the shell: " + (cpu->halted ? cpu->halt_reason : std::string("timeout"));
        if (!cpu->sideload_exe(exe.text, exe.load_addr, exe.pc, exe.gp, exe.sp, 10, 4, 0x801FFFF0u))
            return "Could not start the game: " + cpu->halt_reason;
    } else {
        cpu->load_exe(exe.text, exe.load_addr, exe.pc, exe.gp, exe.sp);
        note = "No BIOS found - controller will NOT work. Put your PS1 BIOS (.bin, 512 KB) next to vibribbon.exe or drop it on the window.";
    }
    emu.cpu.reset();
    emu.img = std::move(img);
    emu.cpu = std::move(cpu);
    return note.empty() ? "" : "!" + note;   // leading '!' = started, but with a warning
}

}  // namespace

int main(int argc, char** argv) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    {
        std::string dir = ".";
        if (char* base = SDL_GetBasePath()) { dir = base; SDL_free(base); }
        if (!g_log.open(dir) && !g_log.open(std::filesystem::current_path().string()))
            std::fprintf(stderr, "Could not create vibribbon-log.txt (folder not writable)\n");
        std::signal(SIGSEGV, crash_handler); std::signal(SIGABRT, crash_handler); std::signal(SIGFPE, crash_handler);
        SDL_version v; SDL_GetVersion(&v);
        g_log.note("App started. SDL %d.%d.%d, video driver: %s", v.major, v.minor, v.patch, SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?");
        g_log.note("Log file: %s", g_log.path().c_str());
        for (int i = 1; i < argc; ++i) g_log.note("Command line argument %d: %s", i, argv[i]);
    }
    SDL_Window* win = SDL_CreateWindow("Vib-Ribbon - drop vibribbon.cue here", SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, 960, 720, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED) : nullptr;
    if (!win || !ren) {
        std::fprintf(stderr, "Window/renderer failed: %s\n", SDL_GetError());
        g_log.note("ERROR: window/renderer failed: %s", SDL_GetError());
        return 1;
    }
    { SDL_RendererInfo ri; if (SDL_GetRendererInfo(ren, &ri) == 0) g_log.note("Renderer: %s", ri.name); }
    SDL_RenderSetLogicalSize(ren, 640, 480);   // always shown 4:3, like a TV
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    open_first_controller();
    g_log.note("Controller: %s", g_pad ? SDL_GameControllerName(g_pad) : "none connected (keyboard only)");

    // Audio: 44.1 kHz stereo, fed one video frame (882 sample frames at 50 Hz) at a time.
    SDL_AudioSpec want{}, have{};
    want.freq = 44100; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
    SDL_AudioDeviceID audio = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (audio) { SDL_PauseAudioDevice(audio, 0); g_log.note("Audio: opened %d Hz, %d channels, driver %s", have.freq, (int)have.channels, SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?"); }
    else { std::fprintf(stderr, "Audio unavailable: %s\n", SDL_GetError()); g_log.note("WARNING: no audio device (%s). The game still runs; sound is silent.", SDL_GetError()); }
    constexpr int kAudioFrames = 44100 / 50;
    std::vector<int16_t> audio_buf((size_t)kAudioFrames * 2);

    Emulator emu;
    SDL_Texture* tex = nullptr;
    int tex_w = 0, tex_h = 0;
    std::vector<uint32_t> pixels;
    bool fullscreen = false;
    uint32_t frames = 0, last_title = SDL_GetTicks();

    // Run statistics for the log.
    uint32_t hb_last = SDL_GetTicks(), hb_frames = 0, total_frames = 0, slow_frames = 0, max_frame_ms = 0;
    uint32_t pad_changes = 0, stall_warnings = 0, hb_count = 0;
    double fps_min = 1e9, fps_max = 0;
    uint64_t last_hash = 0, last_vblanks = 0; int same_hash = 0;
    std::string last_halt = "no", last_disc = "(none)";
    uint16_t cur_pad = 0;
    auto update_summary = [&](const char* how) {
        char b[900]; double run = g_log.elapsed();
        std::snprintf(b, sizeof b,
            "Run time: %.1f s (%s)\nFrames shown: %u (average %.1f fps, target 50)\nHeartbeat fps: min %.1f, max %.1f\n"
            "Slow frames (over 40 ms): %u, worst frame: %u ms\nButton changes: %u\nScreen-unchanged warnings: %u\n"
            "Disc: %s\nBIOS: %s\nCPU stopped: %s\nLast CPU position: %s\nD-pad tap filter: %s",
            run, how, total_frames, run > 0 ? total_frames / run : 0.0, hb_count ? fps_min : 0.0, fps_max, slow_frames, max_frame_ms,
            pad_changes, stall_warnings, last_disc.c_str(), g_bios.empty() ? "none (controller will not work)" : g_bios.c_str(),
            last_halt.c_str(),
            emu.cpu ? ([&] { char t[16]; std::snprintf(t, sizeof t, "%08X", emu.cpu->pc); return std::string(t); })().c_str() : "n/a",
            g_tap.enabled ? "on" : "off");
        g_log.set_summary(b);
    };

    std::string last_cue;
    uint32_t warn_until = 0;
    auto start = [&](const std::string& cue) {
        last_cue = cue; last_disc = cue;
        g_log.note("Loading disc: %s", cue.c_str());
        std::string msg = boot_disc(cue, emu);
        g_log.note("BIOS in use: %s", g_bios.empty() ? "none found" : g_bios.c_str());
        g_log.note("Boot result: %s", msg.empty() ? "started OK" : msg.c_str());
        last_halt = "no"; hb_last = SDL_GetTicks(); hb_frames = 0; same_hash = 0; last_hash = 0; last_vblanks = 0;
        if (msg.empty()) SDL_SetWindowTitle(win, "Vib-Ribbon");
        else {
            bool warn = msg[0] == '!';
            if (warn) msg = msg.substr(1);
            SDL_SetWindowTitle(win, msg.c_str());
            std::printf("%s\n", msg.c_str());
            if (warn) warn_until = SDL_GetTicks() + 15000; else emu.cpu.reset();
        }
    };
    if (argc > 1) start(argv[1]);

    for (bool running = true; running;) {
        uint32_t frame_start = SDL_GetTicks();
        for (SDL_Event e; SDL_PollEvent(&e);) {
            if (e.type == SDL_QUIT) { running = false; g_log.note("Window closed by the user"); }
            if (e.type == SDL_DROPFILE) {
                std::filesystem::path dropped = e.drop.file;
                g_log.note("File dropped on the window: %s", e.drop.file);
                if (looks_like_bios(dropped)) {   // a BIOS dropped on the window: remember it next to the exe and restart
                    g_bios = dropped.string();
                    if (char* base = SDL_GetBasePath()) {
                        std::error_code ec; std::filesystem::copy_file(dropped, std::filesystem::path(base) / "bios.bin",
                                                                       std::filesystem::copy_options::overwrite_existing, ec);
                        SDL_free(base);
                    }
                    if (!last_cue.empty()) start(last_cue); else SDL_SetWindowTitle(win, "BIOS loaded - now drop vibribbon.cue");
                } else start(e.drop.file);
                SDL_free(e.drop.file);
            }
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) { running = false; g_log.note("Esc pressed: closing"); }
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F10) {
                g_tap.enabled = !g_tap.enabled;
                std::fprintf(stderr, "D-pad tap filter %s\n", g_tap.enabled ? "ON" : "OFF");
                g_log.note("D-pad tap filter turned %s (F10)", g_tap.enabled ? "ON" : "OFF");
            }
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F11) {
                fullscreen = !fullscreen;
                g_log.note("Fullscreen %s (F11)", fullscreen ? "on" : "off");
                SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
            }
            if (e.type == SDL_CONTROLLERDEVICEADDED) { open_first_controller(); g_log.note("Controller connected: %s", g_pad ? SDL_GameControllerName(g_pad) : "?"); }
            if (e.type == SDL_CONTROLLERDEVICEREMOVED && g_pad &&
                e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad))) {
                SDL_GameControllerClose(g_pad);
                g_pad = nullptr;
                g_log.note("Controller disconnected");
                open_first_controller();
            }
        }

        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        if (emu.cpu) {
            emu.cpu->pad1 = g_tap.apply(pad::to_ps1(read_controller()) | read_keyboard());
            static uint16_t last_pad = 0;   // log pad changes so you can see the keyboard is being read
            if (emu.cpu->pad1 != last_pad) {
                last_pad = emu.cpu->pad1; cur_pad = last_pad; ++pad_changes;
                std::fprintf(stderr, "pad: %04X\n", last_pad);
                g_log.noisy("Buttons sent to the game: %s", pad_names(last_pad).c_str());
            }
            if (!emu.cpu->run_frame() && emu.cpu->halted) {
                std::string m = "CPU stopped: " + emu.cpu->halt_reason;
                SDL_SetWindowTitle(win, m.c_str());
                std::fprintf(stderr, "%s\n", m.c_str());
                g_log.note("ERROR: %s (CPU position %08X)", m.c_str(), emu.cpu->pc);
                last_halt = emu.cpu->halt_reason + " (at " + [&] { char t[16]; std::snprintf(t, sizeof t, "%08X", emu.cpu->pc); return std::string(t); }() + ")";
                update_summary("CPU stopped"); g_log.save();
                emu.cpu.reset();   // back to waiting for a disc
                if (audio) SDL_ClearQueuedAudio(audio);
            } else {
                // The SPU voice envelopes and the CD-audio read position only advance inside audio_frame(), and
                // the game WAITS on both (e.g. "has the sound effect finished?", "has the CD reached the track?").
                // So this must run once per video frame even when there is no sound device or the queue is full.
                emu.cpu->audio_frame(kAudioFrames, audio_buf.data());
                // Keep the queue shallow (~100 ms) so sound stays in sync with the picture.
                if (audio && SDL_GetQueuedAudioSize(audio) < (Uint32)kAudioFrames * 4 * 5)
                    SDL_QueueAudio(audio, audio_buf.data(), (Uint32)audio_buf.size() * 2);
                int w = 0, h = 0;
                emu.cpu->gpu.display(pixels, w, h);
                if (!tex || w != tex_w || h != tex_h) {
                    if (tex) SDL_DestroyTexture(tex);
                    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, w, h);
                    tex_w = w; tex_h = h;
                }
                SDL_UpdateTexture(tex, nullptr, pixels.data(), w * 4);
                SDL_RenderCopy(ren, tex, nullptr, nullptr);
                ++frames; ++hb_frames; ++total_frames;
            }
        }
        SDL_RenderPresent(ren);

        uint32_t now = SDL_GetTicks();
        if (emu.cpu && now - last_title >= 1000 && now > warn_until) {   // FPS in the title bar
            char t[160];
            std::snprintf(t, sizeof t, "Vib-Ribbon - %u fps (target 50) | F10 tap filter: %s", frames, g_tap.enabled ? "ON" : "OFF");
            SDL_SetWindowTitle(win, t);
            frames = 0; last_title = now;
        }
        if (emu.cpu && now - hb_last >= 5000) {   // heartbeat every 5 s: how the emulation is doing
            double secs = (now - hb_last) / 1000.0, fps = hb_frames / secs;
            fps_min = std::min(fps_min, fps); fps_max = std::max(fps_max, fps); ++hb_count;
            auto& c = *emu.cpu;
            g_log.note("Heartbeat: %.1f fps (target 50) | game frames +%llu | CPU at %08X | CD %s%s, %llu sectors read, %llu commands | SPU key-ons %llu, SPU read-backs %llu, interrupts %llu | SPU CD input: %s | music (reverb) volume %04X/%04X%s | buttons: %s",
                       fps, (unsigned long long)(c.vblanks - last_vblanks), c.pc, c.cd.playing ? "playing audio" : "not playing audio",
                       c.cd.muted ? " (muted)" : "", (unsigned long long)c.cd.sectors_read, (unsigned long long)c.cd.cmds,
                       (unsigned long long)c.spu.key_ons, (unsigned long long)c.dma4_reads, (unsigned long long)c.irqs_taken,
                       c.spu.cd_input_audible() ? "on" : "off", c.spu.rvb_vol_l(), c.spu.rvb_vol_r(),
                       (c.spu.rvb_vol_l() | c.spu.rvb_vol_r()) ? " (music playing)" : " (silent)", pad_names(cur_pad).c_str());
            if (fps < 45.0 && hb_frames > 0) g_log.note("WARNING: running slow (%.1f fps). The game will play too slowly.", fps);
            if (c.vblanks == last_vblanks) g_log.note("WARNING: the game did not advance a single frame in %.0f s", secs);
            last_vblanks = c.vblanks;
            uint64_t h = hash_pixels(pixels);
            if (h == last_hash) {
                ++same_hash;
                if (same_hash == 3 || same_hash == 12 || same_hash == 60) {
                    ++stall_warnings;
                    g_log.note("WARNING: the picture has not changed for %d s (CPU at %08X). Normal on a still screen, but if you expected movement the game may be stuck.", same_hash * 5, c.pc);
                }
            } else {
                if (same_hash >= 3) g_log.note("Picture is changing again after %d s of no change", same_hash * 5);
                same_hash = 0; last_hash = h;
            }
            update_summary("still running"); g_log.save();
            hb_last = now; hb_frames = 0;
        }
        // Hold the PAL 50 Hz pace; if emulation is slower than that we simply run flat out.
        uint32_t spent = SDL_GetTicks() - frame_start;
        if (emu.cpu && spent > 40) { ++slow_frames; max_frame_ms = std::max(max_frame_ms, spent); }
        if (emu.cpu && spent < 20) SDL_Delay(20 - spent);
        else if (!emu.cpu) SDL_Delay(16);
    }
    g_log.note("Shutting down after %u frames", total_frames);
    update_summary("closed normally");
    g_log.save();
    if (tex) SDL_DestroyTexture(tex);
    if (g_pad) SDL_GameControllerClose(g_pad);
    if (audio) SDL_CloseAudioDevice(audio);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
