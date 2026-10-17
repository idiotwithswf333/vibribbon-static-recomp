// The launcher screen: drop the disc (.cue), optionally a mods .zip, pick the upscale (1x-4x), press START.
// Everything here is SDL-free: the model (what is selected, what is valid, keyboard/controller navigation, the saved
// settings file) and a software renderer that draws the whole screen as smooth vector lines into a vec::Canvas.
// main.cpp only shows that canvas and feeds it mouse/keyboard/controller events.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "disc.h"
#include "vec.h"

namespace launcher {

constexpr int kW = 1920, kH = 1080;   // the launcher canvas (scaled to the window)

// ---- saved settings (vibribbon-settings.ini next to the exe) ------------------------------------------------------
struct Settings {
    std::string cue, mods;
    int upscale = 1;   // 1 = original PS1 pixels ... 4 = ~1080p lines
    bool glow = false;   // neon glow (bloom) on at start (F9 still toggles it in game)
    bool ffwd = true;    // hold Tab / right stick to fast-forward
    bool shot = true;    // F12 saves a screenshot
};

inline bool load_settings(const std::string& path, Settings& s) {
    std::ifstream f(path);
    if (!f) return false;
    for (std::string line; std::getline(f, line);) {
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
        if (k == "cue") s.cue = v;
        else if (k == "mods") s.mods = v;
        else if (k == "upscale") s.upscale = std::clamp(std::atoi(v.c_str()), 1, 4);
        else if (k == "glow") s.glow = std::atoi(v.c_str()) != 0;
        else if (k == "fastforward") s.ffwd = std::atoi(v.c_str()) != 0;
        else if (k == "screenshots") s.shot = std::atoi(v.c_str()) != 0;
    }
    return true;
}
inline bool save_settings(const std::string& path, const Settings& s) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << "# Vib-Ribbon recomp launcher settings (edited by the launcher; safe to delete)\n"
      << "cue=" << s.cue << "\nmods=" << s.mods << "\nupscale=" << std::clamp(s.upscale, 1, 4) << "\n"
      << "glow=" << (s.glow ? 1 : 0) << "\nfastforward=" << (s.ffwd ? 1 : 0) << "\nscreenshots=" << (s.shot ? 1 : 0) << "\n";
    return true;
}

// ---- checks on what was dropped -------------------------------------------------------------------------------------
struct DiscCheck { bool ok = false; std::string msg = "NO DISC YET"; };
struct ZipCheck { bool ok = false; std::string msg = "NO MODS"; int files = 0; };

inline std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
inline std::string upper(std::string s) { for (auto& c : s) c = (char)std::toupper((unsigned char)c); return s; }
inline std::string file_name(const std::string& p) { return std::filesystem::path(p).filename().string(); }

inline DiscCheck check_disc(const std::string& cue) {
    DiscCheck r;
    std::error_code ec;
    if (cue.empty()) return r;
    if (!std::filesystem::is_regular_file(cue, ec)) { r.msg = "FILE NOT FOUND"; return r; }
    if (lower(std::filesystem::path(cue).extension().string()) != ".cue") { r.msg = "DROP THE .CUE, NOT THE .BIN"; return r; }
    disc::Image img;
    std::string err = img.open(cue);
    if (!err.empty()) { r.msg = upper(err.substr(0, 60)); return r; }
    std::string boot = disc::boot_file(img);
    if (boot.empty()) { r.msg = "NO PS1 GAME FOUND ON THIS DISC"; return r; }
    if (boot != "SCES_028.73") { r.msg = "NOT THE VIB-RIBBON EU DISC (" + upper(boot) + ")"; return r; }
    r.ok = true; r.msg = "OK  VIB-RIBBON EU DISC";
    return r;
}

inline ZipCheck check_zip(const std::string& path) {
    ZipCheck r;
    std::error_code ec;
    if (path.empty()) return r;
    if (!std::filesystem::is_regular_file(path, ec)) { r.msg = "FILE NOT FOUND"; return r; }
    std::ifstream f(path, std::ios::binary);
    char sig[4] = {};
    f.read(sig, 4);
    const bool local = sig[0] == 'P' && sig[1] == 'K' && sig[2] == 3 && sig[3] == 4, empty = sig[0] == 'P' && sig[1] == 'K' && sig[2] == 5 && sig[3] == 6;
    if (!f || !(local || empty)) { r.msg = "NOT A ZIP FILE"; return r; }
    // entry count lives in the end-of-central-directory record (within the last 64 KB)
    const uint64_t size = std::filesystem::file_size(path, ec), tail = std::min<uint64_t>(size, 65557);
    std::vector<char> buf((size_t)tail);
    f.seekg((std::streamoff)(size - tail)); f.read(buf.data(), (std::streamsize)tail);
    for (long i = (long)tail - 22; i >= 0; --i)
        if (buf[i] == 'P' && buf[i + 1] == 'K' && buf[i + 2] == 5 && buf[i + 3] == 6) { r.files = (uint8_t)buf[i + 10] | (uint8_t)buf[i + 11] << 8; break; }
    r.ok = true; r.msg = std::to_string(r.files) + (r.files == 1 ? " FILE" : " FILES");
    return r;
}

