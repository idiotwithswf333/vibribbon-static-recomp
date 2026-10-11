#include <cstdio>
#include "../src/cpu.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

// tiny assembler helpers
static uint32_t I(int op, int rs, int rt, int imm) { return op << 26 | rs << 21 | rt << 16 | (imm & 0xFFFF); }
static uint32_t R(int rs, int rt, int rd, int sh, int fn) { return rs << 21 | rt << 16 | rd << 11 | sh << 6 | fn; }
static uint32_t J(int op, uint32_t t) { return op << 26 | ((t >> 2) & 0x3FFFFFF); }
static const uint32_t NOP = 0;

static cpu::Cpu run(const std::vector<uint32_t>& prog, int steps) {
    cpu::Cpu c;
    std::vector<uint8_t> t(prog.size() * 4);
    std::memcpy(t.data(), prog.data(), t.size());
    c.load_exe(t, 0x80010000, 0x80010000, 0, 0x801FFF00);
    for (int i = 0; i < steps; ++i) c.step();
    return c;
}

int main() {
    // ALU + lui/ori
    { auto c = run({I(0x0F,0,8,0x1234), I(0x0D,8,8,0x5678), I(0x09,0,9,-1), R(8,9,10,0,0x21)}, 4);
      CHECK(c.r[8] == 0x12345678); CHECK(c.r[10] == 0x12345677); }
    // branch delay slot executes: beq taken, delay-slot addiu still runs, skipped instr doesn't
    { auto c = run({I(0x04,0,0,2), I(0x09,0,8,5), I(0x09,0,9,7), I(0x09,0,10,9)}, 4);
      CHECK(c.r[8] == 5); CHECK(c.r[9] == 0); CHECK(c.r[10] == 9); }
    // load delay slot: instruction right after lw sees OLD value
    { cpu::Cpu c;
      std::vector<uint32_t> p = {I(0x0F,0,8,0x8002), I(0x09,0,9,0x77), I(0x2B,8,9,0x0000), // sw 0x77 -> 0x80020000
                                 I(0x09,0,10,1), I(0x23,8,10,0x0000), R(10,0,11,0,0x21), R(10,0,12,0,0x21)};
      std::vector<uint8_t> t(p.size()*4); std::memcpy(t.data(), p.data(), t.size());
      c.load_exe(t, 0x80010000, 0x80010000, 0, 0x801FFF00);
      for (int i = 0; i < 7; ++i) c.step();
      CHECK(c.r[11] == 1);      // delay slot: still old value
      CHECK(c.r[12] == 0x77); } // after the slot: loaded value
    // jal / jr round trip
    { auto c = run({J(3,0x80010010), NOP, I(0x09,0,9,3), NOP, I(0x09,0,8,1), R(31,0,0,0,0x08), NOP}, 5);
      // jal -> 0x10 (addiu r8), jr ra -> back to 0x08 (addiu r9)
      CHECK(c.r[8] == 1); }
    // mult / div
    { auto c = run({I(0x09,0,8,-6), I(0x09,0,9,4), R(8,9,0,0,0x1A), R(0,0,10,0,0x12), R(0,0,11,0,0x10)}, 5);
      CHECK((int32_t)c.r[10] == -1); CHECK((int32_t)c.r[11] == -2); }
    // lwl/lwr unaligned read, little endian
    { cpu::Cpu c;
      for (int i = 0; i < 8; ++i) c.ram[0x20000 + i] = (uint8_t)(0x10 + i);
      std::vector<uint32_t> p = {I(0x0F,0,8,0x8002), I(0x26,8,9,1), I(0x22,8,9,4), NOP};  // lwr 1(r8); lwl 4(r8)
      std::vector<uint8_t> t(p.size()*4); std::memcpy(t.data(), p.data(), t.size());
      c.load_exe(t, 0x80010000, 0x80010000, 0, 0x801FFF00);
      for (int i = 0; i < 4; ++i) c.step();
      CHECK(c.r[9] == 0x14131211); }
    // HLE strlen via the A0 table
    { cpu::Cpu c;
      const char* s = "vib";
      std::memcpy(&c.ram[0x30000], s, 4);
      std::vector<uint32_t> p = {I(0x0F,0,4,0x8003), I(0x0F,0,10,0), I(0x0D,10,10,0xA0), R(10,0,0,0,0x08), I(0x09,0,9,0x1B), NOP};
      std::vector<uint8_t> t(p.size()*4); std::memcpy(t.data(), p.data(), t.size());
      c.load_exe(t, 0x80010000, 0x80010000, 0, 0x801FFF00);
      for (int i = 0; i < 6; ++i) c.step();
      CHECK(c.r[2] == 3); CHECK(c.trace.bios_calls.size() == 1); }
    if (!failures) std::puts("cpu: all tests passed");
    return failures ? 1 : 0;
}
