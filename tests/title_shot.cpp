// title_shot game.cue out_prefix  -- boots to the title menu, scripts some pad input, saves PPMs.
#include <chrono>
#include <cstdio>
#include "../src/cpu.h"
#include "../src/disc.h"
#include "../src/pad_map.h"

static void shot(cpu::Cpu& c, const std::string& path) {
    std::vector<uint32_t> px; int w, h; c.gpu.display(px, w, h);
    FILE* o = std::fopen(path.c_str(), "wb");
    std::fprintf(o, "P6\n%d %d\n255\n", w, h);
    for (uint32_t p : px) { uint8_t rgb[3] = {(uint8_t)p, (uint8_t)(p >> 8), (uint8_t)(p >> 16)}; std::fwrite(rgb, 1, 3, o); }
    std::fclose(o);
    std::printf("saved %s (frame %llu)\n", path.c_str(), (unsigned long long)c.vblanks);
}
static void frames(cpu::Cpu& c, int n, uint16_t pad) { c.pad1 = pad; for (int i = 0; i < n; ++i) c.run_frame(); }

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    disc::Image img; std::string e = img.open(argv[1]); if (!e.empty()) { std::puts(e.c_str()); return 1; }
    std::vector<uint8_t> f; disc::PsxExe x; img.read_file(disc::boot_file(img), f); disc::parse_psx_exe(f, x);
    cpu::Cpu c; c.trace.enabled = false; c.attach_disc(&img); c.load_exe(x.text, x.load_addr, x.pc, x.gp, x.sp);
    std::string pre = argv[2];
    auto t0 = std::chrono::steady_clock::now();
    frames(c, 1800, 0);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("1800 frames in %.1fs (%.1f fps), halted=%d %s\n", secs, 1800 / secs, c.halted, c.halt_reason.c_str());
    shot(c, pre + "_a.ppm");
    frames(c, 6, pad::DOWN); frames(c, 30, 0);   shot(c, pre + "_b.ppm");
    frames(c, 6, pad::RIGHT); frames(c, 30, 0);  shot(c, pre + "_c.ppm");
    frames(c, 6, pad::CROSS); frames(c, 300, 0); shot(c, pre + "_d.ppm");
    std::printf("sio-related: halted=%d irqs=%llu\n", c.halted, (unsigned long long)c.irqs_taken);
    return 0;
}
