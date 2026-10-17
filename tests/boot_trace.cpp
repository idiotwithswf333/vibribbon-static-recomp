// boot_trace game.cue [EXE_NAME] [max_instructions]
// Runs a PS-X EXE from the disc on the interpreter and reports what it touched.
#include <algorithm>
#include "../src/cpu.h"
#include "../src/disc.h"

int main(int argc, char** argv) {
    if (argc < 2) { std::puts("usage: boot_trace game.cue [exe] [max_instr]"); return 2; }
    disc::Image img;
    std::string err = img.open(argv[1]);
    if (!err.empty()) { std::printf("ERROR: %s\n", err.c_str()); return 1; }
    std::string name = argc > 2 ? argv[2] : disc::boot_file(img);
    uint64_t maxi = argc > 3 ? std::stoull(argv[3]) : 20000000ull;
    std::vector<uint8_t> f; disc::PsxExe x;
    if (!img.read_file(name, f) || !disc::parse_psx_exe(f, x)) { std::printf("cannot load %s\n", name.c_str()); return 1; }

    cpu::Cpu c;
    c.attach_disc(&img);
    c.load_exe(x.text, x.load_addr, x.pc, x.gp, x.sp);
    while (!c.halted && c.icount < maxi) c.step();

    std::printf("== %s: %llu instructions, pc=%08X ==\n", name.c_str(), (unsigned long long)c.icount, c.pc);
    if (c.halted) std::printf("HALTED: %s\n", c.halt_reason.c_str());
    else std::puts("(stopped at instruction limit)");
    std::printf("cd: %llu commands, %llu sectors read; dma runs:", (unsigned long long)c.cd.cmds, (unsigned long long)c.cd.sectors_read);
    for (auto& [ch, n] : c.trace.dma_runs) std::printf(" ch%u=%llu", ch, (unsigned long long)n);
    std::puts("");
    std::puts("\n-- BIOS calls --");
    for (auto& [k, n] : c.trace.bios_calls) std::printf("%8llu  %s\n", (unsigned long long)n, k.c_str());
    std::printf("\nvblanks=%llu irqs_taken=%llu\n", (unsigned long long)c.vblanks, (unsigned long long)c.irqs_taken);
    std::printf("syscalls=%llu gte_ops=%llu unknown_ops=%llu printf=%llu\n",
        (unsigned long long)c.trace.syscalls, (unsigned long long)c.trace.gte_ops,
        (unsigned long long)c.trace.unknown_ops, (unsigned long long)c.trace.printf_calls);
    std::puts("\n-- hardware registers touched (phys addr: reads/writes, first write) --");
    std::map<uint32_t, int> all;
    for (auto& [a, n] : c.trace.io_reads) all[a];
    for (auto& [a, n] : c.trace.io_writes) all[a];
    for (auto& [a, _] : all) {
        auto r = c.trace.io_reads.find(a), w = c.trace.io_writes.find(a);
        auto fw = c.trace.io_first_write.find(a);
        std::printf("%08X: R%-8llu W%-6llu", a, r == c.trace.io_reads.end() ? 0ull : (unsigned long long)r->second,
                    w == c.trace.io_writes.end() ? 0ull : (unsigned long long)w->second);
        if (fw != c.trace.io_first_write.end()) std::printf(" first=%08X", fw->second);
        std::puts("");
    }
    std::vector<std::pair<uint64_t, uint32_t>> hot;
    for (auto& [pc, n] : c.trace.pc_hist) hot.push_back({n, pc});
    std::sort(hot.rbegin(), hot.rend());
    std::printf("\nGTE unsupported=%llu (last cmd %07X)\n", (unsigned long long)c.gte.unsupported, c.gte.last_unsupported);
    std::puts("-- GP0 commands --");
    for (auto& [k, n] : c.gpu.cmd_hist) std::printf("%02X:%llu ", k, (unsigned long long)n);
    std::puts("");
    std::puts("\n-- hottest addresses --");
    for (size_t i = 0; i < std::min<size_t>(8, hot.size()); ++i)
        std::printf("%08X: %llu\n", hot[i].second, (unsigned long long)hot[i].first);
    std::puts("-- GTE commands (full word & 0x1FFFFFF: count) --");
    for (auto& [k, n] : c.trace.gte_cmds) std::printf("%07X (funct %02X): %llu\n", k, k & 0x3F, (unsigned long long)n);
    std::vector<std::pair<uint64_t, uint32_t>> callees;
    for (auto& [t, n] : c.trace.calls) callees.push_back({n, t});
    std::sort(callees.rbegin(), callees.rend());
    std::puts("\n-- most-called functions --");
    for (size_t i = 0; i < std::min<size_t>(8, callees.size()); ++i)
        std::printf("%08X: %llu calls\n", callees[i].second, (unsigned long long)callees[i].first);
    if (!c.trace.log.empty()) std::printf("\n-- printf output --\n%s\n", c.trace.log.c_str());
    if (argc > 4) {   // dump the current display as a PPM
        std::vector<uint32_t> px; int w, h; c.gpu.display(px, w, h);
        FILE* o = std::fopen(argv[4], "wb");
        std::fprintf(o, "P6\n%d %d\n255\n", w, h);
        for (uint32_t p : px) { uint8_t rgb[3] = {(uint8_t)p, (uint8_t)(p >> 8), (uint8_t)(p >> 16)}; std::fwrite(rgb, 1, 3, o); }
        std::fclose(o);
        std::printf("wrote %s (%dx%d)\n", argv[4], w, h);
    }
    return 0;
}
