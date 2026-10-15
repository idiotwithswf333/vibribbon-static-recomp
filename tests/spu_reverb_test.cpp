// spu_reverb_test -- the reverb unit used as a PCM player (how Vib-Ribbon plays its delayed music).
#include <cstdio>
#include <cstdlib>
#include "../src/spu.h"
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
int main() {
    hw::Spu s;
    s.write(0x1AA, 0x8000, 2);                       // SPU enable
    s.write(0x1A2, 0xFFC0, 2);                       // mBASE = 0x7FE00 (512-byte work area at the top of RAM)
    for (uint32_t a = 0x7FE00; a < 0x80000; a += 2) { s.ram[a] = 0xE8; s.ram[a + 1] = 0x03; }   // constant PCM value 1000
    // all-pass volumes 0 => APF output is the buffer content at mLAPF2: left/right read offset 2 (=16 bytes)
    s.write(0x1F8, 2, 2); s.write(0x1FA, 2, 2);      // mLAPF2 / mRAPF2
    s.write(0x184, 0x7FFF, 2); s.write(0x186, 0x7FFF, 2);   // reverb output volume = full
    int l = 0, r = 0;
    for (int i = 0; i < 8; ++i) s.capture_tick(0, 0);
    s.rvb_out(l, r);
    CHECK(std::abs(l - 999) <= 2 && std::abs(r - 999) <= 2);                // buffer content reaches the output
    s.write(0x184, 0, 2); s.write(0x186, 0, 2);      // game "pauses" the music by zeroing the reverb volume
    for (int i = 0; i < 4; ++i) s.capture_tick(0, 0);
    s.rvb_out(l, r);
    CHECK(l == 0 && r == 0);
    s.write(0x184, 0x7FFF, 2); s.write(0x186, 0x7FFF, 2);
    for (int i = 0; i < 700; ++i) s.capture_tick(0, 0);                      // many wraps of the 256-sample ring
    s.rvb_out(l, r);
    CHECK(std::abs(l - 999) <= 2);
    CHECK(s.rvb_vol_l() == 0x7FFF);
    if (failures == 0) std::puts("spu reverb: all tests passed");
    return failures ? 1 : 0;
}
