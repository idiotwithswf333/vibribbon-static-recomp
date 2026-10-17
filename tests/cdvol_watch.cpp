// cdvol_watch bios cue "script" : logs every change of SPU CD-input volume / SPUCNT bit0 / CD play+mute state from boot
#include <cstdio>
#include <sstream>
#include <string>
#include "../src/cpu.h"
#include "../src/disc.h"
#include "../src/pad_map.h"
static cpu::Cpu* g; static pad::TapFilter tf; static uint16_t held = 0; static std::vector<int16_t> abuf(882 * 2);
static long frame = 0; static uint32_t l0 = ~0u, l1 = ~0u, l2 = ~0u; static int lf = -1;
static void run(int n) { for (int i = 0; i < n && !g->halted; ++i) { g->pad1 = tf.apply(held); g->run_frame(); g->audio_frame(882, abuf.data()); ++frame;
    uint32_t cl = g->read(0x1F801DB0,2), cr = g->read(0x1F801DB2,2), cnt = g->read(0x1F801DAA,2) & 1; int fl = (g->cd.playing?1:0)|(g->cd.muted?2:0);
    if (cl != l0 || cr != l1 || cnt != l2 || fl != lf) { std::printf("frame %5ld: SPU CD vol L=%04X R=%04X, SPUCNT.CDenable=%u | CD playing=%d muted=%d\n", frame, cl, cr, cnt, fl&1, (fl>>1)&1); l0=cl; l1=cr; l2=cnt; lf=fl; } } }
int main(int, char** argv) {
    disc::Image img; if (!img.open(argv[2]).empty()) return 2;
    cpu::Cpu c; g = &c; c.trace.enabled = false; c.attach_disc(&img);
    if (!c.load_bios(argv[1])) return 2;
    while (!c.halted && c.pc != 0x80030000u && c.icount < 100000000ull) c.step();
    std::vector<uint8_t> f; disc::PsxExe x; img.read_file(disc::boot_file(img), f); disc::parse_psx_exe(f, x);
    if (!c.sideload_exe(x.text, x.load_addr, x.pc, x.gp, x.sp, 10, 4, 0x801FFFF0u)) return 2;
    std::istringstream in(argv[3]); std::string t;
    while (in >> t) { uint16_t b = 0; switch (t[0]) { case 'X': b = pad::CROSS; break; case 'D': b = pad::DOWN; break; default: break; }
        if (t[0] == 'w') run(atoi(t.c_str() + 1)); else if (t[0] == 'm') std::printf("---- %s\n", t.c_str() + 1); else if (b) { held = b; run(6); held = 0; run(10); } }
}
