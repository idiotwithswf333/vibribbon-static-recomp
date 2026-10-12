// bios_boot bios.bin game.cue [max_Mcycles] -- boots the real BIOS ROM until it reaches the shell (0x80030000).
#include <cstdio>
#include <cstdlib>
#include "../src/cpu.h"
#include "../src/disc.h"
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    disc::Image img; std::string e = img.open(argv[2]); if (!e.empty()) { std::puts(e.c_str()); return 1; }
    cpu::Cpu c; c.trace.enabled = true; c.attach_disc(&img);
    if (!c.load_bios(argv[1])) { std::puts("bad bios"); return 1; }
    uint64_t maxc = (argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 400) * 1000000ull;
    while (!c.halted && c.icount < maxc && c.pc != 0x80030000u) c.step();
    std::printf("pc=%08X icount=%llu halted=%d %s vblanks=%llu irqs=%llu cdcmds=%llu\n", c.pc,
        (unsigned long long)c.icount, c.halted, c.halt_reason.c_str(), (unsigned long long)c.vblanks,
        (unsigned long long)c.irqs_taken, (unsigned long long)c.cd.cmds);
    std::printf("SR=%08X Cause=%08X I_STAT=%04X I_MASK=%04X\n", c.cop0[12], c.cop0[13], c.read(0x1F801070, 4), c.read(0x1F801074, 4));
    return 0;
}
