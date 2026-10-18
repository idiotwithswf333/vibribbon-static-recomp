// PS1 CD-ROM controller (registers 0x1F801800-03). Instruction-count timing.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <algorithm>
#include <deque>
#include <functional>
#include <vector>
#include <memory>
#include <map>
#include "disc.h"
#include "vcd.h"

namespace hw {

class CdRom {
public:
    disc::Image* img = nullptr;
    std::vector<uint8_t> data_buf;        // current sector payload
    size_t data_pos = 0;
    bool drq = false;
    uint64_t cmds = 0, sectors_read = 0, play_cmds = 0;
    bool playing = false, muted = false;
    uint32_t dbg_loc() const { return loc; }
    uint32_t dbg_da_lba() const { return da_lba; }
    int dbg_cdvol(int i) const { return cdvol[i]; }
    uint8_t dbg_mode() const { return mode; }
    std::vector<std::string> cmd_hist;   // every command with timestamp, for diagnostics
    std::vector<uint32_t> setloc_hist;   // every Setloc target (LBA), for diagnostics
    std::vector<std::string> dbg_log;   // first commands, for diagnostics

    // ---- "own CD": swapping the disc in the drive for a virtual audio CD (the user's MP3s) ----
    // The lid opens now (Getstat shows ShellOpen, motor off), stays open ~1.2 s, then closes with `d` inside (nullptr =
    // the original game disc again). While `vcd` is set the drive is an audio-only disc: GetTN / GetTD / GetlocP /
    // Play use its tracks and GetID answers "audio CD", exactly what a real drive reports for a music CD.
    std::shared_ptr<const vcd::Disc> vcd;
    void request_swap(std::shared_ptr<const vcd::Disc> d) { swap_to_ = std::move(d); swap_pending_ = true; swap_phase_ = 0; cmd_counts.clear(); }
    // Diagnostics: while trace_left > 0 every command the game sends and every answer it gets is passed to on_trace.
    std::function<void(const std::string&)> on_trace; int trace_left = 0;
    void trace_line(const std::string& m) {   // commands: skipped if the same as one of the last two; answers: skipped for those, and INT1 (sector ready)
        const bool is_cmd = m.compare(0, 3, "cmd") == 0;
        if (is_cmd) {
            if (m == last_cmd_ || m == prev_cmd_) { skip_ans_ = true; ++trace_rep_; return; }
            if (trace_rep_) { on_trace("   (" + std::to_string(trace_rep_) + " repeated commands/answers left out)"); trace_rep_ = 0; }
            prev_cmd_ = last_cmd_; last_cmd_ = m; skip_ans_ = false;
        } else if (skip_ans_ || m.compare(0, 11, "answer INT1") == 0) return;
        --trace_left; on_trace(m);
    }
    // What the drive does when the game READS (ReadN/ReadS) sectors of an audio-only disc (the own CD): 0 = hands the audio bytes over as
    // data (as a data disc would), 1 = skips them silently (no INT1, no data), 2 = answers the read command with an error (INT5).
    int audio_read_mode = 0;
    std::map<int, int> cmd_counts;   // commands the game sent since the last swap (code -> count), for the log
    std::string cmd_summary() const { std::string r; char b[24]; for (auto& kv : cmd_counts) { std::snprintf(b, sizeof b, "%02X:%d ", kv.first, kv.second); r += b; } return r; }
    bool lid_open() const { return lid_open_; }
    bool swap_busy() const { return swap_pending_; }
    bool own_cd() const { return vcd != nullptr; }

