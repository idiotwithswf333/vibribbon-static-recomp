// Native host for the Vib-Ribbon recompilation.
//
// STATUS: the platform shell works (window, Xbox 360 / any SDL-supported
// gamepad, keyboard fallback). The game code itself is not recompiled yet --
// see README.md "Roadmap". `game_frame()` is where recompiled game code will
// be driven once it exists.
#include <SDL.h>

#include <cstdio>

#include "disc.h"
#include "pad_map.h"

namespace {

SDL_GameController* g_pad = nullptr;

void open_first_controller() {
    if (g_pad) return;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            g_pad = SDL_GameControllerOpen(i);
            if (g_pad) {
                std::printf("Controller: %s\n", SDL_GameControllerName(g_pad));
                return;
            }
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

uint16_t read_keyboard() {
    const Uint8* k = SDL_GetKeyboardState(nullptr);
    uint16_t w = 0;
    if (k[SDL_SCANCODE_UP]) w |= pad::UP;
    if (k[SDL_SCANCODE_DOWN]) w |= pad::DOWN;
    if (k[SDL_SCANCODE_LEFT]) w |= pad::LEFT;
    if (k[SDL_SCANCODE_RIGHT]) w |= pad::RIGHT;
    if (k[SDL_SCANCODE_Z]) w |= pad::CROSS;
    if (k[SDL_SCANCODE_X]) w |= pad::CIRCLE;
    if (k[SDL_SCANCODE_A]) w |= pad::SQUARE;
    if (k[SDL_SCANCODE_S]) w |= pad::TRIANGLE;
    if (k[SDL_SCANCODE_RETURN]) w |= pad::START;
    if (k[SDL_SCANCODE_RSHIFT]) w |= pad::SELECT;
    return w;
}

// Loads a dropped .cue. Returns a status line for the window title and sets
// `ok` when it is a recognisable Vib-Ribbon disc. The game itself cannot run
// yet, so "loaded" means: disc parsed and the boot executables read into memory.
std::string load_dropped_disc(const std::string& cue, bool& ok) {
    ok = false;
    disc::Image img;
    std::string err = img.open(cue);
    if (!err.empty()) return "Error: " + err;
    std::string boot = disc::boot_file(img);
    std::vector<uint8_t> f;
    disc::PsxExe loader, title;
    if (boot.empty() || !img.read_file(boot, f) || !disc::parse_psx_exe(f, loader))
        return "Error: no bootable PS1 executable on disc";
    if (boot != "SCES_028.73" || !img.read_file("MAIN_T.EXE", f) || !disc::parse_psx_exe(f, title))
        return "Loaded disc " + boot + " (not the Vib-Ribbon EU release)";
    ok = true;
    return "Vib-Ribbon (EU) disc OK - boot " + boot + ", MAIN_T.EXE ready. Game execution not implemented yet.";
}

// Placeholder for the recompiled game. Returns the pad word (active-low, as
// the PS1 library would see it) for port 1 so the HLE pad layer can be wired
// in later.
uint16_t game_frame(uint16_t pad_active_low) {
    (void)pad_active_low;
    return pad_active_low;
}

}  // namespace

int main(int, char**) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("Vib-Ribbon (native, WIP)", SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, 640, 480, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC) : nullptr;
    if (!win || !ren) {
        std::fprintf(stderr, "Window/renderer failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_RenderSetLogicalSize(ren, 640, 480);  // keep 4:3 when resized
    open_first_controller();

    uint16_t last = 0xFFFF;
    int disc_state = 0;  // 0 none, 1 recognised Vib-Ribbon disc, 2 error / other disc
    for (bool running = true; running;) {
        for (SDL_Event e; SDL_PollEvent(&e);) {
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_DROPFILE) {
                bool ok;
                std::string msg = load_dropped_disc(e.drop.file, ok);
                SDL_SetWindowTitle(win, msg.c_str());
                std::printf("%s\n", msg.c_str());
                disc_state = ok ? 1 : 2;
                SDL_free(e.drop.file);
            }
            if (e.type == SDL_CONTROLLERDEVICEADDED) open_first_controller();
            if (e.type == SDL_CONTROLLERDEVICEREMOVED && g_pad &&
                e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad))) {
                SDL_GameControllerClose(g_pad);
                g_pad = nullptr;
                open_first_controller();
            }
        }
        uint16_t pressed = pad::to_ps1(read_controller()) | read_keyboard();
        uint16_t wire = game_frame(pad::to_wire(pressed));
        if (wire != last) {
            std::printf("pad word (active-low): 0x%04X\n", wire);
            last = wire;
        }
        if (disc_state == 1) SDL_SetRenderDrawColor(ren, 0, 40, 0, 255);
        else if (disc_state == 2) SDL_SetRenderDrawColor(ren, 50, 0, 0, 255);
        else SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_RenderPresent(ren);
    }
    if (g_pad) SDL_GameControllerClose(g_pad);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
