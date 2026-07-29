#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <chrono>
#include <filesystem>

namespace fs = std::filesystem;

namespace file_ops {

// A file/directory entry for listing
struct DirEntry {
    std::string name;
    bool is_directory;
    uintmax_t size;
    std::chrono::system_clock::time_point last_modified;
    std::chrono::system_clock::time_point creation_time;
};

// Resolve a request path to a filesystem path under root_dir.
// Returns empty path if attempting path traversal.
fs::path resolve_path(const fs::path& root_dir, std::string_view request_path);

// Check if path exists
bool exists(const fs::path& p);

// Check if path is a directory
bool is_directory(const fs::path& p);

// Check if path is a regular file
bool is_regular_file(const fs::path& p);

// Get file size
uintmax_t file_size(const fs::path& p);

// Get last modified time
std::chrono::system_clock::time_point last_modified(const fs::path& p);

// List directory entries
std::vector<DirEntry> list_directory(const fs::path& p);

// Create a directory (and parents if needed)
bool create_directory(const fs::path& p);

// Read entire file
std::string read_file(const fs::path& p);

// Write entire file (binary-safe)
bool write_file(const fs::path& p, std::string_view content);

// Delete a file or empty directory
bool remove(const fs::path& p);

// Recursively delete a directory
bool remove_all(const fs::path& p);

// Rename/move
bool rename(const fs::path& from, const fs::path& to);

// Copy file or directory
bool copy(const fs::path& from, const fs::path& to);

// Get entry info as DirEntry
DirEntry get_entry(const fs::path& p);

} // namespace file_ops
