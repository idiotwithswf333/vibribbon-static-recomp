// PS1 GPU: GP0/GP1 command decoding and a simple software rasterizer.
// Approximations: no dithering, no exact top-left fill rule.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <vector>
#include "vec.h"

namespace hw {

class Gpu {
public:
    static constexpr int W = 1024, H = 512;
    std::vector<uint16_t> vram = std::vector<uint16_t>(W * H);
    std::map<uint32_t, uint64_t> cmd_hist;   // GP0 command byte -> count

    // ---- upscaled output (1x = the original PS1 pixels; 2x..4x = smooth lines) ------------------------------------
    // At scale > 1 a second, N-times larger picture ("hi") is kept next to the normal 1024x512 VRAM. The game still
    // sees and uses the normal VRAM (reads, textures, copies), but every LINE it draws is also drawn into `hi`
    // anti-aliased at N x the resolution, and screen clears, frame copies and uploads are mirrored there.
    // display() then reads from `hi`. At scale 1 nothing changes and no extra memory is used.
    float line_width = 1.0f;   // line thickness in original pixels (only used when scale > 1)
    int scale() const { return scale_; }
    void set_scale(int n) {
        n = std::clamp(n, 1, 4);
        if (n == scale_) return;
        scale_ = n;
        if (n == 1) { hi_.clear(); hi_.shrink_to_fit(); return; }
        hi_.assign((size_t)W * n * H * n, 0xFF000000u);
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) hi_pixel(x, y, vram[y * W + x]);   // start from what is in VRAM now
    }

    // ---- register interface -------------------------------------------------
    std::vector<uint32_t>* gp0_log = nullptr;   // optional capture of every GP0 word (diagnostics)
    void gp0(uint32_t w) {
        if (gp0_log && gp0_log->size() < 200000) gp0_log->push_back(w);
        if (xfer_in_) { feed_xfer(w); return; }
        if (poly_) { polyline_word(w); return; }
        fifo_.push_back(w);
        if (fifo_.size() == 1) need_ = length(w >> 24);
        if ((int)fifo_.size() >= need_) { run_command(); fifo_.clear(); }
    }
    void gp1(uint32_t w) {
        uint32_t cmd = w >> 24;
        switch (cmd) {
        case 0x00: reset(); break;
        case 0x01: fifo_.clear(); break;
        case 0x03: disp_off_ = w & 1; break;
        case 0x04: dma_dir_ = w & 3; break;
        case 0x05: disp_x_ = w & 0x3FE; disp_y_ = (w >> 10) & 0x1FF; break;
        case 0x08: hres1_ = w & 3; vres_ = (w >> 2) & 1; pal_ = (w >> 3) & 1; depth24_ = (w >> 4) & 1;
                   interlace_ = (w >> 5) & 1; hres2_ = (w >> 6) & 1; break;
        case 0x10: gpuread_ = info(w & 0xF); break;
        default: break;
        }
    }
    uint32_t stat() {
        field_ ^= 1;
        uint32_t s = (tpx_ / 64) | (tpy_ / 256) << 4 | semi_ << 5 | depth_ << 7 | (draw_to_display_ << 10) |
                     (mask_set_ << 11) | (mask_check_ << 12) | (1u << 13) | (tex_disable_ << 15) |
                     (hres2_ << 16) | (hres1_ << 17) | (vres_ << 19) | (pal_ << 20) | (depth24_ << 21) |
                     (interlace_ << 22) | (disp_off_ << 23) | (1u << 26) | (1u << 27) | (1u << 28) |
                     (dma_dir_ << 29);
        if (dma_dir_) s |= 1u << 25;
        s |= (uint32_t)field_ << 31;
        return s;
    }
    uint32_t read_gpuread() {
        if (xfer_out_) {
            uint32_t w = 0;
            for (int i = 0; i < 2; ++i) {
                uint16_t px = vram[((oy_ + ocy_) & (H - 1)) * W + ((ox_ + ocx_) & (W - 1))];
                w |= (uint32_t)px << (16 * i);
                if (++ocx_ >= ow_) { ocx_ = 0; if (++ocy_ >= oh_) { xfer_out_ = false; break; } }
            }
            gpuread_ = w;
        }
        return gpuread_;
    }
    bool wants_vram_out() const { return xfer_out_; }

