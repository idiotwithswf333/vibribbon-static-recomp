// A virtual audio CD built from decoded music (MP3s): what the CD-ROM shows after the "lid" is swapped.
// SDL-free and decoder-free so it can be unit-tested; the MP3 decoding itself lives in mp3_audio.h.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace vcd {

constexpr int kFramesPerSector = 588;   // 588 stereo sample frames (2352 bytes) per CD-DA sector, 75 sectors a second
constexpr uint32_t kMinTrackSectors = 300;   // real discs need >= 4 s per track

// Interleaved PCM (1 or 2 channels at `hz`) -> interleaved stereo at 44100 Hz (cubic Hermite interpolation).
inline std::vector<int16_t> to_44k_stereo(const int16_t* in, size_t frames, int channels, int hz) {
    std::vector<int16_t> out;
    if (!in || frames == 0 || channels < 1 || hz < 1000) return out;
    auto at = [&](long i, int c) -> float {   // sample i of channel c, clamped at both ends
        i = std::clamp<long>(i, 0, (long)frames - 1);
        return (float)in[(size_t)i * (size_t)channels + (size_t)(channels > 1 ? c : 0)];
    };
    const size_t n = hz == 44100 ? frames : (size_t)((double)frames * 44100.0 / (double)hz);
    out.resize(n * 2);
    for (size_t i = 0; i < n; ++i) {
        const double pos = hz == 44100 ? (double)i : (double)i * (double)hz / 44100.0;
        const long i0 = (long)pos; const float t = (float)(pos - (double)i0);
        for (int c = 0; c < 2; ++c) {
            const float p0 = at(i0 - 1, c), p1 = at(i0, c), p2 = at(i0 + 1, c), p3 = at(i0 + 2, c);
            float v = p1 + 0.5f * t * (p2 - p0 + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + t * (3.0f * (p1 - p2) + p3 - p0)));
            out[i * 2 + (size_t)c] = (int16_t)std::clamp(v, -32768.0f, 32767.0f);
        }
    }
    return out;
}

struct Disc {
    std::vector<int16_t> pcm;          // every track back to back, stereo 44.1 kHz, each padded to whole sectors
    std::vector<uint32_t> starts;      // first sector (LBA, 0 = 2 s into the disc) of each track
    std::vector<std::string> names;
    uint32_t sectors = 0;              // total length

    // `stereo44k` as returned by to_44k_stereo(). Empty audio is ignored.
    bool add_track(const std::vector<int16_t>& stereo44k, const std::string& name = "") {
        if (stereo44k.size() < 2) return false;
        const size_t frames = stereo44k.size() / 2;
        const uint32_t secs = std::max<uint32_t>(kMinTrackSectors, (uint32_t)((frames + kFramesPerSector - 1) / kFramesPerSector));
        starts.push_back(sectors); names.push_back(name);
        pcm.insert(pcm.end(), stereo44k.begin(), stereo44k.begin() + (long)(frames * 2));
        pcm.resize((size_t)(sectors + secs) * kFramesPerSector * 2, 0);
        sectors += secs;
        return true;
    }
    // One raw 2352-byte audio sector. False past the end of the disc (the lead-out).
    bool sector(uint32_t lba, uint8_t* out) const {
        if (lba >= sectors) return false;
        std::memcpy(out, &pcm[(size_t)lba * kFramesPerSector * 2], (size_t)kFramesPerSector * 4);
        return true;
    }
    size_t tracks() const { return starts.size(); }
};

}  // namespace vcd
