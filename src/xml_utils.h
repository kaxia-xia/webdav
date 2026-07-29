#pragma once

#include <string>
#include <string_view>
#include <vector>
#include "file_ops.h"

namespace xml_utils {

// Generate a minimal PROPFIND response (Multi-Status XML)
// Supports Depth: 0 and Depth: 1
std::string propfind_response(
    std::string_view href_prefix,  // URL prefix (e.g. "/")
    const fs::path& root_dir,
    const fs::path& resolved_path,
    int depth);

} // namespace xml_utils
