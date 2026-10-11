// Reads a PS1 BIN/CUE image (data track only) and loads PS-X EXEs.
// No SDL dependency so it can be tested headlessly.
#pragma once
#include <algorithm>
#include <cctype>
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
        size_t q1 = t.find('"'), q2 = q1 == std::string::npos ? q1 : t.find('"', q1 + 1);
        if (q2 == std::string::npos) return "No FILE entry in cue sheet";
        std::string bin = t.substr(q1 + 1, q2 - q1 - 1);
        size_t slash = cue_path.find_last_of("/\\");
        std::string dir = slash == std::string::npos ? "" : cue_path.substr(0, slash + 1);
        bin_.open(dir + bin, std::ios::binary);
        if (!bin_) return "Cannot open " + dir + bin;
        if (t.find("MODE2/2352") != std::string::npos) { ss_ = 2352; off_ = 24; }
        else if (t.find("MODE1/2352") != std::string::npos) { ss_ = 2352; off_ = 16; }
        else if (t.find("MODE1/2048") != std::string::npos) { ss_ = 2048; off_ = 0; }
        else return "Unsupported track mode";
        auto pvd = read(16, 2048);
        if (pvd.size() < 2048 || pvd[1] != 'C' || pvd[2] != 'D') return "Not an ISO9660 disc";
        uint32_t root = u32(pvd, 158), rlen = u32(pvd, 166);
        entries_.clear();
        walk(root, rlen, "");
        return "";
    }

    const std::vector<Entry>& entries() const { return entries_; }

    bool read_file(const std::string& name, std::vector<uint8_t>& out) {
        std::string want = upper(name);
        for (auto& e : entries_)
            if (upper(e.path) == want) { out = read(e.lba, e.size); return out.size() == e.size; }
        return false;
    }

private:
    std::ifstream bin_;
    uint32_t ss_ = 2352, off_ = 24;
    std::vector<Entry> entries_;

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
            bin_.clear();
            bin_.seekg((std::streamoff)(lba + i) * ss_ + off_);
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