    // ---- display ---------------------------------------------------------------
    // Fills rgba (0xAABBGGRR little-endian => bytes R,G,B,A) and returns size.
    void display(std::vector<uint32_t>& rgba, int& w, int& h) const {
        static const int hr[4] = {256, 320, 512, 640};
        w = hres2_ ? 368 : hr[hres1_];
        h = vres_ ? (pal_ ? 512 : 480) : (pal_ ? 256 : 240);
        if (scale_ > 1 && !depth24_) {   // upscaled picture: read straight from the hi-res mirror
            const int n = scale_, WN = W * n, HN = H * n, ow = w * n, oh = h * n;
            rgba.assign((size_t)ow * oh, 0xFF000000u);
            if (!disp_off_)
                for (int y = 0; y < oh; ++y) {
                    const uint32_t* src = &hi_[(size_t)((disp_y_ * n + y) % HN) * WN];
                    uint32_t* dst = &rgba[(size_t)y * ow];
                    if (disp_x_ * n + ow <= WN) std::copy(src + disp_x_ * n, src + disp_x_ * n + ow, dst);   // the normal case: one straight copy
                    else for (int x = 0; x < ow; ++x) dst[x] = src[(disp_x_ * n + x) % WN];
                }
            w = ow; h = oh;
            return;
        }
        rgba.assign((size_t)w * h, 0xFF000000u);
        if (disp_off_) return;
        for (int y = 0; y < h; ++y) {
            int vy = (disp_y_ + y) & (H - 1);
            if (depth24_) {
                int bx = disp_x_ * 2;   // byte offset within the row
                for (int x = 0; x < w; ++x) {
                    int o = bx + x * 3;
                    auto byte = [&](int bo) { uint16_t hw = vram[vy * W + ((bo >> 1) & (W - 1))]; return (bo & 1) ? hw >> 8 : hw & 0xFF; };
                    rgba[(size_t)y * w + x] = 0xFF000000u | byte(o + 2) << 16 | byte(o + 1) << 8 | byte(o);
                }
            } else {
                for (int x = 0; x < w; ++x) {
                    uint16_t p = vram[vy * W + ((disp_x_ + x) & (W - 1))];
                    uint32_t r = (p & 31) << 3, g = ((p >> 5) & 31) << 3, b = ((p >> 10) & 31) << 3;
                    rgba[(size_t)y * w + x] = 0xFF000000u | b << 16 | g << 8 | r;
                }
            }
        }
    }

private:
    // GP0 state
    std::vector<uint32_t> fifo_; int need_ = 1;
    bool xfer_in_ = false, xfer_out_ = false;
    int ix_ = 0, iy_ = 0, iw_ = 0, ih_ = 0, icx_ = 0, icy_ = 0;
    int ox_ = 0, oy_ = 0, ow_ = 0, oh_ = 0, ocx_ = 0, ocy_ = 0;
    uint32_t gpuread_ = 0;
    // polyline
    bool poly_ = false, poly_gour_ = false, poly_semi_ = false, poly_have_prev_ = false, poly_wait_vtx_ = false;
    int px_ = 0, py_ = 0; uint32_t pcol_ = 0, pcol_next_ = 0;
    // draw state
    int tpx_ = 0, tpy_ = 0, semi_ = 0, depth_ = 0, dither_ = 0, draw_to_display_ = 0, tex_disable_ = 0;
    int mask_set_ = 0, mask_check_ = 0;
    int clip_x1_ = 0, clip_y1_ = 0, clip_x2_ = W - 1, clip_y2_ = H - 1, off_x_ = 0, off_y_ = 0;
    int tw_mx_ = 0, tw_my_ = 0, tw_ox_ = 0, tw_oy_ = 0;
    // display state
    int disp_x_ = 0, disp_y_ = 0, hres1_ = 1, hres2_ = 0, vres_ = 0, pal_ = 1, depth24_ = 0, interlace_ = 1;
    int disp_off_ = 1, dma_dir_ = 0, field_ = 0;
    // upscaled mirror (see set_scale)
    int scale_ = 1; std::vector<uint32_t> hi_, copy_tmp_; bool rec_ = false;   // rec_: mirror each native put() (polygons and rectangles)

