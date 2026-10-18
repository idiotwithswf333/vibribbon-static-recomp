// MIPS R3000A interpreter + minimal PS1 memory map + high-level BIOS.
// SDL-free so it can be tested headlessly.
//
// NOT implemented yet: GTE (COP2 ops are counted and ignored), interrupt
// delivery, GPU/SPU/CD behaviour. Hardware register access is logged so we
// can see exactly what the game needs.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "cdrom.h"
#include "disc.h"
#include "gpu.h"
#include "gte.h"
#include "spu.h"

namespace cpu {

struct Trace {
    std::map<std::string, uint64_t> bios_calls;   // "A0:3F printf" -> count
    std::map<uint32_t, uint64_t> io_reads, io_writes;  // physical addr -> count
    std::map<uint32_t, uint32_t> io_first_write;       // physical addr -> first value
    std::map<uint32_t, uint64_t> dma_runs, gte_cmds, gte_moves;
    std::map<uint32_t, uint64_t> pc_hist, calls;   // calls: jal/jalr target -> count
    uint64_t execs = 0, dma_other = 0, gte_ops = 0, unknown_ops = 0, syscalls = 0, printf_calls = 0;
    std::string log;      // printf output and notable events
    bool verbose_bios = false;
    bool enabled = true;   // per-instruction statistics (slow); turn off for play
};

class Cpu {
public:
    uint32_t r[32] = {}, hi = 0, lo = 0, pc = 0, next_pc = 4, cop0[32] = {};
    uint64_t icount = 0;            // CPU cycles (33.8688 MHz clock)
    uint32_t cycles_per_instr = 2;  // real CPU averages >1 cycle/instr (memory waits); 1 made timeout loops like VSync run too fast
    uint64_t next_vblank = 677376;
    // ---- real BIOS mode: ROM mapped at 0xBFC00000, genuine exceptions, no HLE intercepts ----
    std::vector<uint8_t> rom;
    bool real_bios = false;
    bool siolog = false; std::vector<std::string> siolog_v;   // optional pad-port log (diagnostics)
    size_t cdlog_max = 400; bool cdlog = false; std::vector<std::string> cdlog_v;   // optional CD register access log (diagnostics)
    bool cur_ds = false;   // the instruction being executed sits in the delay slot of a taken branch
    Trace trace;
    std::vector<uint8_t> ram = std::vector<uint8_t>(2 * 1024 * 1024);
    uint8_t scratch[1024] = {};
    uint8_t io[0x2000] = {};
    bool halted = false;
    std::string halt_reason;
    hw::CdRom cd;
    hw::Gpu gpu;
    hw::Gte gte;
    hw::Spu spu;
    void attach_disc(disc::Image* d) { cd.img = d; }

    // ---- memory ---------------------------------------------------------
    static uint32_t phys(uint32_t a) { return a >= 0xFFFE0000u ? a : (a & 0x1FFFFFFFu); }

    uint32_t read(uint32_t va, int size) {
        uint32_t a = phys(va);
        if (a < 0x800000) return rd(&ram[a & 0x1FFFFF], size);
        if (a >= 0x1F800000 && a < 0x1F800400) return rd(&scratch[a - 0x1F800000], size);
        if (a >= 0x1F801000 && a < 0x1F803000) return io_read(a, size);
        if (a >= 0x1F000000 && a < 0x1F800000) return 0xFFFFFFFFu;  // expansion 1: nothing there
        if (a >= 0x1FC00000 && a < 0x1FC80000) return rom.empty() ? 0 : rd(&rom[(a - 0x1FC00000) & 0x7FFFF], size);   // BIOS ROM (0 when running HLE)
        if (a == 0xFFFE0130) return 0;  // cache control
        halt("bus read at " + hex(va)); return 0;
    }
    void write(uint32_t va, uint32_t v, int size) {
        if (cop0[12] & 0x10000) return;  // cache isolated: stores go nowhere
        uint32_t a = phys(va);
        if (a < 0x800000) { wr(&ram[a & 0x1FFFFF], v, size); return; }
        if (a >= 0x1F800000 && a < 0x1F800400) { wr(&scratch[a - 0x1F800000], v, size); return; }
        if (a >= 0x1F801000 && a < 0x1F803000) { io_write(a, v, size); return; }
        if (a == 0xFFFE0130) return;  // cache control
        halt("bus write at " + hex(va));
    }

    // ---- setup ------------------------------------------------------------
    void load_exe(const std::vector<uint8_t>& text, uint32_t load, uint32_t pc0, uint32_t gp, uint32_t sp) {
        std::memcpy(&ram[load & 0x1FFFFF], text.data(), text.size());
        std::memset(r, 0, sizeof r);
        pc = pc0; next_pc = pc0 + 4;
        r[28] = gp; r[29] = r[30] = sp ? sp : 0x801FFFF0;
        cop0[12] = 0x10900000;  // SR as left by the BIOS: IE off
        heap_ptr = 0; halted = false;
    }

