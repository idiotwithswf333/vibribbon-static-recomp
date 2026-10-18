// cdrom_swap_test -- the "own CD" feature without any game files: MP3 resampling, the virtual audio CD, and how the CD-ROM
// controller behaves while the lid is swapped (ShellOpen flag, GetTN / GetTD / GetID, Play of a virtual track).
#include <cmath>
#include <cstdio>
#include "../src/cdrom.h"
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

struct Drive {
    hw::CdRom cd; uint64_t t = 1000;
    void run(uint64_t cycles) { for (uint64_t e = t + cycles; t < e; t += 500) cd.tick(t); }
    // Sends a command and returns {INT number, response bytes}. Runs until the response arrives.
    std::pair<int, std::vector<uint8_t>> cmd(uint8_t c, std::vector<uint8_t> params = {}, bool second = false) {
        if (!second) { cd.write8(0, 0); for (uint8_t p : params) cd.write8(2, p); cd.write8(1, c); }
        for (int i = 0; i < 400000 && !(cd.read8(0) & 0x20); ++i) { t += 500; cd.tick(t); }
        cd.write8(0, 1); int irq = cd.read8(3) & 7; cd.write8(0, 0);
        std::vector<uint8_t> r; while (cd.read8(0) & 0x20) r.push_back(cd.read8(1));
        cd.write8(0, 1); cd.write8(3, 0x1F); cd.write8(0, 0);   // acknowledge
        return {irq, r};
    }
    uint8_t stat() { return cmd(0x01).second.at(0); }
};
static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

int main() {
    // ---- resampling: a 1 kHz tone at 48 kHz mono keeps its pitch and loudness at 44.1 kHz stereo ----
    {
        std::vector<int16_t> mono(48000);
        for (size_t i = 0; i < mono.size(); ++i) mono[i] = (int16_t)(12000 * std::sin(2 * 3.14159265 * 1000.0 * (double)i / 48000.0));
        auto st = vcd::to_44k_stereo(mono.data(), mono.size(), 1, 48000);
        CHECK(st.size() == 2 * 44100);
        double sum = 0; int zc = 0; for (size_t i = 1000; i < 40000; ++i) { sum += (double)st[2 * i] * st[2 * i]; if ((st[2 * i] < 0) != (st[2 * (i - 1)] < 0)) ++zc; }
        const double rms = std::sqrt(sum / 39000.0); CHECK(rms > 8200 && rms < 8700);                 // 12000 / sqrt(2) = 8485
        CHECK(std::abs(zc - 2 * 1000 * 39000 / 44100) <= 3);                                          // ~1768 zero crossings for 1 kHz
        CHECK(st[2 * 500] == st[2 * 500 + 1]);                                                        // mono -> both channels
        auto same = vcd::to_44k_stereo(mono.data(), 100, 1, 44100); CHECK(same.size() == 200 && same[20] == mono[10]);
        CHECK(vcd::to_44k_stereo(nullptr, 0, 2, 44100).empty());
    }
    // ---- the virtual disc: tracks are padded to whole sectors (min 4 s) and start one after another ----
    auto disc = std::make_shared<vcd::Disc>();
    {
        std::vector<int16_t> a(2 * 44100 * 6), b(2 * 44100 * 2);            // 6 s and 2 s (padded to 4 s)
        for (size_t i = 0; i < a.size(); ++i) a[i] = (int16_t)(1000 + (int)(i % 997));
        for (size_t i = 0; i < b.size(); ++i) b[i] = (int16_t)(-2000 - (int)(i % 991));
        CHECK(disc->add_track(a, "a") && disc->add_track(b, "b") && !disc->add_track({}, "empty"));
        CHECK(disc->tracks() == 2 && disc->starts[0] == 0 && disc->starts[1] == 450 && disc->sectors == 450 + 300);
        uint8_t sec[2352]; CHECK(disc->sector(0, sec) && sec[0] == (a[0] & 255) && sec[1] == (a[0] >> 8 & 255));
        CHECK(!disc->sector(750, sec));
    }
    // ---- CD-ROM: before the swap it behaves like a plain drive ----
    Drive d; d.cd.vcd = nullptr;
    CHECK(d.stat() == 0x02 && !d.cd.own_cd());
    // ---- lid open: ShellOpen set, motor off ----
    d.cd.request_swap(disc);
    d.run(1000);
    CHECK(d.cd.lid_open() && (d.stat() & 0x10) && !(d.stat() & 0x02));
    d.run(30000000); CHECK(d.cd.lid_open());                                   // still open after ~0.9 s
    d.run(15000000);                                                           // closed after ~1.2 s
    CHECK(!d.cd.lid_open() && d.cd.own_cd() && !d.cd.swap_busy());
    { uint8_t s1 = d.stat(), s2 = d.stat(); CHECK((s1 & 0x10) && (s1 & 0x02)); CHECK(s2 == 0x02); }   // "lid was opened" is reported once
    // ---- TOC ----
    { auto r = d.cmd(0x13); CHECK(r.first == 3 && r.second.size() == 3 && r.second[1] == 0x01 && r.second[2] == 0x02); }
    { auto r = d.cmd(0x14, {bcd(1)}); CHECK(r.second.size() == 3 && r.second[1] == 0x00 && r.second[2] == 0x02); }          // track 1 starts at 00:02
    { auto r = d.cmd(0x14, {bcd(2)}); CHECK(r.second[1] == 0x00 && r.second[2] == 0x08); }                                // 450 sectors = 6 s later: 00:08
    { auto r = d.cmd(0x14, {bcd(3)}); CHECK(r.second[1] == 0x00 && r.second[2] == 0x12); }                                // lead-out: 750 sectors = 10 s + 2 s
    // ---- GetID: an audio CD, not a game disc ----
    { auto r1 = d.cmd(0x1A); auto r2 = d.cmd(0x1A, {}, true); CHECK(r1.first == 3 && r2.first == 5 && r2.second.size() == 8 && r2.second[0] == 0x0A && r2.second[1] == 0x90); }
    // ---- Play track 2: the audio comes from the second MP3 ----
    d.cmd(0x03, {bcd(2)});
    { int16_t buf[2 * 1000]; d.cd.audio_samples(1000, buf);
      const size_t off = (size_t)disc->starts[1] * vcd::kFramesPerSector * 2;
      CHECK(buf[0] == disc->pcm[off] && buf[1] == disc->pcm[off + 1] && buf[1998] == disc->pcm[off + 1998]); CHECK(d.cd.playing); }
    { auto r = d.cmd(0x11); CHECK(r.second.size() == 8 && r.second[0] == 0x02); }                                         // GetlocP: track 2
    // ---- played to the end: the drive stops at the lead-out ----
    d.cmd(0x03, {bcd(2)}); { int16_t big[2 * 588]; for (int i = 0; i < 320; ++i) d.cd.audio_samples(588, big); } CHECK(!d.cd.playing);
    // ---- swapping back to the game disc ----
    d.cd.request_swap(nullptr); d.run(1000); CHECK(d.cd.lid_open() && (d.stat() & 0x10)); d.run(45000000);
    CHECK(!d.cd.lid_open() && !d.cd.own_cd());
    { d.stat(); auto r1 = d.cmd(0x1A); auto r2 = d.cmd(0x1A, {}, true); CHECK(r2.first == 2 && r2.second[0] == 0x02 && r2.second[4] == 'S'); }   // licensed game disc again
    if (failures == 0) std::printf("cdrom_swap: all tests passed\n");
    return failures ? 1 : 0;
}