    void reset() {
        fifo_.clear(); xfer_in_ = xfer_out_ = poly_ = false;
        tpx_ = tpy_ = semi_ = depth_ = dither_ = draw_to_display_ = tex_disable_ = mask_set_ = mask_check_ = 0;
        clip_x1_ = clip_y1_ = off_x_ = off_y_ = 0; clip_x2_ = W - 1; clip_y2_ = H - 1;
        disp_off_ = 1; dma_dir_ = 0;
    }
    uint32_t info(uint32_t n) const {
        switch (n) {
        case 2: return tw_mx_ | tw_my_ << 5 | tw_ox_ << 10 | tw_oy_ << 15;
        case 3: return clip_x1_ | clip_y1_ << 10;
        case 4: return clip_x2_ | clip_y2_ << 10;
        case 5: return (off_x_ & 0x7FF) | (off_y_ & 0x7FF) << 11;
        case 7: return 2;   // GPU version
        default: return 0;
        }
    }
    static int length(uint32_t cmd) {
        if (cmd == 0x02) return 3;
        if (cmd >= 0x20 && cmd <= 0x3F) {
            int n = (cmd & 8) ? 4 : 3; bool tex = cmd & 4, gour = cmd & 0x10;
            return 1 + n + (tex ? n : 0) + (gour ? n - 1 : 0);
        }
        if (cmd >= 0x40 && cmd <= 0x5F) return (cmd & 0x10) ? 4 : 3;   // polyline continues after the first segment
        if (cmd >= 0x60 && cmd <= 0x7F) { int sz = (cmd >> 3) & 3; return 2 + ((cmd & 4) ? 1 : 0) + (sz == 0 ? 1 : 0); }
        if (cmd >= 0x80 && cmd <= 0x9F) return 4;
        if (cmd >= 0xA0 && cmd <= 0xDF) return 3;
        return 1;
    }
    static int s11(uint32_t v) { return (int)((int32_t)(v << 21) >> 21); }

    struct V { int x, y; int r, g, b; int u, v; };

