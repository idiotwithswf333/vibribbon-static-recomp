// music_timeline bios cue [out.wav] : Bronze level. Prints output RMS per 25 frames (0.5 s),
// pauses with START at level frame 1000 and resumes at 1500. Optionally writes the whole level audio to a WAV.
#include <cmath>
#include <cstdio>
#include <string>
#include "../src/cpu.h"
#include "../src/disc.h"
#include "../src/pad_map.h"
static cpu::Cpu* g; static pad::TapFilter tf; static uint16_t held = 0; static std::vector<int16_t> abuf(882 * 2), all;
static double ss = 0; static long ns = 0; static int pk = 0; static bool rec = false;
static void run(int n) { for (int i = 0; i < n && !g->halted; ++i) { g->pad1 = tf.apply(held); g->run_frame(); g->audio_frame(882, abuf.data());
    for (int k = 0; k < 882 * 2; ++k) { int v = abuf[k]; ss += (double)v * v; ++ns; if (std::abs(v) > pk) pk = std::abs(v); } if (rec) all.insert(all.end(), abuf.begin(), abuf.end()); } }
static void tap(uint16_t b) { held = b; run(6); held = 0; run(10); }
int main(int argc, char** argv) {
    disc::Image img; if (!img.open(argv[2]).empty()) return 2;
    cpu::Cpu c; g = &c; c.trace.enabled = false; c.attach_disc(&img);
    if (!c.load_bios(argv[1])) return 2;
    while (!c.halted && c.pc != 0x80030000u && c.icount < 100000000ull) c.step();
    std::vector<uint8_t> f; disc::PsxExe x; img.read_file(disc::boot_file(img), f); disc::parse_psx_exe(f, x);
    if (!c.sideload_exe(x.text, x.load_addr, x.pc, x.gp, x.sp, 10, 4, 0x801FFFF0u)) return 2;
    run(1500); tap(pad::CROSS); run(400); tap(pad::CROSS); run(1500); tap(pad::CROSS); run(150); tap(pad::CROSS);
    rec = true; ss = 0; ns = 0; pk = 0;
    int lf = 0; bool paused = false, resumed = false;
    for (int blk = 0; blk < 130; ++blk) {
        if (!paused && lf >= 1000) { paused = true; std::printf("   -- START (pause)\n"); tap(pad::START); lf += 16; }
        if (paused && !resumed && lf >= 1500) { resumed = true; std::printf("   -- START (resume)\n"); tap(pad::START); lf += 16; }
        run(25); lf += 25;
        std::printf("level %4d frames (%5.1f s): rms=%5.0f peak=%5d | reverb out vol=%04X/%04X mBASE=%04X\n", lf, lf / 50.0, ns ? std::sqrt(ss / ns) : 0, pk, (unsigned)c.read(0x1F801D84,2), (unsigned)c.read(0x1F801D86,2), (unsigned)c.read(0x1F801DA2,2));
        ss = 0; ns = 0; pk = 0;
    }
    if (argc > 3) { FILE* o = std::fopen(argv[3], "wb"); uint32_t n = (uint32_t)all.size() * 2, rate = 44100, br = rate * 4; uint16_t ch = 2, ba = 4, bps = 16, fmt = 1; uint32_t fl = 16, riff = 36 + n;
        std::fwrite("RIFF", 1, 4, o); std::fwrite(&riff, 4, 1, o); std::fwrite("WAVEfmt ", 1, 8, o); std::fwrite(&fl, 4, 1, o); std::fwrite(&fmt, 2, 1, o); std::fwrite(&ch, 2, 1, o); std::fwrite(&rate, 4, 1, o); std::fwrite(&br, 4, 1, o); std::fwrite(&ba, 2, 1, o); std::fwrite(&bps, 2, 1, o); std::fwrite("data", 1, 4, o); std::fwrite(&n, 4, 1, o); std::fwrite(all.data(), 2, all.size(), o); std::fclose(o); }
}