// ---- the model ----------------------------------------------------------------------------------------------------
enum Focus { F_DISC = 0, F_MODS, F_UP1, F_UP2, F_UP3, F_UP4, F_OPT_GLOW, F_OPT_FF, F_OPT_SHOT, F_START, F_BACK, F_COUNT, F_CLEAR = 100, F_NONE = -1 };

struct Model {
    Settings s;
    DiscCheck disc;
    ZipCheck zip;
    bool bios_ok = false; std::string bios_name;
    int focus = F_START, hover = F_NONE;
    bool start_requested = false;
    bool page_mods = false;   // the "LIST MODS" sub-page (mods zip + neon glow) is open
    std::string toast; int toast_ttl = 0;   // short message, ttl counts frames

    void init(const Settings& saved, bool bios_found, const std::string& bios) {
        s = saved; bios_ok = bios_found; bios_name = bios;
        disc = check_disc(s.cue);
        zip = check_zip(s.mods);
        if (!disc.ok) focus = F_DISC;
    }
    bool can_start() const { return disc.ok; }
    void say(const std::string& m, int frames = 200) { toast = m; toast_ttl = frames; }
    void tick() { if (toast_ttl > 0 && --toast_ttl == 0) toast.clear(); }

    // Routes a dropped file by what it is. Returns a one-line description of what happened.
    std::string drop(const std::string& path, bool is_bios) {
        std::string ext = lower(std::filesystem::path(path).extension().string());
        if (is_bios) { bios_ok = true; bios_name = file_name(path); say("BIOS LOADED: " + upper(bios_name)); return "bios"; }
        if (ext == ".zip") { set_mods(path); return "mods"; }
        set_cue(path);
        return "cue";
    }
    void set_cue(const std::string& p) {
        s.cue = p; disc = check_disc(p);
        if (disc.ok) { focus = F_START; say("DISC READY - PRESS START"); } else say(disc.msg);
    }
    void set_mods(const std::string& p) {
        s.mods = p; zip = check_zip(p);
        say(zip.ok ? "MODS ZIP ADDED (MODS ARE NOT SUPPORTED YET)" : zip.msg);
    }
    void clear_mods() { s.mods.clear(); zip = ZipCheck(); say("MODS CLEARED"); }
    void set_upscale(int n) { s.upscale = std::clamp(n, 1, 4); }
    static bool is_opt(int f) { return f == F_OPT_FF || f == F_OPT_SHOT; }   // main-page check boxes (glow lives on the mods page)
    void open_mods() { page_mods = true; focus = F_OPT_GLOW; hover = F_NONE; }
    void close_mods() { page_mods = false; focus = F_MODS; hover = F_NONE; }
    void tab() {   // Tab: next thing on the current page
        static const int main_order[] = {F_DISC, F_MODS, F_UP1, F_UP2, F_UP3, F_UP4, F_OPT_FF, F_OPT_SHOT, F_START};
        static const int mods_order[] = {F_MODS, F_OPT_GLOW, F_BACK};
        const int* o = page_mods ? mods_order : main_order; const int n = page_mods ? 3 : 9;
        int i = 0; for (; i < n; ++i) if (o[i] == focus) break;
        focus = o[(i + 1) % n];
    }
    bool& opt_ref(int f) { return f == F_OPT_GLOW ? s.glow : (f == F_OPT_FF ? s.ffwd : s.shot); }
    void toggle_opt(int f) {
        bool& b = opt_ref(f); b = !b;
        static const char* names[3] = {"NEON GLOW", "FAST-FORWARD", "SCREENSHOTS"};
        say(std::string(names[f - F_OPT_GLOW]) + (b ? " ON" : " OFF"), 120);
    }