    void run_command() {
        uint32_t cmd = fifo_[0] >> 24;
        cmd_hist[cmd]++;
        if (cmd >= 0x20 && cmd <= 0x3F) { polygon(cmd); return; }
        if (cmd >= 0x40 && cmd <= 0x5F) { line_first(cmd); return; }
        if (cmd >= 0x60 && cmd <= 0x7F) { rect(cmd); return; }
        switch (cmd) {
        case 0x02: {
            int x = fifo_[1] & 0x3F0, y = (fifo_[1] >> 16) & 0x1FF;
            int w = (((fifo_[2] & 0x3FF) + 0xF) & ~0xF), h = (fifo_[2] >> 16) & 0x1FF;
            uint16_t c = rgb15(fifo_[0] & 0xFF, (fifo_[0] >> 8) & 0xFF, (fifo_[0] >> 16) & 0xFF);
            for (int j = 0; j < h; ++j) for (int i = 0; i < w; ++i) vram[((y + j) & (H - 1)) * W + ((x + i) & (W - 1))] = c;
            if (scale_ > 1) hi_fill(x, y, w, h, c);
            break; }
        case 0xE1: { uint32_t w = fifo_[0]; tpx_ = (w & 0xF) * 64; tpy_ = ((w >> 4) & 1) * 256; semi_ = (w >> 5) & 3;
                     depth_ = (w >> 7) & 3; dither_ = (w >> 9) & 1; draw_to_display_ = (w >> 10) & 1; tex_disable_ = (w >> 11) & 1; break; }
        case 0xE2: { uint32_t w = fifo_[0]; tw_mx_ = w & 31; tw_my_ = (w >> 5) & 31; tw_ox_ = (w >> 10) & 31; tw_oy_ = (w >> 15) & 31; break; }
        case 0xE3: clip_x1_ = fifo_[0] & 0x3FF; clip_y1_ = (fifo_[0] >> 10) & 0x3FF; break;
        case 0xE4: clip_x2_ = fifo_[0] & 0x3FF; clip_y2_ = (fifo_[0] >> 10) & 0x3FF; break;
        case 0xE5: off_x_ = s11(fifo_[0] & 0x7FF); off_y_ = s11((fifo_[0] >> 11) & 0x7FF); break;
        case 0xE6: mask_set_ = fifo_[0] & 1; mask_check_ = (fifo_[0] >> 1) & 1; break;
        default: break;
        }
        if (cmd >= 0x80 && cmd <= 0x9F) {
            int sx = fifo_[1] & 0x3FF, sy = (fifo_[1] >> 16) & 0x1FF, dx = fifo_[2] & 0x3FF, dy = (fifo_[2] >> 16) & 0x1FF;
            int w = ((((fifo_[3] & 0x3FF) - 1) & 0x3FF) + 1), h = ((((fifo_[3] >> 16) & 0x1FF) - 1) & 0x1FF) + 1;
            for (int j = 0; j < h; ++j) for (int i = 0; i < w; ++i)
                vram[((dy + j) & (H - 1)) * W + ((dx + i) & (W - 1))] = vram[((sy + j) & (H - 1)) * W + ((sx + i) & (W - 1))];
            if (scale_ > 1) hi_copy(sx, sy, dx, dy, w, h);   // copy the hi-res picture too (keeps the smooth lines)
            return; }
        if (cmd >= 0xA0 && cmd <= 0xBF) {
            ix_ = fifo_[1] & 0x3FF; iy_ = (fifo_[1] >> 16) & 0x1FF;
            iw_ = ((((fifo_[2] & 0x3FF) - 1) & 0x3FF) + 1); ih_ = ((((fifo_[2] >> 16) & 0x1FF) - 1) & 0x1FF) + 1;
            icx_ = icy_ = 0; xfer_in_ = true; return; }
        if (cmd >= 0xC0 && cmd <= 0xDF) {
            ox_ = fifo_[1] & 0x3FF; oy_ = (fifo_[1] >> 16) & 0x1FF;
            ow_ = ((((fifo_[2] & 0x3FF) - 1) & 0x3FF) + 1); oh_ = ((((fifo_[2] >> 16) & 0x1FF) - 1) & 0x1FF) + 1;
            ocx_ = ocy_ = 0; xfer_out_ = true; return; }
    }
    void feed_xfer(uint32_t w) {
        for (int i = 0; i < 2 && xfer_in_; ++i) {
            vram[((iy_ + icy_) & (H - 1)) * W + ((ix_ + icx_) & (W - 1))] = (uint16_t)(w >> (16 * i));
            if (scale_ > 1) hi_pixel((ix_ + icx_) & (W - 1), (iy_ + icy_) & (H - 1), (uint16_t)(w >> (16 * i)));
            if (++icx_ >= iw_) { icx_ = 0; if (++icy_ >= ih_) xfer_in_ = false; }
        }
    }