    // ---- execution --------------------------------------------------------
    void step() {
        if (halted) return;
        if (real_bios) {
            // Hardware interrupt line (I_STAT & I_MASK) feeds Cause.IP2.
            if (rd(&io[0x70], 4) & rd(&io[0x74], 4) & 0x7FF) cop0[13] |= 0x400; else cop0[13] &= ~0x400u;
            if ((cop0[12] & 1) && (cop0[12] & cop0[13] & 0xFF00)) {
                if (pend.reg) r[pend.reg] = pend.val;   // an in-flight load completes before the handler runs
                pend = {}; prev = {};
                take_exception(0, pc, next_pc != pc + 4);
                return;
            }
        } else {
            if (hook_buf && irq_ready()) take_irq();
            uint32_t p = phys(pc);
            if (p == 0xA0 || p == 0xB0 || p == 0xC0) { bios_call(p); return; }
        }
        uint32_t cur = pc;
        cur_ds = next_pc != pc + 4;
        uint32_t i = read(cur, 4);
        if (halted) return;
        if (trace.enabled) trace.pc_hist[cur]++;
        pc = next_pc; next_pc += 4;
        prev = pend; pend = {};
        wreg = 0;
        exec(i, cur);
        if (prev.reg && prev.reg != wreg && prev.reg != pend.reg) r[prev.reg] = prev.val;
        r[0] = 0;
        icount += cycles_per_instr;
        if (cd.tick(icount)) irq_set(2);
        if ((spu_acc_ += cycles_per_instr) >= 768) { spu_acc_ -= 768; spu_cd_tick(); }
        if (ack_due && icount >= ack_due) { ack_due = 0; sio_irq = true; if (rd(&io[0x4A], 2) & 0x1000) irq_set(7); }
        if (icount >= next_vblank) { next_vblank += gpu.pal() ? 677376 : 564480; uint32_t st = rd(&io[0x70], 4) | 1u; wr(&io[0x70], st, 4); ++vblanks; }  // PAL 50 Hz or NTSC 60 Hz, as the game set it
    }
    static constexpr size_t kRing = 16384;          // interleaved stereo int16 slots of CD-DA samples awaiting mixing
    std::vector<int16_t> ring_ = std::vector<int16_t>(kRing, 0), rvb_ring_ = std::vector<int16_t>(kRing, 0), rvb_buf_; size_t ring_r_ = 0, ring_w_ = 0; uint32_t spu_acc_ = 0;
    // Audio sample frames per video frame at 44.1 kHz: 882 (PAL, 50 Hz) or 735 (NTSC, 60 Hz).
    int frame_samples() const { return gpu.pal() ? 882 : 735; }
    void spu_cd_tick() {   // one 44.1 kHz tick = 768 CPU cycles
        int16_t s[2] = {0, 0};
        cd.audio_samples(1, s);
        spu.capture_tick(s[0], s[1]);
        if (spu.take_irq()) irq_set(9);
        { int rl, rr; spu.rvb_out(rl, rr); rvb_ring_[ring_w_] = (int16_t)rl; rvb_ring_[ring_w_ + 1] = (int16_t)rr; }
        ring_[ring_w_] = s[0]; ring_[ring_w_ + 1] = s[1]; ring_w_ = (ring_w_ + 2) % kRing;
        if (ring_w_ == ring_r_) ring_r_ = (ring_r_ + 2) % kRing;   // drop the oldest sample if nobody is mixing
    }
    uint64_t vblanks = 0, irqs_taken = 0, dma4_reads = 0, dma4_writes = 0, dma4_write_words = 0;
    uint32_t dma4_wr_last_xfer = 0, dma4_wr_last_words = 0, dma4_wr_last_madr = 0; uint64_t dma4_wr_abs_total = 0; uint32_t dma4_wr_pc = 0, dma4_wr_ra = 0; std::vector<uint8_t>* dma4_rd_log = nullptr; std::vector<uint8_t>* dma4_wr_log = nullptr; std::map<uint32_t, uint64_t>* dma4_wr_addr_hist = nullptr;
    uint32_t dma4_last_madr = 0, dma4_last_words = 0, dma4_last_xfer = 0;
    // n stereo frames (interleaved int16) of everything audible: SPU voices + CD audio.
    // Audio output for one video frame. The CD-DA stream, its SPU capture buffers and the SPU IRQ are advanced by
    // the CPU loop (see spu_cd_tick), so they stay correct even if nobody calls this; it only mixes the voices with
    // the CD samples queued by the loop. SPU voice envelopes still advance here, so call it once per video frame.
    void audio_frame(int n, int16_t* out) {
        cd_buf_.assign((size_t)n * 2, 0); rvb_buf_.assign((size_t)n * 2, 0);
        for (int i = 0; i < n && ring_r_ != ring_w_; ++i) {
            cd_buf_[2 * i] = ring_[ring_r_]; cd_buf_[2 * i + 1] = ring_[ring_r_ + 1];
            rvb_buf_[2 * i] = rvb_ring_[ring_r_]; rvb_buf_[2 * i + 1] = rvb_ring_[ring_r_ + 1];
            ring_r_ = (ring_r_ + 2) % kRing;
        }
        spu.mix(n, out, cd_buf_.data(), rvb_buf_.data());
        if (spu.take_irq()) irq_set(9);
    }
    uint16_t pad1 = 0;   // controller 1, ACTIVE-HIGH (see pad_map.h); set by the frontend
    // Runs until the next vblank has occurred (or max instructions). Returns false if halted.
    bool run_frame(uint64_t max_instr = 2000000) {
        uint64_t v = vblanks, n = 0;
        while (!halted && vblanks == v && n++ < max_instr) step();
        return !halted;
    }

private:
    struct Pend { uint32_t reg = 0, val = 0; };
    Pend pend, prev;
    uint32_t wreg = 0;
    uint32_t heap_ptr = 0, heap_end = 0;
    uint32_t rand_state = 1;
    uint32_t gpustat_toggle = 0;

    static std::string hex(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "%08X", v); return b; }
    void halt(const std::string& why) { if (!halted) { halted = true; halt_reason = why + " (pc=" + hex(pc) + ")"; } }
    static uint32_t rd(const uint8_t* p, int s) {
        uint32_t v = 0; std::memcpy(&v, p, s); return v;
    }
    static void wr(uint8_t* p, uint32_t v, int s) { std::memcpy(p, &v, s); }

