// Small anti-aliased vector drawing: used by the launcher and by the GPU's upscaled line mode (1x-4x), so both have
// the same smooth look. No SDL dependency.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace vec {

// Calls px(x, y, coverage 0..1, t) for every pixel touched by a round-capped line from (x0,y0) to (x1,y1).
// Coordinates are in pixels (a pixel's centre is at +0.5); `half` is half the line width in pixels; t (0..1) is the
// position along the segment (for colour gradients). Only pixels inside [cx0,cx1) x [cy0,cy1) are visited, and each
// pixel is visited at most once. Cost is proportional to the line's length, not its bounding box.
template <class F>
inline void aa_line(float x0, float y0, float x1, float y1, float half, int cx0, int cy0, int cx1, int cy1, F&& px) {
    const float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy, len = std::sqrt(len2);
    const bool horiz = std::fabs(dx) >= std::fabs(dy);
    const float reach = half + 1.0f;
    float a0 = horiz ? x0 : y0, a1 = horiz ? x1 : y1;
    if (a0 > a1) std::swap(a0, a1);
    const int m0 = std::max(horiz ? cx0 : cy0, (int)std::floor(a0 - reach));
    const int m1 = std::min(horiz ? cx1 : cy1, (int)std::ceil(a1 + reach) + 1);
    const float major_d = std::max(std::fabs(horiz ? dx : dy), 1e-6f);
    const float stretch = len > 1e-6f ? len / major_d : 1.0f;   // 1/cos(angle): how much wider the minor-axis span is on a slope
    const float span = reach * stretch + 1.0f;
    for (int m = m0; m < m1; ++m) {
        const float am = m + 0.5f;
        float t = horiz ? (dx != 0 ? (am - x0) / dx : 0.0f) : (dy != 0 ? (am - y0) / dy : 0.0f);
        t = std::min(1.0f, std::max(0.0f, t));
        const float cm = horiz ? y0 + t * dy : x0 + t * dx;   // where the centre line crosses this column/row
        const int n0 = std::max(horiz ? cy0 : cx0, (int)std::floor(cm - span));
        const int n1 = std::min(horiz ? cy1 : cx1, (int)std::ceil(cm + span) + 1);
        for (int n = n0; n < n1; ++n) {
            const float pxc = horiz ? am : n + 0.5f, pyc = horiz ? n + 0.5f : am;
            float tt = len2 > 1e-9f ? ((pxc - x0) * dx + (pyc - y0) * dy) / len2 : 0.0f;
            tt = std::min(1.0f, std::max(0.0f, tt));
            const float qx = x0 + tt * dx - pxc, qy = y0 + tt * dy - pyc;
            float cov = half + 0.5f - std::sqrt(qx * qx + qy * qy);
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            px(horiz ? m : n, horiz ? n : m, cov, tt);
        }
    }
}

// ---- a pixel canvas --------------------------------------------------------------------------------------------
// Pixels are 0xAABBGGRR (bytes R,G,B,A in memory), opaque.
constexpr uint32_t rgb(int r, int g, int b) { return 0xFF000000u | (uint32_t)b << 16 | (uint32_t)g << 8 | (uint32_t)r; }