    // ---- upscaled mirror helpers ---------------------------------------------------------------------------------
    static uint32_t px32(uint16_t p) {   // 15-bit colour -> 0xAABBGGRR with proper 5->8 bit expansion (white = 255)
        auto e = [](int v) { return (uint32_t)((v << 3) | (v >> 2)); };
        return 0xFF000000u | e((p >> 10) & 31) << 16 | e((p >> 5) & 31) << 8 | e(p & 31);
    }
    void hi_pixel(int x, int y, uint16_t p) {   // one native pixel -> an N x N block
        const int n = scale_, WN = W * n; const uint32_t c = px32(p);
        for (int j = 0; j < n; ++j) { uint32_t* row = &hi_[(size_t)(y * n + j) * WN + (size_t)x * n]; for (int i = 0; i < n; ++i) row[i] = c; }
    }
    void hi_fill(int x, int y, int w, int h, uint16_t p) {
        const int n = scale_, WN = W * n, HN = H * n; const uint32_t c = px32(p);
        for (int j = 0; j < h * n; ++j) {
            uint32_t* row = &hi_[(size_t)(((y * n + j) % HN)) * WN];
            if (x * n + w * n <= WN) std::fill(row + x * n, row + x * n + w * n, c);
            else for (int i = 0; i < w * n; ++i) row[(x * n + i) % WN] = c;
        }
    }
    void hi_copy(int sx, int sy, int dx, int dy, int w, int h) {
        const int n = scale_, WN = W * n, HN = H * n, bw = w * n, bh = h * n;
        copy_tmp_.resize((size_t)bw * bh);   // reused every frame: no allocation
        const bool s_ok = sx * n + bw <= WN, d_ok = dx * n + bw <= WN;
        for (int j = 0; j < bh; ++j) {
            const uint32_t* src = &hi_[(size_t)((sy * n + j) % HN) * WN];
            uint32_t* t = &copy_tmp_[(size_t)j * bw];
            if (s_ok) std::copy(src + sx * n, src + sx * n + bw, t); else for (int i = 0; i < bw; ++i) t[i] = src[(sx * n + i) % WN];
        }
        for (int j = 0; j < bh; ++j) {
            uint32_t* dst = &hi_[(size_t)((dy * n + j) % HN) * WN];
            const uint32_t* t = &copy_tmp_[(size_t)j * bw];
            if (d_ok) std::copy(t, t + bw, dst + dx * n); else for (int i = 0; i < bw; ++i) dst[(dx * n + i) % WN] = t[i];
        }
    }
    // A smooth line into the mirror. (x0,y0)-(x1,y1) are native pixel coordinates (draw offset already applied).
    void hi_line(int x0, int y0, uint32_t c0, int x1, int y1, uint32_t c1, bool gour, bool semi) {
        const int n = scale_, WN = W * n;
        const float f = (float)n;
        const int cx0 = std::max(clip_x1_, 0) * n, cx1 = std::min(clip_x2_ + 1, W) * n;
        const int cy0 = std::max(clip_y1_, 0) * n, cy1 = std::min(clip_y2_ + 1, H) * n;
        if (cx0 >= cx1 || cy0 >= cy1) return;
        vec::aa_line((x0 + 0.5f) * f, (y0 + 0.5f) * f, (x1 + 0.5f) * f, (y1 + 0.5f) * f, 0.5f * f * line_width, cx0, cy0, cx1, cy1,
            [&](int px, int py, float cov, float t) {
                int sr = c0 & 0xFF, sg = (c0 >> 8) & 0xFF, sb = (c0 >> 16) & 0xFF;
                if (gour) { sr += (int)(((int)(c1 & 0xFF) - sr) * t); sg += (int)(((int)((c1 >> 8) & 0xFF) - sg) * t); sb += (int)(((int)((c1 >> 16) & 0xFF) - sb) * t); }
                uint32_t& d = hi_[(size_t)py * WN + px];
                int dr = d & 0xFF, dg = (d >> 8) & 0xFF, db = (d >> 16) & 0xFF;
                if (semi) {   // same four blend modes as the native put()
                    switch (semi_) {
                    case 0: sr = (dr + sr) / 2; sg = (dg + sg) / 2; sb = (db + sb) / 2; break;
                    case 1: sr = dr + sr; sg = dg + sg; sb = db + sb; break;
                    case 2: sr = dr - sr; sg = dg - sg; sb = db - sb; break;
                    default: sr = dr + sr / 4; sg = dg + sg / 4; sb = db + sb / 4; break;
                    }
                    sr = std::clamp(sr, 0, 255); sg = std::clamp(sg, 0, 255); sb = std::clamp(sb, 0, 255);
                }
                const int r = dr + (int)((sr - dr) * cov + 0.5f), g = dg + (int)((sg - dg) * cov + 0.5f), b = db + (int)((sb - db) * cov + 0.5f);
                d = 0xFF000000u | (uint32_t)b << 16 | (uint32_t)g << 8 | (uint32_t)r;
            });
    }

