#include "utils.h"
#include <algorithm>
#include <cctype>

namespace utils {

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
            oss << '%' << std::uppercase << std::hex << static_cast<int>(static_cast<unsigned char>(c));
            result += oss.str();
        }
    }
    return result;
}

std::string rfc1123_time(const std::chrono::system_clock::time_point& tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_buf;
    gmtime_r(&t, &tm_buf);
    std::ostringstream oss;
    const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
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

std::string mime_type(std::string_view path) {
    // Extract extension
    auto dot = path.rfind('.');
    if (dot == std::string_view::npos) return "application/octet-stream";
    std::string ext(path.substr(dot));
    // Lowercase
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".css")  return "text/css; charset=utf-8";
    if (ext == ".js")   return "application/javascript; charset=utf-8";
    if (ext == ".json") return "application/json; charset=utf-8";
    if (ext == ".xml")  return "application/xml; charset=utf-8";
    if (ext == ".txt")  return "text/plain; charset=utf-8";
    if (ext == ".md")   return "text/markdown; charset=utf-8";
    if (ext == ".csv")  return "text/csv; charset=utf-8";
    if (ext == ".pdf")  return "application/pdf";
    if (ext == ".png")  return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif")  return "image/gif";
    if (ext == ".svg")  return "image/svg+xml";
    if (ext == ".webp") return "image/webp";
    if (ext == ".ico")  return "image/x-icon";
    if (ext == ".mp3")  return "audio/mpeg";
    if (ext == ".mp4")  return "video/mp4";
    if (ext == ".webm") return "video/webm";
    if (ext == ".zip")  return "application/zip";
    if (ext == ".tar")  return "application/x-tar";
    if (ext == ".gz")   return "application/gzip";
    if (ext == ".bz2")  return "application/x-bzip2";
    if (ext == ".xz")   return "application/x-xz";
    if (ext == ".c" || ext == ".h")   return "text/plain; charset=utf-8";
    if (ext == ".cpp" || ext == ".hpp" || ext == ".cc" || ext == ".hh" || ext == ".cxx" || ext == ".hxx")
        return "text/plain; charset=utf-8";
    if (ext == ".py")   return "text/plain; charset=utf-8";
    if (ext == ".rs")   return "text/plain; charset=utf-8";
    if (ext == ".go")   return "text/plain; charset=utf-8";
    if (ext == ".java") return "text/plain; charset=utf-8";
    if (ext == ".sh")   return "text/plain; charset=utf-8";
    if (ext == ".yaml" || ext == ".yml") return "text/plain; charset=utf-8";
    if (ext == ".toml") return "text/plain; charset=utf-8";

    return "application/octet-stream";
}

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
