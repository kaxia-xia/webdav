#include "utils.h"
#include <algorithm>
#include <cctype>
#include <array>

namespace utils {

// ── URL codec ────────────────────────────────────────────────────────────────

std::string url_decode(std::string_view str) {
    std::string result;
    result.reserve(str.size());
    for (size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '%' && i + 2 < str.size()) {
            auto from_hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return -1;
            };
            int h1 = from_hex(str[i + 1]);
            int h2 = from_hex(str[i + 2]);
            if (h1 >= 0 && h2 >= 0) {
                result.push_back(static_cast<char>((h1 << 4) | h2));
                i += 2;
                continue;
            }
        } else if (str[i] == '+') {
            result.push_back(' ');
            continue;
        }
        result.push_back(str[i]);
    }
    return result;
}

std::string url_encode(std::string_view str) {
    std::string result;
    result.reserve(str.size() * 3);
    for (char c : str) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~' || c == '/') {
            result.push_back(c);
        } else {
            std::ostringstream oss;
            oss << '%' << std::uppercase << std::hex
                << static_cast<int>(static_cast<unsigned char>(c));
            result += oss.str();
        }
    }
    return result;
}

// ── Time formatting ──────────────────────────────────────────────────────────

std::string rfc1123_time(const std::chrono::system_clock::time_point& tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_buf;
    gmtime_r(&t, &tm_buf);
    std::ostringstream oss;
    const char* days[]   = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                            "Jul","Aug","Sep","Oct","Nov","Dec"};
    oss << days[tm_buf.tm_wday] << ", "
        << std::setfill('0') << std::setw(2) << tm_buf.tm_mday << ' '
        << months[tm_buf.tm_mon] << ' '
        << (tm_buf.tm_year + 1900) << ' '
        << std::setw(2) << tm_buf.tm_hour << ':'
        << std::setw(2) << tm_buf.tm_min << ':'
        << std::setw(2) << tm_buf.tm_sec << " GMT";
    return oss.str();
}

std::string iso8601_time(const std::chrono::system_clock::time_point& tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_buf;
    gmtime_r(&t, &tm_buf);
    std::ostringstream oss;
    oss << std::setfill('0')
        << (tm_buf.tm_year + 1900) << '-'
        << std::setw(2) << (tm_buf.tm_mon + 1) << '-'
        << std::setw(2) << tm_buf.tm_mday << 'T'
        << std::setw(2) << tm_buf.tm_hour << ':'
        << std::setw(2) << tm_buf.tm_min << ':'
        << std::setw(2) << tm_buf.tm_sec << 'Z';
    return oss.str();
}

// ── Base64 decode (RFC 4648, for HTTP Basic auth) ────────────────────────────

