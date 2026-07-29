#pragma once

#include <string>
#include <string_view>
#include <filesystem>

namespace fs = std::filesystem;

namespace html_dir {

// Generate a nice HTML directory listing page.
// If 'media_only' is true, only media files are shown (for AJAX partial updates).
std::string generate(std::string_view path, const fs::path& resolved_path,
                     std::string_view server_origin = "");

} // namespace html_dir
