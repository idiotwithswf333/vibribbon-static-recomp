// runlog_test -- checks the log file is written, rotated to -previous, capped, and saved on destruction.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "../src/runlog.h"
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
static std::string slurp(const std::string& p) { std::ifstream f(p); std::stringstream s; s << f.rdbuf(); return s.str(); }
int main() {
    namespace fs = std::filesystem;
    fs::path d = fs::temp_directory_path() / "vibribbon_runlog_test"; fs::remove_all(d); fs::create_directories(d);
    {
        runlog::Log a; CHECK(a.open(d.string()));
        a.note("hello %d", 42); a.set_summary("frames: 7");
        for (int i = 0; i < 1000; ++i) a.noisy("press %d", i);
    }   // destructor saves
    std::string t = slurp((d / "vibribbon-log.txt").string());
    CHECK(t.find("hello 42") != std::string::npos);
    CHECK(t.find("frames: 7") != std::string::npos);
    CHECK(t.find("press 399") != std::string::npos && t.find("press 400") == std::string::npos);   // noisy cap
    CHECK(t.find("600 further button-press lines") != std::string::npos);
    {
        runlog::Log b; CHECK(b.open(d.string())); b.note("second run");
    }
    CHECK(slurp((d / "vibribbon-log-previous.txt").string()).find("hello 42") != std::string::npos);   // rotated
    CHECK(slurp((d / "vibribbon-log.txt").string()).find("second run") != std::string::npos);
    runlog::Log bad; CHECK(!bad.open((d / "no_such_dir" / "x").string()));                               // unwritable: no crash
    bad.note("ignored"); bad.save();
    fs::remove_all(d);
    if (failures == 0) std::puts("runlog: all tests passed");
    return failures ? 1 : 0;
}