    uint32_t io_read(uint32_t a, int size) {
        if (trace.enabled) trace.io_reads[a]++;
        uint32_t off = a - 0x1F801000;
        if (siolog && siolog_v.size() < 400 && (a == 0x1F801040 || a == 0x1F801044)) { char b[96]; std::snprintf(b, sizeof b, "  %s %s rx=%02X ready=%d irq=%d (pc=%08X)", a == 0x1F801040 ? "RD data" : "RD stat", "", sio_rx, (int)sio_rx_ready, (int)sio_irq, pc); siolog_v.push_back(b); }
        if (a == 0x1F801040) { sio_rx_ready = false; return sio_rx; }
        if (a == 0x1F801044) return 0x5u | (sio_rx_ready ? 2u : 0u) | (sio_irq ? 0x200u : 0u) | 0x80u * (!sio_irq);
        if (a == 0x1F801814) return gpu.stat();
        if (a >= 0x1F801800 && a <= 0x1F801803) { uint32_t v = cd.read8(a - 0x1F801800); if (cdlog && cdlog_v.size() < cdlog_max) { char b[80]; std::snprintf(b, sizeof b, "R %d -> %02X  (pc=%08X)", (int)(a - 0x1F801800), v, pc); cdlog_v.push_back(b); } return v; }
        if (a >= 0x1F801C00 && a < 0x1F801E00) return spu.read(a - 0x1F801C00, size);
        if (a == 0x1F801810) return gpu.read_gpuread();
        // Root counters: current values derived from elapsed instructions (~1 cycle each).
        if (a == 0x1F801100) return (uint32_t)((icount / 6) & 0xFFFF);      // dot clock
        if (a == 0x1F801110) return (uint32_t)((icount / 2169) & 0xFFFF);   // hblank (PAL line)
        if (a == 0x1F801120) return (uint32_t)((icount / 8) & 0xFFFF);      // sysclk/8
        return rd(&io[off], size);
    }
    void io_write(uint32_t a, uint32_t v, int size) {
        if (trace.enabled) { trace.io_writes[a]++; if (!trace.io_first_write.count(a)) trace.io_first_write[a] = v; }
        uint32_t off = a - 0x1F801000;
        if (a == 0x1F802023) { trace.log.push_back((char)v); return; }   // BIOS tty (DUART) output
        if (a == 0x1F801050 && trace.log.size() < 65536) { trace.log.push_back((char)v); return; }   // SIO1 TX (BIOS tty over serial)
        if (a >= 0x1F801800 && a <= 0x1F801803) { if (cdlog && cdlog_v.size() < cdlog_max) { char b[96]; std::snprintf(b, sizeof b, "W %d <- %02X  (pc=%08X ra=%08X sp=%08X)", (int)(a - 0x1F801800), v & 0xFF, pc, r[31], r[29]); cdlog_v.push_back(b); } cd.write8(a - 0x1F801800, (uint8_t)v); return; }
        if (a == 0x1F801040) { sio_tx((uint8_t)v); return; }
        if (a == 0x1F80104A) {
            wr(&io[off], v, 2);
            if (v & 0x40) { sio_idx = 0; sio_rx_ready = false; sio_irq = false; ack_due = 0; }   // reset
            if (v & 0x10) sio_irq = false;                                                   // acknowledge
            if (!(v & 2)) { sio_idx = 0; ack_due = 0; }                                       // DTR low: deselect
            return;
        }
        if (a >= 0x1F801C00 && a < 0x1F801E00) { spu.write(a - 0x1F801C00, v, size); if (spu.take_irq()) irq_set(9); return; }
        if (a == 0x1F801810) { gpu.gp0(v); return; }
        if (a == 0x1F801814) { gpu.gp1(v); return; }
        if (a == 0x1F801070) { wr(&io[off], rd(&io[off], 4) & v, 4); return; }  // I_STAT: write-0-to-ack
        if (a == 0x1F8010F4) {   // DICR: flags (bits 24-30) are write-1-to-clear
            uint32_t old = rd(&io[off], 4);
            uint32_t nv = (old & 0x7F000000u & ~(v & 0x7F000000u)) | (v & 0x00FFFFFFu);
            wr(&io[off], nv, 4); dicr_update(); return;
        }
        if (a >= 0x1F801088 && a <= 0x1F8010E8 && ((a & 0xF) == 8) && (v & 0x01000000u)) {
            wr(&io[off], v, 4); dma_run((a - 0x1F801080) >> 4); return;
        }
        wr(&io[off], v, size);
    }

    void set(uint32_t reg, uint32_t v) { if (reg) { r[reg] = v; wreg = reg; } }
    void load_to(uint32_t reg, uint32_t v) { if (reg) { pend.reg = reg; pend.val = v; } }
    void branch(bool cond, uint32_t cur, int32_t off) { if (cond) next_pc = cur + 4 + (off << 2); }