    // Keyboard / controller navigation: dx, dy in {-1,0,1}. Left/right over the upscale chips also picks that size.
    void nav(int dx, int dy) {
        if (page_mods) {
            static const int order[] = {F_MODS, F_OPT_GLOW, F_BACK};
            int i = 0; for (; i < 3; ++i) if (order[i] == focus) break;
            if (i == 3) { focus = F_OPT_GLOW; return; }
            focus = order[std::clamp(i + dy, 0, 2)];
            return;
        }
        if (focus >= F_UP1 && focus <= F_UP4 && dx) { focus = std::clamp(focus + dx, (int)F_UP1, (int)F_UP4); set_upscale(focus - F_UP1 + 1); return; }
        if (is_opt(focus) && dx) { focus = dx < 0 ? (int)F_OPT_FF : (int)F_OPT_SHOT; return; }
        if (focus == F_DISC && dx > 0) { focus = F_MODS; return; }
        if (focus == F_MODS && dx < 0) { focus = F_DISC; return; }
        if (dy > 0) {
            if (focus == F_DISC || focus == F_MODS) focus = F_UP1 + s.upscale - 1;
            else if (focus >= F_UP1 && focus <= F_UP4) focus = F_OPT_FF;
            else if (is_opt(focus)) focus = F_START;
        } else if (dy < 0) {
            if (focus == F_START) focus = F_OPT_FF;
            else if (is_opt(focus)) focus = F_UP1 + s.upscale - 1;
            else if (focus >= F_UP1 && focus <= F_UP4) focus = (focus <= F_UP2) ? F_DISC : F_MODS;
        }
    }
    // Enter / controller CROSS / mouse click on `f`.
    void activate(int f) {
        focus = f == F_CLEAR ? (int)F_MODS : f;
        if (f == F_BACK) { close_mods(); return; }
        if (f == F_MODS && !page_mods) { open_mods(); return; }
        if (f == F_START) { if (can_start()) start_requested = true; else say("DROP YOUR VIB-RIBBON .CUE FILE FIRST"); }
        else if (f >= F_UP1 && f <= F_UP4) set_upscale(f - F_UP1 + 1);
        else if (is_opt(f) || f == F_OPT_GLOW) toggle_opt(f);
        else if (f == F_CLEAR) clear_mods();
        else if (f == F_DISC) say("DRAG YOUR .CUE FILE ONTO THIS WINDOW");
        else if (f == F_MODS) say("DRAG A MODS .ZIP ONTO THIS WINDOW (OPTIONAL)");
    }
};

// ---- layout ------------------------------------------------------------------------------------------------------
struct Ellipse { float cx, cy, rx, ry; };
inline Ellipse shape_of(int f, bool mods_page = false) {
    if (mods_page) {
        switch (f) {
        case F_MODS: return {820, 400, 560, 102};
        case F_CLEAR: return {1330, 400, 40, 30};
        case F_OPT_GLOW: return {960, 640, 330, 52};
        case F_BACK: return {960, 942, 330, 58};
        default: return {0, 0, 1, 1};
        }
    }
    switch (f) {
    case F_DISC: return {520, 385, 400, 102};
    case F_MODS: return {1400, 385, 400, 102};
    case F_UP1: return {585, 600, 105, 44};
    case F_UP2: return {835, 600, 105, 44};
    case F_UP3: return {1085, 600, 105, 44};
    case F_UP4: return {1335, 600, 105, 44};
    case F_OPT_GLOW: return {0, 0, 1, 1};   // lives on the mods page
    case F_OPT_FF: return {620, 825, 300, 44};
    case F_OPT_SHOT: return {1300, 825, 300, 44};
    case F_START: return {960, 942, 330, 58};
    case F_CLEAR: return {0, 0, 1, 1};
    default: return {0, 0, 1, 1};
    }
}
inline int hit_test(const Model& m, float x, float y) {
    static const int main_ids[] = {F_START, F_OPT_FF, F_OPT_SHOT, F_UP1, F_UP2, F_UP3, F_UP4, F_DISC, F_MODS};
    static const int mods_ids[] = {F_CLEAR, F_BACK, F_OPT_GLOW, F_MODS};
    const int* ids = m.page_mods ? mods_ids : main_ids; const int n = m.page_mods ? 4 : 9;
    for (int k = 0; k < n; ++k) {
        const int f = ids[k];
        if (f == F_CLEAR && m.s.mods.empty()) continue;
        Ellipse e = shape_of(f, m.page_mods);
        float a = (x - e.cx) / e.rx, b = (y - e.cy) / e.ry;
        if (a * a + b * b <= 1.0f) return f;
    }
    return F_NONE;
}

