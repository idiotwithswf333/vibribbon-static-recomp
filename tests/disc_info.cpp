// Headless check: disc_info path/to/game.cue
#include "../src/disc.h"
int main(int argc, char** argv) {
    if (argc < 2) { std::puts("usage: disc_info game.cue"); return 2; }
    disc::Image img;
    std::string err = img.open(argv[1]);
    if (!err.empty()) { std::printf("ERROR: %s\n", err.c_str()); return 1; }
    std::string boot = disc::boot_file(img);
    std::printf("files: %zu, boot: %s\n", img.entries().size(), boot.c_str());
    for (const char* n : {"SCES_028.73", "MAIN_T.EXE", "MAIN_G.EXE"}) {
        std::vector<uint8_t> f; disc::PsxExe x;
        bool ok = img.read_file(n, f) && disc::parse_psx_exe(f, x);
        std::printf("%-12s ok=%d pc=%08X load=%08X text=%zu\n", n, ok, x.pc, x.load_addr, x.text.size());
        if (!ok) return 1;
    }
    return 0;
}