    void exec(uint32_t i, uint32_t cur) {
        uint32_t op = i >> 26, rs = (i >> 21) & 31, rt = (i >> 16) & 31, rd_ = (i >> 11) & 31, sh = (i >> 6) & 31;
        uint32_t imm = i & 0xFFFF; int32_t simm = (int16_t)imm;
        uint32_t A = r[rs], B = r[rt];
        switch (op) {
        case 0x00:
            switch (i & 0x3F) {
            case 0x00: set(rd_, B << sh); break;
            case 0x02: set(rd_, B >> sh); break;
            case 0x03: set(rd_, (int32_t)B >> sh); break;
            case 0x04: set(rd_, B << (A & 31)); break;
            case 0x06: set(rd_, B >> (A & 31)); break;
            case 0x07: set(rd_, (int32_t)B >> (A & 31)); break;
            case 0x08: next_pc = A; break;
            case 0x09: { uint32_t t = A; set(rd_, cur + 8); next_pc = t; if (trace.enabled) trace.calls[t]++; break; }
            case 0x0C: if (real_bios) take_exception(8, cur, cur_ds); else syscall(); break;
            case 0x0D: if (real_bios) take_exception(9, cur, cur_ds); else halt("BREAK executed"); break;
            case 0x10: set(rd_, hi); break;
            case 0x11: hi = A; break;
            case 0x12: set(rd_, lo); break;
            case 0x13: lo = A; break;
            case 0x18: { int64_t m = (int64_t)(int32_t)A * (int32_t)B; lo = (uint32_t)m; hi = (uint32_t)(m >> 32); break; }
            case 0x19: { uint64_t m = (uint64_t)A * B; lo = (uint32_t)m; hi = (uint32_t)(m >> 32); break; }
            case 0x1A:
                if (B == 0) { hi = A; lo = (int32_t)A < 0 ? 1 : 0xFFFFFFFFu; }
                else if (A == 0x80000000u && B == 0xFFFFFFFFu) { lo = 0x80000000u; hi = 0; }
                else { lo = (uint32_t)((int32_t)A / (int32_t)B); hi = (uint32_t)((int32_t)A % (int32_t)B); }
                break;
            case 0x1B:
                if (B == 0) { hi = A; lo = 0xFFFFFFFFu; } else { lo = A / B; hi = A % B; }
                break;
            case 0x20: case 0x21: set(rd_, A + B); break;
            case 0x22: case 0x23: set(rd_, A - B); break;
            case 0x24: set(rd_, A & B); break;
            case 0x25: set(rd_, A | B); break;
            case 0x26: set(rd_, A ^ B); break;
            case 0x27: set(rd_, ~(A | B)); break;
            case 0x2A: set(rd_, (int32_t)A < (int32_t)B); break;
            case 0x2B: set(rd_, A < B); break;
            default: ++trace.unknown_ops; halt("unknown SPECIAL " + hex(i)); break;
            }
            break;
        case 0x01:
            switch (rt) {
            case 0x00: branch((int32_t)A < 0, cur, simm); break;
            case 0x01: branch((int32_t)A >= 0, cur, simm); break;
            case 0x10: set(31, cur + 8); branch((int32_t)A < 0, cur, simm); break;
            case 0x11: set(31, cur + 8); branch((int32_t)A >= 0, cur, simm); break;
            default: halt("unknown REGIMM " + hex(i)); break;
            }
            break;
        case 0x02: next_pc = (cur & 0xF0000000u) | ((i & 0x3FFFFFF) << 2); break;
        case 0x03: set(31, cur + 8); next_pc = (cur & 0xF0000000u) | ((i & 0x3FFFFFF) << 2); if (trace.enabled) trace.calls[next_pc]++; break;
        case 0x04: branch(A == B, cur, simm); break;
        case 0x05: branch(A != B, cur, simm); break;
        case 0x06: branch((int32_t)A <= 0, cur, simm); break;
        case 0x07: branch((int32_t)A > 0, cur, simm); break;
        case 0x08: case 0x09: set(rt, A + simm); break;
        case 0x0A: set(rt, (int32_t)A < simm); break;
        case 0x0B: set(rt, A < (uint32_t)simm); break;
        case 0x0C: set(rt, A & imm); break;
        case 0x0D: set(rt, A | imm); break;
        case 0x0E: set(rt, A ^ imm); break;
        case 0x0F: set(rt, imm << 16); break;
        case 0x10:  // COP0
            if (rs == 0x00) set(rt, cop0[rd_]);
            else if (rs == 0x04) { if (real_bios && rd_ == 13) cop0[13] = (cop0[13] & ~0x300u) | (B & 0x300u); else if (real_bios && rd_ == 14) {} else cop0[rd_] = B; }
            else if ((i & 0x3F) == 0x10) cop0[12] = (cop0[12] & ~0xFu) | ((cop0[12] >> 2) & 0xF);  // RFE
            else halt("unknown COP0 " + hex(i));
            break;
        case 0x12: ++trace.gte_ops;
            if (i & 0x02000000u) { if (trace.enabled) trace.gte_cmds[i & 0x1FFFFFFu]++; gte.exec(i & 0x1FFFFFFu); }
            else if (rs == 0x00) load_to(rt, gte.read(rd_));
            else if (rs == 0x02) load_to(rt, gte.read_ctl(rd_));
            else if (rs == 0x04) gte.write(rd_, B);
            else if (rs == 0x06) gte.write_ctl(rd_, B);
            break;
        case 0x20: { uint32_t v = read(A + simm, 1); load_to(rt, (uint32_t)(int8_t)v); break; }
        case 0x21: { uint32_t v = read(A + simm, 2); load_to(rt, (uint32_t)(int16_t)v); break; }
        case 0x22: { uint32_t a = A + simm, w = read(a & ~3u, 4), c = cur_val(rt);
            static const uint32_t m[4] = {0x00FFFFFF, 0x0000FFFF, 0x000000FF, 0};
            static const int s[4] = {24, 16, 8, 0};
            load_to(rt, (c & m[a & 3]) | (w << s[a & 3])); break; }
        case 0x23: load_to(rt, read(A + simm, 4)); break;
        case 0x24: load_to(rt, read(A + simm, 1)); break;
        case 0x25: load_to(rt, read(A + simm, 2)); break;
        case 0x26: { uint32_t a = A + simm, w = read(a & ~3u, 4), c = cur_val(rt);
            static const uint32_t m[4] = {0, 0xFF000000u, 0xFFFF0000u, 0xFFFFFF00u};
            static const int s[4] = {0, 8, 16, 24};
            load_to(rt, (c & m[a & 3]) | (w >> s[a & 3])); break; }
        case 0x28: write(A + simm, B, 1); break;
        case 0x29: write(A + simm, B, 2); break;
        case 0x2A: { uint32_t a = A + simm, w = read(a & ~3u, 4);
            static const uint32_t m[4] = {0xFFFFFF00u, 0xFFFF0000u, 0xFF000000u, 0};
            static const int s[4] = {24, 16, 8, 0};
            write(a & ~3u, (w & m[a & 3]) | (B >> s[a & 3]), 4); break; }
        case 0x2B: write(A + simm, B, 4); break;
        case 0x2E: { uint32_t a = A + simm, w = read(a & ~3u, 4);
            static const uint32_t m[4] = {0, 0xFF, 0xFFFF, 0xFFFFFF};
            static const int s[4] = {0, 8, 16, 24};
            write(a & ~3u, (w & m[a & 3]) | (B << s[a & 3]), 4); break; }
        case 0x32: ++trace.gte_ops; gte.write(rt, read(A + simm, 4)); break;     // LWC2
        case 0x3A: ++trace.gte_ops; write(A + simm, gte.read(rt), 4); break;     // SWC2
        default: ++trace.unknown_ops; halt("unknown opcode " + hex(i)); break;
        }
    }
    uint32_t cur_val(uint32_t reg) { return (prev.reg == reg) ? prev.val : r[reg]; }  // LWL/LWR see an in-flight load

