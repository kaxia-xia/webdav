#pragma once

#include <string>
#include <string_view>
#include <filesystem>
#include <unordered_map>

namespace fs = std::filesystem;

namespace html_dir {

// Generate a nice HTML directory listing page.
// 'media_tokens': optional map from filename → mtoken for auth-bypassed thumbnails.
std::string generate(std::string_view path, const fs::path& resolved_path,
                     std::string_view server_origin = "",
                     const std::unordered_map<std::string, std::string>* media_tokens = nullptr);

} // namespace html_dir
