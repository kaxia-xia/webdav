#include "file_ops.h"
#include <fstream>
#include <iostream>
#include <algorithm>

#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>

namespace file_ops {

fs::path resolve_path(const fs::path& root_dir, std::string_view request_path) {
    while (!request_path.empty() && request_path.front() == '/') {
        request_path.remove_prefix(1);
    }

    fs::path resolved = root_dir;
    if (!request_path.empty()) {
        resolved /= request_path;
    }

    resolved = resolved.lexically_normal();

    std::string root_str = root_dir.lexically_normal().string();
    std::string resolved_str = resolved.string();

    if (!root_str.empty() && root_str.back() != '/') {
        root_str += '/';
    }
    if (!resolved_str.empty() && resolved_str.back() != '/') {
        resolved_str += '/';
    }

    if (!resolved_str.starts_with(root_str)) {
        return {};
    }
    return resolved;
}

bool exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

bool is_directory(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

bool is_regular_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

uintmax_t file_size(const fs::path& p) {
    std::error_code ec;
    return fs::file_size(p, ec);
}

std::chrono::system_clock::time_point last_modified(const fs::path& p) {
    std::error_code ec;
    auto ftime = fs::last_write_time(p, ec);
    if (ec) return {};
    return std::chrono::file_clock::to_sys(ftime);
}

std::vector<DirEntry> list_directory(const fs::path& p) {
    std::vector<DirEntry> entries;
    entries.reserve(64);
    std::error_code ec;

    for (const auto& entry : fs::directory_iterator(p, ec)) {
        DirEntry de;
        de.name = entry.path().filename().string();
        de.is_directory = entry.is_directory(ec);
        if (de.is_directory) {
            de.size = 0;
        } else {
            de.size = entry.file_size(ec);
        }
        auto ftime = entry.last_write_time(ec);
        de.last_modified = std::chrono::file_clock::to_sys(ftime);
        de.creation_time = de.last_modified;
        entries.push_back(std::move(de));
    }

    std::sort(entries.begin(), entries.end(), [](const DirEntry& a, const DirEntry& b) {
        if (a.is_directory != b.is_directory) return a.is_directory;
        return a.name < b.name;
    });
    return entries;
}

bool create_directory(const fs::path& p) {
    std::error_code ec;
    return fs::create_directories(p, ec);
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return {};
    auto size = f.tellg();
    f.seekg(0);
    std::string content(static_cast<size_t>(size), '\0');
    f.read(content.data(), size);
    return content;
}

bool write_file(const fs::path& p, std::string_view content) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    return f.good();
}

bool remove(const fs::path& p) {
    std::error_code ec;
    return fs::remove(p, ec);
}

bool remove_all(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
    return !ec;
}

bool rename(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::rename(from, to, ec);
    return !ec;
}

bool copy(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    if (fs::is_directory(from, ec)) {
        fs::copy(from, to, fs::copy_options::recursive, ec);
    } else {
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    }
    return !ec;
}

DirEntry get_entry(const fs::path& p) {
    DirEntry de;
    // Handle paths with trailing slash (filename() returns "" for "foo/")
    std::string ps = p.string();
    if (ps.size() > 1 && ps.back() == '/') ps.pop_back();
    de.name = fs::path(ps).filename().string();
    de.is_directory = file_ops::is_directory(p);
    if (de.is_directory) {
        de.size = 0;
    } else {
        de.size = file_ops::file_size(p);
    }
    de.last_modified = file_ops::last_modified(p);
    de.creation_time = de.last_modified;
    return de;
}

// ── Advisory file locking ────────────────────────────────────────────────────

int try_lock_exclusive(const fs::path& p) {
    int fd = ::open(p.c_str(), O_RDONLY);
    if (fd < 0) return -2;

    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            return -1;
        }
        return -2;
    }
    return fd;
}

bool lock_shared(int fd) {
    return ::flock(fd, LOCK_SH) == 0;
}

} // namespace file_ops