    // Genuine R3000 exception entry. epc_pc = address of the faulting instruction (or of the
    // instruction that would run next for an interrupt); ds = it is in a taken branch's delay slot.
    void take_exception(uint32_t code, uint32_t epc_pc, bool ds) {
        cop0[14] = ds ? epc_pc - 4 : epc_pc;
        cop0[13] = (cop0[13] & ~0x8000007Cu) | (code << 2) | (ds ? 0x80000000u : 0);
        cop0[12] = (cop0[12] & ~0x3Fu) | ((cop0[12] << 2) & 0x3Cu);
        pc = (cop0[12] & 0x400000) ? 0xBFC00180u : 0x80000080u;
        next_pc = pc + 4;
        if (code == 0) ++irqs_taken;
    }
public:
    bool load_bios(const std::string& path) {
        FILE* f = std::fopen(path.c_str(), "rb"); if (!f) return false;
        rom.assign(512 * 1024, 0);
        size_t n = std::fread(rom.data(), 1, rom.size(), f); std::fclose(f);
        if (n < 0x10000) { rom.clear(); return false; }
        real_bios = true; reset_cpu(); return true;
    }
    // Call a kernel function through the real BIOS jump tables (table = 0xA0/0xB0/0xC0) and run until it returns.
    bool call_bios(uint32_t table, uint32_t fn, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint64_t limit = 20000000) {
        const uint32_t magic = 0x8000FFF0u;
        uint32_t sv_sp = r[29];
        r[4] = a0; r[5] = a1; r[6] = a2; r[9] = fn; r[31] = magic;
        if (r[29] < 0x80000000u || r[29] > 0x80200000u) r[29] = 0x801FFF00u;
        pc = table; next_pc = table + 4; pend = {}; prev = {};
        uint64_t end = icount + limit;
        while (!halted && pc != magic && icount < end) step();
        r[29] = sv_sp;
        return !halted && pc == magic;
    }
    // What the BIOS shell does for a disc: SetConf from SYSTEM.CNF, load the EXE, flush, jump to it.
    bool sideload_exe(const std::vector<uint8_t>& text, uint32_t load, uint32_t pc0, uint32_t gp, uint32_t sp,
                      uint32_t events, uint32_t tcbs, uint32_t stack_top) {
        cop0[12] &= ~1u;   // interrupts off while we reconfigure the kernel
        if (!call_bios(0xA0, 0x9C, events, tcbs, stack_top)) { halt("SetConf failed"); return false; }
        std::memcpy(&ram[load & 0x1FFFFF], text.data(), text.size());
        if (!call_bios(0xA0, 0x44)) { halt("FlushCache failed"); return false; }   // FlushCache
        for (int i = 1; i < 32; ++i) r[i] = 0;
        r[28] = gp; r[29] = r[30] = sp ? sp : stack_top; r[4] = 1; r[5] = 0; r[31] = 0x80030000u;
        pc = pc0; next_pc = pc0 + 4; pend = {}; prev = {};
        return true;
    }
    void reset_cpu() {
        std::memset(r, 0, sizeof r); hi = lo = 0;
        std::memset(cop0, 0, sizeof cop0);
        cop0[12] = 0x00400000;   // BEV=1, interrupts off
        cop0[15] = 0x00000002;   // PRId
        pc = 0xBFC00000u; next_pc = pc + 4; pend = {}; prev = {};
        halted = false; icount = 0; next_vblank = 677376;
    }
private:

    // ---- syscall (HLE of the BIOS exception handler's syscall paths) -------
    void syscall() {
        ++trace.syscalls;
        switch (r[4]) {
        case 1: { uint32_t was = cop0[12] & 1; cop0[12] &= ~0x401u; set(2, was); break; }  // EnterCriticalSection
        case 2: cop0[12] |= 0x401u; break;                                                 // ExitCriticalSection
        default: trace.bios_calls["SYSCALL " + std::to_string(r[4])]++; break;
        }
    }

    // ---- HLE BIOS ----------------------------------------------------------
    static const char* name(uint32_t table, uint32_t fn) {
        if (table == 0xA0) switch (fn) {
            case 0x13: return "setjmp"; case 0x14: return "longjmp"; case 0x17: return "strcmp";
            case 0x15: return "strcat"; case 0x16: return "strncat"; case 0x18: return "strncmp"; case 0x1A: return "strncpy"; case 0x1C: return "index"; case 0x1D: return "rindex"; case 0x1E: return "strchr"; case 0x1F: return "strrchr"; case 0x19: return "strcpy"; case 0x1B: return "strlen"; case 0x25: return "toupper";
            case 0x26: return "tolower"; case 0x27: return "bcopy"; case 0x28: return "bzero";
            case 0x2A: return "memcpy"; case 0x2B: return "memset"; case 0x2C: return "memmove";
            case 0x2D: return "memcmp"; case 0x2F: return "rand"; case 0x30: return "srand";
            case 0x33: return "malloc"; case 0x34: return "free"; case 0x39: return "InitHeap";
            case 0x3F: return "printf"; case 0x44: return "FlushCache"; case 0x45: return "init_a0_b0_c0_vectors";
            case 0x46: return "GPU_dw"; case 0x47: return "gpu_send_dma"; case 0x48: return "SendGP1Command";
            case 0x49: return "GPU_cw"; case 0x4A: return "GPU_cwp"; case 0x4B: return "send_gpu_linked_list";
            case 0x72: return "_96_remove"; case 0x70: return "_bu_init"; case 0x71: return "_96_init";
            case 0x00: return "open"; case 0x02: return "read"; case 0x03: return "write"; case 0x04: return "close";
            case 0x0E: return "abs"; case 0x10: return "atoi"; default: return "?";
        }
        if (table == 0xB0) switch (fn) {
            case 0x00: return "alloc_kernel_memory"; case 0x07: return "DeliverEvent"; case 0x08: return "OpenEvent";
            case 0x09: return "CloseEvent"; case 0x0A: return "WaitEvent"; case 0x0B: return "TestEvent";
            case 0x0C: return "EnableEvent"; case 0x0D: return "DisableEvent"; case 0x0E: return "OpenThread";
            case 0x12: return "InitPad"; case 0x13: return "StartPad"; case 0x14: return "StopPad";
            case 0x15: return "PAD_init"; case 0x16: return "PAD_dr"; case 0x17: return "ReturnFromException";
            case 0x18: return "SetDefaultExitFromException"; case 0x19: return "SetCustomExitFromException";
            case 0x20: return "UnDeliverEvent"; case 0x32: return "FileOpen"; case 0x35: return "write"; case 0x3C: return "getchar";
            case 0x3D: return "putchar"; case 0x3F: return "puts"; case 0x56: return "GetC0Table"; case 0x57: return "GetB0Table";
            default: return "?";
        }
        switch (fn) {
            case 0x00: return "EnqueueTimerAndVblankIrqs"; case 0x01: return "EnqueueSyscallHandler";
            case 0x02: return "SysEnqIntRP"; case 0x03: return "SysDeqIntRP"; case 0x07: return "InstallExceptionHandlers";
            case 0x0A: return "ChangeClearRCnt"; case 0x12: return "InstallDevices"; case 0x1C: return "AdjustA0Table";
            default: return "?";
        }
    }

