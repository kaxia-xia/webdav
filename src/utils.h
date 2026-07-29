#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <ctime>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace utils {

// URL-decode a percent-encoded string
std::string url_decode(std::string_view str);

// URL-encode a string (path-safe)
std::string url_encode(std::string_view str);

// Format time as RFC 1123 (HTTP-date)
std::string rfc1123_time(const std::chrono::system_clock::time_point& tp);

// Format time as ISO 8601 (for WebDAV XML)
std::string iso8601_time(const std::chrono::system_clock::time_point& tp);

// MIME type from file extension
std::string mime_type(std::string_view path);

// Trim whitespace from start/end of string
std::string_view trim(std::string_view s);

// Split string by delimiter
std::vector<std::string_view> split(std::string_view s, char delim);

// Case-insensitive string comparison
bool iequals(std::string_view a, std::string_view b);

// Get current time in RFC 1123 format
std::string rfc1123_now();

} // namespace utils
