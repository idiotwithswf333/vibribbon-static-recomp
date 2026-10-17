// Unit test for the neon glow post-process (no game files needed).
#include <cstdio>
#include "glow.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

int main() {
    const int w = 512, h = 256;
    std::vector<uint32_t> img((size_t)w * h, 0xFF000000u);
    for (int x = 200; x < 300; ++x) img[(size_t)128 * w + x] = 0xFFFFFFFFu;   // one white horizontal line
    auto before = img;
    glow::apply(img, w, h, 0.6f);
    CHECK(img[(size_t)128 * w + 250] == 0xFFFFFFFFu);                         // the line itself stays pure white
    CHECK((img[(size_t)134 * w + 250] & 255) > 0);                            // a halo appears next to it
    CHECK((img[(size_t)134 * w + 250] & 255) > (img[(size_t)145 * w + 250] & 255));   // fading with distance
    CHECK(img[(size_t)10 * w + 10] == 0xFF000000u);                           // far away stays black
    for (size_t i = 0; i < img.size(); ++i) { CHECK((img[i] >> 24) == 0xFF); if (fails) break; }   // alpha untouched
    auto off = before; glow::apply(off, w, h, 0.0f); CHECK(off == before);    // strength 0 = no change
    std::vector<uint32_t> tiny(16, 0xFF123456u), t2 = tiny; glow::apply(t2, 4, 4); CHECK(t2 == tiny);   // too small: untouched
    std::vector<uint32_t> big((size_t)2048 * 1024, 0xFF000000u); for (int x = 900; x < 1100; ++x) for (int y = 510; y < 514; ++y) big[(size_t)y * 2048 + x] = 0xFFFFFFFFu;   // a 4 px thick line
    glow::apply(big, 2048, 1024); CHECK((big[(size_t)520 * 2048 + 1000] & 255) > 0);   // 4x size works too
    if (fails) return 1;
    std::printf("glow: all tests passed\n");
    return 0;
}
