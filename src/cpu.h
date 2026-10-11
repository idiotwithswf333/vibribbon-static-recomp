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

namespace cpu {

struct Trace {
    std::map<std::string, uint64_t> bios_calls;   // "A0:3F printf" -> count
    std::map<uint32_t, uint64_t> io_reads, io_writes;  // physical addr -> count
    std::map<uint32_t, uint32_t> io_first_write;       // physical addr -> first value
    std::map<uint32_t, uint64_t> pc_hist, calls;   // calls: jal/jalr target -> count
    uint64_t gte_ops = 0, unknown_ops = 0, syscalls = 0, printf_calls = 0;
    std::string log;      // printf output and notable events
    bool verbose_bios = false;
};

class Cpu {
public:
    uint32_t r[32] = {}, hi = 0, lo = 0, pc = 0, next_pc = 4, cop0[32] = {};
    uint64_t icount = 0;
    Trace trace;
    std::vector<uint8_t> ram = std::vector<uint8_t>(2 * 1024 * 1024);
    uint8_t scratch[1024] = {};
    uint8_t io[0x2000] = {};
    bool halted = false;
    std::string halt_reason;

    // ---- memory ---------------------------------------------------------
    static uint32_t phys(uint32_t a) { return a >= 0xFFFE0000u ? a : (a & 0x1FFFFFFFu); }

    uint32_t read(uint32_t va, int size) {
        uint32_t a = phys(va);
        if (a < 0x800000) return rd(&ram[a & 0x1FFFFF], size);
        if (a >= 0x1F800000 && a < 0x1F800400) return rd(&scratch[a - 0x1F800000], size);
        if (a >= 0x1F801000 && a < 0x1F803000) return io_read(a, size);
        if (a >= 0x1F000000 && a < 0x1F800000) return 0xFFFFFFFFu;  // expansion 1: nothing there
        if (a >= 0x1FC00000 && a < 0x1FC80000) return 0;            // BIOS ROM absent (HLE)
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
        if (hook_buf && irq_ready()) take_irq();
        uint32_t p = phys(pc);
        if (p == 0xA0 || p == 0xB0 || p == 0xC0) { bios_call(p); return; }
        uint32_t cur = pc;
        uint32_t i = read(cur, 4);
        if (halted) return;
        trace.pc_hist[cur]++;
        pc = next_pc; next_pc += 4;
        prev = pend; pend = {};
        wreg = 0;
        exec(i, cur);
        if (prev.reg && prev.reg != wreg && prev.reg != pend.reg) r[prev.reg] = prev.val;
        r[0] = 0;
        ++icount;
        if (icount % 677376 == 0) { uint32_t st = rd(&io[0x70], 4) | 1u; wr(&io[0x70], st, 4); ++vblanks; }  // PAL 50 Hz
    }
    uint64_t vblanks = 0, irqs_taken = 0;

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
        trace.io_reads[a]++;
        uint32_t off = a - 0x1F801000;
        if (a == 0x1F801814) { gpustat_toggle ^= 0x80000000u; return 0x1C000000u | gpustat_toggle; }
        if (a == 0x1F801810) return 0;
        // Root counters: current values derived from elapsed instructions (~1 cycle each).
        if (a == 0x1F801100) return (uint32_t)((icount / 6) & 0xFFFF);      // dot clock
        if (a == 0x1F801110) return (uint32_t)((icount / 2169) & 0xFFFF);   // hblank (PAL line)
        if (a == 0x1F801120) return (uint32_t)((icount / 8) & 0xFFFF);      // sysclk/8
        return rd(&io[off], size);
    }
    void io_write(uint32_t a, uint32_t v, int size) {
        trace.io_writes[a]++;
        if (!trace.io_first_write.count(a)) trace.io_first_write[a] = v;
        uint32_t off = a - 0x1F801000;
        if (a == 0x1F801070) { wr(&io[off], rd(&io[off], 4) & v, 4); return; }  // I_STAT: write-0-to-ack
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
            case 0x09: { uint32_t t = A; set(rd_, cur + 8); next_pc = t; trace.calls[t]++; break; }
            case 0x0C: syscall(); break;
            case 0x0D: halt("BREAK executed"); break;
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
        case 0x03: set(31, cur + 8); next_pc = (cur & 0xF0000000u) | ((i & 0x3FFFFFF) << 2); trace.calls[next_pc]++; break;
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
            else if (rs == 0x04) cop0[rd_] = B;
            else if ((i & 0x3F) == 0x10) cop0[12] = (cop0[12] & ~0xFu) | ((cop0[12] >> 2) & 0xF);  // RFE
            else halt("unknown COP0 " + hex(i));
            break;
        case 0x12: ++trace.gte_ops;
            if (rs == 0x00 || rs == 0x02) set(rt, 0);  // MFC2/CFC2 read as 0 for now
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
        case 0x32: case 0x3A: ++trace.gte_ops; break;  // LWC2/SWC2
        default: ++trace.unknown_ops; halt("unknown opcode " + hex(i)); break;
        }
    }
    uint32_t cur_val(uint32_t reg) { return (prev.reg == reg) ? prev.val : r[reg]; }  // LWL/LWR see an in-flight load

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
            case 0x19: return "strcpy"; case 0x1B: return "strlen"; case 0x25: return "toupper";
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
            case 0x20: return "UnDeliverEvent"; case 0x32: return "FileOpen"; case 0x3C: return "getchar";
            case 0x3D: return "putchar"; case 0x56: return "GetC0Table"; case 0x57: return "GetB0Table";
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
        std::snprintf(key, sizeof key, "%02X:%02X %s", table, fn, name(table, fn));
        trace.bios_calls[key]++;
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
            case 0x44: case 0x45: break;
            default: handled = false; break;
            }
        } else if (table == 0xB0) {
            switch (fn) {
            case 0x08: ret = ++event_id; break;      // OpenEvent -> fresh handle
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
            std::snprintf(key, sizeof key, "UNHANDLED %02X:%02X %s a0=%08X a1=%08X ra=%08X", table, fn, name(table, fn), r[4], r[5], r[31]);
            trace.bios_calls[key]++;
        }
        set(2, ret);
        pc = r[31]; next_pc = pc + 4;
    }
    uint32_t event_id = 0xF1000000u;

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
