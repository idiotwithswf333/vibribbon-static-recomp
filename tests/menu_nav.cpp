// menu_nav bios.bin game.cue out_prefix -- same boot path as the app, then taps buttons and screenshots each step.
#include <cstdio>
#include <string>
#include "../src/cpu.h"
#include "../src/disc.h"
#include "../src/pad_map.h"
static cpu::Cpu* g;
static void shot(const std::string& fn) {
    std::vector<uint32_t> px; int w = 0, h = 0; g->gpu.display(px, w, h);
    FILE* o = std::fopen(fn.c_str(), "wb"); std::fprintf(o, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; ++i) { uint8_t b[3] = {(uint8_t)px[i], (uint8_t)(px[i] >> 8), (uint8_t)(px[i] >> 16)}; std::fwrite(b, 1, 3, o); }
    std::fclose(o);
}
static void run(int n) { for (int i = 0; i < n && !g->halted; ++i) g->run_frame(); }
static void tap(uint16_t b, int after) { g->pad1 = b; run(6); g->pad1 = 0; run(after); }
int main(int, char** argv) {
    disc::Image img; if (!img.open(argv[2]).empty()) return 1;
    cpu::Cpu c; g = &c; c.trace.enabled = false; c.attach_disc(&img);
    if (!c.load_bios(argv[1])) return 1;
    while (!c.halted && c.pc != 0x80030000u && c.icount < 100000000ull) c.step();
    std::vector<uint8_t> f; disc::PsxExe x; img.read_file(disc::boot_file(img), f); disc::parse_psx_exe(f, x);
    if (!c.sideload_exe(x.text, x.load_addr, x.pc, x.gp, x.sp, 10, 4, 0x801FFFF0u)) return 1;
    std::string p = argv[3];
    run(1500);              shot(p + "_0.ppm");
    tap(pad::CROSS, 150);   shot(p + "_1.ppm");   // confirm language
    tap(pad::DOWN, 40);     shot(p + "_2.ppm");
    tap(pad::DOWN, 40);     shot(p + "_3.ppm");
    tap(pad::UP, 40);       shot(p + "_4.ppm");
    tap(pad::UP, 40);       shot(p + "_5.ppm");
    std::printf("halted=%d %s frames=%llu\n", c.halted, c.halt_reason.c_str(), (unsigned long long)c.vblanks);
}
