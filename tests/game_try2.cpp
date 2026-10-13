#include <cstdio>
#include <string>
#include "../src/cpu.h"
#include "../src/disc.h"
#include "../src/pad_map.h"
static void shot(cpu::Cpu& c, const std::string& fn) { std::vector<uint32_t> px; int w = 0, h = 0; c.gpu.display(px, w, h); FILE* o = std::fopen(fn.c_str(), "wb"); std::fprintf(o, "P6\n%d %d\n255\n", w, h); for (int i = 0; i < w * h; ++i) { uint8_t b[3] = {(uint8_t)px[i], (uint8_t)(px[i] >> 8), (uint8_t)(px[i] >> 16)}; std::fwrite(b, 1, 3, o); } std::fclose(o); }
int main(int, char** argv) {
    disc::Image img; if (!img.open(argv[2]).empty()) return 1;
    cpu::Cpu c; c.trace.enabled = true; c.attach_disc(&img);
    if (!c.load_bios(argv[1])) return 1;
    while (!c.halted && c.pc != 0x80030000u) c.step();
    std::vector<uint8_t> f; disc::PsxExe x; img.read_file(disc::boot_file(img), f); disc::parse_psx_exe(f, x);
    c.sideload_exe(x.text, x.load_addr, x.pc, x.gp, x.sp, 10, 4, 0x801FFFF0u);
    for (int i = 0; i < 1500 && !c.halted; ++i) c.run_frame();
    c.pad1 = pad::CROSS; for (int i = 0; i < 6; ++i) c.run_frame(); c.pad1 = 0;
    for (int i = 0; i < 300 && !c.halted; ++i) c.run_frame();
    c.pad1 = pad::CROSS; for (int i = 0; i < 6; ++i) c.run_frame(); c.pad1 = 0;   // choose GAME
    size_t cmd0 = c.cd.cmd_hist.size();
    for (int k = 0; k < 8; ++k) {
        for (int i = 0; i < 150 && !c.halted; ++i) c.run_frame();
        std::printf("+%d frames: loc=%u sectors=%llu cmds=%llu pc=%08X halted=%d %s\n", (k + 1) * 150, c.cd.dbg_loc(), (unsigned long long)c.cd.sectors_read, (unsigned long long)c.cd.cmds, c.pc, c.halted, c.halt_reason.c_str());
        shot(c, std::string("/tmp/g_") + std::to_string(k) + ".ppm");
    }
    std::printf("LOG tail:\n%s\n", c.trace.log.substr(c.trace.log.size() > 1200 ? c.trace.log.size() - 1200 : 0).c_str());
    auto& h = c.cd.cmd_hist; std::printf("commands since choosing GAME (%zu):\n", h.size() - cmd0);
    for (size_t i = cmd0; i < h.size() && i < cmd0 + 70; ++i) if (h[i].find("cmd 11") == std::string::npos) std::printf("  %s\n", h[i].c_str());
}