    std::string cstr(uint32_t a) {
        std::string s;
        for (int n = 0; n < 512; ++n) { uint32_t c = read(a + n, 1); if (!c) break; s += (char)c; }
        return s;
    }

    void bios_call(uint32_t table) {
        uint32_t fn = r[9] & 0xFF;
        char key[96];
        if (trace.enabled) std::snprintf(key, sizeof key, "%02X:%02X %s", table, fn, name(table, fn));
        if (trace.enabled) trace.bios_calls[key]++;
        uint32_t ret = 0;
        bool handled = true;
        if (table == 0xA0) {
            switch (fn) {
            case 0x13: { uint32_t b = r[4]; write(b, r[31], 4); write(b + 4, r[29], 4); write(b + 8, r[30], 4);
                for (int k = 0; k < 8; ++k) write(b + 12 + 4 * k, r[16 + k], 4);
                write(b + 44, r[28], 4); ret = 0; break; }
            case 0x14: { uint32_t b = r[4]; r[31] = read(b, 4); r[29] = read(b + 4, 4); r[30] = read(b + 8, 4);
                for (int k = 0; k < 8; ++k) r[16 + k] = read(b + 12 + 4 * k, 4);
                r[28] = read(b + 44, 4); ret = r[5] ? r[5] : 1; break; }
            case 0x17: { std::string a = cstr(r[4]), b = cstr(r[5]); ret = a < b ? (uint32_t)-1 : (a > b); break; }
            case 0x19: { uint32_t d = r[4], s = r[5]; for (;; ++d, ++s) { uint32_t c = read(s, 1); write(d, c, 1); if (!c) break; } ret = r[4]; break; }
            case 0x1B: ret = (uint32_t)cstr(r[4]).size(); break;
            case 0x15: case 0x16: {   // strcat / strncat
                uint32_t d = r[4] + (uint32_t)cstr(r[4]).size(); uint32_t s2 = r[5], n = fn == 0x16 ? r[6] : 0xFFFFFFFFu;
                for (; n; --n, ++d, ++s2) { uint32_t c = read(s2, 1); write(d, c, 1); if (!c) break; }
                if (fn == 0x16 && n == 0) write(d, 0, 1);
                ret = r[4]; break; }
            case 0x18: { uint32_t n = r[6]; ret = 0;   // strncmp
                for (uint32_t k = 0; k < n; ++k) { int a = read(r[4] + k, 1), b = read(r[5] + k, 1); if (a != b) { ret = (uint32_t)(a - b); break; } if (!a) break; }
                break; }
            case 0x1A: { uint32_t n = r[6], k = 0;     // strncpy
                for (; k < n; ++k) { uint32_t c = read(r[5] + k, 1); write(r[4] + k, c, 1); if (!c) { ++k; break; } }
                for (; k < n; ++k) write(r[4] + k, 0, 1);
                ret = r[4]; break; }
            case 0x1C: case 0x1E: { ret = 0; for (uint32_t k = 0; k < 4096; ++k) { uint32_t c = read(r[4] + k, 1); if (c == (r[5] & 0xFF)) { ret = r[4] + k; break; } if (!c) break; } break; }  // index / strchr
            case 0x1D: case 0x1F: { ret = 0; for (uint32_t k = 0; k < 4096; ++k) { uint32_t c = read(r[4] + k, 1); if (c == (r[5] & 0xFF)) ret = r[4] + k; if (!c) break; } break; }  // rindex / strrchr
            case 0x25: { uint32_t c = r[4]; ret = (c >= 'a' && c <= 'z') ? c - 32 : c; break; }
            case 0x26: { uint32_t c = r[4]; ret = (c >= 'A' && c <= 'Z') ? c + 32 : c; break; }
            case 0x27: case 0x2A: case 0x2C: {
                uint32_t d = r[4], s = r[5], n = r[6];
                if (fn == 0x27) { d = r[5]; s = r[4]; }   // bcopy(src, dst, n)
                std::vector<uint8_t> t(n);
                for (uint32_t k = 0; k < n; ++k) t[k] = (uint8_t)read(s + k, 1);
                for (uint32_t k = 0; k < n; ++k) write(d + k, t[k], 1);
                ret = fn == 0x27 ? 0 : r[4]; break; }
            case 0x28: for (uint32_t k = 0; k < r[5]; ++k) write(r[4] + k, 0, 1); break;
            case 0x2B: for (uint32_t k = 0; k < r[6]; ++k) write(r[4] + k, r[5], 1); ret = r[4]; break;
            case 0x2D: { ret = 0; for (uint32_t k = 0; k < r[6]; ++k) { int a = read(r[4] + k, 1), b = read(r[5] + k, 1); if (a != b) { ret = (uint32_t)(a - b); break; } } break; }
            case 0x0E: ret = (int32_t)r[4] < 0 ? -r[4] : r[4]; break;
            case 0x2F: rand_state = rand_state * 1103515245u + 12345u; ret = (rand_state >> 16) & 0x7FFF; break;
            case 0x30: rand_state = r[4]; break;
            case 0x39: heap_ptr = r[4]; heap_end = r[4] + r[5]; break;
            case 0x33: { uint32_t n = (r[4] + 7) & ~7u; if (heap_ptr && heap_ptr + n <= heap_end) { ret = heap_ptr; heap_ptr += n; } break; }
            case 0x34: break;   // free: leak (bump allocator)
            case 0x3F: { ++trace.printf_calls; hle_printf(); break; }
            case 0x43: {   // DoExecute(EXEC* e, argc, argv): start the loaded program
                uint32_t e = r[4], argc = r[5], argv = r[6];
                uint32_t pc0 = read(e, 4), gp0 = read(e + 4, 4), b_addr = read(e + 0x18, 4), b_size = read(e + 0x1C, 4);
                uint32_t s_addr = read(e + 0x20, 4), s_size = read(e + 0x24, 4);
                for (uint32_t k = 0; k < b_size; ++k) write(b_addr + k, 0, 1);
                if (s_addr) r[29] = r[30] = s_addr + s_size;
                r[28] = gp0; r[4] = argc; r[5] = argv;
                frames.clear(); hook_buf = 0; pend = {}; prev = {};
                cop0[12] = 0x10900000;
                ++trace.execs; trace.log += "[DoExecute pc=" + hex(pc0) + "]\n";
                pc = pc0; next_pc = pc + 4;
                return; }
            case 0x44: case 0x45: break;
            default: handled = false; break;
            }
        } else if (table == 0xB0) {
            switch (fn) {
            case 0x08: ret = ++event_id; break;      // OpenEvent -> fresh handle
            case 0x3F: if (trace.log.size() < 65536) trace.log += cstr(r[4]) + "\n"; ret = 1; break;   // std_out_puts
            case 0x35: { uint32_t n = r[6]; if (r[4] == 1 && trace.log.size() < 65536) for (uint32_t k = 0; k < n && k < 512; ++k) trace.log += (char)read(r[5] + k, 1); ret = n; break; }   // write(fd, buf, n): fd 1 = console
            case 0x3D: trace.log += (char)r[4]; ret = r[4]; break;         // std_out_putchar
            case 0x19: hook_buf = r[4]; ret = 0; break;   // library registers its interrupt entry jmp_buf
            case 0x17: return_from_exception(); return;   // ReturnFromException: no normal return
            case 0x0C: ret = 1; break;               // EnableEvent
            case 0x0B: ret = 1; break;               // TestEvent: optimistic (event "fired")
            case 0x07: case 0x09: case 0x0A: case 0x0D: case 0x20: ret = 1; break;
            case 0x56: ret = 0; break; case 0x57: ret = 0; break;
            default: handled = false; break;
            }
        } else {
            handled = true;  // C0 table: accept silently (kernel-internal setup)
        }
        if (!handled) {
            if (trace.enabled) { std::snprintf(key, sizeof key, "UNHANDLED %02X:%02X %s a0=%08X a1=%08X ra=%08X", table, fn, name(table, fn), r[4], r[5], r[31]); trace.bios_calls[key]++; }
        }
        set(2, ret);
        pc = r[31]; next_pc = pc + 4;
    }
    uint32_t event_id = 0xF1000000u;
    std::vector<int16_t> cd_buf_;

