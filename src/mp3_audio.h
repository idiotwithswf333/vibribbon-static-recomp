// MP3 file -> 44.1 kHz stereo PCM, using the public-domain minimp3 (fetched by CMake). Only main.cpp includes this.
#pragma once
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#define MINIMP3_IMPLEMENTATION
#include "minimp3_ex.h"
#include "vcd.h"

namespace mp3audio {

// `path_utf8` is what SDL hands over for a dropped file (UTF-8, so Japanese etc. file names work on Windows).
inline bool load(const std::string& path_utf8, std::vector<int16_t>& stereo44k, std::string& err) {
    std::ifstream f(std::filesystem::u8path(path_utf8), std::ios::binary | std::ios::ate);
    if (!f) { err = "cannot open file"; return false; }
    const std::streamoff size = f.tellg();
    if (size <= 0) { err = "empty file"; return false; }
    std::vector<uint8_t> buf((size_t)size);
    f.seekg(0); f.read((char*)buf.data(), size);
    if (!f) { err = "cannot read file"; return false; }
    mp3dec_t dec; mp3dec_init(&dec);
    mp3dec_file_info_t info{};
    if (mp3dec_load_buf(&dec, buf.data(), buf.size(), &info, nullptr, nullptr) != 0 || !info.buffer || info.samples == 0 || info.channels < 1) {
        if (info.buffer) std::free(info.buffer);
        err = "not a readable MP3"; return false;
    }
    stereo44k = vcd::to_44k_stereo((const int16_t*)info.buffer, info.samples / (size_t)info.channels, info.channels, info.hz);
    std::free(info.buffer);
    if (stereo44k.empty()) { err = "no audio found"; return false; }
    return true;
}

}  // namespace mp3audio
