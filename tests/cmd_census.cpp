// cmd_census bios cue : which GP0 commands does the game use in the title, hub, tutorial and a level?
#include <cstdio>
#include <string>
#include "../src/cpu.h"
#include "../src/disc.h"
#include "../src/pad_map.h"
static cpu::Cpu* g; static pad::TapFilter tf; static uint16_t held = 0; static std::vector<int16_t> abuf(882 * 2);
static void run(int n) { for (int i = 0; i < n && !g->halted; ++i) { g->pad1 = tf.apply(held); g->run_frame(); g->audio_frame(882, abuf.data()); } }
static void tap(uint16_t b) { held = b; run(6); held = 0; run(10); }
static void report(const char* what) {
    std::printf("%-22s", what); for (auto& kv : g->gpu.cmd_hist) std::printf(" %02X:%llu", kv.first, (unsigned long long)kv.second); std::printf("\n"); g->gpu.cmd_hist.clear(); }
int main(int, char** argv) {
    disc::Image img; if (!img.open(argv[2]).empty()) return 2;
    cpu::Cpu c; g = &c; c.trace.enabled = false; c.attach_disc(&img);
    if (!c.load_bios(argv[1])) return 2;
    while (!c.halted && c.pc != 0x80030000u && c.icount < 100000000ull) c.step();
    std::vector<uint8_t> f; disc::PsxExe x; img.read_file(disc::boot_file(img), f); disc::parse_psx_exe(f, x);
    if (!c.sideload_exe(x.text, x.load_addr, x.pc, x.gp, x.sp, 10, 4, 0x801FFFF0u)) return 2;
    g->gpu.cmd_hist.clear(); run(1500); report("language screen");
    tap(pad::CROSS); run(500); g->gpu.cmd_hist.clear(); run(100); report("title menu (100 fr)");
    tap(pad::CROSS); run(1800); g->gpu.cmd_hist.clear(); run(100); report("game hub (100 fr)");
    tap(pad::CROSS); run(300); tap(pad::CROSS); g->gpu.cmd_hist.clear(); run(300); report("level (300 fr)");
    run(600); g->gpu.cmd_hist.clear(); run(100); report("level, later (100)");
}