std::string base64_decode(std::string_view str) {
    static const signed char kDecode[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59, 60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6,  7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22, 23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32, 33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48, 49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
    };
    std::string result;
    result.reserve((str.size() + 3) / 4 * 3);
    int val = 0, bits = -8;
    for (size_t i = 0; i < str.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(str[i]);
        if (c == '=' || c == '\r' || c == '\n') break;
        int v = kDecode[c];
        if (v < 0) continue;
        val = (val << 6) | v;
        bits += 6;
        if (bits >= 0) {
            result.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return result;
}

// ── MIME type (sorted array + binary search → O(log n), no allocations) ─────

namespace {
    using MimeEntry = std::pair<std::string_view, std::string_view>;

    // MUST stay sorted by extension for binary_search
    constexpr std::array<MimeEntry, 55> kMimeMap = {{
        {".aac",   "audio/aac"},
        {".avi",   "video/x-msvideo"},
        {".bmp",   "image/bmp"},
        {".bz2",   "application/x-bzip2"},
        {".c",     "text/plain; charset=utf-8"},
        {".cc",    "text/plain; charset=utf-8"},
        {".cpp",   "text/plain; charset=utf-8"},
        {".css",   "text/css; charset=utf-8"},
        {".csv",   "text/csv; charset=utf-8"},
        {".cxx",   "text/plain; charset=utf-8"},
        {".flac",  "audio/flac"},
        {".gif",   "image/gif"},
        {".go",    "text/plain; charset=utf-8"},
        {".gz",    "application/gzip"},
        {".h",     "text/plain; charset=utf-8"},
        {".hh",    "text/plain; charset=utf-8"},
        {".hpp",   "text/plain; charset=utf-8"},
        {".htm",   "text/html; charset=utf-8"},
        {".html",  "text/html; charset=utf-8"},
        {".hxx",   "text/plain; charset=utf-8"},
        {".ico",   "image/x-icon"},
        {".java",  "text/plain; charset=utf-8"},
        {".jpeg",  "image/jpeg"},
        {".jpg",   "image/jpeg"},
        {".js",    "application/javascript; charset=utf-8"},
        {".json",  "application/json; charset=utf-8"},
        {".m4a",   "audio/mp4"},
        {".md",    "text/markdown; charset=utf-8"},
        {".mkv",   "video/x-matroska"},
        {".mov",   "video/quicktime"},
        {".mp3",   "audio/mpeg"},
        {".mp4",   "video/mp4"},
        {".ogg",   "audio/ogg"},
        {".ogv",   "video/ogg"},
        {".opus",  "audio/opus"},
        {".pdf",   "application/pdf"},
        {".png",   "image/png"},
        {".py",    "text/plain; charset=utf-8"},
        {".rs",    "text/plain; charset=utf-8"},
        {".sh",    "text/plain; charset=utf-8"},
        {".svg",   "image/svg+xml"},
        {".tar",   "application/x-tar"},
        {".tiff",  "image/tiff"},
        {".toml",  "text/plain; charset=utf-8"},
        {".txt",   "text/plain; charset=utf-8"},
        {".wav",   "audio/wav"},
        {".webm",  "video/webm"},
        {".webp",  "image/webp"},
        {".woff",  "font/woff"},
        {".woff2", "font/woff2"},
        {".xml",   "application/xml; charset=utf-8"},
        {".xz",    "application/x-xz"},
        {".yaml",  "text/plain; charset=utf-8"},
        {".yml",   "text/plain; charset=utf-8"},
        {".zip",   "application/zip"},
    }};
} // anonymous namespace

std::string mime_type(std::string_view path) {
    auto dot = path.rfind('.');
    if (dot == std::string_view::npos) return "application/octet-stream";

    // Build lowercased extension
    std::string_view ext_sv = path.substr(dot);
    std::string ext;
    ext.reserve(ext_sv.size());
    for (char c : ext_sv) {
        ext.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    // Binary search in sorted array
    auto it = std::lower_bound(kMimeMap.begin(), kMimeMap.end(), ext,
        [](const MimeEntry& e, const std::string& key) { return e.first < key; });

    if (it != kMimeMap.end() && it->first == ext) {
        return std::string(it->second);
    }
    return "application/octet-stream";
}

// ── String utilities ─────────────────────────────────────────────────────────

std::string_view trim(std::string_view s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return {};
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::vector<std::string_view> split(std::string_view s, char delim) {
    std::vector<std::string_view> result;
    size_t start = 0;
    size_t end;
    while ((end = s.find(delim, start)) != std::string_view::npos) {
        result.push_back(trim(s.substr(start, end - start)));
        start = end + 1;
    }
    result.push_back(trim(s.substr(start)));
    return result;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    return std::equal(a.begin(), a.end(), b.begin(),
                      [](char ca, char cb) {
                          return std::tolower(static_cast<unsigned char>(ca)) ==
                                 std::tolower(static_cast<unsigned char>(cb));
                      });
}

std::string rfc1123_now() {
    return rfc1123_time(std::chrono::system_clock::now());
}

} // namespace utils
