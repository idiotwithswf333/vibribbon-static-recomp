// script_run bios.bin game.cue out_prefix "script"   (script tokens separated by spaces)
//   wN = run N frames   X/O/T/Q = Cross/Circle/Triangle/Square tap   U/D/L/R = D-pad tap   S = Start tap
//   p  = screenshot (out_prefix_NN.ppm) + one-line state    All presses go through pad::TapFilter, 6-frame taps.
// Every frame also calls audio_frame(): the SPU, SPU IRQ and CD-DA clocks only advance there.
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <sstream>
#include <string>
#include "../src/cpu.h"
#include "../src/disc.h"
#include "../src/pad_map.h"
static cpu::Cpu* g; static pad::TapFilter tf; static uint16_t held = 0; static std::vector<int16_t> abuf(882 * 2);
static double g_sumsq = 0; static long g_samples = 0; static int g_peak = 0;
static void run(int n) { for (int i = 0; i < n && !g->halted; ++i) { g->pad1 = tf.apply(held); g->run_frame(); g->audio_frame(882, abuf.data());
    for (int k = 0; k < 882 * 2; ++k) { int v = abuf[k]; g_sumsq += (double)v * v; ++g_samples; if (std::abs(v) > g_peak) g_peak = std::abs(v); } } }
static void shot(const std::string& fn) {
    std::vector<uint32_t> px; int w = 0, h = 0; g->gpu.display(px, w, h);
    FILE* o = std::fopen(fn.c_str(), "wb"); std::fprintf(o, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; ++i) { uint8_t b[3] = {(uint8_t)px[i], (uint8_t)(px[i] >> 8), (uint8_t)(px[i] >> 16)}; std::fwrite(b, 1, 3, o); }
    std::fclose(o);
}
int main(int, char** argv) {
    disc::Image img; if (!img.open(argv[2]).empty()) return 2;
    cpu::Cpu c; g = &c; c.trace.enabled = false; c.attach_disc(&img);
    if (!c.load_bios(argv[1])) return 2;
    while (!c.halted && c.pc != 0x80030000u && c.icount < 100000000ull) c.step();
    std::vector<uint8_t> f; disc::PsxExe x; img.read_file(disc::boot_file(img), f); disc::parse_psx_exe(f, x);
    if (!c.sideload_exe(x.text, x.load_addr, x.pc, x.gp, x.sp, 10, 4, 0x801FFFF0u)) return 2;
    std::istringstream in(argv[4]); std::string tok; int n = 0;
    while (in >> tok) {
        uint16_t b = 0;
        switch (tok[0]) { case 'X': b = pad::CROSS; break; case 'O': b = pad::CIRCLE; break; case 'T': b = pad::TRIANGLE; break; case 'Q': b = pad::SQUARE; break;
            case 'U': b = pad::UP; break; case 'D': b = pad::DOWN; break; case 'L': b = pad::LEFT; break; case 'R': b = pad::RIGHT; break; case 'S': b = pad::START; break; default: break; }
        if (tok == "c") { static size_t last = 0; auto& h = c.cd.cmd_hist; int cnt = 0, getlocp = 0;
            for (size_t i = last; i < h.size(); ++i) { if (h[i].find("cmd 11") != std::string::npos) { ++getlocp; continue; } if (cnt++ < 60) std::printf("   %s\n", h[i].c_str()); }
            std::printf("   (+ %d GetlocP)\n", getlocp); last = h.size(); }
        else if (tok == "a0") c.music_assist = false;
        else if (tok == "a1") c.music_assist = true;
        else if (tok[0] == 'w') run(std::atoi(tok.c_str() + 1));
        else if (tok[0] == 'p') { char nm[256]; std::snprintf(nm, sizeof nm, "%s_%02d.ppm", argv[3], n); shot(nm);
            std::printf("shot %02d: pc=%08X halted=%d %s cd.playing=%d muted=%d lba=%u sectors=%llu\n", n, c.pc, c.halted, c.halt_reason.c_str(), (int)c.cd.playing, (int)c.cd.muted, c.cd.dbg_loc(), (unsigned long long)c.cd.sectors_read); std::printf("         spu: key_ons=%llu dma4_reads=%llu last read madr=%08X words=%u spu_xfer=%05X irq_addr=%04X cnt=%04X\n", (unsigned long long)c.spu.key_ons, (unsigned long long)c.dma4_reads, c.dma4_last_madr, c.dma4_last_words, c.dma4_last_xfer, (unsigned)c.read(0x1F801DA4,2), (unsigned)c.read(0x1F801DAA,2));
            { long sumcap = 0, sumdst = 0; for (int k = 0; k < 0x800; k += 2) sumcap += std::abs((int16_t)(c.spu.ram[k] | c.spu.ram[k+1] << 8));
              for (int k = 0; k < 1536; k += 2) sumdst += std::abs((int16_t)(c.ram[(c.dma4_last_madr & 0x1FFFFF) + k] | c.ram[(c.dma4_last_madr & 0x1FFFFF) + k + 1] << 8));
              std::printf("         SPU cd-vol regs 1B0=%04X 1B2=%04X mainvol=%04X/%04X | capture(CD L+R) abs-sum=%ld | last DMA dest abs-sum=%ld\n", (unsigned)c.read(0x1F801DB0,2), (unsigned)c.read(0x1F801DB2,2), (unsigned)c.read(0x1F801D80,2), (unsigned)c.read(0x1F801D82,2), sumcap, sumdst); }
            { int act = 0; for (int v = 0; v < 24; ++v) if (c.read(0x1F801C0C + v * 16, 2)) ++act;
              std::printf("         AUDIO OUT since last shot: rms=%.0f peak=%d | active voices=%d | SPUCNT=%04X mainvol=%04X/%04X cdvol(SPU)=%04X/%04X | cd.muted=%d playing=%d | assist_active=%d\n",
                  g_samples ? std::sqrt(g_sumsq / g_samples) : 0.0, g_peak, act, (unsigned)c.read(0x1F801DAA,2), (unsigned)c.read(0x1F801D80,2), (unsigned)c.read(0x1F801D82,2), (unsigned)c.read(0x1F801DB0,2), (unsigned)c.read(0x1F801DB2,2), (int)c.cd.muted, (int)c.cd.playing, (int)c.assist_active);
              g_sumsq = 0; g_samples = 0; g_peak = 0; }
            ++n; }
        else if (b) { held = b; run(6); held = 0; run(10); }
    }
    return c.halted ? 1 : 0;
}
