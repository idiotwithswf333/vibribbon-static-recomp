// bios_title bios.bin game.cue out_prefix -- real BIOS -> sideload the disc's boot EXE -> run the title.
#include <cstdio>
#include <cstdlib>
#include "../src/cpu.h"
#include "../src/disc.h"
int main(int argc, char** argv) {
    if (argc < 4) return 2;
    disc::Image img; std::string e = img.open(argv[2]); if (!e.empty()) { std::puts(e.c_str()); return 1; }
    cpu::Cpu c; c.trace.enabled = true; c.attach_disc(&img);
    if (!c.load_bios(argv[1])) { std::puts("bad bios"); return 1; }
    while (!c.halted && c.icount < 400000000ull && c.pc != 0x80030000u) c.step();
    std::vector<uint8_t> f; disc::PsxExe x; img.read_file(disc::boot_file(img), f); disc::parse_psx_exe(f, x);
    if (!c.sideload_exe(x.text, x.load_addr, x.pc, x.gp, x.sp, 10, 4, 0x801FFFF0u)) { std::printf("sideload failed: %s\n", c.halt_reason.c_str()); return 1; }
    int frames = argc > 4 ? atoi(argv[4]) : 1800;
    for (int i = 0; i < frames && !c.halted; ++i) c.run_frame();
    if (argc > 5) {   // scripted input: tap RIGHT, then CROSS; shots after each
        auto shot = [&](const char* tag) {
            std::vector<uint32_t> px; int w = 0, h = 0; c.gpu.display(px, w, h);
            std::string fn = std::string(argv[3]) + "_" + tag + ".ppm"; FILE* o = std::fopen(fn.c_str(), "wb");
            std::fprintf(o, "P6\n%d %d\n255\n", w, h);
            for (int i = 0; i < w * h; ++i) { uint8_t b[3] = {(uint8_t)px[i], (uint8_t)(px[i] >> 8), (uint8_t)(px[i] >> 16)}; std::fwrite(b, 1, 3, o); }
            std::fclose(o); };
        shot("before");
        c.pad1 = 0x0020 /*RIGHT*/; for (int i = 0; i < 6; ++i) c.run_frame(); c.pad1 = 0; for (int i = 0; i < 60; ++i) c.run_frame();
        shot("right");
        c.pad1 = 0x4000 /*CROSS*/; for (int i = 0; i < 6; ++i) c.run_frame(); c.pad1 = 0; for (int i = 0; i < 300; ++i) c.run_frame();
        shot("cross");
    }
    std::printf("frames=%d halted=%d %s pc=%08X vblanks=%llu irqs=%llu cdcmds=%llu\n", frames, c.halted, c.halt_reason.c_str(), c.pc,
        (unsigned long long)c.vblanks, (unsigned long long)c.irqs_taken, (unsigned long long)c.cd.cmds);
    std::printf("LOG:\n%s\n", c.trace.log.substr(0, 3000).c_str());
    { size_t n = c.cd.dbg_log.size(); std::printf("CD log (%zu entries), first 25 and last 25:\n", n);
      for (size_t i = 0; i < n && i < 25; ++i) std::printf("  %s\n", c.cd.dbg_log[i].c_str());
      for (size_t i = n > 50 ? n - 25 : 25; i < n; ++i) std::printf("  %s\n", c.cd.dbg_log[i].c_str()); }
    std::printf("SIO tx=%llu  key_ons=%llu\n", (unsigned long long)c.trace.io_writes[0x1F801040], (unsigned long long)c.spu.key_ons);
    std::vector<uint32_t> px; int w = 0, h = 0; c.gpu.display(px, w, h);
    std::string fn = std::string(argv[3]) + ".ppm"; FILE* o = std::fopen(fn.c_str(), "wb");
    std::fprintf(o, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; ++i) { uint8_t b[3] = {(uint8_t)px[i], (uint8_t)(px[i] >> 8), (uint8_t)(px[i] >> 16)}; std::fwrite(b, 1, 3, o); }
    std::fclose(o);
    return 0;
}
