// Bathos - writing a depth map: .png keeps it lossless, anything else is a JPEG
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#ifdef _WIN32
#define STBIW_WINDOWS_UTF8
#endif
#include "stb_image_write.h"

inline bool saveDepth(const std::string& path, const std::vector<uint8_t>& px, int w, int h, int ch, int quality = 95) {
    const size_t dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    if (ext == "png") return stbi_write_png(path.c_str(), w, h, ch, px.data(), w * ch) != 0;
    return stbi_write_jpg(path.c_str(), w, h, ch, px.data(), quality) != 0;
}
