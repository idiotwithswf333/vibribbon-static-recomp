// PS1 SPU (sound chip): 24 ADPCM voices with ADSR envelopes, 512 KB sound RAM,
// mixed at 44.1 kHz together with CD audio. Not implemented: reverb, noise,
// pitch modulation, volume sweeps (treated as full volume).
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace hw {

class Spu {
public:
    std::vector<uint8_t> ram = std::vector<uint8_t>(512 * 1024);
    uint32_t xfer = 0;   // current sound-RAM transfer address (bytes)
    uint64_t key_ons = 0;

    // SPU IRQ (IRQ9): raised when a voice block fetch, a FIFO/DMA write, or the capture-buffer write pointer touches
    // the sound-RAM address in the IRQ-address register (0x1A4) while SPUCNT bit 6 is set. The flag shows in
    // SPUSTAT bit 6 and is cleared by writing SPUCNT with bit 6 = 0. The CPU polls take_irq() and sets I_STAT bit 9.
    bool take_irq() { bool f = irq_fired_; irq_fired_ = false; return f; }

    uint32_t read(uint32_t off, int size) const {
        uint32_t lo = rd16(off);
        if (size == 4) return lo | (uint32_t)rd16(off + 2) << 16;
        if (size == 1) return (off & 1) ? lo >> 8 : lo & 0xFF;
        return lo;
    }
    void write(uint32_t off, uint32_t v, int size) {
        if (size == 4) { write16(off, (uint16_t)v); write16(off + 2, (uint16_t)(v >> 16)); }
        else if (size == 2) write16(off, (uint16_t)v);
        else {   // byte write: merge into the halfword
            uint16_t cur = reg_[(off >> 1) & 0xFF];
            if (off & 1) cur = (uint16_t)((cur & 0x00FF) | (v & 0xFF) << 8); else cur = (uint16_t)((cur & 0xFF00) | (v & 0xFF));
            write16(off & ~1u, cur);
        }
    }
    void dma_write_word(uint32_t w) {   // RAM -> SPU RAM via DMA channel 4
        check_irq(xfer); check_irq(xfer + 2);
        for (int i = 0; i < 4; ++i) ram[(xfer++) & 0x7FFFF] = (uint8_t)(w >> (8 * i));
    }

    // One 44.1 kHz tick of the capture buffers (CD-L @0x000, CD-R @0x400, voice1 @0x800, voice3 @0xC00, 512 samples
    // each) and the SPU IRQ check. Driven by the CPU loop every 768 cycles so the IRQ cadence matches hardware.
    void capture_tick(int cdl, int cdr) {
        uint16_t cnt = reg_[0xD5];
        if (!(cnt & 0x8000)) return;
        uint32_t o = cap_idx_ * 2;
        // The capture buffers receive the CD signal as it leaves the CD controller, regardless of the SPU's CD input
        // volume / enable bit. Vib-Ribbon relies on this: it silences the CD input and still reads the audio back to
        // analyze the music and build the ribbon.
        int l = cdl, r = cdr;
        put16(0x000 + o, l); put16(0x400 + o, r);
        put16(0x800 + o, voices_[1].phase != Off ? voice_out_[1] : 0);
        put16(0xC00 + o, voices_[3].phase != Off ? voice_out_[3] : 0);
        if ((cnt & 0x40) && !irq_flag_) for (uint32_t base = 0; base < 0x1000; base += 0x400) check_irq(base + o);
        cap_idx_ = (cap_idx_ + 1) & 0x1FF;
    }
    uint32_t dma_read_word() {   // SPU RAM -> RAM via DMA channel 4
        check_irq(xfer); check_irq(xfer + 2);
        uint32_t w = 0;
        for (int i = 0; i < 4; ++i) w |= (uint32_t)ram[(xfer++) & 0x7FFFF] << (8 * i);
        return w;
    }

    // Produce n stereo frames. cd may be null, or n interleaved stereo samples of CD audio.
    void mix(int n, int16_t* out, const int16_t* cd) {
        uint16_t cnt = reg_[0xD5];   // SPUCNT @ 0x1AA
        bool enabled = (cnt & 0x8000) != 0, unmuted = (cnt & 0x4000) != 0, cd_on = (cnt & 1) != 0;
        int mvl = vol(reg_[0xC0]), mvr = vol(reg_[0xC1]);
        int cvl = (int16_t)reg_[0xD8], cvr = (int16_t)reg_[0xD9];   // CD input volume @ 0x1B0/0x1B2
        for (int i = 0; i < n; ++i) {
            int64_t l = 0, r = 0;
            if (enabled) {
                for (int v = 0; v < 24; ++v) {
                    Voice& vo = voices_[v];
                    if (vo.phase == Off) continue;
                    int s = voice_sample(v);
                    s = (s * vo.env) >> 15;
                    voice_out_[v] = (int16_t)std::clamp(s, -32768, 32767);
                    l += (s * vol(reg_[v * 8])) >> 15;
                    r += (s * vol(reg_[v * 8 + 1])) >> 15;
                }
                if (cd && cd_on) { l += (cd[2 * i] * cvl) >> 15; r += (cd[2 * i + 1] * cvr) >> 15; }
            }
            l = (l * mvl) >> 15; r = (r * mvr) >> 15;
            if (!unmuted) l = r = 0;
            out[2 * i] = (int16_t)std::clamp<int64_t>(l, -32768, 32767);
            out[2 * i + 1] = (int16_t)std::clamp<int64_t>(r, -32768, 32767);
        }
    }

private:
    enum Phase { Off, Attack, Decay, Sustain, Release };
    struct Voice {
        Phase phase = Off;
        uint32_t addr = 0, repeat = 0, counter = 0;
        int idx = 28;              // next sample within the decoded block
        int16_t buf[28] = {};
        int old1 = 0, old2 = 0, prev = 0, cur = 0;
        int env = 0, env_cnt = 1;
        uint8_t flags = 0;
    };
    uint16_t reg_[0x100] = {};   // halfword registers, index = (addr - 0x1F801C00) / 2
    Voice voices_[24];
    uint32_t endx_ = 0;
    bool irq_flag_ = false, irq_fired_ = false;
    int16_t voice_out_[24] = {};
    uint32_t cap_idx_ = 0;   // capture-buffer write pointer (0..511 samples; 4 buffers of 0x400 bytes at 0x000..0xFFF)

    void check_irq(uint32_t byte_addr) {
        if (!(reg_[0xD5] & 0x8000) || !(reg_[0xD5] & 0x40)) return;   // SPU enabled and IRQ enabled
        if (((byte_addr & 0x7FFFF) >> 3) != ((uint32_t)reg_[0xD2] >> 0)) return;   // IRQ address reg is in 8-byte units
        if (!irq_flag_) { irq_flag_ = true; irq_fired_ = true; }
    }
    void put16(uint32_t a, int v) { ram[a] = (uint8_t)v; ram[a + 1] = (uint8_t)(v >> 8); }

    static int vol(uint16_t r) { return (r & 0x8000) ? 0x3FFF : (int16_t)(r << 1); }   // sweep -> approx full
    uint16_t rd16(uint32_t off) const {
        uint32_t i = (off >> 1) & 0xFF;
        if (i < 0xC0 && (i & 7) == 6) return (uint16_t)std::min(voices_[i >> 3].env, 0x7FFF);   // current envelope (+0xC)
        if (i == 0xCE) return (uint16_t)endx_;
        if (i == 0xCF) return (uint16_t)(endx_ >> 16);
        if (i == 0xD7) return (uint16_t)((reg_[0xD5] & 0x3F) | (irq_flag_ ? 0x40 : 0));   // SPUSTAT: SPUCNT low bits + IRQ flag
        return reg_[i];
    }
    void write16(uint32_t off, uint16_t v) {
        uint32_t i = (off >> 1) & 0xFF;
        reg_[i] = v;
        switch (i) {
        case 0xC4: for (int b = 0; b < 16; ++b) if (v >> b & 1) key_on(b); break;          // KON lo
        case 0xC5: for (int b = 0; b < 8; ++b) if (v >> b & 1) key_on(16 + b); break;       // KON hi
        case 0xC6: for (int b = 0; b < 16; ++b) if (v >> b & 1) key_off(b); break;         // KOFF lo
        case 0xC7: for (int b = 0; b < 8; ++b) if (v >> b & 1) key_off(16 + b); break;     // KOFF hi
        case 0xD5: if (!(v & 0x40)) irq_flag_ = false; break;                               // SPUCNT: IRQ off = acknowledge
        case 0xD3: xfer = (uint32_t)v * 8; break;                                           // transfer address
        case 0xD4: check_irq(xfer); ram[(xfer++) & 0x7FFFF] = (uint8_t)v; ram[(xfer++) & 0x7FFFF] = (uint8_t)(v >> 8); break;  // FIFO
        default: if (i < 0xC0 && (i & 7) == 7) voices_[i >> 3].repeat = (uint32_t)v * 8; break;   // repeat address
        }
    }
    void key_on(int n) {
        Voice& v = voices_[n];
        v.addr = (uint32_t)reg_[n * 8 + 3] * 8; v.repeat = v.addr;
        v.counter = 0; v.idx = 28; v.old1 = v.old2 = v.prev = v.cur = 0;
        v.env = 0; v.env_cnt = 1; v.phase = Attack;
        endx_ &= ~(1u << n);
        decode_block(v, true);
        ++key_ons;
    }
    void key_off(int n) { if (voices_[n].phase != Off) { voices_[n].phase = Release; voices_[n].env_cnt = 1; } }

    void decode_block(Voice& v, bool first) {
        (void)first;
        uint32_t a = v.addr & 0x7FFFF;
        for (uint32_t k = 0; k < 16; k += 8) check_irq(a + k);   // block fetch touches the IRQ address?
        uint8_t hdr = ram[a]; v.flags = ram[(a + 1) & 0x7FFFF];
        int shift = hdr & 15, filter = std::min((hdr >> 4) & 7, 4);
        if (shift > 12) shift = 9;
        if (v.flags & 4) v.repeat = v.addr;
        static const int pos[5] = {0, 60, 115, 98, 122}, neg[5] = {0, 0, -52, -55, -60};
        for (int k = 0; k < 14; ++k) {
            uint8_t b = ram[(a + 2 + k) & 0x7FFFF];
            for (int h = 0; h < 2; ++h) {
                int nib = h ? b >> 4 : b & 15;
                int s = ((int16_t)(nib << 12)) >> shift;
                s += (pos[filter] * v.old1 + neg[filter] * v.old2 + 32) >> 6;
                s = std::clamp(s, -32768, 32767);
                v.old2 = v.old1; v.old1 = s;
                v.buf[2 * k + h] = (int16_t)s;
            }
        }
        v.idx = 0;
    }
    void next_block(int n) {
        Voice& v = voices_[n];
        if (v.flags & 1) {   // end flag
            endx_ |= 1u << n;
            if (v.flags & 2) v.addr = v.repeat;
            else { v.phase = Off; v.env = 0; return; }
        } else v.addr += 16;
        decode_block(v, false);
    }
    int voice_sample(int n) {
        Voice& v = voices_[n];
        uint32_t pitch = std::min<uint32_t>(reg_[n * 8 + 2], 0x3FFF);
        v.counter += pitch;
        while (v.counter >= 0x1000 && v.phase != Off) {
            v.counter -= 0x1000;
            v.prev = v.cur; v.cur = v.buf[v.idx++];
            if (v.idx >= 28) next_block(n);
        }
        step_env(v, reg_[n * 8 + 4], reg_[n * 8 + 5]);
        return v.prev + (((v.cur - v.prev) * (int)(v.counter & 0xFFF)) >> 12);
    }
    void step_env(Voice& v, uint16_t a1, uint16_t a2) {
        if (v.phase == Off) return;
        if (--v.env_cnt > 0) return;
        int shift, step; bool dec, ex;
        switch (v.phase) {
        case Attack:  shift = (a1 >> 10) & 31; step = 7 - ((a1 >> 8) & 3); dec = false; ex = (a1 >> 15) & 1; break;
        case Decay:   shift = (a1 >> 4) & 15; step = -8; dec = true; ex = true; break;
        case Sustain: shift = (a2 >> 8) & 31; step = (a2 >> 6) & 3; dec = (a2 >> 14) & 1; step = dec ? step - 8 : 7 - step; ex = (a2 >> 15) & 1; break;
        default:      shift = a2 & 31; step = -8; dec = true; ex = (a2 >> 5) & 1; break;
        }
        int wait = 1 << std::max(0, shift - 11);
        int st = step * (1 << std::max(0, 11 - shift));
        if (ex && !dec && v.env > 0x6000) wait *= 4;
        if (ex && dec) st = (st * v.env) >> 15;
        v.env_cnt = wait;
        v.env = std::clamp(v.env + st, 0, 0x7FFF);
        if (v.phase == Attack && v.env >= 0x7FFF) v.phase = Decay;
        else if (v.phase == Decay && v.env <= ((a1 & 15) + 1) * 0x800) v.phase = Sustain;
        else if (v.phase == Release && v.env == 0) v.phase = Off;
    }
};

}  // namespace hw
