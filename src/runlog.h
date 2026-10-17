// Run log: collects timestamped notes while the app runs and saves them to vibribbon-log.txt in the folder
// given to open() (main.cpp passes the folder the exe lives in). The previous run's log is kept as
// vibribbon-log-previous.txt. The file is rewritten every few seconds and again on close, so even a crash leaves a
// log behind. No SDL dependency, so it can be tested headlessly.
#pragma once
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

namespace runlog {

class Log {
public:
    static constexpr size_t kMaxLines = 6000;    // hard cap on stored lines
    static constexpr size_t kMaxNoisy = 400;     // per-input lines (button presses) are only logged up to this many

    // Returns false if the folder is not writable (the app keeps running, it just has no log).
    bool open(const std::string& dir) {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path d = dir.empty() ? fs::path(".") : fs::path(dir);
        path_ = (d / "vibribbon-log.txt").string();
        std::string prev = (d / "vibribbon-log-previous.txt").string();
        if (fs::exists(path_, ec)) { fs::remove(prev, ec); fs::rename(path_, prev, ec); }
        start_ = std::chrono::steady_clock::now();
        std::time_t t = std::time(nullptr); char buf[64] = "";
        std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
        started_at_ = buf;
        std::FILE* f = std::fopen(path_.c_str(), "w");
        if (!f) { opened_ = false; return false; }
        std::fclose(f);
        opened_ = true;
        save();
        return true;
    }
    bool is_open() const { return opened_; }
    const std::string& path() const { return path_; }
    double elapsed() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count(); }

    void note(const char* fmt, ...) {          // always stored (until kMaxLines)
        va_list ap; va_start(ap, fmt); add(fmt, ap); va_end(ap);
    }
    void noisy(const char* fmt, ...) {         // high-volume lines: only the first kMaxNoisy are stored
        if (noisy_seen_++ >= kMaxNoisy) { ++noisy_dropped_; return; }
        va_list ap; va_start(ap, fmt); add(fmt, ap); va_end(ap);
    }
    void set_summary(const std::string& s) { summary_ = s; }   // rewritten by the caller before each save()

    void save() {
        if (!opened_) return;
        std::FILE* f = std::fopen(path_.c_str(), "w");
        if (!f) return;
        std::fprintf(f, "Vib-Ribbon run log\nStarted: %s\nLog written: %.1f s after start\n\n", started_at_.c_str(), elapsed());
        for (const auto& l : lines_) std::fprintf(f, "%s\n", l.c_str());
        if (dropped_lines_) std::fprintf(f, "(%zu further lines not stored: log is full)\n", dropped_lines_);
        if (noisy_dropped_) std::fprintf(f, "(%zu further button-press lines not stored)\n", noisy_dropped_);
        if (!summary_.empty()) std::fprintf(f, "\n---- summary ----\n%s\n", summary_.c_str());
        std::fclose(f);
    }
    ~Log() { save(); }

private:
    void add(const char* fmt, va_list ap) {
        char msg[1024]; std::vsnprintf(msg, sizeof msg, fmt, ap);
        if (lines_.size() >= kMaxLines) { ++dropped_lines_; return; }
        char head[32]; std::snprintf(head, sizeof head, "[%8.1fs] ", elapsed());
        lines_.push_back(std::string(head) + msg);
    }
    bool opened_ = false;
    std::string path_, started_at_, summary_;
    std::vector<std::string> lines_;
    size_t noisy_seen_ = 0, noisy_dropped_ = 0, dropped_lines_ = 0;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

}  // namespace runlog
