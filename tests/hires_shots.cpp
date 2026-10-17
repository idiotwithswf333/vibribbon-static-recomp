// hires_shots bios cue scale out_prefix : title menu, hub and a level at the given upscale; reports emulation cost per frame
#include <chrono>
#include <cstdio>
#include <string>
#include "../src/cpu.h"
#include "../src/disc.h"
#include "../src/pad_map.h"
static cpu::Cpu* g; static pad::TapFilter tf; static uint16_t held = 0; static std::vector<int16_t> abuf(882 * 2);
static double t_emu = 0, t_disp = 0; static long nfr = 0; static std::vector<uint32_t> px;
static void run(int n, bool time = false) { for (int i = 0; i < n && !g->halted; ++i) { g->pad1 = tf.apply(held);
    auto a = std::chrono::steady_clock::now(); g->run_frame(); g->audio_frame(882, abuf.data());
    auto b = std::chrono::steady_clock::now(); int w, h; g->gpu.display(px, w, h); auto c = std::chrono::steady_clock::now();
    if (time) { t_emu += std::chrono::duration<double, std::milli>(b - a).count(); t_disp += std::chrono::duration<double, std::milli>(c - b).count(); ++nfr; } } }
static void tap(uint16_t b) { held = b; run(6); held = 0; run(10); }
static void shot(const std::string& fn) { int w, h; g->gpu.display(px, w, h); FILE* o = std::fopen(fn.c_str(), "wb"); std::fprintf(o, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; ++i) { uint8_t b[3] = {(uint8_t)px[i], (uint8_t)(px[i] >> 8), (uint8_t)(px[i] >> 16)}; std::fwrite(b, 1, 3, o); } std::fclose(o); std::printf("saved %s (%dx%d)\n", fn.c_str(), w, h); }
int main(int, char** argv) {
    int scale = atoi(argv[3]); std::string p = argv[4];
    disc::Image img; if (!img.open(argv[2]).empty()) return 2;
    cpu::Cpu c; g = &c; c.trace.enabled = false; c.attach_disc(&img); c.gpu.set_scale(scale);
    if (!c.load_bios(argv[1])) return 2;
    while (!c.halted && c.pc != 0x80030000u && c.icount < 100000000ull) c.step();
    std::vector<uint8_t> f; disc::PsxExe x; img.read_file(disc::boot_file(img), f); disc::parse_psx_exe(f, x);
    if (!c.sideload_exe(x.text, x.load_addr, x.pc, x.gp, x.sp, 10, 4, 0x801FFFF0u)) return 2;
    run(1500); shot(p + "_1_language.ppm");
    tap(pad::CROSS); run(500); shot(p + "_2_title.ppm");
    tap(pad::CROSS); run(1800); shot(p + "_3_hub.ppm");
    tap(pad::CROSS); run(300); tap(pad::CROSS); run(700, true); shot(p + "_4_level.ppm");
    run(300, true); shot(p + "_5_level.ppm");
    std::printf("scale %dx: per frame, emulation %.2f ms + picture build %.2f ms (50 fps budget = 20 ms)\n", scale, t_emu / nfr, t_disp / nfr);
}
