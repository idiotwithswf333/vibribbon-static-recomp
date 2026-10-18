// launcher_test [game.cue] [out_prefix] -- model logic + renders the launcher in several states to PPM files.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include "../src/launcher.h"
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
static void save_ppm(const vec::Canvas& cv, const std::string& fn) {
    FILE* o = std::fopen(fn.c_str(), "wb"); std::fprintf(o, "P6\n%d %d\n255\n", cv.w, cv.h);
    for (uint32_t p : cv.px) { uint8_t b[3] = {(uint8_t)p, (uint8_t)(p >> 8), (uint8_t)(p >> 16)}; std::fwrite(b, 1, 3, o); } std::fclose(o);
}
int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    fs::path d = fs::temp_directory_path() / "vibribbon_launcher_test"; fs::remove_all(d); fs::create_directories(d);
    // ---- settings file ---------------------------------------------------------------------------------------------------
    {
        launcher::Settings s; s.cue = "C:\\games\\vib ribbon.cue"; s.mods = "D:\\mods\\a.zip"; s.upscale = 3;
        std::string f = (d / "settings.ini").string();
        CHECK(launcher::save_settings(f, s));
        launcher::Settings t; CHECK(launcher::load_settings(f, t));
        CHECK(t.cue == s.cue && t.mods == s.mods && t.upscale == 3);
        CHECK(!t.glow && t.ffwd && t.shot);                                                  // option defaults survive a round trip
        s.glow = true; s.ffwd = false; s.shot = false; CHECK(launcher::save_settings(f, s));
        launcher::Settings t2; CHECK(launcher::load_settings(f, t2)); CHECK(t2.glow && !t2.ffwd && !t2.shot);
        std::ofstream(f) << "cue=x\n"; launcher::Settings old; launcher::load_settings(f, old); CHECK(!old.glow && old.ffwd && old.shot);   // old ini without the keys
        std::ofstream(f) << "upscale=99\ncue=x\n"; launcher::Settings u; launcher::load_settings(f, u); CHECK(u.upscale == 4);   // clamped
        launcher::Settings none; CHECK(!launcher::load_settings((d / "missing.ini").string(), none) && none.upscale == 1);
    }
    // ---- own-CD playlist (dropped MP3s) survives a round trip, in order ----
    {
        launcher::Settings s; s.cue = "x.cue"; s.mp3 = {"C:\\music\\one.mp3", "C:\\music\\two.mp3"};
        std::string f = (d / "mp3.ini").string(); CHECK(launcher::save_settings(f, s));
        launcher::Settings t; CHECK(launcher::load_settings(f, t)); CHECK(t.mp3.size() == 2 && t.mp3[0] == s.mp3[0] && t.mp3[1] == s.mp3[1]);
        launcher::Settings none; std::ofstream(f) << "cue=x\n"; launcher::load_settings(f, none); CHECK(none.mp3.empty());
    }
    // ---- zip check -------------------------------------------------------------------------------------------------------
    {
        std::string z = (d / "m.zip").string();
        { std::ofstream f(z, std::ios::binary); const char hdr[] = {'P','K',3,4}; f.write(hdr, 4); for (int i = 0; i < 50; ++i) f.put('x');
          char eocd[22] = {'P','K',5,6}; eocd[10] = 3; eocd[8] = 3; f.write(eocd, 22); }
        auto r = launcher::check_zip(z); CHECK(r.ok && r.files == 3 && r.msg == "3 FILES");
        std::string bad = (d / "n.zip").string(); std::ofstream(bad) << "this is not a zip";
        CHECK(!launcher::check_zip(bad).ok && !launcher::check_zip((d / "nope.zip").string()).ok && !launcher::check_zip("").ok);
    }
    // ---- disc check ------------------------------------------------------------------------------------------------------
    {
        std::string bin = (d / "x.bin").string(); std::ofstream(bin) << "data";
        CHECK(!launcher::check_disc((d / "none.cue").string()).ok);
        CHECK(launcher::check_disc(bin).msg.find(".CUE") != std::string::npos);   // told to drop the cue, not the bin
        std::string cue = (d / "bad.cue").string(); std::ofstream(cue) << "FILE \"missing.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n";
        CHECK(!launcher::check_disc(cue).ok);
        if (argc > 1) { auto r = launcher::check_disc(argv[1]); std::printf("real disc: %s\n", r.msg.c_str()); CHECK(r.ok); }
    }
    // ---- model: drop routing, navigation, start gating --------------------------------------------------------------------
    {
        launcher::Model m; m.init(launcher::Settings(), true, "bios.bin");
        CHECK(m.focus == launcher::F_DISC && !m.can_start());
        m.activate(launcher::F_START); CHECK(!m.start_requested && !m.toast.empty());   // refuses without a disc
        std::string z = (d / "m.zip").string();
        CHECK(m.drop(z, false) == "mods" && m.zip.ok && m.s.mods == z);
        CHECK(m.drop((d / "a.bin").string(), false) == "cue" && !m.disc.ok);               // wrong file goes to the disc slot and is rejected
        CHECK(m.drop("whatever/bios.bin", true) == "bios");
        m.disc.ok = true; m.s.cue = "x.cue";                                                // pretend a valid disc
        m.focus = launcher::F_DISC; m.nav(1, 0); CHECK(m.focus == launcher::F_MODS);
        m.nav(0, 1); CHECK(m.focus == launcher::F_UP1);                                     // down from mods -> the selected size's chip
        m.nav(1, 0); CHECK(m.focus == launcher::F_UP2 && m.s.upscale == 2);                 // moving over chips selects them
        m.nav(1, 0); m.nav(1, 0); m.nav(1, 0); CHECK(m.focus == launcher::F_UP4 && m.s.upscale == 4);   // clamped at 4x
        m.nav(0, 1); CHECK(m.focus == launcher::F_OPT_FF);                                  // down from the sizes -> the option check boxes
        m.nav(1, 0); m.nav(1, 0); CHECK(m.focus == launcher::F_OPT_SHOT);                   // clamped at the last box
        m.nav(0, 1); CHECK(m.focus == launcher::F_START);
        m.nav(0, -1); CHECK(m.focus == launcher::F_OPT_FF);
        m.nav(0, -1); CHECK(m.focus == launcher::F_UP4);
        m.nav(0, -1); CHECK(m.focus == launcher::F_MODS);                                   // up from a right-hand chip -> mods
        CHECK(!m.s.glow && m.s.ffwd && m.s.shot);                                           // defaults: glow off, fast-forward + screenshots on
        m.activate(launcher::F_MODS); CHECK(m.page_mods);                                  // LIST MODS opens the sub-page
        m.activate(launcher::F_OPT_GLOW); CHECK(m.s.glow && m.focus == launcher::F_OPT_GLOW);   // neon glow box lives there
        m.activate(launcher::F_BACK); CHECK(!m.page_mods);
        m.activate(launcher::F_OPT_FF); CHECK(!m.s.ffwd);
        m.activate(launcher::F_OPT_SHOT); CHECK(!m.s.shot);
        m.activate(launcher::F_OPT_FF); CHECK(m.s.ffwd);
        { auto o = launcher::shape_of(launcher::F_OPT_SHOT); CHECK(launcher::hit_test(m, o.cx, o.cy) == launcher::F_OPT_SHOT); }
        m.activate(launcher::F_UP1); CHECK(m.s.upscale == 1);
        m.clear_mods(); CHECK(m.s.mods.empty() && !m.zip.ok);
        m.activate(launcher::F_START); CHECK(m.start_requested);
        // hit testing
        auto e = launcher::shape_of(launcher::F_START); CHECK(launcher::hit_test(m, e.cx, e.cy) == launcher::F_START);
        e = launcher::shape_of(launcher::F_UP3); CHECK(launcher::hit_test(m, e.cx, e.cy) == launcher::F_UP3);
        CHECK(launcher::hit_test(m, 5, 5) == launcher::F_NONE);
        for (int i = 0; i < 300; ++i) { m.tick(); }
        CHECK(m.toast.empty());
    }
    // ---- screenshots ------------------------------------------------------------------------------------------------------
    if (argc > 2) {
        vec::Canvas cv; std::string p = argv[2];
        launcher::Model a; a.init(launcher::Settings(), false, ""); launcher::draw(cv, a); save_ppm(cv, p + "_a_empty.ppm");
        launcher::Model b; launcher::Settings s; s.cue = argc > 1 ? argv[1] : "vibribbon.cue"; s.mods = (d / "m.zip").string(); s.upscale = 4;
        b.init(s, true, "bios.bin"); b.hover = launcher::F_UP2; b.say("DISC READY - PRESS START"); launcher::draw(cv, b); save_ppm(cv, p + "_b_ready.ppm");
        launcher::Model c; launcher::Settings s2; s2.cue = (d / "a.bin").string(); s2.mods = (d / "n.zip").string(); s2.upscale = 2;
        std::ofstream(s2.cue) << "x"; c.init(s2, true, "bios.bin"); c.focus = launcher::F_MODS; launcher::draw(cv, c); save_ppm(cv, p + "_c_errors.ppm");
    }
    fs::remove_all(d);
    if (failures == 0) std::puts("launcher: all tests passed");
    return failures ? 1 : 0;
}
