// Vib-Ribbon native host: drop (or pass on the command line) vibribbon.cue and it boots.
//
// The game's own code runs on a MIPS interpreter (src/cpu.h) with the PS1 GPU, GTE,
// CD-ROM, SPU (24 ADPCM voices + ADSR), CD-DA audio, DMA and interrupts emulated.
#include <SDL.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "cpu.h"
#include "disc.h"
#include <initializer_list>
#include "pad_map.h"

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

// One running game. The disc image must outlive the CPU that reads from it.
struct Emulator {
    std::unique_ptr<disc::Image> img;
    std::unique_ptr<cpu::Cpu> cpu;
};

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
    auto cpu = std::make_unique<cpu::Cpu>();
    cpu->trace.enabled = false;   // statistics are for the headless tools; they cost a lot of speed
    cpu->attach_disc(img.get());
    cpu->load_exe(exe.text, exe.load_addr, exe.pc, exe.gp, exe.sp);
    emu.cpu.reset();
    emu.img = std::move(img);
    emu.cpu = std::move(cpu);
    return "";
}

}  // namespace

int main(int argc, char** argv) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("Vib-Ribbon - drop vibribbon.cue here", SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, 960, 720, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED) : nullptr;
    if (!win || !ren) {
        std::fprintf(stderr, "Window/renderer failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_RenderSetLogicalSize(ren, 640, 480);   // always shown 4:3, like a TV
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    open_first_controller();

    // Audio: 44.1 kHz stereo, fed one video frame (882 sample frames at 50 Hz) at a time.
    SDL_AudioSpec want{}, have{};
    want.freq = 44100; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
    SDL_AudioDeviceID audio = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (audio) SDL_PauseAudioDevice(audio, 0);
    else std::fprintf(stderr, "Audio unavailable: %s\n", SDL_GetError());
    constexpr int kAudioFrames = 44100 / 50;
    std::vector<int16_t> audio_buf((size_t)kAudioFrames * 2);

    Emulator emu;
    SDL_Texture* tex = nullptr;
    int tex_w = 0, tex_h = 0;
    std::vector<uint32_t> pixels;
    bool fullscreen = false;
    uint32_t frames = 0, last_title = SDL_GetTicks();

    auto start = [&](const std::string& cue) {
        std::string msg = boot_disc(cue, emu);
        if (!msg.empty()) { SDL_SetWindowTitle(win, msg.c_str()); std::printf("%s\n", msg.c_str()); }
        else SDL_SetWindowTitle(win, "Vib-Ribbon");
    };
    if (argc > 1) start(argv[1]);

    for (bool running = true; running;) {
        uint32_t frame_start = SDL_GetTicks();
        for (SDL_Event e; SDL_PollEvent(&e);) {
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_DROPFILE) { start(e.drop.file); SDL_free(e.drop.file); }
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) running = false;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F11) {
                fullscreen = !fullscreen;
                SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
            }
            if (e.type == SDL_CONTROLLERDEVICEADDED) open_first_controller();
            if (e.type == SDL_CONTROLLERDEVICEREMOVED && g_pad &&
                e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad))) {
                SDL_GameControllerClose(g_pad);
                g_pad = nullptr;
                open_first_controller();
            }
        }

        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        if (emu.cpu) {
            emu.cpu->pad1 = pad::to_ps1(read_controller()) | read_keyboard();
            static uint16_t last_pad = 0;   // log pad changes so you can see the keyboard is being read
            if (emu.cpu->pad1 != last_pad) { last_pad = emu.cpu->pad1; std::fprintf(stderr, "pad: %04X\n", last_pad); }
            if (!emu.cpu->run_frame() && emu.cpu->halted) {
                std::string m = "CPU stopped: " + emu.cpu->halt_reason;
                SDL_SetWindowTitle(win, m.c_str());
                std::fprintf(stderr, "%s\n", m.c_str());
                emu.cpu.reset();   // back to waiting for a disc
                if (audio) SDL_ClearQueuedAudio(audio);
            } else {
                if (audio) {
                    // Keep the queue shallow (~100 ms) so sound stays in sync with the picture.
                    if (SDL_GetQueuedAudioSize(audio) < (Uint32)kAudioFrames * 4 * 5) {
                        emu.cpu->audio_frame(kAudioFrames, audio_buf.data());
                        SDL_QueueAudio(audio, audio_buf.data(), (Uint32)audio_buf.size() * 2);
                    } else emu.cpu->audio_frame(kAudioFrames, audio_buf.data());   // still advance the SPU
                }
                int w = 0, h = 0;
                emu.cpu->gpu.display(pixels, w, h);
                if (!tex || w != tex_w || h != tex_h) {
                    if (tex) SDL_DestroyTexture(tex);
                    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, w, h);
                    tex_w = w; tex_h = h;
                }
                SDL_UpdateTexture(tex, nullptr, pixels.data(), w * 4);
                SDL_RenderCopy(ren, tex, nullptr, nullptr);
                ++frames;
            }
        }
        SDL_RenderPresent(ren);

        uint32_t now = SDL_GetTicks();
        if (emu.cpu && now - last_title >= 1000) {   // FPS in the title bar
            char t[96];
            std::snprintf(t, sizeof t, "Vib-Ribbon - %u fps (target 50)", frames);
            SDL_SetWindowTitle(win, t);
            frames = 0; last_title = now;
        }
        // Hold the PAL 50 Hz pace; if emulation is slower than that we simply run flat out.
        uint32_t spent = SDL_GetTicks() - frame_start;
        if (emu.cpu && spent < 20) SDL_Delay(20 - spent);
        else if (!emu.cpu) SDL_Delay(16);
    }
    if (tex) SDL_DestroyTexture(tex);
    if (g_pad) SDL_GameControllerClose(g_pad);
    if (audio) SDL_CloseAudioDevice(audio);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
