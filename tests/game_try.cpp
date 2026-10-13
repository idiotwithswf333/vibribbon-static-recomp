// game_try bios.bin game.cue -- boot, confirm language, run MAIN_G; reports CD activity and where it waits.
#include <cstdio>
#include "../src/cpu.h"
#include "../src/disc.h"
#include "../src/pad_map.h"
int main(int, char** argv) {
    disc::Image img; if (!img.open(argv[2]).empty()) return 1;
    cpu::Cpu c; c.trace.enabled = true; c.attach_disc(&img);
    if (!c.load_bios(argv[1])) return 1;
    while (!c.halted && c.pc != 0x80030000u) c.step();
    std::vector<uint8_t> f; disc::PsxExe x; img.read_file(disc::boot_file(img), f); disc::parse_psx_exe(f, x);
    c.sideload_exe(x.text, x.load_addr, x.pc, x.gp, x.sp, 10, 4, 0x801FFFF0u);
    for (int i = 0; i < 1500 && !c.halted; ++i) c.run_frame();
    c.pad1 = pad::CROSS; for (int i = 0; i < 6; ++i) c.run_frame(); c.pad1 = 0;
    for (int k = 0; k < 8; ++k) {
        uint64_t s0 = c.cd.sectors_read, m0 = c.cd.cmds;
        for (int i = 0; i < 100 && !c.halted; ++i) c.run_frame();
        std::printf("t+%d00: sectors+%llu cmds+%llu pc=%08X halted=%d\n", k + 1, (unsigned long long)(c.cd.sectors_read - s0), (unsigned long long)(c.cd.cmds - m0), c.pc, c.halted);
    }
    std::printf("LOG:\n%s\n", c.trace.log.substr(c.trace.log.size() > 800 ? c.trace.log.size() - 800 : 0).c_str());
    { auto& h2 = c.cd.cmd_hist; size_t first13 = 0; for (size_t i = 0; i < h2.size(); ++i) if (h2[i].find("cmd 13") != std::string::npos) { first13 = i; break; }
      std::printf("commands from first GetTN (index %zu of %zu):\n", first13, h2.size()); for (size_t i = first13 > 6 ? first13 - 6 : 0; i < h2.size() && i < first13 + 24; ++i) std::printf("  [%zu] %s\n", i, h2[i].c_str()); }
    std::printf("last CD cmds:\n"); auto& h = c.cd.cmd_hist; for (size_t i = h.size() > 14 ? h.size() - 14 : 0; i < h.size(); ++i) std::printf("  %s\n", h[i].c_str());
    std::printf("setlocs last: "); auto& sl = c.cd.setloc_hist; for (size_t i = sl.size() > 8 ? sl.size() - 8 : 0; i < sl.size(); ++i) std::printf("%u ", sl[i]); std::puts("");
}
