// gpu_hires_test -- the upscaled (2x-4x) line mode of the GPU, plus vec.h's anti-aliased line.
#include <cstdio>
#include <map>
#include <set>
#include "../src/gpu.h"
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
static uint32_t xy(int x, int y) { return (uint32_t)(y & 0x7FF) << 16 | (uint32_t)(x & 0x7FF); }
static void setup(hw::Gpu& g) { g.gp1(0x08000000u | 2 | 8); g.gp1(0x03000000u); g.gp1(0x05000000u); }   // 512x256 PAL, display on, area at 0,0
static void line(hw::Gpu& g, int x0, int y0, int x1, int y1) { g.gp0(0x40FFFFFFu); g.gp0(xy(x0, y0)); g.gp0(xy(x1, y1)); }
static int lum(uint32_t p) { return p & 0xFF; }   // red channel (the game draws white, so all channels match)
int main() {
    // ---- vec::aa_line visits every pixel at most once, and stays inside the clip ----------------------------------------
    {
        std::map<std::pair<int, int>, int> seen; bool inside = true;
        vec::aa_line(5.3f, 7.1f, 40.6f, 22.9f, 1.5f, 0, 0, 50, 30, [&](int x, int y, float, float) { ++seen[{x, y}]; if (x < 0 || y < 0 || x >= 50 || y >= 30) inside = false; });
        bool once = true; for (auto& kv : seen) if (kv.second != 1) once = false;
        CHECK(!seen.empty() && once && inside);
        int n = 0; vec::aa_line(-20, 5, 80, 5, 1.0f, 10, 0, 20, 10, [&](int x, int, float, float) { if (x < 10 || x >= 20) n = -1000; else ++n; });
        CHECK(n > 0);   // clipped to the box
    }
    // ---- 1x: unchanged, no extra memory ----------------------------------------------------------------------------------
    {
        hw::Gpu g; setup(g); std::vector<uint32_t> px; int w, h;
        line(g, 10, 10, 60, 10); g.display(px, w, h);
        CHECK(w == 512 && h == 256 && g.scale() == 1);
        CHECK(lum(px[10 * 512 + 30]) > 200 && lum(px[11 * 512 + 30]) == 0 && lum(px[9 * 512 + 30]) == 0);   // 1 px, hard edges
    }
    // ---- 4x ----------------------------------------------------------------------------------------------------------------
    {
        hw::Gpu g; setup(g); g.set_scale(4); std::vector<uint32_t> px; int w, h;
        g.display(px, w, h);
        CHECK(w == 2048 && h == 1024 && g.scale() == 4);
        // horizontal line at native y=10 => centre at hi-res y=42; width 4 px => rows 40..43 solid, rows 39 and 44 empty
        line(g, 10, 10, 60, 10); g.display(px, w, h);
        CHECK(lum(px[41 * 2048 + 120]) > 250 && lum(px[42 * 2048 + 120]) > 250);
        CHECK(lum(px[37 * 2048 + 120]) == 0 && lum(px[46 * 2048 + 120]) == 0);
        // a diagonal line must contain intermediate (anti-aliased) values, not only 0 and 255
        line(g, 100, 100, 163, 131); g.display(px, w, h);
        std::set<int> levels; for (int y = 400; y < 524; ++y) for (int x = 400; x < 652; ++x) { int v = lum(px[(size_t)y * w + x]); if (v > 8 && v < 247) levels.insert(v); }
        CHECK(levels.size() >= 8);   // many distinct in-between greys = smooth edge
        // the native VRAM the game reads is untouched by the upscaler (still a 1-pixel line)
        CHECK((g.vram[10 * 1024 + 30] & 31) == 31 && (g.vram[11 * 1024 + 30] & 31) == 0);
        // frame copy keeps the smooth lines: copy rows 100..130 to rows 200..230
        g.gp0(0x80000000u); g.gp0(xy(100, 100)); g.gp0(xy(100, 200)); g.gp0(xy(70, 40));   // 70x40 block
        g.gp1(0x05000000u | (0) | (0 << 10));   // display area stays at 0,0
        g.display(px, w, h);
        std::set<int> lv2; for (int y = 800; y < 924; ++y) for (int x = 400; x < 652; ++x) { int v = lum(px[(size_t)y * w + x]); if (v > 8 && v < 247) lv2.insert(v); }
        CHECK(lv2.size() >= 8);
        // screen clear (GP0 02) empties the hi-res picture
        g.gp0(0x02000000u); g.gp0(xy(0, 0)); g.gp0(512 | (256u << 16)); g.display(px, w, h);
        bool all0 = true; for (size_t i = 0; i < px.size(); ++i) if (lum(px[i]) != 0) { all0 = false; break; }
        CHECK(all0);
        // CPU->VRAM upload of 2 white pixels at (20,20) becomes two 4x4 white blocks
        g.gp0(0xA0000000u); g.gp0(xy(20, 20)); g.gp0(2 | (1u << 16)); g.gp0(0x7FFF7FFFu); g.display(px, w, h);
        CHECK(lum(px[80 * 2048 + 80]) == 255 && lum(px[83 * 2048 + 87]) == 255 && lum(px[84 * 2048 + 80]) == 0);
        // semi-transparent line blends with what is below
        g.gp0(0x02404040u); g.gp0(xy(0, 0)); g.gp0(512 | (256u << 16));
        g.gp0(0xE1000000u | (1 << 5));   // semi mode 1: add
        g.gp0(0x42404040u); g.gp0(xy(10, 50)); g.gp0(xy(60, 50)); g.display(px, w, h);
        CHECK(lum(px[(50 * 4 + 1) * 2048 + 120]) > 0x40);
    }
    // ---- scale changes ---------------------------------------------------------------------------------------------------
    {
        hw::Gpu g; setup(g); line(g, 10, 10, 60, 10); g.set_scale(2);
        std::vector<uint32_t> px; int w, h; g.display(px, w, h);
        CHECK(w == 1024 && h == 512 && lum(px[20 * 1024 + 60]) > 200);   // earlier VRAM content carried over (as blocks)
        g.set_scale(1); g.display(px, w, h); CHECK(w == 512 && h == 256);
        g.set_scale(9); CHECK(g.scale() == 4); g.set_scale(0); CHECK(g.scale() == 1);
    }
    if (failures == 0) std::puts("gpu hires: all tests passed");
    return failures ? 1 : 0;
}
