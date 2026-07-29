#include "thumbnail.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <fstream>
#include <array>
#include <algorithm>

#ifdef __linux__
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace thumbnail {

// ── Globals ──────────────────────────────────────────────────────────────────
static bool s_ffmpeg_available = false;
static fs::path s_cache_dir;

// ── Initialization ───────────────────────────────────────────────────────────

void init() {
    // Check if ffmpeg is available
#ifdef __linux__
    int ret = std::system("ffmpeg -version > /dev/null 2>&1");
    s_ffmpeg_available = (ret == 0);
#else
    s_ffmpeg_available = false;
#endif

    if (s_ffmpeg_available) {
        std::cout << "[INFO] ffmpeg found — media thumbnails enabled" << std::endl;
    } else {
        std::cout << "[INFO] ffmpeg not found — using media icons for thumbnails" << std::endl;
    }

    // Create cache directory in system temp (isolated from served files)
    s_cache_dir = fs::path("/tmp/webdav-thumbnails");
    std::error_code ec;
    if (!fs::exists(s_cache_dir, ec)) {
        fs::create_directory(s_cache_dir, ec);
    }
    std::cout << "[INFO] Thumbnail cache: " << s_cache_dir << std::endl;
}

// ── Helpers ──────────────────────────────────────────────────────────────────

bool is_media_file(std::string_view filename) {
    auto dot = filename.rfind('.');
    if (dot == std::string_view::npos) return false;
    std::string ext(filename.substr(dot));
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".mp4"  || ext == ".webm" || ext == ".ogv"  ||
           ext == ".mkv"  || ext == ".avi"  || ext == ".mov"  ||
           ext == ".mp3"  || ext == ".ogg"  || ext == ".opus" ||
           ext == ".flac" || ext == ".wav"  || ext == ".aac"  ||
           ext == ".m4a"  || ext == ".wma";
}

bool is_video_file(std::string_view filename) {
    auto dot = filename.rfind('.');
    if (dot == std::string_view::npos) return false;
    std::string ext(filename.substr(dot));
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".mp4"  || ext == ".webm" || ext == ".ogv" ||
           ext == ".mkv"  || ext == ".avi"  || ext == ".mov";
}

// ── Cache key: hash of file path + mtime ─────────────────────────────────────

static std::string cache_key(const fs::path& filepath) {
    auto ftime = fs::last_write_time(filepath);
    auto t = std::chrono::duration_cast<std::chrono::seconds>(
        ftime.time_since_epoch()).count();
    // Simple hash: djb2 of canonical path + mtime
    std::string key = fs::canonical(filepath).string() + "|" + std::to_string(t);
    uint64_t hash = 5381;
    for (char c : key) {
        hash = ((hash << 5) + hash) + static_cast<unsigned char>(c);
    }
    char hex[17];
    snprintf(hex, sizeof(hex), "%016lx", hash);
    return std::string(hex);
}

static fs::path cache_path_for(const fs::path& filepath, int size) {
    return s_cache_dir / (cache_key(filepath) + "_" + std::to_string(size) + ".jpg");
}

// ── Run ffmpeg to extract thumbnail ──────────────────────────────────────────

#ifdef __linux__
static std::string run_ffmpeg_thumbnail(const fs::path& filepath, int size, bool is_video) {
    if (!s_ffmpeg_available) return {};

    std::string tmp_output = (s_cache_dir / "tmp_thumb.jpg").string();

    std::string cmd;
    if (is_video) {
        // Extract frame at 2 seconds (to avoid black first frames)
        char buf[1024];
        snprintf(buf, sizeof(buf),
            "ffmpeg -y -ss 2 -i '%s' -vframes 1 -vf 'scale=%d:%d:force_original_aspect_ratio=decrease' "
            "-q:v 3 '%s' 2>/dev/null",
            filepath.c_str(), size, size, tmp_output.c_str());
        cmd = buf;
    } else {
        // Audio: try to extract embedded cover art
        char buf[1024];
        snprintf(buf, sizeof(buf),
            "ffmpeg -y -i '%s' -an -vcodec copy -vf 'scale=%d:%d:force_original_aspect_ratio=decrease' "
            "'%s' 2>/dev/null",
            filepath.c_str(), size, size, tmp_output.c_str());
        cmd = buf;
    }

    int ret = std::system(cmd.c_str());
    if (ret != 0 || !fs::exists(tmp_output)) {
        // Try alternative: extract frame at 0 seconds
        if (is_video) {
            char buf2[1024];
            snprintf(buf2, sizeof(buf2),
                "ffmpeg -y -ss 0 -i '%s' -vframes 1 -vf 'scale=%d:%d:force_original_aspect_ratio=decrease' "
                "-q:v 3 '%s' 2>/dev/null",
                filepath.c_str(), size, size, tmp_output.c_str());
            cmd = buf2;
            ret = std::system(cmd.c_str());
        }
    }

    if (ret != 0 || !fs::exists(tmp_output)) {
        return {};
    }

    // Read the generated file
    std::ifstream f(tmp_output, std::ios::binary | std::ios::ate);
    if (!f) return {};
    auto fsize = f.tellg();
    f.seekg(0);
    std::string data(static_cast<size_t>(fsize), '\0');
    f.read(data.data(), fsize);

    // Remove temp
    std::error_code ec;
    fs::remove(tmp_output, ec);

    return data;
}
#else
static std::string run_ffmpeg_thumbnail(const fs::path&, int, bool) {
    return {};
}
#endif

// ── SVG icon generators (fallback) ───────────────────────────────────────────

static std::string svg_video_icon(int size) {
    std::string s;
    s.reserve(1024);
    s += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    s += "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 128 128\" "
         "width=\"" + std::to_string(size) + "\" height=\"" + std::to_string(size) + "\">\n";
    s += "  <rect width=\"128\" height=\"128\" rx=\"12\" fill=\"#2c3e50\"/>\n";
    s += "  <polygon points=\"48,32 48,96 100,64\" fill=\"#3498db\" stroke=\"#3498db\" "
         "stroke-width=\"2\" stroke-linejoin=\"round\"/>\n";
    s += "</svg>\n";
    return s;
}

static std::string svg_audio_icon(int size) {
    std::string s;
    s.reserve(1024);
    s += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    s += "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 128 128\" "
         "width=\"" + std::to_string(size) + "\" height=\"" + std::to_string(size) + "\">\n";
    s += "  <rect width=\"128\" height=\"128\" rx=\"12\" fill=\"#2c3e50\"/>\n";
    s += "  <circle cx=\"64\" cy=\"64\" r=\"36\" fill=\"none\" stroke=\"#e74c3c\" stroke-width=\"6\"/>\n";
    s += "  <circle cx=\"64\" cy=\"64\" r=\"18\" fill=\"#e74c3c\"/>\n";
    s += "  <line x1=\"100\" y1=\"28\" x2=\"100\" y2=\"100\" stroke=\"#e74c3c\" stroke-width=\"5\" "
         "stroke-linecap=\"round\"/>\n";
    s += "</svg>\n";
    return s;
}

// ── Main generator ───────────────────────────────────────────────────────────

std::string generate(const fs::path& filepath, int size) {
    if (size <= 0) size = 256;

    // 1. Check cache
    fs::path cache_file = cache_path_for(filepath, size);
    if (fs::exists(cache_file)) {
        std::ifstream f(cache_file, std::ios::binary | std::ios::ate);
        if (f) {
            auto fsize = f.tellg();
            f.seekg(0);
            std::string data(static_cast<size_t>(fsize), '\0');
            f.read(data.data(), fsize);
            if (!data.empty()) return data;
        }
    }

    // 2. Try ffmpeg
    bool is_vid = is_video_file(filepath.filename().string());
    std::string result = run_ffmpeg_thumbnail(filepath, size, is_vid);

    if (!result.empty()) {
        // Cache it
        std::ofstream f(cache_file, std::ios::binary | std::ios::trunc);
        if (f) {
            f.write(result.data(), static_cast<std::streamsize>(result.size()));
        }
        return result;
    }

    // 3. Fallback: SVG icon
    // Cache SVG icon too (but as .svg so we know)
    fs::path svg_cache = s_cache_dir / (cache_key(filepath) + "_" + std::to_string(size) + ".svg");
    std::string svg = is_vid ? svg_video_icon(size) : svg_audio_icon(size);

    // Don't bother caching SVG — it's fast to generate
    return svg;
}

std::string_view mime_type(const std::string& data) {
    if (data.size() >= 5 && std::string_view(data.data(), 5) == "<?xml") {
        return "image/svg+xml";
    }
    // Check for JPEG magic
    if (data.size() >= 2 &&
        static_cast<unsigned char>(data[0]) == 0xFF &&
        static_cast<unsigned char>(data[1]) == 0xD8) {
        return "image/jpeg";
    }
    // Check for PNG magic
    if (data.size() >= 8 &&
        static_cast<unsigned char>(data[0]) == 0x89 &&
        data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
        return "image/png";
    }
    return "image/svg+xml";
}

} // namespace thumbnail