    // ---- SIO0: controller port (digital pad on port 1) ------------------------
    uint8_t sio_rx = 0xFF; bool sio_rx_ready = false, sio_irq = false; int sio_idx = 0; uint64_t ack_due = 0;
    void sio_tx(uint8_t v) {
        bool port2 = (rd(&io[0x4A], 2) & 0x2000) != 0;
        uint8_t reply = 0xFF; bool more = false;
        if (!port2) {
            uint16_t w = (uint16_t)~pad1;   // wire format is active-low
            switch (sio_idx) {
            case 0: if (v == 0x01) { reply = 0xFF; more = true; sio_idx = 1; } break;
            case 1: reply = 0x41; more = true; sio_idx = 2; break;                 // digital pad ID
            case 2: reply = 0x5A; more = true; sio_idx = 3; break;
            case 3: reply = (uint8_t)(w & 0xFF); more = true; sio_idx = 4; break;
            case 4: reply = (uint8_t)(w >> 8); more = true; sio_idx = 0; break;   // ACK after the last byte too: the game's pad driver waits for it
            }
        }
        if (siolog && siolog_v.size() < 400) { char b[96]; std::snprintf(b, sizeof b, "tx=%02X rx=%02X port2=%d idx=%d ctrl=%04X (pc=%08X)", v, reply, (int)port2, sio_idx, rd(&io[0x4A], 2), pc); siolog_v.push_back(b); }
        sio_rx = reply; sio_rx_ready = true;
        if (more) ack_due = icount + 300;
    }

