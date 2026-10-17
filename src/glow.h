// Neon glow: a cheap bloom post-process for the finished picture (ABGR8888 pixels, as Gpu::display() returns them).
// Vib-Ribbon is bright line art on black, so a soft halo around the lines looks great. SDL-free so it can be unit tested.
// Steps: average the picture down into blocks, blur that small image, scale it back up smoothly and add it on top.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace glow {

// strength 0..1 (0.6 is a nice default). Does nothing for tiny images or strength <= 0. Alpha is left alone.
inline void apply(std::vector<uint32_t>& px, int w, int h, float strength = 0.6f) {
    if (strength <= 0.0f || w < 32 || h < 32 || px.size() < (size_t)w * (size_t)h) return;
    const int B = std::max(4, w / 128);                       // block size: 4 px at 512 wide, 16 px at 2048 wide (the halo is soft anyway)
    const int St = std::max(1, B / 4);
    const int sw = (w + B - 1) / B, sh = (h + B - 1) / B;
    std::vector<float> a((size_t)sw * sh * 3, 0.0f), t(a.size());

    for (int by = 0; by < sh; ++by)                            // 1. box downsample
        for (int bx = 0; bx < sw; ++bx) {
            float s[3] = {0, 0, 0}; int n = 0;
            for (int y = by * B; y < std::min(h, by * B + B); y += St)       // St > 1 samples a grid inside big blocks (faster at 4x)
                for (int x = bx * B; x < std::min(w, bx * B + B); x += St) {
                    uint32_t p = px[(size_t)y * w + x];
                    s[0] += (float)(p & 255); s[1] += (float)((p >> 8) & 255); s[2] += (float)((p >> 16) & 255); ++n;
                }
            float* o = &a[((size_t)by * sw + bx) * 3];
            for (int c = 0; c < 3; ++c) o[c] = s[c] / (float)n;
        }

    static const float k[5] = {1 / 16.f, 4 / 16.f, 6 / 16.f, 4 / 16.f, 1 / 16.f};   // 2. separable blur, two rounds
    for (int round = 0; round < 2; ++round) {
        for (int y = 0; y < sh; ++y)
            for (int x = 0; x < sw; ++x)
                for (int c = 0; c < 3; ++c) {
                    float v = 0; for (int i = -2; i <= 2; ++i) v += k[i + 2] * a[((size_t)y * sw + std::clamp(x + i, 0, sw - 1)) * 3 + c];
                    t[((size_t)y * sw + x) * 3 + c] = v;
                }
        for (int y = 0; y < sh; ++y)
            for (int x = 0; x < sw; ++x)
                for (int c = 0; c < 3; ++c) {
                    float v = 0; for (int i = -2; i <= 2; ++i) v += k[i + 2] * t[((size_t)std::clamp(y + i, 0, sh - 1) * sw + x) * 3 + c];
                    a[((size_t)y * sw + x) * 3 + c] = v;
                }
    }

    const float gain = strength * 4.0f;                        // blurred lines are faint, so boost before adding
    std::vector<int> xi((size_t)w); std::vector<float> xw((size_t)w);   // per-column source cell and weight
    for (int x = 0; x < w; ++x) {
        const float fx = ((float)x + 0.5f) / B - 0.5f; const int x0 = (int)std::floor(fx);
        xi[(size_t)x] = std::clamp(x0, 0, sw - 1); xw[(size_t)x] = x0 < 0 ? 0.0f : (x0 >= sw - 1 ? 0.0f : fx - (float)x0);
    }
    std::vector<float> row((size_t)sw * 3);
    for (int y = 0; y < h; ++y) {                              // 3. bilinear upsample + additive blend
        const float fy = ((float)y + 0.5f) / B - 0.5f; const int y0 = (int)std::floor(fy);
        const float wy = (y0 < 0 || y0 >= sh - 1) ? 0.0f : fy - (float)y0;
        const int ya = std::clamp(y0, 0, sh - 1), yb = std::clamp(y0 + 1, 0, sh - 1);
        float rowmax = 0;
        for (int i = 0; i < sw * 3; ++i) {                     // blend the two source rows once per output row, with gain
            row[(size_t)i] = (a[(size_t)ya * sw * 3 + i] * (1 - wy) + a[(size_t)yb * sw * 3 + i] * wy) * gain;
            rowmax = std::max(rowmax, row[(size_t)i]);
        }
        if (rowmax < 1.0f) continue;                           // nothing to add on this row (adds are truncated to whole levels)
        uint32_t* dst = &px[(size_t)y * w];
        for (int x = 0; x < w; ++x) {
            const int xa = xi[(size_t)x], xb = std::min(xa + 1, sw - 1); const float wx = xw[(size_t)x];
            const float* r0 = &row[(size_t)xa * 3]; const float* r1 = &row[(size_t)xb * 3];
            if (r0[0] + r0[1] + r0[2] + r1[0] + r1[1] + r1[2] < 1.0f) continue;   // too faint to change any pixel
            const uint32_t p = dst[x]; uint32_t out = p & 0xFF000000u;
            for (int c = 0; c < 3; ++c) {
                const int v = std::min(255, (int)((p >> (8 * c)) & 255) + (int)(r0[c] + (r1[c] - r0[c]) * wx));
                out |= (uint32_t)v << (8 * c);
            }
            dst[x] = out;
        }
    }
}

}  // namespace glow