    uint8_t read8(uint32_t reg) {
        switch (reg) {
        case 0: {
            uint8_t s = index;
            if (params.empty()) s |= 0x08;
            if (params.size() < 16) s |= 0x10;
            if (!resp.empty()) s |= 0x20;
            if (drq && data_pos < data_buf.size()) s |= 0x40;
            if (busy_until > now_) s |= 0x80;
            return s;
        }
        case 1: { if (resp.empty()) return 0; uint8_t v = resp.front(); resp.pop_front(); return v; }
        case 2: return data_byte();
        default: return (index & 1) ? (ifr | 0xE0) : (ier | 0xE0);
        }
    }
    void write8(uint32_t reg, uint8_t v) {
        if (reg == 0) { index = v & 3; return; }
        if (index == 0) {
            if (reg == 1) command(v);
            else if (reg == 2) params.push_back(v);
            else { if ((v & 0x80) && !drq) data_pos = 0; drq = (v & 0x80) != 0; if (dbg_log.size() < 60) { char b[64]; std::snprintf(b, sizeof b, "req=%02X pos=%zu size=%zu", v, data_pos, data_buf.size()); dbg_log.push_back(b); } }
        } else if (index == 2) {
            if (reg == 2) cdvol[0] = v; else if (reg == 3) cdvol[1] = v;     // L->L, L->R
        } else if (index == 3) {
            if (reg == 1) cdvol[3] = v; else if (reg == 2) cdvol[2] = v;     // R->R, R->L
        } else if (index == 1) {
            if (reg == 2) ier = v & 0x1F;
            else if (reg == 3) { ifr &= ~(v & 0x1F); if (v & 0x40) params.clear(); }
        }
    }
    // CD-DA: n stereo frames of 44.1 kHz audio (interleaved), after the CD volume matrix.
    void audio_samples(int n, int16_t* out) {
        for (int i = 0; i < n; ++i) {
            int l = 0, r = 0;
            // Note: Mute (cmd 0x0B) only silences the output. The disc keeps spinning and the read position
            // keeps advancing, so the game can poll GetlocP and Demute once it reaches the track start.
            // (Vib-Ribbon does exactly that; freezing the position while muted deadlocks its loading screen.)
            if (playing) {
                if (!da_loaded) {
                    uint32_t sz = 0; da_sec.assign(2352, 0);
                    if (vcd) { if (!vcd->sector(da_lba, da_sec.data())) playing = false; }   // past the last track = lead-out: the drive stops
                    else if (!img || !img->raw_sector(da_lba, da_sec.data(), &sz) || sz != 2352) { playing = false; }
                    da_loaded = true; da_off = 0;
                }
                if (playing) {
                    if (!muted) {
                        const uint8_t* p = &da_sec[da_off * 4];
                        int L = (int16_t)(p[0] | p[1] << 8), R = (int16_t)(p[2] | p[3] << 8);
                        l = (L * cdvol[0] + R * cdvol[2]) >> 7; r = (R * cdvol[3] + L * cdvol[1]) >> 7;
                    }
                    if (++da_off >= 588) { da_off = 0; ++da_lba; da_loaded = false; }
                }
            }
            out[2 * i] = (int16_t)std::clamp(l, -32768, 32767); out[2 * i + 1] = (int16_t)std::clamp(r, -32768, 32767);
        }
    }
    uint8_t data_byte() { if (data_pos < data_buf.size()) { ++data_bytes_read_; return data_buf[data_pos++]; } return 0; }
    uint32_t data_word() { uint32_t w = 0; for (int i = 0; i < 4; ++i) w |= (uint32_t)data_byte() << (8 * i); return w; }