    static uint16_t rgb15(int r, int g, int b) { return (uint16_t)((r >> 3) | (g >> 3) << 5 | (b >> 3) << 10); }

    // ---- pixel output -----------------------------------------------------------
    void put(int x, int y, uint16_t c, bool semi) {
        if (x < clip_x1_ || x > clip_x2_ || y < clip_y1_ || y > clip_y2_) return;
        if (x < 0 || x >= W || y < 0 || y >= H) return;
        uint16_t& dst = vram[y * W + x];
        if (mask_check_ && (dst & 0x8000)) return;
        if (semi) {
            int br = dst & 31, bg = (dst >> 5) & 31, bb = (dst >> 10) & 31, fr = c & 31, fg = (c >> 5) & 31, fb = (c >> 10) & 31;
            int r, g, b;
            switch (semi_) {
            case 0: r = (br + fr) / 2; g = (bg + fg) / 2; b = (bb + fb) / 2; break;
            case 1: r = br + fr; g = bg + fg; b = bb + fb; break;
            case 2: r = br - fr; g = bg - fg; b = bb - fb; break;
            default: r = br + fr / 4; g = bg + fg / 4; b = bb + fb / 4; break;
            }
            c = (uint16_t)(std::clamp(r, 0, 31) | std::clamp(g, 0, 31) << 5 | std::clamp(b, 0, 31) << 10);
        }
        dst = c | (mask_set_ ? 0x8000 : 0);
        if (rec_) hi_pixel(x, y, dst);   // polygons/rectangles: mirrored block by block
    }

    // texture fetch; returns false for fully transparent texels
    bool texel(int u, int v, int clutx, int cluty, uint16_t& out) const {
        u &= 255; v &= 255;
        u = (u & ~(tw_mx_ * 8)) | ((tw_ox_ & tw_mx_) * 8);
        v = (v & ~(tw_my_ * 8)) | ((tw_oy_ & tw_my_) * 8);
        uint16_t t;
        int row = (tpy_ + v) & (H - 1);
        if (depth_ == 0) { uint16_t hw = vram[row * W + ((tpx_ + u / 4) & (W - 1))]; int idx = (hw >> ((u & 3) * 4)) & 0xF; t = vram[(cluty & (H - 1)) * W + ((clutx + idx) & (W - 1))]; }
        else if (depth_ == 1) { uint16_t hw = vram[row * W + ((tpx_ + u / 2) & (W - 1))]; int idx = (hw >> ((u & 1) * 8)) & 0xFF; t = vram[(cluty & (H - 1)) * W + ((clutx + idx) & (W - 1))]; }
        else t = vram[row * W + ((tpx_ + u) & (W - 1))];
        if (t == 0) return false;
        out = t; return true;
    }
    uint16_t modulate(uint16_t t, int r, int g, int b, bool raw) const {
        if (raw) return t & 0x7FFF;
        int tr = (t & 31) << 3, tg = ((t >> 5) & 31) << 3, tb = ((t >> 10) & 31) << 3;
        return rgb15(std::min(255, tr * r >> 7), std::min(255, tg * g >> 7), std::min(255, tb * b >> 7));
    }

