#include "thumbnail.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <fstream>
#include <array>
#include <algorithm>
#include <csignal>

#ifdef __linux__
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#include <vector>

namespace thumbnail {

// ── Globals ──────────────────────────────────────────────────────────────────
static bool s_ffmpeg_available = false;
static fs::path s_cache_dir;

// ── Initialization ───────────────────────────────────────────────────────────

void init() {
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
           ext == ".m4a"  || ext == ".wma"  ||
           is_image_file(filename);
}

bool is_video_file(std::string_view filename) {
    auto dot = filename.rfind('.');
    if (dot == std::string_view::npos) return false;
    std::string ext(filename.substr(dot));
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".mp4"  || ext == ".webm" || ext == ".ogv" ||
           ext == ".mkv"  || ext == ".avi"  || ext == ".mov";
}

bool is_image_file(std::string_view filename) {
    auto dot = filename.rfind('.');
    if (dot == std::string_view::npos) return false;
    std::string ext(filename.substr(dot));
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".jpg"  || ext == ".jpeg" || ext == ".png"  ||
           ext == ".gif"  || ext == ".webp" || ext == ".bmp"  ||
           ext == ".svg"  || ext == ".tiff" || ext == ".tif"  ||
           ext == ".ico"  || ext == ".heic" || ext == ".heif";
}

// ── Cache key: hash of file path + mtime ─────────────────────────────────────

static std::string cache_key(const fs::path& filepath) {
    auto ftime = fs::last_write_time(filepath);
    auto t = std::chrono::duration_cast<std::chrono::seconds>(
        ftime.time_since_epoch()).count();
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

// ── Run ffmpeg via fork+exec (no shell — immune to command injection) ────────

#ifdef __linux__

// Timeout alarm handler — does nothing, just interrupts waitpid
static volatile sig_atomic_t s_ffmpeg_timeout = 0;
static void ffmpeg_alarm_handler(int) { s_ffmpeg_timeout = 1; }

static std::string run_ffmpeg_thumbnail(const fs::path& filepath, int size,
                                         bool is_video, bool is_image) {
    if (!s_ffmpeg_available) return {};

    std::string tmp_output = (s_cache_dir / "tmp_thumb.jpg").string();
    std::string path_str = filepath.string();

    // Build argv — no shell involved, path is a direct argument
    std::vector<const char*> args;
    args.push_back("ffmpeg");
    args.push_back("-y");

    if (is_video) {
        args.push_back("-ss");
        args.push_back("2");
    }

    args.push_back("-i");
    args.push_back(path_str.c_str());

    if (is_image || is_video) {
        if (is_video) {
            args.push_back("-vframes");
            args.push_back("1");
        }
        static char scale_buf[128];
        snprintf(scale_buf, sizeof(scale_buf),
                 "scale=%d:%d:force_original_aspect_ratio=decrease", size, size);
        args.push_back("-vf");
        args.push_back(scale_buf);
        args.push_back("-q:v");
        args.push_back("3");
    } else {
        // Audio: try embedded cover art with -vcodec copy
        args.push_back("-an");
        args.push_back("-vcodec");
        args.push_back("copy");
        static char scale_buf[128];
        snprintf(scale_buf, sizeof(scale_buf),
                 "scale=%d:%d:force_original_aspect_ratio=decrease", size, size);
        args.push_back("-vf");
        args.push_back(scale_buf);
    }

    args.push_back(tmp_output.c_str());
    args.push_back(nullptr);

    // Set up timeout (30 seconds)
    struct sigaction old_sa, new_sa;
    new_sa.sa_handler = ffmpeg_alarm_handler;
    sigemptyset(&new_sa.sa_mask);
    new_sa.sa_flags = 0;
    sigaction(SIGALRM, &new_sa, &old_sa);
    s_ffmpeg_timeout = 0;
    alarm(30);

    pid_t pid = fork();
    if (pid == 0) {
        // Child: restore default alarm, exec ffmpeg
        alarm(0);
        sigaction(SIGALRM, &old_sa, nullptr);

        // Redirect stderr to /dev/null
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }

        execvp("ffmpeg", const_cast<char* const*>(args.data()));
        _exit(1); // exec failed
    }

    // Parent: wait for child
    int status;
    pid_t waited;
    while ((waited = waitpid(pid, &status, 0)) < 0) {
        if (errno == EINTR) {
            if (s_ffmpeg_timeout) {
                // Timeout — kill ffmpeg
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                alarm(0);
                sigaction(SIGALRM, &old_sa, nullptr);
                return {};
            }
            continue;
        }
        break;
    }

    alarm(0);
    sigaction(SIGALRM, &old_sa, nullptr);

    // Retry with fallback strategies if first attempt failed
    if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0) || !fs::exists(tmp_output)) {
        // Retry 1: video at offset 0 (some files have corrupt frames at 2s)
        if (is_video) {
            std::vector<const char*> args2;
            args2.push_back("ffmpeg");
            args2.push_back("-y");
            args2.push_back("-ss");
            args2.push_back("0");
            args2.push_back("-i");
            args2.push_back(path_str.c_str());
            args2.push_back("-vframes");
            args2.push_back("1");
            static char scale_buf[128];
            snprintf(scale_buf, sizeof(scale_buf),
                     "scale=%d:%d:force_original_aspect_ratio=decrease", size, size);
            args2.push_back("-vf");
            args2.push_back(scale_buf);
            args2.push_back("-q:v");
            args2.push_back("3");
            args2.push_back(tmp_output.c_str());
            args2.push_back(nullptr);

            s_ffmpeg_timeout = 0;
            alarm(15);
            pid_t pid2 = fork();
            if (pid2 == 0) {
                alarm(0);
                int devnull = ::open("/dev/null", O_WRONLY);
                if (devnull >= 0) {
                    dup2(devnull, STDERR_FILENO);
                    ::close(devnull);
                }
                execvp("ffmpeg", const_cast<char* const*>(args2.data()));
                _exit(1);
            }
            while ((waited = waitpid(pid2, &status, 0)) < 0) {
                if (errno == EINTR) {
                    if (s_ffmpeg_timeout) {
                        kill(pid2, SIGKILL);
                        waitpid(pid2, &status, 0);
                        alarm(0);
                        return {};
                    }
                    continue;
                }
                break;
            }
            alarm(0);
        }

        // Retry 2: audio without forced codec copy
        if (!is_video && !is_image &&
            !(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
            std::vector<const char*> args3;
            args3.push_back("ffmpeg");
            args3.push_back("-y");
            args3.push_back("-i");
            args3.push_back(path_str.c_str());
            args3.push_back("-an");
            static char scale_buf[128];
            snprintf(scale_buf, sizeof(scale_buf),
                     "scale=%d:%d:force_original_aspect_ratio=decrease", size, size);
            args3.push_back("-vf");
            args3.push_back(scale_buf);
            args3.push_back("-q:v");
            args3.push_back("3");
            args3.push_back(tmp_output.c_str());
            args3.push_back(nullptr);

            s_ffmpeg_timeout = 0;
            alarm(15);
            pid_t pid3 = fork();
            if (pid3 == 0) {
                alarm(0);
                int devnull = ::open("/dev/null", O_WRONLY);
                if (devnull >= 0) {
                    dup2(devnull, STDERR_FILENO);
                    ::close(devnull);
                }
                execvp("ffmpeg", const_cast<char* const*>(args3.data()));
                _exit(1);
            }
            while ((waited = waitpid(pid3, &status, 0)) < 0) {
                if (errno == EINTR) {
                    if (s_ffmpeg_timeout) {
                        kill(pid3, SIGKILL);
                        waitpid(pid3, &status, 0);
                        alarm(0);
                        return {};
                    }
                    continue;
                }
                break;
            }
            alarm(0);
        }
    }

    if (!fs::exists(tmp_output)) return {};

    // Read the generated thumbnail
    std::ifstream f(tmp_output, std::ios::binary | std::ios::ate);
    if (!f) return {};
    auto fsize = f.tellg();
    f.seekg(0);
    std::string data(static_cast<size_t>(fsize), '\0');
    f.read(data.data(), fsize);

    // Clean up temp file
    std::error_code ec;
    fs::remove(tmp_output, ec);

    return data;
}
#else
static std::string run_ffmpeg_thumbnail(const fs::path&, int, bool, bool) {
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

static std::string svg_image_icon(int size) {
    std::string s;
    s.reserve(1024);
    s += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    s += "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 128 128\" "
         "width=\"" + std::to_string(size) + "\" height=\"" + std::to_string(size) + "\">\n";
    s += "  <rect width=\"128\" height=\"128\" rx=\"12\" fill=\"#2c3e50\"/>\n";
    s += "  <rect x=\"24\" y=\"20\" width=\"80\" height=\"70\" rx=\"4\" fill=\"none\" "
         "stroke=\"#27ae60\" stroke-width=\"4\"/>\n";
    s += "  <circle cx=\"48\" cy=\"44\" r=\"10\" fill=\"#27ae60\" opacity=\"0.8\"/>\n";
    s += "  <polygon points=\"24,90 50,60 68,76 88,50 104,70 104,90\" "
         "fill=\"#27ae60\" opacity=\"0.5\"/>\n";
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

    // 2. Try ffmpeg (via fork+exec — safe from command injection)
    std::string fname = filepath.filename().string();
    bool is_vid = is_video_file(fname);
    bool is_img = is_image_file(fname);
    std::string result = run_ffmpeg_thumbnail(filepath, size, is_vid, is_img);

    if (!result.empty()) {
        // Cache it
        std::ofstream f(cache_file, std::ios::binary | std::ios::trunc);
        if (f) {
            f.write(result.data(), static_cast<std::streamsize>(result.size()));
        }
        return result;
    }

    // 3. Fallback: SVG icon
    std::string svg;
    if (is_vid) {
        svg = svg_video_icon(size);
    } else if (is_img) {
        svg = svg_image_icon(size);
    } else {
        svg = svg_audio_icon(size);
    }
    return svg;
}

std::string_view mime_type(const std::string& data) {
    if (data.size() >= 5 && std::string_view(data.data(), 5) == "<?xml") {
        return "image/svg+xml";
    }
    if (data.size() >= 2 &&
        static_cast<unsigned char>(data[0]) == 0xFF &&
        static_cast<unsigned char>(data[1]) == 0xD8) {
        return "image/jpeg";
    }
    if (data.size() >= 8 &&
        static_cast<unsigned char>(data[0]) == 0x89 &&
        data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
        return "image/png";
    }
    return "image/svg+xml";
}

} // namespace thumbnail