    // Returns true when I_STAT bit 2 should be raised.
    bool tick(uint64_t now) {
        now_ = now;
        if (swap_pending_) {
            if (swap_phase_ == 0) {   // lid opens: spinning stops, reading/playing is cut off
                lid_open_ = true; shell_flag_ = true; playing = false; reading = false;
                q.erase(std::remove_if(q.begin(), q.end(), [](const Ev& e) { return e.sector; }), q.end());
                swap_t_ = now + kLidOpenCycles; swap_phase_ = 1;
            } else if (now >= swap_t_) {   // lid closes on the new disc
                lid_open_ = false; vcd = swap_to_; swap_to_.reset(); swap_pending_ = false;
                loc = 0; da_lba = 0; da_loaded = false; data_buf.clear(); data_pos = 0;
            }
        }
        if (q.empty() || now < q.front().t || ifr != 0) return false;
        Ev e = q.front(); q.erase(q.begin());
        if (e.sector) {
            if (!reading) return false;
            if (vcd && loc >= vcd->sectors) {   // read past the end of the music CD: the drive stops at the lead-out (INT4 "data end")
                reading = false; e.resp = {0x02}; e.irq = 4;
                if (on_trace && trace_left > 0) { --trace_left; on_trace("reading reached the lead-out (end of the own CD)"); }
            } else if (vcd && audio_read_mode == 1) {   // audio sector read as data: the drive just moves on, nothing is delivered
                ++loc; schedule_sector(now); return false;
            } else {
                load_sector();
                e.resp = {stat_reading()}; e.irq = 1;
                schedule_sector(now);
                if (on_trace && trace_left > 0 && (sectors_read % 2000) == 0) { --trace_left; on_trace("reading: next sector " + std::to_string(loc) + ", game has taken " + std::to_string(data_bytes_read_) + " data bytes so far"); }
            }
        }
        if (on_trace && trace_left > 0) {
            char b[120]; int n = std::snprintf(b, sizeof b, "answer INT%d:", e.irq);
            for (size_t i = 0; i < e.resp.size() && i < 8 && n < 100; ++i) n += std::snprintf(b + n, sizeof b - n, " %02X", e.resp[i]);
            trace_line(b);
        }
        resp.assign(e.resp.begin(), e.resp.end());
        ifr = (uint8_t)e.irq;
        return (ier & ifr) != 0;
    }

private:
    struct Ev { uint64_t t; int irq; std::vector<uint8_t> resp; bool sector = false; };
    std::vector<Ev> q;
    uint8_t index = 0, ier = 0x1F, ifr = 0, mode = 0;   // BIOS leaves all CD interrupts enabled
    std::deque<uint8_t> params, resp;
    uint64_t now_ = 0, busy_until = 0;
    uint32_t loc = 0;           // next sector index in the .bin (MSF - 150)
    bool reading = false;
    std::vector<uint8_t> da_sec; int da_off = 0; bool da_loaded = false; uint32_t da_lba = 0;
    int cdvol[4] = {0x80, 0, 0, 0x80};   // BIOS default: L->L, L->R, R->L, R->R

    static constexpr uint64_t kFirst = 15000, kSecond = 30000, kSector = 90000;   // ~375 sectors/s: faster than a real 2x drive (150/s) but slower than the real kernel IRQ path

