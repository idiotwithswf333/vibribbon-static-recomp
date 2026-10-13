// gpu_capture bios.bin game.cue -- boot, pick GAME, and dump the GP0 words of a title frame vs a loading-scene frame.
#include <cstdio>
#include <map>
#include "../src/cpu.h"
#include "../src/pad_map.h"
static void dump(const char* tag, std::vector<uint32_t>& L, size_t from, size_t n) {
    std::printf("%s (%zu words total):\n", tag, L.size());
    for (size_t i = from; i < L.size() && i < from + n; ++i) std::printf("  %08X\n", L[i]);
}
int main(int, char** argv) {
    disc::Image img; img.open(argv[2]);
    cpu::Cpu c; c.trace.enabled=false; c.attach_disc(&img); c.load_bios(argv[1]);
    while(!c.halted&&c.pc!=0x80030000u) c.step();
    std::vector<uint8_t> f; disc::PsxExe x; img.read_file(disc::boot_file(img), f); disc::parse_psx_exe(f, x);
    c.sideload_exe(x.text,x.load_addr,x.pc,x.gp,x.sp,10,4,0x801FFFF0u);
    for(int i=0;i<1500;++i) c.run_frame();
    c.pad1=pad::CROSS; for(int i=0;i<6;++i) c.run_frame(); c.pad1=0;
    for(int i=0;i<300;++i) c.run_frame();
    // one normal title frame for comparison
    std::vector<uint32_t> L0; c.gpu.gp0_log=&L0; c.run_frame(); c.gpu.gp0_log=nullptr;
    c.pad1=pad::CROSS; for(int i=0;i<6;++i) c.run_frame(); c.pad1=0;
    for(int i=0;i<100;++i) c.run_frame();
    std::vector<uint32_t> L1; c.gpu.gp0_log=&L1; c.run_frame(); c.gpu.gp0_log=nullptr;
    auto hist=[&](std::vector<uint32_t>& L){ std::map<int,int> h; size_t i=0; for(uint32_t w: L) if((w>>24)>=0x20 && (w>>24)<0x80 && !(w==0x55555555)) {} return 0; };
    (void)hist;
    dump("TITLE frame", L0, 0, 40);
    dump("LOADING-scene frame", L1, 0, 90);
}