struct Canvas {
    int w = 0, h = 0;
    std::vector<uint32_t> px;
    void resize(int nw, int nh) { w = nw; h = nh; px.assign((size_t)w * h, 0xFF000000u); }
    void clear(uint32_t c = 0xFF000000u) { std::fill(px.begin(), px.end(), c); }
    void blend(int x, int y, uint32_t c, float a) {
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0.0f) return;
        uint32_t& d = px[(size_t)y * w + x];
        auto mix = [&](int sh) { float s = (float)((c >> sh) & 255), t = (float)((d >> sh) & 255); return (uint32_t)(t + (s - t) * a + 0.5f) & 255u; };
        d = 0xFF000000u | mix(16) << 16 | mix(8) << 8 | mix(0);
    }
    void line(float x0, float y0, float x1, float y1, uint32_t c, float width = 3.0f, float alpha = 1.0f) {
        aa_line(x0, y0, x1, y1, width * 0.5f, 0, 0, w, h, [&](int x, int y, float cov, float) { blend(x, y, c, cov * alpha); });
    }
    void polyline(const std::vector<std::pair<float, float>>& p, uint32_t c, float width = 3.0f, bool closed = false, float alpha = 1.0f) {
        for (size_t i = 0; i + 1 < p.size(); ++i) line(p[i].first, p[i].second, p[i + 1].first, p[i + 1].second, c, width, alpha);
        if (closed && p.size() > 2) line(p.back().first, p.back().second, p[0].first, p[0].second, c, width, alpha);
    }
    // The rounded "speech bubble" outline the game's menus use: a 12-sided squashed circle.
    void bubble(float cx, float cy, float rx, float ry, uint32_t c, float width = 3.0f, float alpha = 1.0f) {
        std::vector<std::pair<float, float>> p;
        for (int i = 0; i < 12; ++i) { float a = 6.2831853f * (i + 0.5f) / 12.0f; p.push_back({cx + rx * std::cos(a), cy + ry * std::sin(a)}); }
        polyline(p, c, width, true, alpha);
    }
};

// ---- stroke font ------------------------------------------------------------------------------------------------
// Each glyph is a set of polylines on a 4 x 6 grid (x right, y down), written "x,y x,y ...|x,y ...".
inline const char* glyph(char ch) {
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    switch (ch) {
    case 'A': return "0,6 0,2 2,0 4,2 4,6|0,3 4,3";
    case 'B': return "0,0 0,6 3,6 4,5 4,4 3,3 0,3|3,3 4,2 4,1 3,0 0,0";
    case 'C': return "4,1 3,0 1,0 0,1 0,5 1,6 3,6 4,5";
    case 'D': return "0,0 0,6 2,6 4,4 4,2 2,0 0,0";
    case 'E': return "4,0 0,0 0,6 4,6|0,3 3,3";
    case 'F': return "4,0 0,0 0,6|0,3 3,3";
    case 'G': return "4,1 3,0 1,0 0,1 0,5 1,6 3,6 4,5 4,3 2,3";
    case 'H': return "0,0 0,6|4,0 4,6|0,3 4,3";
    case 'I': return "1,0 3,0|2,0 2,6|1,6 3,6";
    case 'J': return "4,0 4,5 3,6 1,6 0,5";
    case 'K': return "0,0 0,6|4,0 0,3 4,6";
    case 'L': return "0,0 0,6 4,6";
    case 'M': return "0,6 0,0 2,3 4,0 4,6";
    case 'N': return "0,6 0,0 4,6 4,0";
    case 'O': return "1,0 3,0 4,1 4,5 3,6 1,6 0,5 0,1 1,0";
    case 'P': return "0,6 0,0 3,0 4,1 4,2 3,3 0,3";
    case 'Q': return "1,0 3,0 4,1 4,5 3,6 1,6 0,5 0,1 1,0|2,4 4,6";
    case 'R': return "0,6 0,0 3,0 4,1 4,2 3,3 0,3|2,3 4,6";
    case 'S': return "4,1 3,0 1,0 0,1 0,2 1,3 3,3 4,4 4,5 3,6 1,6 0,5";
    case 'T': return "0,0 4,0|2,0 2,6";
    case 'U': return "0,0 0,5 1,6 3,6 4,5 4,0";
    case 'V': return "0,0 2,6 4,0";
    case 'W': return "0,0 1,6 2,3 3,6 4,0";
    case 'X': return "0,0 4,6|4,0 0,6";
    case 'Y': return "0,0 2,3 4,0|2,3 2,6";
    case 'Z': return "0,0 4,0 0,6 4,6";
    case '0': return "1,0 3,0 4,1 4,5 3,6 1,6 0,5 0,1 1,0";
    case '1': return "1,1 2,0 2,6|1,6 3,6";
    case '2': return "0,1 1,0 3,0 4,1 4,2 0,6 4,6";
    case '3': return "0,0 4,0 2,2 3,3 4,4 4,5 3,6 1,6 0,5";
    case '4': return "3,6 3,0 0,4 4,4";
    case '5': return "4,0 0,0 0,3 3,3 4,4 4,5 3,6 0,6";
    case '6': return "4,1 3,0 1,0 0,1 0,5 1,6 3,6 4,5 4,4 3,3 0,3";
    case '7': return "0,0 4,0 1,6";
    case '8': return "1,0 3,0 4,1 4,2 3,3 1,3 0,2 0,1 1,0|1,3 0,4 0,5 1,6 3,6 4,5 4,4 3,3";
    case '9': return "0,5 1,6 3,6 4,5 4,1 3,0 1,0 0,1 0,2 1,3 4,3";
    case '.': return "2,5 2,6";
    case ',': return "2,5 1,7";
    case ':': return "2,1 2,2|2,4 2,5";
    case '-': return "0,3 4,3";
    case '_': return "0,6 4,6";
    case '/': return "0,6 4,0";
    case '\\': return "0,0 4,6";
    case '(': return "3,0 1,2 1,4 3,6";
    case ')': return "1,0 3,2 3,4 1,6";
    case '!': return "2,0 2,4|2,5 2,6";
    case '?': return "0,1 1,0 3,0 4,1 4,2 2,3 2,4|2,5 2,6";
    case '+': return "0,3 4,3|2,1 2,5";
    case '=': return "0,2 4,2|0,4 4,4";
    case '>': return "0,0 4,3 0,6";
    case '<': return "4,0 0,3 4,6";
    case '~': return "0,4 1,2 3,4 4,2";
    case '\'': return "2,0 2,2";
    case '%': return "0,0 1,0 1,1 0,1 0,0|4,5 3,5 3,6 4,6 4,5|4,0 0,6";
    case ' ': return "";
    default: return "0,1 1,0 3,0 4,1 4,2 2,3 2,4|2,5 2,6";   // unknown characters show as '?'
    }
}

