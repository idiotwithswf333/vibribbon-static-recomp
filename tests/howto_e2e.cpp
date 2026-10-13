// howto_e2e bios.bin game.cue out_prefix -- human-style 6-frame taps (through pad::TapFilter) from boot to the
// HOW TO PLAY tutorial. Exit code 0 = reached the tutorial (CD audio demuted and playing, no halt).
#include <cstdio>
#include <string>
#include "../src/cpu.h"
#include "../src/disc.h"
#include "../src/pad_map.h"
static cpu::Cpu* g;
static pad::TapFilter tf;
static std::vector<int16_t> abuf(882 * 2);
static void shot(const std::string& fn) {
    std::vector<uint32_t> px; int w = 0, h = 0; g->gpu.display(px, w, h);
    FILE* o = std::fopen(fn.c_str(), "wb"); std::fprintf(o, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; ++i) { uint8_t b[3] = {(uint8_t)px[i], (uint8_t)(px[i] >> 8), (uint8_t)(px[i] >> 16)}; std::fwrite(b, 1, 3, o); }
    std::fclose(o);
}
static uint16_t held = 0;
static void run(int n) { for (int i = 0; i < n && !g->halted; ++i) { g->pad1 = tf.apply(held); g->run_frame(); g->audio_frame(882, abuf.data()); } }
static void tap(uint16_t b, int hold, int after) { held = b; run(hold); held = 0; run(after); }
int main(int, char** argv) {
    disc::Image img; if (!img.open(argv[2]).empty()) return 2;
    cpu::Cpu c; g = &c; c.trace.enabled = false; c.attach_disc(&img);
    if (!c.load_bios(argv[1])) return 2;
    while (!c.halted && c.pc != 0x80030000u && c.icount < 100000000ull) c.step();
    std::vector<uint8_t> f; disc::PsxExe x; img.read_file(disc::boot_file(img), f); disc::parse_psx_exe(f, x);
    if (!c.sideload_exe(x.text, x.load_addr, x.pc, x.gp, x.sp, 10, 4, 0x801FFFF0u)) return 2;
    std::string p = argv[3];
    run(1500);                       shot(p + "_1_language.ppm");
    tap(pad::CROSS, 6, 400);         shot(p + "_2_menu.ppm");
    tap(pad::DOWN, 6, 60);           shot(p + "_3_howto_highlighted.ppm");
    tap(pad::CROSS, 6, 60);          shot(p + "_4_loading.ppm");
    run(500);                        shot(p + "_5_tutorial.ppm");
    run(600);                        shot(p + "_6_tutorial.ppm");
    bool ok = !c.halted && c.cd.playing && !c.cd.muted;
    std::printf("%s: halted=%d cd.playing=%d cd.muted=%d\n", ok ? "PASS reached HOW TO PLAY" : "FAIL", c.halted, (int)c.cd.playing, (int)c.cd.muted);
    return ok ? 0 : 1;
}