    // ---- DMA -----------------------------------------------------------------
    uint32_t ioreg(uint32_t a) { return rd(&io[a - 0x1F801000], 4); }
    void irq_set(int bit) { wr(&io[0x70], rd(&io[0x70], 4) | (1u << bit), 4); }
    void dicr_update() {
        uint32_t d = ioreg(0x1F8010F4);
        bool master = (d & (1u << 15)) || ((d & (1u << 23)) && (((d >> 16) & 0x7F) & ((d >> 24) & 0x7F)));
        bool was = d & 0x80000000u;
        d = master ? (d | 0x80000000u) : (d & ~0x80000000u);
        wr(&io[0xF4], d, 4);
        if (master && !was) irq_set(3);
    }
    void dma_done(uint32_t ch) {
        uint32_t chcr = 0x1F801088 + ch * 0x10;
        wr(&io[chcr - 0x1F801000], ioreg(chcr) & ~0x11000000u, 4);
        uint32_t d = ioreg(0x1F8010F4);
        if (d & (1u << (16 + ch))) { d |= 1u << (24 + ch); wr(&io[0xF4], d, 4); }
        dicr_update();
    }
    void dma_run(uint32_t ch) {
        uint32_t base = 0x1F801080 + ch * 0x10;
        uint32_t madr = ioreg(base) & 0x00FFFFFC, bcr = ioreg(base + 4), chcr = ioreg(base + 8);
        uint32_t blk = bcr & 0xFFFF, cnt = bcr >> 16;
        uint32_t words = ((chcr >> 9) & 3) == 1 ? blk * (cnt ? cnt : 1) : (blk ? blk : 0x10000);
        switch (ch) {
        case 2: {   // GPU
            uint32_t sync = (chcr >> 9) & 3;
            if (chcr & 1) {   // RAM -> GPU
                if (sync == 2) {   // linked list (ordering table)
                    uint32_t addr = madr; int guard = 0;
                    while (guard++ < 1000000) {
                        uint32_t hdr = read(addr, 4), n = hdr >> 24;
                        for (uint32_t k = 1; k <= n; ++k) gpu.gp0(read(addr + 4 * k, 4));
                        uint32_t next = hdr & 0x00FFFFFF;
                        if (next == 0x00FFFFFF || (next & 0x800000)) break;
                        addr = next;
                    }
                } else for (uint32_t i = 0; i < words; ++i) gpu.gp0(read(madr + 4 * i, 4));
            } else for (uint32_t i = 0; i < words; ++i) write(madr + 4 * i, gpu.read_gpuread(), 4);   // GPU -> RAM
            break; }
        case 3:   // CD-ROM -> RAM
            if (cd.dbg_log.size() < 60) { char b[64]; std::snprintf(b, sizeof b, "dma3 words=%u madr=%08X", words, madr); cd.dbg_log.push_back(b); }
            for (uint32_t i = 0; i < words; ++i) write(madr + 4 * i, cd.data_word(), 4);
            break;
        case 4:   // RAM <-> SPU RAM
            if (!(chcr & 1)) {
                ++dma4_reads; dma4_last_madr = madr; dma4_last_words = words; dma4_last_xfer = spu.xfer;
                for (uint32_t i = 0; i < words; ++i) write(madr + 4 * i, spu.dma_read_word(), 4);
                if (dma4_rd_log) for (uint32_t i = 0; i < words * 4; ++i) dma4_rd_log->push_back(ram[(madr + i) & 0x1FFFFF]);
            }
            if (chcr & 1) { ++dma4_writes; dma4_write_words += words; dma4_wr_last_xfer = spu.xfer; dma4_wr_last_words = words; dma4_wr_last_madr = madr; dma4_wr_pc = pc; dma4_wr_ra = r[31]; if (dma4_wr_addr_hist) (*dma4_wr_addr_hist)[spu.xfer * 1 + 0] += 1; if (dma4_wr_log) for (uint32_t i = 0; i < words * 4; ++i) dma4_wr_log->push_back(ram[(madr + i) & 0x1FFFFF]); { long s = 0; for (uint32_t i = 0; i < words; ++i) { int16_t a = (int16_t)read(madr + 4 * i, 2), b = (int16_t)read(madr + 4 * i + 2, 2); s += std::abs((int)a) + std::abs((int)b); } dma4_wr_abs_total += (uint64_t)s; }
                            for (uint32_t i = 0; i < words; ++i) spu.dma_write_word(read(madr + 4 * i, 4)); }
            if (spu.take_irq()) irq_set(9);
            break;
        case 6: {  // ordering-table clear
            uint32_t n = blk ? blk : 0x10000;
            for (uint32_t i = 0; i < n; ++i) {
                uint32_t addr = madr - 4 * i;
                write(addr, i == n - 1 ? 0x00FFFFFFu : ((addr - 4) & 0x00FFFFFFu), 4);
            }
            break; }
        default: ++trace.dma_other; break;
        }
        if (trace.enabled) ++trace.dma_runs[ch];
        dma_done(ch);
    }
    // ---- interrupts (HLE of the BIOS exception handler) ----------------------
    // The game library registers an "interrupt entry" jmp_buf (B0:19). On an IRQ
    // we save the interrupted context, switch to that buffer's regs/pc, and the
    // handler finishes with ReturnFromException (B0:17), which restores it.
    struct Frame { uint32_t r[32], hi, lo, pc, sr; };
    std::vector<Frame> frames;
    uint32_t hook_buf = 0;

    bool irq_ready() {
        if (!(cop0[12] & 1) || !(cop0[12] & 0x400)) return false;
        if (next_pc != pc + 4) return false;  // never interrupt inside a delay slot
        return (rd(&io[0x70], 4) & rd(&io[0x74], 4) & 0x7FF) != 0;
    }
    void take_irq() {
        if (pend.reg) { r[pend.reg] = pend.val; pend = {}; }
        Frame f; std::memcpy(f.r, r, sizeof r); f.hi = hi; f.lo = lo; f.pc = pc; f.sr = cop0[12];
        frames.push_back(f);
        cop0[12] = (cop0[12] & ~0x3Fu) | ((cop0[12] << 2) & 0x3Cu);  // push mode/IE stack: IE off
        cop0[13] = (cop0[13] & ~0x7C) | 0x400;
        cop0[14] = pc;
        uint32_t b = hook_buf;
        r[31] = read(b, 4); r[29] = read(b + 4, 4); r[30] = read(b + 8, 4);
        for (int k = 0; k < 8; ++k) r[16 + k] = read(b + 12 + 4 * k, 4);
        r[28] = read(b + 44, 4);
        r[2] = 1;  // the buffer is a setjmp() return point; resume it as if longjmp(buf, 1)
        pc = r[31]; next_pc = pc + 4;
        ++irqs_taken;
    }
    void return_from_exception() {
        if (frames.empty()) { halt("ReturnFromException with no saved frame"); return; }
        Frame f = frames.back(); frames.pop_back();
        std::memcpy(r, f.r, sizeof r); hi = f.hi; lo = f.lo;
        cop0[12] = f.sr;
        pc = f.pc; next_pc = pc + 4;
    }

    void hle_printf() {
        std::string f = cstr(r[4]), out;
        uint32_t argi = 5;
        auto arg = [&]() -> uint32_t {
            if (argi < 8) return r[argi++];
            uint32_t v = read(r[29] + 16 + 4 * (argi - 4), 4); ++argi; return v;
        };
        for (size_t k = 0; k < f.size(); ++k) {
            if (f[k] != '%') { out += f[k]; continue; }
            std::string spec = "%";
            while (++k < f.size() && std::strchr("0123456789.-+ #l", f[k])) if (f[k] != 'l') spec += f[k];
            char c = k < f.size() ? f[k] : '%', buf[64];
            if (c == 's') out += cstr(arg());
            else if (c == 'c') out += (char)arg();
            else if (c == '%') out += '%';
            else { spec += c == 'u' ? 'u' : c; std::snprintf(buf, sizeof buf, spec.c_str(), arg()); out += buf; }
        }
        trace.log += out;
    }
};

}  // namespace cpu