    // ---- primitives -----------------------------------------------------------------
    void polygon(uint32_t cmd) {
        bool quad = cmd & 8, tex = cmd & 4, gour = cmd & 0x10, semi = cmd & 2, raw = cmd & 1;
        int n = quad ? 4 : 3, idx = 0;
        V v[4]; int clutx = 0, cluty = 0;
        uint32_t col = fifo_[0] & 0xFFFFFF;
        for (int i = 0; i < n; ++i) {
            if (i == 0) idx = 1; else if (gour) col = fifo_[idx++] & 0xFFFFFF;
            uint32_t xy = fifo_[idx++];
            v[i].x = s11(xy & 0x7FF) + off_x_; v[i].y = s11((xy >> 16) & 0x7FF) + off_y_;
            v[i].r = col & 0xFF; v[i].g = (col >> 8) & 0xFF; v[i].b = (col >> 16) & 0xFF;
            if (tex) {
                uint32_t t = fifo_[idx++];
                v[i].u = t & 0xFF; v[i].v = (t >> 8) & 0xFF;
                if (i == 0) { clutx = ((t >> 16) & 0x3F) * 16; cluty = (t >> 22) & 0x1FF; }
                if (i == 1) { tpx_ = (t >> 16 & 0xF) * 64; tpy_ = ((t >> 20) & 1) * 256; semi_ = (t >> 21) & 3; depth_ = (t >> 23) & 3; }
            }
        }
        rec_ = scale_ > 1;
        tri(v[0], v[1], v[2], tex, semi, raw, clutx, cluty);
        if (quad) tri(v[1], v[2], v[3], tex, semi, raw, clutx, cluty);
        rec_ = false;
    }
    static int64_t edge(const V& a, const V& b, int px, int py) { return (int64_t)(b.x - a.x) * (py - a.y) - (int64_t)(b.y - a.y) * (px - a.x); }
    void tri(V a, V b, V c, bool tex, bool semi, bool raw, int clutx, int cluty) {
        int64_t area = edge(a, b, c.x, c.y);
        if (area == 0) return;
        if (area < 0) { std::swap(b, c); area = -area; }
        if (std::max({a.x, b.x, c.x}) - std::min({a.x, b.x, c.x}) > 1023 || std::max({a.y, b.y, c.y}) - std::min({a.y, b.y, c.y}) > 511) return;
        int x0 = std::max(std::min({a.x, b.x, c.x}), clip_x1_), x1 = std::min(std::max({a.x, b.x, c.x}), clip_x2_);
        int y0 = std::max(std::min({a.y, b.y, c.y}), clip_y1_), y1 = std::min(std::max({a.y, b.y, c.y}), clip_y2_);
        for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) {
            int64_t w0 = edge(b, c, x, y), w1 = edge(c, a, x, y), w2 = edge(a, b, x, y);
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            int r = (int)((a.r * w0 + b.r * w1 + c.r * w2) / area);
            int g = (int)((a.g * w0 + b.g * w1 + c.g * w2) / area);
            int bl = (int)((a.b * w0 + b.b * w1 + c.b * w2) / area);
            if (tex) {
                int u = (int)((a.u * w0 + b.u * w1 + c.u * w2) / area), vv = (int)((a.v * w0 + b.v * w1 + c.v * w2) / area);
                uint16_t t;
                if (!texel(u, vv, clutx, cluty, t)) continue;
                put(x, y, modulate(t, r, g, bl, raw), semi && (t & 0x8000));
            } else put(x, y, rgb15(r, g, bl), semi);
        }
    }
    void rect(uint32_t cmd) {
        bool tex = cmd & 4, semi = cmd & 2, raw = cmd & 1; int sz = (cmd >> 3) & 3, idx = 1;
        uint32_t col = fifo_[0] & 0xFFFFFF;
        uint32_t xy = fifo_[idx++];
        int x = s11(xy & 0x7FF) + off_x_, y = s11((xy >> 16) & 0x7FF) + off_y_;
        int u0 = 0, v0 = 0, clutx = 0, cluty = 0;
        if (tex) { uint32_t t = fifo_[idx++]; u0 = t & 0xFF; v0 = (t >> 8) & 0xFF; clutx = ((t >> 16) & 0x3F) * 16; cluty = (t >> 22) & 0x1FF; }
        int w, h;
        if (sz == 0) { uint32_t wh = fifo_[idx++]; w = wh & 0x3FF; h = (wh >> 16) & 0x1FF; }
        else if (sz == 1) w = h = 1; else if (sz == 2) w = h = 8; else w = h = 16;
        int r = col & 0xFF, g = (col >> 8) & 0xFF, b = (col >> 16) & 0xFF;
        rec_ = scale_ > 1;
        for (int j = 0; j < h; ++j) for (int i = 0; i < w; ++i) {
            if (tex) { uint16_t t; if (!texel(u0 + i, v0 + j, clutx, cluty, t)) continue; put(x + i, y + j, modulate(t, r, g, b, raw), semi && (t & 0x8000)); }
            else put(x + i, y + j, rgb15(r, g, b), semi);
        }
        rec_ = false;
    }
    void line_seg(int x0, int y0, uint32_t c0, int x1, int y1, uint32_t c1, bool gour, bool semi) {
        x0 += off_x_; y0 += off_y_; x1 += off_x_; y1 += off_y_;
        int dx = x1 - x0, dy = y1 - y0, n = std::max(std::abs(dx), std::abs(dy));
        if (std::abs(dx) > 1023 || std::abs(dy) > 511) return;
        if (scale_ > 1) hi_line(x0, y0, c0, x1, y1, c1, gour, semi);   // smooth version for the upscaled picture
        for (int i = 0; i <= n; ++i) {
            int x = n ? x0 + dx * i / n : x0, y = n ? y0 + dy * i / n : y0;
            int r = c0 & 0xFF, g = (c0 >> 8) & 0xFF, b = (c0 >> 16) & 0xFF;
            if (gour && n) {
                int r1 = c1 & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = (c1 >> 16) & 0xFF;
                r += (r1 - r) * i / n; g += (g1 - g) * i / n; b += (b1 - b) * i / n;
            }
            put(x, y, rgb15(r, g, b), semi);
        }
    }
    void line_first(uint32_t cmd) {
        bool gour = cmd & 0x10, semi = cmd & 2, poly = cmd & 8;
        uint32_t c0 = fifo_[0] & 0xFFFFFF, v0 = fifo_[1], c1 = gour ? fifo_[2] & 0xFFFFFF : c0, v1 = gour ? fifo_[3] : fifo_[2];
        int x0 = s11(v0 & 0x7FF), y0 = s11((v0 >> 16) & 0x7FF), x1 = s11(v1 & 0x7FF), y1 = s11((v1 >> 16) & 0x7FF);
        line_seg(x0, y0, c0, x1, y1, c1, gour, semi);
        if (poly) { poly_ = true; poly_gour_ = gour; poly_semi_ = semi; px_ = x1; py_ = y1; pcol_ = c1; poly_wait_vtx_ = !gour ? true : false; pcol_next_ = 0; poly_have_prev_ = false; }
    }
    void polyline_word(uint32_t w) {
        if ((w & 0xF000F000u) == 0x50005000u) { poly_ = false; return; }   // terminator
        if (poly_gour_ && !poly_have_prev_) { pcol_next_ = w & 0xFFFFFF; poly_have_prev_ = true; return; }   // colour word first
        int x1 = s11(w & 0x7FF), y1 = s11((w >> 16) & 0x7FF);
        uint32_t c1 = poly_gour_ ? pcol_next_ : pcol_;
        line_seg(px_, py_, pcol_, x1, y1, c1, poly_gour_, poly_semi_);
        px_ = x1; py_ = y1; pcol_ = c1; poly_have_prev_ = false;
    }
};

}  // namespace hw
