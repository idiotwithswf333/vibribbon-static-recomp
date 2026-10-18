// Reads a PS1 BIN/CUE image and loads PS-X EXEs. Works with a single .bin (all tracks in one file) and with one
// .bin per track: every FILE in the cue sheet becomes a segment of one continuous disc, in order.
// No SDL dependency so it can be tested headlessly.
#pragma once
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace disc {

struct Entry { std::string path; uint32_t lba, size; };

struct PsxExe {
    uint32_t pc = 0, gp = 0, load_addr = 0, sp = 0;
    std::vector<uint8_t> text;
};

class Image {
public:
    // Returns empty string on success, else an error message.
    std::string open(const std::string& cue_path) {
        std::ifstream cue(cue_path);
        if (!cue) return "Cannot open cue sheet";
        std::stringstream ss; ss << cue.rdbuf();
        std::string t = ss.str();
        size_t slash = cue_path.find_last_of("/\\");
        std::string dir = slash == std::string::npos ? "" : cue_path.substr(0, slash + 1);
        segs_.clear(); tracks_.clear(); open_seg_ = -1; bin_.close();
        std::vector<std::pair<int, uint32_t>> idx1;   // INDEX 01 of each track: (segment, frames from the start of its file)
        {
            std::istringstream lines(t);
            for (std::string ln; std::getline(lines, ln);) {
                size_t q1 = ln.find('"'), q2 = q1 == std::string::npos ? q1 : ln.find('"', q1 + 1);
                if (ln.find("FILE") != std::string::npos && q2 != std::string::npos && ln.find("FILE") < q1) { Seg g; g.path = dir + ln.substr(q1 + 1, q2 - q1 - 1); segs_.push_back(g); continue; }
                size_t p = ln.find("INDEX 01 ");
                if (p != std::string::npos && !segs_.empty()) {
                    int mm = 0, ss = 0, ff = 0; std::sscanf(ln.c_str() + p + 9, "%d:%d:%d", &mm, &ss, &ff);
                    idx1.push_back({(int)segs_.size() - 1, (uint32_t)((mm * 60 + ss) * 75 + ff)});
                }
            }
        }
        if (segs_.empty()) return "No FILE entry in cue sheet";
        for (size_t i = 0; i < segs_.size(); ++i) {   // cue sheets often name files that were later renamed: fall back to "Track N"
            std::error_code ec;
            if (!std::filesystem::exists(segs_[i].path, ec)) { std::string alt = find_track_file(dir, (int)i + 1); if (!alt.empty()) segs_[i].path = alt; }
        }
        if (t.find("MODE2/2352") != std::string::npos) { ss_ = 2352; off_ = 24; }
        else if (t.find("MODE1/2352") != std::string::npos) { ss_ = 2352; off_ = 16; }
        else if (t.find("MODE1/2048") != std::string::npos) { ss_ = 2048; off_ = 0; }
        else return "Unsupported track mode";
        {   // size of every file => where each segment starts on the disc
            uint32_t at = 0;
            for (auto& g : segs_) {
                std::ifstream f(g.path, std::ios::binary | std::ios::ate);
                if (!f) return "Cannot open " + g.path;
                g.start = at; g.count = (uint32_t)((uint64_t)f.tellg() / ss_); at += g.count;
            }
            for (auto& i : idx1) tracks_.push_back(segs_[(size_t)i.first].start + i.second);
        }
        auto pvd = read(16, 2048);
        if (pvd.size() < 2048 || pvd[1] != 'C' || pvd[2] != 'D') return "Not an ISO9660 disc";
        uint32_t root = u32(pvd, 158), rlen = u32(pvd, 166);
        entries_.clear();
        walk(root, rlen, "");
        return "";
    }

    const std::vector<Entry>& entries() const { return entries_; }

    // Raw sector (as stored in the .bin). Returns false past the end of the image.
    bool raw_sector(uint32_t lba, uint8_t* out, uint32_t* size) {
        *size = ss_;
        if (!seek_sector(lba, 0)) return false;
        bin_.read((char*)out, ss_);
        return bin_.gcount() == (std::streamsize)ss_;
    }
    uint32_t data_offset() const { return off_; }
    uint32_t sector_size() const { return ss_; }
    uint32_t total_sectors() const { return segs_.empty() ? 0 : segs_.back().start + segs_.back().count; }
    const std::vector<uint32_t>& track_starts() const { return tracks_; }  // frames, index 01 per track (from cue)

    bool read_file(const std::string& name, std::vector<uint8_t>& out) {
        std::string want = upper(name);
        for (auto& e : entries_)
            if (upper(e.path) == want) { out = read(e.lba, e.size); return out.size() == e.size; }
        return false;
    }

private:
    struct Seg { std::string path; uint32_t start = 0, count = 0; };
    std::vector<Seg> segs_;
    int open_seg_ = -1;
    std::ifstream bin_;
    // The one .bin in `dir` whose name has "Track <n>" in it (e.g. "Game (Track 2).bin"), or "" if none/ambiguous.
    static std::string find_track_file(const std::string& dir, int n) {
        namespace fs = std::filesystem;
        std::error_code ec; std::string hit;
        for (auto& e : fs::directory_iterator(dir.empty() ? std::string(".") : dir, ec)) {
            std::string name = e.path().filename().string(), low = name;
            for (auto& c : low) c = (char)std::tolower((unsigned char)c);
            if (low.size() < 4 || low.substr(low.size() - 4) != ".bin") continue;
            for (size_t p = low.find("track "); p != std::string::npos; p = low.find("track ", p + 1)) {
                int v = std::atoi(low.c_str() + p + 6);
                if (v == n) { if (!hit.empty()) return ""; hit = e.path().string(); break; }
            }
        }
        return hit;
    }
    // Positions bin_ at byte `extra` of disc sector `lba`, opening that sector's file if needed.
    bool seek_sector(uint32_t lba, uint32_t extra) {
        int s = -1;
        for (size_t i = 0; i < segs_.size(); ++i) if (lba >= segs_[i].start && (lba < segs_[i].start + segs_[i].count || i + 1 == segs_.size())) { s = (int)i; break; }
        if (s < 0) return false;
        if (s != open_seg_) { bin_.close(); bin_.clear(); bin_.open(segs_[(size_t)s].path, std::ios::binary); open_seg_ = s; }
        if (!bin_) { open_seg_ = -1; return false; }
        bin_.clear();
        bin_.seekg((std::streamoff)(lba - segs_[(size_t)s].start) * ss_ + extra);
        return true;
    }
    uint32_t ss_ = 2352, off_ = 24;
    std::vector<Entry> entries_;
    std::vector<uint32_t> tracks_;

    static uint32_t u32(const std::vector<uint8_t>& d, size_t i) {
        return d[i] | d[i + 1] << 8 | d[i + 2] << 16 | (uint32_t)d[i + 3] << 24;
    }
    static std::string upper(std::string s) {
        for (auto& c : s) c = (char)std::toupper((unsigned char)c);
        return s;
    }
    std::vector<uint8_t> read(uint32_t lba, uint32_t size) {
        std::vector<uint8_t> out;
        for (uint32_t i = 0; i < (size + 2047) / 2048; ++i) {
            uint8_t sec[2048];
            if (!seek_sector(lba + i, off_)) break;
            bin_.read((char*)sec, 2048);
            if (bin_.gcount() != 2048) break;
            out.insert(out.end(), sec, sec + 2048);
        }
        if (out.size() > size) out.resize(size);
        return out;
    }
    void walk(uint32_t lba, uint32_t size, const std::string& prefix) {
        auto d = read(lba, size);
        size_t i = 0;
        while (i + 33 < d.size()) {
            uint8_t n = d[i];
            if (n == 0) { i = (i / 2048 + 1) * 2048; continue; }
            uint32_t ext = u32(d, i + 2), len = u32(d, i + 10);
            bool is_dir = d[i + 25] & 2;
            uint8_t nl = d[i + 32];
            std::string name((const char*)&d[i + 33], nl);
            i += n;
            if (name == std::string(1, '\0') || name == "\x01") continue;
            name = name.substr(0, name.find(';'));
            if (is_dir) walk(ext, len, prefix + name + "/");
            else entries_.push_back({prefix + name, ext, len});
        }
    }
};

inline bool parse_psx_exe(const std::vector<uint8_t>& f, PsxExe& exe) {
    if (f.size() < 0x800 || std::string((const char*)f.data(), 8) != "PS-X EXE") return false;
    auto w = [&](size_t o) { return f[o] | f[o + 1] << 8 | f[o + 2] << 16 | (uint32_t)f[o + 3] << 24; };
    exe.pc = w(0x10); exe.gp = w(0x14); exe.load_addr = w(0x18);
    uint32_t sz = w(0x1C); exe.sp = w(0x30);
    if (0x800 + (size_t)sz > f.size()) return false;
    exe.text.assign(f.begin() + 0x800, f.begin() + 0x800 + sz);
    return true;
}

// Boot path from SYSTEM.CNF ("BOOT = cdrom:\SCES_028.73;1"), or "" if missing.
inline std::string boot_file(Image& img) {
    std::vector<uint8_t> cnf;
    if (!img.read_file("SYSTEM.CNF", cnf)) return "";
    std::string s(cnf.begin(), cnf.end());
    size_t p = s.find("cdrom:");
    if (p == std::string::npos) return "";
    p += 6;
    while (p < s.size() && (s[p] == '\\' || s[p] == '/')) ++p;
    size_t e = s.find_first_of(";\r\n", p);
    return s.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

}  // namespace disc
