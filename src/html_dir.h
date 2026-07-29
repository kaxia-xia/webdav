#pragma once

#include <string>
#include <string_view>
#include <filesystem>

namespace fs = std::filesystem;

namespace html_dir {

// Generate a nice HTML directory listing page
std::string generate(std::string_view path, const fs::path& resolved_path);

} // namespace html_dir
