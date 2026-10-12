// audio_test game.cue [out.wav] -- boots the game, runs ~60 s of frames, reports SPU/CD-DA activity
// and writes the mixed audio to a 44.1 kHz stereo WAV.
#include <cstdio>
#include <cstdlib>
#include "../src/cpu.h"
#include "../src/disc.h"

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    disc::Image img; std::string e = img.open(argv[1]); if (!e.empty()) { std::puts(e.c_str()); return 1; }
    std::vector<uint8_t> f; disc::PsxExe x; img.read_file(disc::boot_file(img), f); disc::parse_psx_exe(f, x);
    cpu::Cpu c; c.trace.enabled = false; c.attach_disc(&img); c.load_exe(x.text, x.load_addr, x.pc, x.gp, x.sp);
    int frames = argc > 3 ? std::atoi(argv[3]) : 3000;
    std::vector<int16_t> all, buf(882 * 2);
    int64_t peak = 0; uint64_t nonzero = 0;
    for (int i = 0; i < frames && !c.halted; ++i) {
        c.run_frame();
        c.audio_frame(882, buf.data());
        for (int16_t s : buf) { if (s) ++nonzero; peak = std::max<int64_t>(peak, std::abs((int)s)); }
        all.insert(all.end(), buf.begin(), buf.end());
    }
    std::printf("frames=%d halted=%d %s\nkey_ons=%llu cd_play_cmds=%llu cd_playing=%d\npeak=%lld nonzero_samples=%llu of %zu\n",
                frames, c.halted, c.halt_reason.c_str(), (unsigned long long)c.spu.key_ons,
                (unsigned long long)c.cd.play_cmds, c.cd.playing, (long long)peak, (unsigned long long)nonzero, all.size());
    if (argc > 2) {
        FILE* o = std::fopen(argv[2], "wb");
        uint32_t bytes = (uint32_t)all.size() * 2, v;
        std::fwrite("RIFF", 1, 4, o); v = 36 + bytes; std::fwrite(&v, 4, 1, o); std::fwrite("WAVEfmt ", 1, 8, o);
        v = 16; std::fwrite(&v, 4, 1, o); uint16_t h = 1; std::fwrite(&h, 2, 1, o); h = 2; std::fwrite(&h, 2, 1, o);
        v = 44100; std::fwrite(&v, 4, 1, o); v = 44100 * 4; std::fwrite(&v, 4, 1, o); h = 4; std::fwrite(&h, 2, 1, o); h = 16; std::fwrite(&h, 2, 1, o);
        std::fwrite("data", 1, 4, o); std::fwrite(&bytes, 4, 1, o); std::fwrite(all.data(), 2, all.size(), o); std::fclose(o);
    }
    return 0;
}