inline float text_width(const std::string& s, float unit) { return s.empty() ? 0.0f : ((float)s.size() * 6.0f - 2.0f) * unit; }

// Draws `s` with its top-left at (x,y); one grid unit = `unit` pixels. align: 0 left, 1 centre, 2 right (about x).
inline void text(Canvas& cv, float x, float y, float unit, const std::string& s, uint32_t c, float width = 0.0f, int align = 0, float alpha = 1.0f) {
    if (width <= 0.0f) width = std::max(1.5f, unit * 0.55f);
    const float tw = text_width(s, unit);
    float ox = align == 1 ? x - tw * 0.5f : align == 2 ? x - tw : x;
    for (char ch : s) {
        const char* g = glyph(ch);
        std::vector<std::pair<float, float>> pts;
        for (const char* p = g;; ++p) {
            if (*p == '|' || *p == 0) {
                if (pts.size() > 1) cv.polyline(pts, c, width, false, alpha);
                else if (pts.size() == 1) cv.line(pts[0].first, pts[0].second, pts[0].first, pts[0].second, c, width, alpha);
                pts.clear();
                if (*p == 0) break;
                continue;
            }
            if (*p == ' ') continue;
            int gx = 0, gy = 0;
            while (*p >= '0' && *p <= '9') gx = gx * 10 + (*p++ - '0');
            if (*p == ',') ++p;
            while (*p >= '0' && *p <= '9') gy = gy * 10 + (*p++ - '0');
            --p;
            pts.push_back({ox + gx * unit, y + gy * unit});
        }
        ox += 6.0f * unit;
    }
}

}  // namespace vec