// Shrinks the text size so `s` fits in `maxw` pixels (never grows it).
inline float fit(const std::string& s, float unit, float maxw) { float w = vec::text_width(s, unit); return w > maxw ? unit * maxw / w : unit; }

inline std::string shorten(const std::string& s, size_t max) {   // keeps the start and the extension: "very_long_na~.cue"
    if (s.size() <= max || max < 8) return s;
    return s.substr(0, max - 5) + "~" + s.substr(s.size() - 4);
}

// ---- drawing -----------------------------------------------------------------------------------------------------
inline void draw(vec::Canvas& cv, const Model& m, int frame = 0) {
    using vec::rgb;
    const uint32_t WHITE = rgb(235, 235, 235), DIM = rgb(105, 105, 105), GREEN = rgb(130, 255, 170), RED = rgb(255, 120, 120), VIOLET = rgb(150, 150, 255);
    if (cv.w != kW || cv.h != kH) cv.resize(kW, kH);
    cv.clear();

    // title
    vec::text(cv, 960, 55, 13, "VIB-RIBBON", WHITE, 7.0f, 1);
    vec::text(cv, 960, 160, 5, "RECOMP LAUNCHER", DIM, 3.5f, 1);

    // a ribbon with every obstacle (loop, wave, pit, block), across the top: the game's own motif
    {
        std::vector<std::pair<float, float>> r; float x = 140, y = 232;
        auto go = [&](float dx, float dy) { x += dx; y += dy; r.push_back({x, y}); };
        r.push_back({x, y}); go(430, 0);
        { const float R = 44.0f, c = 15.0f, x0 = x, y0 = y;   // loop: a prolate cycloid that crosses itself
          for (int i = 1; i <= 28; ++i) { float a = 6.2831853f * i / 28.0f; x = x0 + c * a - R * std::sin(a); y = y0 - R * (1.0f - std::cos(a)); r.push_back({x, y}); } }
        go(90, 0);
        for (int i = 0; i < 4; ++i) { go(13, -20); go(26, 40); go(13, -20); }   // wave
        go(100, 0); go(45, 38); go(45, -38);                                      // pit
        go(110, 0); go(0, -34); go(64, 0); go(0, 34);                             // block
        go(1780 - x, 0);   // flat to the right edge
        cv.polyline(r, WHITE, 3.5f);
    }

    vec::text(cv, 1860, 100, 3.2f, "V0.13.0", DIM, 2.2f, 2);
    if (m.page_mods) {   // ---- the LIST MODS page: mods zip + neon glow ----
        auto bub = [&](int f, uint32_t col, float width) {
            Ellipse e = shape_of(f, true); const bool hv = m.hover == f, fo = m.focus == f;
            cv.bubble(e.cx, e.cy, e.rx, e.ry, hv ? WHITE : col, hv ? width + 1.5f : width);
            if (fo) cv.bubble(e.cx, e.cy, e.rx + 16, e.ry + 12, VIOLET, 3.0f, 0.9f);
        };
        vec::text(cv, 960, 300, 5, "MODS", DIM, 3.0f, 1);
        { Ellipse e = shape_of(F_MODS, true); const bool have = !m.s.mods.empty();
          bub(F_MODS, have ? (m.zip.ok ? VIOLET : RED) : WHITE, 3.5f);
          if (!have) { vec::text(cv, e.cx, e.cy - 30, 6, "DROP A MODS .ZIP", WHITE, 3.5f, 1); vec::text(cv, e.cx, e.cy + 36, 4, "OPTIONAL - NO MODS YET", DIM, 2.5f, 1); }
          else {
              std::string n = upper(file_name(m.s.mods)); vec::text(cv, e.cx, e.cy - 30, fit(n, 6, 900), n, WHITE, 3.5f, 1);
              vec::text(cv, e.cx, e.cy + 28, fit(m.zip.msg, 4, 900), m.zip.msg, m.zip.ok ? VIOLET : RED, 2.5f, 1);
              if (m.zip.ok) vec::text(cv, e.cx, e.cy + 66, 3.4f, "NOT SUPPORTED YET", DIM, 2.2f, 1);
              Ellipse c = shape_of(F_CLEAR, true);
              cv.bubble(c.cx, c.cy, c.rx, c.ry, m.hover == F_CLEAR ? WHITE : DIM, 3.0f);
              vec::text(cv, c.cx, c.cy - 13, 4.4f, "X", m.hover == F_CLEAR ? WHITE : DIM, 3.0f, 1);
          } }
        { Ellipse e = shape_of(F_OPT_GLOW, true); const bool on = m.s.glow;
          bub(F_OPT_GLOW, on ? GREEN : DIM, on ? 4.0f : 3.0f);
          const float bx = e.cx - e.rx + 70, by = e.cy, hs = 22;
          cv.polyline({{bx - hs, by - hs}, {bx + hs, by - hs}, {bx + hs, by + hs}, {bx - hs, by + hs}}, on ? GREEN : (m.hover == F_OPT_GLOW ? WHITE : DIM), 3.5f, true);
          if (on) cv.polyline({{bx - 12, by + 1}, {bx - 3, by + 12}, {bx + 14, by - 13}}, GREEN, 5.5f);
          const float tx = e.cx + 42;
          vec::text(cv, tx, e.cy - 28, 4.6f, "NEON GLOW", on ? GREEN : (m.hover == F_OPT_GLOW ? WHITE : DIM), 3.0f, 1);
          vec::text(cv, tx, e.cy + 8, fit("SOFT GLOW AROUND THE LINES (F9)", 2.8f, 440), "SOFT GLOW AROUND THE LINES (F9)", DIM, 2.0f, 1); }
        { Ellipse e = shape_of(F_BACK, true); bub(F_BACK, GREEN, 4.5f); vec::text(cv, e.cx, e.cy - 28, 7, "BACK", GREEN, 4.5f, 1); }
        if (!m.toast.empty()) vec::text(cv, 960, 1008, fit(m.toast, 4.0f, 1500), m.toast, VIOLET, 2.6f, 1);
        vec::text(cv, 60, 1048, 3.2f, m.bios_ok ? "BIOS: " + upper(m.bios_name) : "BIOS MISSING - DROP BIOS.BIN HERE", m.bios_ok ? GREEN : RED, 2.2f, 0);
        vec::text(cv, 1860, 1048, 3.2f, "ARROWS + ENTER   ESC BACK", DIM, 2.2f, 2);
        return;
    }

    // ---- the focus / hover / selected look for one bubble ----
    auto bubble = [&](int f, uint32_t col, float width) {
        Ellipse e = shape_of(f, m.page_mods);
        const bool hv = m.hover == f, fo = m.focus == f;
        cv.bubble(e.cx, e.cy, e.rx, e.ry, hv ? WHITE : col, hv ? width + 1.5f : width);
        if (fo) cv.bubble(e.cx, e.cy, e.rx + 16, e.ry + 12, VIOLET, 3.0f, 0.9f);
    };

    // ---- 1: disc ----
    {
        Ellipse e = shape_of(F_DISC);
        const bool have = !m.s.cue.empty();
        bubble(F_DISC, m.disc.ok ? GREEN : (have ? RED : WHITE), 3.5f);
        vec::text(cv, e.cx, e.cy - 64, 3.6f, "1  DISC", DIM, 2.3f, 1);
        if (!have) {
            vec::text(cv, e.cx, e.cy - 30, 6, "DROP YOUR .CUE", WHITE, 3.5f, 1);
            vec::text(cv, e.cx, e.cy + 28, 6, "ON THIS WINDOW", WHITE, 3.5f, 1);
        } else {
            { std::string n = upper(file_name(m.s.cue)); vec::text(cv, e.cx, e.cy - 30, fit(n, 6, 680), n, WHITE, 3.5f, 1); }
            vec::text(cv, e.cx, e.cy + 36, fit(m.disc.msg, 4, 640), m.disc.msg, m.disc.ok ? GREEN : RED, 2.5f, 1);
        }
    }
    // ---- 2: list mods (opens the mods page) ----
    {
        Ellipse e = shape_of(F_MODS);
        const bool have = !m.s.mods.empty();
        bubble(F_MODS, have ? (m.zip.ok ? VIOLET : RED) : WHITE, 3.5f);
        vec::text(cv, e.cx, e.cy - 64, 3.6f, "2  MODS + GLOW", DIM, 2.3f, 1);
        vec::text(cv, e.cx, e.cy - 30, 6, "LIST MODS", WHITE, 3.5f, 1);
        vec::text(cv, e.cx, e.cy + 36, 4, have ? "MODS ZIP ADDED" : "NEON GLOW AND MODS ZIP", have ? VIOLET : DIM, 2.5f, 1);
    }
    // ---- 3: upscale ----
    {
        vec::text(cv, 960, 505, 5, "3  UPSCALE", DIM, 3.0f, 1);
        static const char* names[4] = {"1X", "2X", "3X", "4X"};
        for (int i = 0; i < 4; ++i) {
            const int f = F_UP1 + i; Ellipse e = shape_of(f);
            const bool sel = m.s.upscale == i + 1;
            bubble(f, sel ? GREEN : DIM, sel ? 4.5f : 3.0f);
            if (sel) cv.bubble(e.cx, e.cy, e.rx - 12, e.ry - 9, GREEN, 3.0f, 0.8f);
            vec::text(cv, e.cx, e.cy - 25, 8.5f, names[i], sel ? GREEN : (m.hover == f ? WHITE : DIM), 5.0f, 1);
        }
        static const char* caps[4] = {"ORIGINAL", "", "", "~1080P"};
        for (int i = 0; i < 4; ++i) if (caps[i][0]) vec::text(cv, shape_of(F_UP1 + i).cx, 650, 3.4f, caps[i], DIM, 2.2f, 1);
        const int n = m.s.upscale;
        std::string info = n == 1 ? "ORIGINAL PS1 PIXELS (512 X 256)" : "LINES DRAWN AT " + std::to_string(512 * n) + " X " + std::to_string(256 * n) + " AND SMOOTHED";
        vec::text(cv, 960, 686, 4, info, WHITE, 2.5f, 1);
    }
    // ---- 4: options (check boxes) ----
    {
        vec::text(cv, 960, 718, 5, "4  OPTIONS", DIM, 3.0f, 1);
        struct Opt { int f; const char* name; const char* hint; bool on; };
        const Opt opts[2] = {{F_OPT_FF, "FAST-FORWARD", "HOLD TAB OR PRESS R-STICK", m.s.ffwd},
                             {F_OPT_SHOT, "SCREENSHOTS", "F12 SAVES A .BMP PICTURE", m.s.shot}};
        for (const Opt& o : opts) {
            Ellipse e = shape_of(o.f);
            bubble(o.f, o.on ? GREEN : DIM, o.on ? 4.0f : 3.0f);
            const float bx = e.cx - e.rx + 70, by = e.cy, hs = 22;   // the check box
            cv.polyline({{bx - hs, by - hs}, {bx + hs, by - hs}, {bx + hs, by + hs}, {bx - hs, by + hs}}, o.on ? GREEN : (m.hover == o.f ? WHITE : DIM), 3.5f, true);
            if (o.on) cv.polyline({{bx - 12, by + 1}, {bx - 3, by + 12}, {bx + 14, by - 13}}, GREEN, 5.5f);
            const float tx = e.cx + 42;
            vec::text(cv, tx, e.cy - 28, fit(o.name, 4.6f, 330), o.name, o.on ? GREEN : (m.hover == o.f ? WHITE : DIM), 3.0f, 1);
            vec::text(cv, tx, e.cy + 8, fit(o.hint, 2.8f, 330), o.hint, DIM, 2.0f, 1);
        }
    }
    // ---- start ----
    {
        Ellipse e = shape_of(F_START);
        const bool ok = m.can_start();
        bubble(F_START, ok ? GREEN : DIM, ok ? 5.0f : 3.0f);
        if (ok) cv.bubble(e.cx, e.cy, e.rx - 14, e.ry - 11, GREEN, 3.0f, 0.8f);
        vec::text(cv, e.cx, e.cy - (ok ? 34 : 48), ok ? 9 : 8.5f, "START", ok ? GREEN : DIM, 5.5f, 1);
        if (!ok) vec::text(cv, e.cx, e.cy + 18, 3.4f, "DROP YOUR DISC FIRST", DIM, 2.4f, 1);
    }
    // ---- toast + footer ----
    if (!m.toast.empty()) vec::text(cv, 960, 1008, fit(m.toast, 4.0f, 1500), m.toast, VIOLET, 2.6f, 1);
    vec::text(cv, 60, 1048, 3.2f, m.bios_ok ? "BIOS: " + upper(m.bios_name) : "BIOS MISSING - DROP BIOS.BIN HERE", m.bios_ok ? GREEN : RED, 2.2f, 0);
    vec::text(cv, 1860, 1048, 3.2f, "ARROWS + ENTER   F11 FULLSCREEN   ESC QUIT", DIM, 2.2f, 2);
    (void)frame;
}

}  // namespace launcher