    std::string last_cmd_, prev_cmd_; int trace_rep_ = 0; bool skip_ans_ = false; uint64_t data_bytes_read_ = 0;
    std::shared_ptr<const vcd::Disc> swap_to_; bool swap_pending_ = false, lid_open_ = false, shell_flag_ = false; int swap_phase_ = 0; uint64_t swap_t_ = 0;
    static constexpr uint64_t kLidOpenCycles = 40000000;   // ~1.2 s of emulated time with the lid open
    // Status byte: bit1 motor on, bit7 playing, bit4 ShellOpen (lid open now, or opened since the last Getstat).
    uint8_t stat() const { return (uint8_t)((lid_open_ ? 0x00 : (playing ? 0x82 : 0x02)) | (shell_flag_ ? 0x10 : 0x00)); }
    const std::vector<uint32_t>& tracks() const { static const std::vector<uint32_t> none; return vcd ? vcd->starts : (img ? img->track_starts() : none); }
    uint32_t total_sectors() const { return vcd ? vcd->sectors : (img ? img->total_sectors() : 0); }
    uint8_t stat_reading() const { return 0x22; }
    static uint8_t bcd(unsigned v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
    static unsigned unbcd(uint8_t b) { return (b >> 4) * 10 + (b & 15); }
    void push(uint64_t t, int irq, std::vector<uint8_t> r) {
        Ev e{t, irq, std::move(r)};
        auto it = q.begin(); while (it != q.end() && it->t <= t) ++it;
        q.insert(it, std::move(e));
    }
    void schedule_sector(uint64_t t) {
        Ev e{t + kSector, 1, {}}; e.sector = true;
        auto it = q.begin(); while (it != q.end() && it->t <= e.t) ++it;
        q.insert(it, std::move(e));
    }
    void load_sector() {
        uint8_t raw[2352]; uint32_t sz = 0;
        data_buf.clear(); data_pos = 0;
        if (vcd) { data_buf.assign(2048, 0); if (vcd->sector(loc, raw)) data_buf.assign(raw, raw + 2048); }   // audio disc: no files, just raw audio bytes
        else if (img && img->raw_sector(loc, raw, &sz)) {
            if (sz == 2352) {
                if (mode & 0x20) data_buf.assign(raw + 12, raw + 12 + 2340);
                else data_buf.assign(raw + img->data_offset(), raw + img->data_offset() + 2048);
            } else data_buf.assign(raw, raw + 2048);
        } else data_buf.assign(2048, 0);
        ++loc; ++sectors_read;
    }
    void command(uint8_t c) {
        ++cmds;
        ++cmd_counts[c];
        if (cmd_hist.size() < 100000) { char b[80]; std::snprintf(b, sizeof b, "%llu cmd %02X np=%zu p=%02X %02X %02X", (unsigned long long)now_, c, params.size(), params.size()>0?params[0]:0, params.size()>1?params[1]:0, params.size()>2?params[2]:0); cmd_hist.push_back(b); }
        if (dbg_log.size() < 40) { char b[96]; std::snprintf(b, sizeof b, "cmd %02X params=%zu p0=%02X p1=%02X p2=%02X", c, params.size(), params.size()>0?params[0]:0, params.size()>1?params[1]:0, params.size()>2?params[2]:0); dbg_log.push_back(b); }
        std::vector<uint8_t> p(params.begin(), params.end());
        if (on_trace && trace_left > 0) {
            char b[120]; int n = std::snprintf(b, sizeof b, "cmd %02X (lid %s, %s):", c, lid_open_ ? "open" : "closed", vcd ? "own CD" : "game disc");
            for (size_t i = 0; i < p.size() && i < 6; ++i) n += std::snprintf(b + n, sizeof b - n, " %02X", p[i]);
            trace_line(b);
        }
        params.clear(); resp.clear();
        uint64_t t = now_ + kFirst;
        busy_until = t;
        if (lid_open_) {   // door open: like a real drive, anything that needs a disc fails (INT5, error bit + ShellOpen, "not ready")
            switch (c) {
            case 0x03: case 0x06: case 0x1B: case 0x11: case 0x13: case 0x14: case 0x15: case 0x16: case 0x1E:
            if (vcd && audio_read_mode == 2) { push(t, 5, {(uint8_t)(stat() | 1), 0x80}); break; }   // an audio disc cannot be read as data
                push(t, 5, {(uint8_t)(stat() | 1), 0x80}); return;
            default: break;
            }
        }
        switch (c) {
        case 0x01: push(t, 3, {stat()}); if (!lid_open_) shell_flag_ = false; break;   // Getstat (reading it after the lid closed clears ShellOpen)
        case 0x02: if (p.size() >= 3) {                                          // Setloc
                       unsigned f = (unbcd(p[0]) * 60 + unbcd(p[1])) * 75 + unbcd(p[2]);
                       loc = f >= 150 ? f - 150 : 0;
                       if (setloc_hist.size() < 100000) setloc_hist.push_back(loc);
                   }
                   push(t, 3, {stat()}); break;
        case 0x06: case 0x1B:                                                    // ReadN / ReadS
            reading = true;
            push(t, 3, {stat()});
            { Ev e{t + kSector, 1, {}}; e.sector = true;
              auto it = q.begin(); while (it != q.end() && it->t <= e.t) ++it; q.insert(it, std::move(e)); }
            break;
        case 0x09: playing = false; reading = false; q.erase(std::remove_if(q.begin(), q.end(), [](const Ev& e){ return e.sector; }), q.end());
                   push(t, 3, {stat_reading()}); push(t + kSecond, 2, {stat()}); break;   // Pause
        case 0x08: playing = false; reading = false; push(t, 3, {stat()}); push(t + kSecond, 2, {0x00}); break;  // Stop
        case 0x0A: mode = 0x20; playing = false; reading = false; push(t, 3, {stat()}); push(t + kSecond, 2, {stat()}); break;  // Init
        case 0x07: push(t, 3, {stat()}); push(t + kSecond, 2, {stat()}); break;  // MotorOn
        case 0x0B: muted = true; push(t, 3, {stat()}); break;                  // Mute
        case 0x0C: muted = false; push(t, 3, {stat()}); break;                 // Demute
        case 0x0D: push(t, 3, {stat()}); break;                                // Setfilter
        case 0x03: {                                                           // Play (CD-DA)
            ++play_cmds;
            unsigned tr = p.empty() ? 0 : unbcd(p[0]);
            if (tr >= 1 && tr <= tracks().size()) da_lba = tracks()[tr - 1]; else da_lba = loc;
            da_off = 0; da_loaded = false; playing = true; reading = false;
            q.erase(std::remove_if(q.begin(), q.end(), [](const Ev& e){ return e.sector; }), q.end());
            push(t, 3, {stat()}); break; }
        case 0x11: {                                                           // GetlocP
            uint32_t lba = playing ? da_lba : loc, tr = 1;
            { auto& ts = tracks(); for (size_t k = 0; k < ts.size(); ++k) if (ts[k] <= lba) tr = (uint32_t)k + 1; }
            uint32_t base = (tr >= 1 && tr <= tracks().size()) ? tracks()[tr - 1] : 0;
            uint32_t rel = lba >= base ? lba - base : 0, ab = lba + 150;
            push(t, 3, {bcd(tr), 0x01, bcd(rel / 75 / 60), bcd(rel / 75 % 60), bcd(rel % 75), bcd(ab / 75 / 60), bcd(ab / 75 % 60), bcd(ab % 75)});
            break; }
        case 0x0E: if (!p.empty()) mode = p[0]; push(t, 3, {stat()}); break;     // Setmode
        case 0x13: push(t, 3, {stat(), 0x01, bcd((unsigned)std::max<size_t>(1, tracks().size()))}); break;  // GetTN
        case 0x14: {                                                              // GetTD
            unsigned n = p.empty() ? 0 : unbcd(p[0]); uint32_t fr = 0;
            { auto& ts = tracks(); if (n >= 1 && n <= ts.size()) fr = ts[n - 1]; else fr = total_sectors(); }
            fr += 150;
            push(t, 3, {stat(), bcd(fr / 75 / 60), bcd(fr / 75 % 60)}); break; }
        case 0x15: case 0x16: push(t, 3, {stat()}); push(t + kSecond, 2, {stat()}); break;   // SeekL / SeekP
        case 0x19:                                                                // Test
            if (!p.empty() && p[0] == 0x20) push(t, 3, {0x94, 0x09, 0x19, 0xC0});
            else push(t, 5, {0x11, 0x40});
            break;
        case 0x1A: push(t, 3, {stat()});                                          // GetID
                   if (lid_open_) push(t + kSecond, 5, {0x08, 0x40, 0, 0, 0, 0, 0, 0});          // lid open: no disc
                   else if (vcd) push(t + kSecond, 5, {0x0A, 0x90, 0, 0, 0, 0, 0, 0});          // a music CD: not a game disc ("audio CD" answer)
                   else push(t + kSecond, 2, {0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'E'});    // licensed Mode2 disc, SCEE region
                   break;
        case 0x1E: push(t, 3, {stat()}); push(t + kSecond * 3, 2, {stat()}); break;  // ReadTOC
        default: push(t, 5, {(uint8_t)(stat() | 1), 0x40}); break;                // unknown: error
        }
    }
};

}  // namespace hw
