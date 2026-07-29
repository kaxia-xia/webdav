main.cpp
```
#include "server.h"
#include <iostream>
#include <csignal>
#include <filesystem>
#include <string>
#include <optional>

namespace fs = std::filesystem;

// Global server pointer for signal handling
Server* g_server = nullptr;

void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        std::cout << "\n[INFO] Received signal " << sig << ", shutting down..." << std::endl;
        if (g_server) {
            g_server->shutdown();
        }
    }
}

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [OPTIONS]\n"
              << "\n"
              << "  WebDAV Server — serve files over WebDAV + browse via browser\n"
              << "\n"
              << "Options:\n"
              << "  -d, --dir <path>      Directory to serve (default: current directory)\n"
              << "  -p, --port <port>     Port to listen on (default: 9000)\n"
              << "  -u, --user <name>     Username for HTTP Basic authentication\n"
              << "  -w, --pass <password> Password for HTTP Basic authentication\n"
              << "  --no-browser          Disable browser-friendly HTML directory listing\n"
              << "  -h, --help            Show this help\n"
              << "\n"
              << "Examples:\n"
              << "  " << prog << "                                       # no auth\n"
              << "  " << prog << " -u admin -w secret123                  # with auth\n"
              << "  " << prog << " -d /srv/files -p 8080 -u alice -w pwd  # custom dir + port + auth\n"
              << std::endl;
}

int main(int argc, char* argv[]) {
    std::string root_dir = fs::current_path().string();
    int port = 9000;
    bool allow_browser = true;
    std::optional<std::string> username;
    std::optional<std::string> password;

    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "-d" || arg == "--dir") {
            if (i + 1 < argc) {
                root_dir = argv[++i];
            } else {
                std::cerr << "Missing argument for --dir" << std::endl;
                return 1;
            }
        } else if (arg == "-p" || arg == "--port") {
            if (i + 1 < argc) {
                port = std::stoi(argv[++i]);
            } else {
                std::cerr << "Missing argument for --port" << std::endl;
                return 1;
            }
        } else if (arg == "-u" || arg == "--user") {
            if (i + 1 < argc) {
                username = argv[++i];
            } else {
                std::cerr << "Missing argument for --user" << std::endl;
                return 1;
            }
        } else if (arg == "-w" || arg == "--pass") {
            if (i + 1 < argc) {
                password = argv[++i];
            } else {
                std::cerr << "Missing argument for --pass" << std::endl;
                return 1;
            }
        } else if (arg == "--no-browser") {
            allow_browser = false;
        } else {
            std::cerr << "Unknown option: " << arg << std::endl;
            print_usage(argv[0]);
            return 1;
        }
    }

    // Validate: if one of user/pass is given, the other must also be
    if (username.has_value() != password.has_value()) {
        std::cerr << "Error: both --user and --pass must be provided together." << std::endl;
        return 1;
    }

    // Validate port
    if (port < 1 || port > 65535) {
        std::cerr << "Invalid port: " << port << std::endl;
        return 1;
    }

    // Validate root directory
    fs::path root_path = fs::absolute(root_dir);
    if (!fs::exists(root_path)) {
        std::cerr << "Directory does not exist: " << root_dir << std::endl;
        std::cerr << "Creating directory..." << std::endl;
        std::error_code ec;
        if (!fs::create_directories(root_path, ec)) {
            std::cerr << "Failed to create directory: " << ec.message() << std::endl;
            return 1;
        }
    }

    // Setup signal handling
    Server server(root_path, port, allow_browser,
                  std::move(username), std::move(password));
    g_server = &server;

    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    // Ignore SIGPIPE — we use MSG_NOSIGNAL
    signal(SIGPIPE, SIG_IGN);

    server.run();

    return 0;
}
```

server.cpp
```
#include "server.h"
#include "http_parser.h"
#include "thumbnail.h"
#include "file_ops.h"
#include "utils.h"
#include <iostream>
#include <cstring>
#include <cerrno>

#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/sendfile.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

// ── Constants ────────────────────────────────────────────────────────────────
static constexpr int MAX_EPOLL_EVENTS = 256;
static constexpr int EPOLL_TIMEOUT_MS = 1000;
static constexpr int SOCKET_TIMEOUT_SEC = 30;

// ── Constructor / Destructor ─────────────────────────────────────────────────

Server::Server(const fs::path& root_dir, int port, bool allow_browser,
              std::optional<std::string> username,
              std::optional<std::string> password)
    : port_(port)
    , handler_(root_dir, allow_browser, std::move(username), std::move(password))
    , num_workers_(std::thread::hardware_concurrency())
{
    if (num_workers_ < 1) num_workers_ = 4;
    std::cout << "[INFO] Starting with " << num_workers_ << " worker threads" << std::endl;

    // Initialize thumbnail subsystem
    thumbnail::init();
}

Server::~Server() {
    shutdown();
}

void Server::shutdown() {
    if (!running_.exchange(false)) return;  // already shut down

    // Poke the eventfd so workers wake up from epoll_wait
    if (shutdown_fd_ >= 0) {
        uint64_t val = 1;
        ssize_t n = write(shutdown_fd_, &val, sizeof(val));
        (void)n; // best effort
    }

    // Close server socket to unblock accept
    if (server_fd_ >= 0) {
        ::close(server_fd_);
        server_fd_ = -1;
    }
}

// ── sendfile helper ──────────────────────────────────────────────────────────

ssize_t Server::send_file_range(int out_fd, const std::string& path, off_t offset, size_t count) {
    if (count == 0) return 0;

    int file_fd = ::open(path.c_str(), O_RDONLY);
    if (file_fd < 0) {
        std::cerr << "[ERROR] Cannot open file for sendfile: " << path << std::endl;
        return -1;
    }

    // Acquire shared advisory lock — prevents concurrent PUT/DELETE/MOVE
    // while the file is being streamed. Released automatically on close().
    file_ops::lock_shared(file_fd);

    off_t cur_off = offset;
    size_t remaining = count;
    ssize_t total_sent = 0;

    while (remaining > 0) {
        ssize_t n = ::sendfile(out_fd, file_fd, &cur_off, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            ::close(file_fd);
            return -1;
        }
        if (n == 0) break;
        total_sent += n;
        remaining -= static_cast<size_t>(n);
    }

    ::close(file_fd);
    return total_sent;
}

// ── Handle a single client connection (blocking IO, one-shot) ────────────────

void Server::handle_client(int client_fd) {
    http::Parser parser;

    // ── Socket options ───────────────────────────────────────────────────
    struct timeval tv;
    tv.tv_sec = SOCKET_TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    // NOTE: Do NOT set SO_SNDTIMEO — it breaks sendfile() for large files

    int optval = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));

    char buf[65536];
    std::string pending;
    bool keep_alive = true;

    while (running_ && keep_alive) {
        // ── Read data ────────────────────────────────────────────────────
        if (pending.empty()) {
            ssize_t n = ::recv(client_fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    break; // Timeout — keep-alive idle, close gracefully
                }
                break; // connection closed or error
            }
            pending.assign(buf, static_cast<size_t>(n));
        }

        // Parse and handle all complete requests in the buffer
        while (!pending.empty() && running_) {
            if (!parser.parse(pending)) {
                if (parser.needs_more()) {
                    // Read more data into pending
                    ssize_t n = ::recv(client_fd, buf, sizeof(buf), 0);
                    if (n <= 0) {
                        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                            goto close_connection; // incomplete request + timeout
                        }
                        goto close_connection;
                    }
                    pending.append(buf, static_cast<size_t>(n));
                    continue; // retry parsing with more data
                }
                // Parse error — send 400 and close
                http::Response resp;
                resp.status_code = 400;
                resp.set_header("Connection", "close");
                resp.set_header("Content-Length", "0");
                std::string resp_str = resp.to_string();
                ::send(client_fd, resp_str.data(), resp_str.size(), MSG_NOSIGNAL);
                goto close_connection;
            }

            // Handle the request
            const auto& req = parser.request();
            http::Response resp = handler_.handle(req);

            // Check Connection header
            keep_alive = true;
            auto conn = req.header("Connection");
            if (conn && utils::iequals(*conn, "close")) {
                keep_alive = false;
                resp.set_header("Connection", "close");
            } else {
                resp.set_header("Connection", "keep-alive");
            }

            // Build and send response headers + body (if any, in-memory)
            std::string resp_str = resp.to_string();
            ssize_t sent = ::send(client_fd, resp_str.data(), resp_str.size(), MSG_NOSIGNAL);
            if (sent < 0) {
                goto close_connection;
            }

            // If response has a file to send, use zero-copy sendfile
            if (resp.file_to_send) {
                auto cl = resp.content_length_opt();
                size_t file_size = cl.value_or(0);
                ssize_t fsent = send_file_range(client_fd, *resp.file_to_send, resp.file_offset, file_size);
                if (fsent < 0) {
                    goto close_connection;
                }
            }

            // Capture leftover (unparsed pipelined data) before resetting parser
            // leftover() returns a string_view into parser's internal buffer,
            // so we must copy it before reset() invalidates it.
            std::string leftover(parser.leftover());
            parser.reset();
            pending = std::move(leftover);

            if (!keep_alive) {
                goto close_connection;
            }
        }
    }

close_connection:
    ::close(client_fd);
}

// ── Worker thread loop ───────────────────────────────────────────────────────

void Server::worker_loop() {
    struct epoll_event events[MAX_EPOLL_EVENTS];

    while (running_) {
        int nfds = ::epoll_wait(epoll_fd_, events, MAX_EPOLL_EVENTS, EPOLL_TIMEOUT_MS);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;

            if (fd == server_fd_) {
                // Accept all pending connections
                while (true) {
                    struct sockaddr_in client_addr{};
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = ::accept4(server_fd_,
                        reinterpret_cast<struct sockaddr*>(&client_addr),
                        &client_len, SOCK_NONBLOCK);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (!running_) break;
                        break;
                    }

                    char client_ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
                    std::cout << "[CONNECT] " << client_ip << ":"
                              << ntohs(client_addr.sin_port) << std::endl;

                    // Register client fd with one-shot
                    struct epoll_event ev;
                    ev.events = EPOLLIN | EPOLLONESHOT | EPOLLRDHUP;
                    ev.data.fd = client_fd;
                    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
                        std::cerr << "[WARN] epoll_ctl add client failed: "
                                  << std::strerror(errno) << std::endl;
                        ::close(client_fd);
                    }
                }
            } else if (fd == shutdown_fd_) {
                // Drain eventfd
                uint64_t val;
                ssize_t n = ::read(shutdown_fd_, &val, sizeof(val));
                (void)n;
            } else {
                // Client fd — set to blocking mode for handle_client
                int flags = ::fcntl(fd, F_GETFL, 0);
                if (flags >= 0) {
                    ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
                }
                handle_client(fd);
            }
        }
    }
}

// ── Run server ───────────────────────────────────────────────────────────────

void Server::run() {
    // ── Create server socket ──────────────────────────────────────────────
    server_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (server_fd_ < 0) {
        std::cerr << "[ERROR] Failed to create socket: " << std::strerror(errno) << std::endl;
        return;
    }

    int reuse = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port_));

    if (::bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[ERROR] Failed to bind port " << port_ << ": "
                  << std::strerror(errno) << std::endl;
        ::close(server_fd_);
        server_fd_ = -1;
        return;
    }

    if (::listen(server_fd_, SOMAXCONN) < 0) {
        std::cerr << "[ERROR] Failed to listen: " << std::strerror(errno) << std::endl;
        ::close(server_fd_);
        server_fd_ = -1;
        return;
    }

    // ── Create epoll ──────────────────────────────────────────────────────
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        std::cerr << "[ERROR] Failed to create epoll: " << std::strerror(errno) << std::endl;
        ::close(server_fd_);
        server_fd_ = -1;
        return;
    }

    // ── Create shutdown eventfd ───────────────────────────────────────────
    shutdown_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (shutdown_fd_ < 0) {
        std::cerr << "[ERROR] Failed to create eventfd: " << std::strerror(errno) << std::endl;
        ::close(epoll_fd_);
        ::close(server_fd_);
        epoll_fd_ = -1;
        server_fd_ = -1;
        return;
    }

    // Register shutdown fd
    {
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = shutdown_fd_;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, shutdown_fd_, &ev);
    }

    // Register server fd with EPOLLEXCLUSIVE to avoid thundering herd
    {
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLEXCLUSIVE;
        ev.data.fd = server_fd_;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, server_fd_, &ev) < 0) {
            // EPOLLEXCLUSIVE not supported? fall back
            ev.events = EPOLLIN;
            ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, server_fd_, &ev);
        }
    }

    std::cout << "[INFO] WebDAV server listening on http://0.0.0.0:" << port_ << std::endl;
    std::cout << "[INFO] Serving: " << handler_.root_dir() << std::endl;
    std::cout << "[INFO] Browser access: http://localhost:" << port_ << "/" << std::endl;

    // ── Spawn worker threads ──────────────────────────────────────────────
    workers_.reserve(num_workers_);
    for (unsigned i = 0; i < num_workers_; ++i) {
        workers_.emplace_back(&Server::worker_loop, this);
    }

    // Main thread waits for workers (or signal)
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();

    // ── Cleanup ───────────────────────────────────────────────────────────
    if (shutdown_fd_ >= 0) {
        ::close(shutdown_fd_);
        shutdown_fd_ = -1;
    }
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
    }
    if (server_fd_ >= 0) {
        ::close(server_fd_);
        server_fd_ = -1;
    }

    std::cout << "[INFO] Server stopped." << std::endl;
}
```

server.h
```
#pragma once

#include "webdav_handler.h"
#include <filesystem>
#include <string>
#include <atomic>
#include <optional>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

class Server {
public:
    Server(const fs::path& root_dir, int port = 9000, bool allow_browser = true,
          std::optional<std::string> username = std::nullopt,
          std::optional<std::string> password = std::nullopt);
    ~Server();

    // Start the server (blocking — waits for shutdown)
    void run();

    // Signal shutdown (thread-safe)
    void shutdown();

    int port() const { return port_; }

private:
    int port_;
    WebDavHandler handler_;
    int server_fd_ = -1;
    int epoll_fd_ = -1;
    int shutdown_fd_ = -1;
    std::atomic<bool> running_{true};
    unsigned num_workers_;
    std::vector<std::jthread> workers_;

    void worker_loop();
    void handle_client(int client_fd);
    static ssize_t send_file_range(int out_fd, const std::string& path, off_t offset, size_t count);
};
```

file_ops.cpp
```
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
    entries.reserve(64);  // avoid reallocs for typical directories
    std::error_code ec;

    for (const auto& entry : fs::directory_iterator(p, ec)) {
        DirEntry de;
        de.name = entry.path().filename().string();

        // Use the directory_entry's cached type (from readdir d_type on Linux)
        // to avoid a separate stat syscall for is_directory.
        // fs::is_directory on a directory_entry uses the cached status.
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

    // Sort: directories first, then alphabetical
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
    de.name = p.filename().string();
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
    if (fd < 0) return -2;  // file doesn't exist or can't open

    // LOCK_EX | LOCK_NB: try exclusive lock without blocking
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            return -1;  // file is in use (shared lock held by reader)
        }
        return -2;  // other error
    }
    // Lock acquired — caller MUST close(fd) to release
    return fd;
}

bool lock_shared(int fd) {
    return ::flock(fd, LOCK_SH) == 0;
}

} // namespace file_ops
```

file_ops.h
```
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

// ── Advisory file locking ──────────────────────────────────────────────
// Try to get an exclusive lock on a file (non-blocking).
// Returns an fd (>=0) if lock acquired — close it to release.
// Returns -1 if file is in use by readers, -2 on other errors.
int try_lock_exclusive(const fs::path& p);

// Acquire a shared advisory lock on an already-open fd (blocking).
// Used during sendfile to prevent concurrent modification/deletion.
bool lock_shared(int fd);

} // namespace file_ops
```

html_dir.cpp
```
#include "html_dir.h"
#include "file_ops.h"
#include "thumbnail.h"
#include "utils.h"
#include <cstdio>
#include <algorithm>

namespace html_dir {

// ── Cached CSS (computed once, reused for all requests) ──────────────────────
static const std::string& cached_css() {
    static const std::string css = [] {
        std::string s;
        s.reserve(4000);
        s += "  * { box-sizing: border-box; margin: 0; padding: 0; }\r\n";
        s += "  body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; "
             "background: #f5f5f5; color: #333; }\r\n";
        s += "  .header { background: #2c3e50; color: white; padding: 16px 24px; }\r\n";
        s += "  .header h1 { font-size: 1.3em; font-weight: 500; }\r\n";
        s += "  .header .server { font-size: 0.85em; opacity: 0.7; margin-top: 4px; }\r\n";
        s += "  .container { max-width: 1200px; margin: 24px auto; padding: 0 16px; }\r\n";
        s += "  .breadcrumb { background: white; padding: 12px 20px; border-radius: 8px; "
             "margin-bottom: 16px; box-shadow: 0 1px 3px rgba(0,0,0,0.08); font-size: 0.9em; }\r\n";
        s += "  .breadcrumb a { color: #3498db; text-decoration: none; }\r\n";
        s += "  .breadcrumb a:hover { text-decoration: underline; }\r\n";
        s += "  .breadcrumb span { color: #999; margin: 0 6px; }\r\n";
        s += "  .section-title { font-size: 1.1em; font-weight: 600; color: #2c3e50; "
             "padding: 12px 0 8px 0; margin-top: 8px; border-bottom: 2px solid #eee; }\r\n";
        s += "  .section-title .count { font-weight: 400; color: #888; font-size: 0.85em; }\r\n";

        // ── Table styles (for directories and other files) ──────────────
        s += "  table { width: 100%; background: white; border-radius: 8px; "
             "box-shadow: 0 1px 3px rgba(0,0,0,0.08); border-collapse: collapse; }\r\n";
        s += "  th { text-align: left; padding: 12px 20px; font-size: 0.8em; text-transform: uppercase; "
             "color: #888; border-bottom: 2px solid #eee; letter-spacing: 0.5px; }\r\n";
        s += "  td { padding: 10px 20px; border-bottom: 1px solid #f0f0f0; }\r\n";
        s += "  tr:hover { background: #f8f9fa; }\r\n";
        s += "  .icon { width: 24px; text-align: center; padding-right: 8px; }\r\n";
        s += "  .name a { color: #2c3e50; text-decoration: none; }\r\n";
        s += "  .name a:hover { color: #3498db; text-decoration: underline; }\r\n";
        s += "  .dir a { font-weight: 500; color: #2980b9; }\r\n";
        s += "  .size { color: #888; text-align: right; white-space: nowrap; }\r\n";
        s += "  .date { color: #888; text-align: right; white-space: nowrap; font-size: 0.9em; }\r\n";

        // ── Grid styles (for media files) ────────────────────────────────
        s += "  .media-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(180px, 1fr)); "
             "gap: 16px; margin: 12px 0 20px 0; }\r\n";
        s += "  .media-card { background: white; border-radius: 10px; overflow: hidden; "
             "box-shadow: 0 2px 8px rgba(0,0,0,0.08); transition: transform 0.15s, box-shadow 0.15s; "
             "cursor: pointer; }\r\n";
        s += "  .media-card:hover { transform: translateY(-2px); box-shadow: 0 4px 16px rgba(0,0,0,0.15); }\r\n";
        s += "  .media-card a { text-decoration: none; color: inherit; display: block; }\r\n";
        s += "  .media-thumb { width: 100%; aspect-ratio: 1; background: #1a1a2e; "
             "display: flex; align-items: center; justify-content: center; overflow: hidden; position: relative; }\r\n";
        s += "  .media-thumb img { width: 100%; height: 100%; object-fit: cover; }\r\n";
        s += "  .media-thumb .fallback-icon { display: flex; align-items: center; justify-content: center; "
             "width: 100%; height: 100%; }\r\n";
        s += "  .media-thumb .fallback-icon svg { width: 64px; height: 64px; opacity: 0.7; }\r\n";
        s += "  .media-badge { position: absolute; top: 8px; left: 8px; background: rgba(0,0,0,0.7); "
             "color: #fff; font-size: 0.7em; padding: 2px 8px; border-radius: 4px; "
             "text-transform: uppercase; letter-spacing: 0.5px; }\r\n";
        s += "  .media-duration { position: absolute; bottom: 8px; right: 8px; background: rgba(0,0,0,0.7); "
             "color: #fff; font-size: 0.7em; padding: 2px 6px; border-radius: 4px; }\r\n";
        s += "  .media-info { padding: 10px 12px; }\r\n";
        s += "  .media-name { font-size: 0.85em; font-weight: 500; color: #2c3e50; "
             "white-space: nowrap; overflow: hidden; text-overflow: ellipsis; margin-bottom: 4px; }\r\n";
        s += "  .media-meta { font-size: 0.75em; color: #888; }\r\n";

        // ── Footer ────────────────────────────────────────────────────────
        s += "  .footer { text-align: center; padding: 24px; color: #aaa; font-size: 0.85em; }\r\n";

        // ── Responsive ────────────────────────────────────────────────────
        s += "  @media (max-width: 600px) {\r\n";
        s += "    .date { display: none; }\r\n";
        s += "    td { padding: 8px 12px; }\r\n";
        s += "    .media-grid { grid-template-columns: repeat(auto-fill, minmax(140px, 1fr)); gap: 10px; }\r\n";
        s += "  }\r\n";

        return s;
    }();
    return css;
}

// ── Helpers ──────────────────────────────────────────────────────────────────

static std::string escape_html(std::string_view s) {
    std::string result;
    result.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '&':  result += "&amp;"; break;
        case '<':  result += "&lt;"; break;
        case '>':  result += "&gt;"; break;
        case '"':  result += "&quot;"; break;
        default:   result.push_back(c); break;
        }
    }
    return result;
}

// ── SVG fallback icons (inline, base64-encoded for use in <img> tags) ──────

static std::string video_icon_svg() {
    return "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 128 128\" "
           "width=\"64\" height=\"64\">"
           "<rect width=\"128\" height=\"128\" rx=\"12\" fill=\"#2c3e50\"/>"
           "<polygon points=\"48,32 48,96 100,64\" fill=\"#3498db\" opacity=\"0.8\"/>"
           "</svg>";
}

static std::string audio_icon_svg() {
    return "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 128 128\" "
           "width=\"64\" height=\"64\">"
           "<rect width=\"128\" height=\"128\" rx=\"12\" fill=\"#2c3e50\"/>"
           "<circle cx=\"64\" cy=\"64\" r=\"32\" fill=\"none\" stroke=\"#e74c3c\" stroke-width=\"6\" opacity=\"0.8\"/>"
           "<circle cx=\"64\" cy=\"64\" r=\"14\" fill=\"#e74c3c\" opacity=\"0.8\"/>"
           "</svg>";
}

// ── Main generator ───────────────────────────────────────────────────────────

std::string generate(std::string_view path, const fs::path& resolved_path,
                     std::string_view server_origin) {
    auto entries = file_ops::list_directory(resolved_path);

    // ── Separate entries by type ──────────────────────────────────────────
    std::vector<file_ops::DirEntry> dirs;
    std::vector<file_ops::DirEntry> media_files;
    std::vector<file_ops::DirEntry> other_files;

    for (const auto& e : entries) {
        if (e.is_directory) {
            dirs.push_back(e);
        } else if (thumbnail::is_media_file(e.name)) {
            media_files.push_back(e);
        } else {
            other_files.push_back(e);
        }
    }

    // Pre-allocate
    size_t est_size = 4000 + entries.size() * 300;
    std::string h;
    h.reserve(est_size);

    // ── Parent path ───────────────────────────────────────────────────────
    std::string parent_path;
    std::string_view clean_path(path);
    while (clean_path.size() > 1 && clean_path.back() == '/')
        clean_path.remove_suffix(1);
    if (!clean_path.empty() && clean_path != "/") {
        auto last_slash = clean_path.rfind('/');
        if (last_slash == 0) {
            parent_path = "/";
        } else if (last_slash != std::string_view::npos) {
            parent_path = std::string(clean_path.substr(0, last_slash));
        }
    }

    std::string display_path(path);
    if (display_path.empty() || display_path.back() != '/') {
        display_path += '/';
    }

    // Build thumb prefix for the thumbnail endpoint
    std::string thumb_prefix(server_origin);
    thumb_prefix += "/__thumb__?path=";

    // ── HTML head ────────────────────────────────────────────────────────
    h += "<!DOCTYPE html>\r\n";
    h += "<html lang=\"en\">\r\n";
    h += "<head>\r\n";
    h += "<meta charset=\"UTF-8\">\r\n";
    h += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\r\n";
    h += "<title>Index of ";
    h += escape_html(display_path);
    h += "</title>\r\n";
    h += "<style>\r\n";
    h += cached_css();
    h += "</style>\r\n";
    h += "</head>\r\n";
    h += "<body>\r\n";

    // ── Header ────────────────────────────────────────────────────────────
    h += "<div class=\"header\">\r\n";
    h += "  <h1>📁 Index of ";
    h += escape_html(display_path);
    h += "</h1>\r\n";
    h += "  <div class=\"server\">WebDAV Server &bull; ";
    h += std::to_string(entries.size());
    h += " items";
    if (!media_files.empty()) {
        h += " (";
        h += std::to_string(media_files.size());
        h += " media)";
    }
    h += "</div>\r\n";
    h += "</div>\r\n";

    h += "<div class=\"container\">\r\n";

    // ── Breadcrumb ────────────────────────────────────────────────────────
    h += "<div class=\"breadcrumb\">\r\n";
    h += "  <a href=\"/\">🏠 Home</a>\r\n";
    if (!display_path.empty() && display_path != "/") {
        std::string accum;
        size_t start = 0;
        while (start < display_path.size() && display_path[start] == '/') ++start;
        auto remaining = std::string_view(display_path).substr(start);
        size_t pos = 0;
        while (pos < remaining.size()) {
            auto slash = remaining.find('/', pos);
            if (slash == std::string_view::npos) break;
            accum += '/' + std::string(remaining.substr(pos, slash - pos));
            h += "  <span>/</span>\r\n";
            h += "  <a href=\"";
            h += escape_html(utils::url_encode(accum));
            h += "/\">";
            h += escape_html(remaining.substr(pos, slash - pos));
            h += "</a>\r\n";
            pos = slash + 1;
        }
    }
    h += "</div>\r\n";

    // ══════════════════════════════════════════════════════════════════════
    // ── Media Grid Section ───────────────────────────────────────────────
    // ══════════════════════════════════════════════════════════════════════
    if (!media_files.empty()) {
        h += "<div class=\"section-title\">🎬 Media <span class=\"count\">(";
        h += std::to_string(media_files.size());
        h += " files)</span></div>\r\n";
        h += "<div class=\"media-grid\">\r\n";

        for (const auto& entry : media_files) {
            bool is_vid = thumbnail::is_video_file(entry.name);
            std::string badge = is_vid ? "VIDEO" : "AUDIO";

            // Encode the file path for the thumbnail URL
            std::string file_url = utils::url_encode(display_path) + utils::url_encode(entry.name);
            std::string thumb_url = thumb_prefix + utils::url_encode(display_path + entry.name);

            h += "<div class=\"media-card\">\r\n";
            h += "  <a href=\"";
            h += escape_html(file_url);
            h += "\">\r\n";
            h += "    <div class=\"media-thumb\">\r\n";
            // Use <img> tag pointing to thumbnail endpoint; onerror shows fallback
            h += "      <img src=\"";
            h += escape_html(thumb_url);
            h += "\" alt=\"";
            h += escape_html(entry.name);
            h += "\" loading=\"lazy\"";
            // If server_origin is empty (no thumb support), img will 404 → onerror fallback
            if (server_origin.empty()) {
                h += " onerror=\"this.style.display='none';this.nextElementSibling.style.display='flex';\"";
            } else {
                h += " onerror=\"this.style.display='none';this.nextElementSibling.style.display='flex';\"";
            }
            h += ">\r\n";
            // Fallback icon (hidden until img fails)
            h += "      <div class=\"fallback-icon\" style=\"display:none;\">\r\n";
            if (is_vid) {
                h += "        " + video_icon_svg() + "\r\n";
            } else {
                h += "        " + audio_icon_svg() + "\r\n";
            }
            h += "      </div>\r\n";
            h += "      <span class=\"media-badge\">" + badge + "</span>\r\n";
            h += "    </div>\r\n";
            h += "    <div class=\"media-info\">\r\n";
            h += "      <div class=\"media-name\" title=\"";
            h += escape_html(entry.name);
            h += "\">";
            h += escape_html(entry.name);
            h += "</div>\r\n";
            h += "      <div class=\"media-meta\">";
            h += utils::format_size(entry.size);
            h += "</div>\r\n";
            h += "    </div>\r\n";
            h += "  </a>\r\n";
            h += "</div>\r\n";
        }

        h += "</div>\r\n";
    }

    // ══════════════════════════════════════════════════════════════════════
    // ── Directory Table Section ──────────────────────────────────────────
    // ══════════════════════════════════════════════════════════════════════
    bool has_dirs_or_other = !dirs.empty() || !other_files.empty();
    bool has_parent = !parent_path.empty() || path == "/";

    if (has_dirs_or_other || has_parent) {
        if (!media_files.empty()) {
            h += "<div class=\"section-title\">📂 Files &amp; Folders</div>\r\n";
        }

        h += "<table>\r\n";
        h += "<thead><tr>"
             "<th></th><th>Name</th><th class=\"size\">Size</th><th class=\"date\">Modified</th>"
             "</tr></thead>\r\n";
        h += "<tbody>\r\n";

        // Parent ".." link
        if (has_parent) {
            h += "<tr>";
            h += "<td class=\"icon\">📂</td>";
            std::string parent_href;
            if (parent_path.empty() || parent_path == "/")
                parent_href = "/";
            else
                parent_href = utils::url_encode(parent_path) + '/';
            h += "<td class=\"name dir\"><a href=\"";
            h += escape_html(parent_href);
            h += "\">..</a></td>";
            h += "<td class=\"size\">—</td>";
            h += "<td class=\"date\">—</td>";
            h += "</tr>\r\n";
        }

        // Directory entries
        for (const auto& entry : dirs) {
            h += "<tr>";
            h += "<td class=\"icon\">📁</td>";
            h += "<td class=\"name dir\"><a href=\"";
            h += escape_html(utils::url_encode(display_path) + utils::url_encode(entry.name));
            h += "/\">";
            h += escape_html(entry.name);
            h += "</a></td>";
            h += "<td class=\"size\">—</td>";
            h += "<td class=\"date\">";
            h += utils::rfc1123_time(entry.last_modified);
            h += "</td>";
            h += "</tr>\r\n";
        }

        // Other file entries
        for (const auto& entry : other_files) {
            h += "<tr>";
            h += "<td class=\"icon\">📄</td>";
            h += "<td class=\"name\"><a href=\"";
            h += escape_html(utils::url_encode(display_path) + utils::url_encode(entry.name));
            h += "\">";
            h += escape_html(entry.name);
            h += "</a></td>";
            h += "<td class=\"size\">";
            h += utils::format_size(entry.size);
            h += "</td>";
            h += "<td class=\"date\">";
            h += utils::rfc1123_time(entry.last_modified);
            h += "</td>";
            h += "</tr>\r\n";
        }

        h += "</tbody>\r\n";
        h += "</table>\r\n";
    }

    // Footer
    h += "<div class=\"footer\">WebDAV Server / C++20</div>\r\n";
    h += "</div>\r\n";
    h += "</body>\r\n";
    h += "</html>\r\n";

    return h;
}

} // namespace html_dir
```

html_dir.h
```
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
```

http_parser.cpp
```
#include "http_parser.h"
#include "utils.h"
#include <algorithm>
#include <charconv>

namespace http {

// ── Request ──────────────────────────────────────────────────────────────────

std::optional<std::string_view> Request::header(std::string_view name) const {
    for (const auto& h : headers) {
        if (utils::iequals(h.name, name)) return h.value;
    }
    return std::nullopt;
}

std::optional<size_t> Request::content_length() const {
    auto h = header("Content-Length");
    if (!h) return std::nullopt;
    size_t val = 0;
    auto result = std::from_chars(h->data(), h->data() + h->size(), val);
    if (result.ec != std::errc{}) return std::nullopt;
    return val;
}

std::string Request::depth() const {
    auto h = header("Depth");
    if (!h) return "infinity";
    return std::string(*h);
}

std::optional<ByteRange> Request::parse_range(std::optional<uintmax_t> file_size) const {
    auto h = header("Range");
    if (!h) return std::nullopt;

    std::string_view v = *h;
    // Must be "bytes="
    if (v.size() < 6 || !utils::iequals(v.substr(0, 6), "bytes=")) return std::nullopt;

    std::string_view range_val = v.substr(6);
    // Only handle first range if multiple ("bytes=0-1023,2048-4095")
    auto comma = range_val.find(',');
    if (comma != std::string_view::npos) range_val = range_val.substr(0, comma);

    auto dash = range_val.find('-');
    if (dash == std::string_view::npos) return std::nullopt;

    std::string_view start_str = range_val.substr(0, dash);
    std::string_view end_str   = range_val.substr(dash + 1);

    ByteRange br;

    if (start_str.empty()) {
        // Suffix range: "bytes=-500" → last 500 bytes
        if (!file_size || *file_size == 0) return std::nullopt;

        size_t suffix_count = 0;
        auto res = std::from_chars(end_str.data(), end_str.data() + end_str.size(), suffix_count);
        if (res.ec != std::errc{}) return std::nullopt;
        if (suffix_count == 0) return std::nullopt;

        if (suffix_count > *file_size) suffix_count = static_cast<size_t>(*file_size);
        br.start = static_cast<size_t>(*file_size - suffix_count);
        br.end   = static_cast<size_t>(*file_size - 1);
    } else {
        auto res = std::from_chars(start_str.data(), start_str.data() + start_str.size(), br.start);
        if (res.ec != std::errc{}) return std::nullopt;

        if (!end_str.empty()) {
            size_t end = 0;
            auto res2 = std::from_chars(end_str.data(), end_str.data() + end_str.size(), end);
            if (res2.ec != std::errc{}) return std::nullopt;
            br.end = end;
        }
        // else: br.end == nullopt → from start to EOF
    }

    // Sanity: end must be >= start if specified
    if (br.end && *br.end < br.start) return std::nullopt;

    return br;
}

// ── Parser ───────────────────────────────────────────────────────────────────

Parser::Parser() { reset(); }

void Parser::reset() {
    state_ = State::REQUEST_LINE;
    request_ = Request{};
    buffer_.clear();
    leftover_ = {};
    body_expected_ = 0;
    body_received_ = 0;
}

bool Parser::parse(std::string_view data) {
    buffer_.append(data);

    while (state_ != State::COMPLETE) {
        switch (state_) {
        case State::REQUEST_LINE: {
            auto pos = buffer_.find("\r\n");
            if (pos == std::string::npos) {
                if (buffer_.size() > 8192) return false;
                return false;
            }
            std::string line = buffer_.substr(0, pos);
            buffer_.erase(0, pos + 2);

            auto parts = utils::split(line, ' ');
            if (parts.size() < 3) return false;

            request_.method_str = std::string(parts[0]);
            request_.method = parse_method(request_.method_str);
            request_.uri = std::string(parts[1]);
            request_.version = std::string(parts[2]);

            auto qpos = request_.uri.find('?');
            if (qpos != std::string::npos) {
                request_.path = utils::url_decode(request_.uri.substr(0, qpos));
                request_.query = request_.uri.substr(qpos + 1);
            } else {
                request_.path = utils::url_decode(request_.uri);
            }

            state_ = State::HEADERS;
            break;
        }
        case State::HEADERS: {
            auto pos = buffer_.find("\r\n");
            if (pos == std::string::npos) {
                if (buffer_.size() > 65536) return false;
                return false;
            }
            std::string line = buffer_.substr(0, pos);
            buffer_.erase(0, pos + 2);

            if (line.empty()) {
                auto cl = request_.content_length();
                if (cl.has_value()) {
                    body_expected_ = *cl;
                    if (body_expected_ > 0) {
                        state_ = State::BODY;
                    } else {
                        state_ = State::COMPLETE;
                        leftover_ = buffer_;
                    }
                } else {
                    state_ = State::COMPLETE;
                    leftover_ = buffer_;
                }
            } else {
                auto colon = line.find(':');
                if (colon != std::string::npos) {
                    Header h;
                    h.name = std::string(utils::trim(line.substr(0, colon)));
                    h.value = std::string(utils::trim(line.substr(colon + 1)));
                    request_.headers.push_back(std::move(h));
                }
            }
            break;
        }
        case State::BODY: {
            if (buffer_.size() >= body_expected_) {
                request_.body = buffer_.substr(0, body_expected_);
                buffer_.erase(0, body_expected_);
                state_ = State::COMPLETE;
                leftover_ = buffer_;
            } else {
                return false;
            }
            break;
        }
        case State::COMPLETE:
            break;
        }
    }
    return true;
}

// ── Response ─────────────────────────────────────────────────────────────────

void Response::set_header(std::string_view name, std::string_view value) {
    headers.push_back(Header{std::string(name), std::string(value)});
}

void Response::set_content_type(std::string_view ct) {
    set_header("Content-Type", ct);
}

void Response::set_content_length(size_t len) {
    set_header("Content-Length", std::to_string(len));
}

std::optional<size_t> Response::content_length_opt() const {
    for (const auto& h : headers) {
        if (utils::iequals(h.name, "Content-Length")) {
            size_t val = 0;
            auto result = std::from_chars(h.value.data(), h.value.data() + h.value.size(), val);
            if (result.ec == std::errc{}) return val;
        }
    }
    return std::nullopt;
}

std::string Response::to_string() const {
    std::string result;

    // ── Pre-compute exact size to avoid reallocs ──────────────────────────
    size_t estimate = 20;  // "HTTP/1.1 XXX ...\r\n"
    for (const auto& h : headers) {
        estimate += h.name.size() + h.value.size() + 4;  // ": " + "\r\n"
    }
    estimate += 2;           // trailing "\r\n"
    estimate += body.size(); // body (empty when using sendfile)
    result.reserve(estimate);

    // ── Status line ──────────────────────────────────────────────────────
    result += "HTTP/1.1 ";
    result += std::to_string(status_code);
    result += " ";
    result += status_text.empty() ? std::string(status_message(status_code)) : status_text;
    result += "\r\n";

    // ── Headers ───────────────────────────────────────────────────────────
    for (const auto& h : headers) {
        result += h.name;
        result += ": ";
        result += h.value;
        result += "\r\n";
    }

    result += "\r\n";
    result += body;
    return result;
}

// ── Helpers ──────────────────────────────────────────────────────────────────

Method parse_method(std::string_view s) {
    if (utils::iequals(s, "GET"))       return Method::GET;
    if (utils::iequals(s, "HEAD"))      return Method::HEAD;
    if (utils::iequals(s, "PUT"))       return Method::PUT;
    if (utils::iequals(s, "DELETE"))    return Method::DELETE;
    if (utils::iequals(s, "MKCOL"))     return Method::MKCOL;
    if (utils::iequals(s, "OPTIONS"))   return Method::OPTIONS;
    if (utils::iequals(s, "PROPFIND"))  return Method::PROPFIND;
    if (utils::iequals(s, "PROPPATCH")) return Method::PROPPATCH;
    if (utils::iequals(s, "MOVE"))      return Method::MOVE;
    if (utils::iequals(s, "COPY"))      return Method::COPY;
    if (utils::iequals(s, "LOCK"))      return Method::LOCK;
    if (utils::iequals(s, "UNLOCK"))    return Method::UNLOCK;
    if (utils::iequals(s, "POST"))      return Method::POST;
    return Method::UNKNOWN;
}

const char* status_message(int code) {
    switch (code) {
    case 100: return "Continue";
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 207: return "Multi-Status";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 412: return "Precondition Failed";
    case 415: return "Unsupported Media Type";
    case 423: return "Locked";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 507: return "Insufficient Storage";
    default:  return "Unknown";
    }
}

} // namespace http
```

http_parser.h
```
#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <optional>
#include <cstddef>
#include <cstdint>

namespace http {

// HTTP method
enum class Method {
    UNKNOWN,
    GET,
    HEAD,
    PUT,
    DELETE,
    MKCOL,
    OPTIONS,
    PROPFIND,
    PROPPATCH,
    MOVE,
    COPY,
    LOCK,
    UNLOCK,
    POST,
};

struct Header {
    std::string name;
    std::string value;
};

// Parsed Range: bytes=START-END
// start and end are inclusive. If end is nullopt → from start to EOF.
// If both nullopt → no range.
struct ByteRange {
    size_t start = 0;
    std::optional<size_t> end;  // inclusive
};

struct Request {
    Method method = Method::UNKNOWN;
    std::string method_str;
    std::string uri;
    std::string path;   // decoded path
    std::string query;  // query string (after ?)
    std::string version;
    std::vector<Header> headers;
    std::string body;

    // Get header value by name (case-insensitive)
    std::optional<std::string_view> header(std::string_view name) const;

    // Content-Length as integer
    std::optional<size_t> content_length() const;

    // Get Depth header value
    std::string depth() const;

    // Parse Range: bytes= header. Returns nullopt if missing or invalid.
    // file_size is required to resolve suffix ranges (bytes=-N).
    // If file_size is nullopt, suffix ranges return nullopt.
    std::optional<ByteRange> parse_range(std::optional<uintmax_t> file_size = std::nullopt) const;
};

// Parser states for streaming parse
class Parser {
public:
    Parser();

    // Feed data to parser. Returns true when a complete request has been parsed.
    // Data beyond the request is stored in leftover.
    bool parse(std::string_view data);

    // Get the parsed request
    const Request& request() const { return request_; }

    // Reset for next request
    void reset();

    // Check if parser needs more data
    bool needs_more() const { return state_ != State::COMPLETE; }

    // Get leftover data after parsing
    std::string_view leftover() const { return leftover_; }

private:
    enum class State {
        REQUEST_LINE,
        HEADERS,
        BODY,
        COMPLETE,
    };

    State state_ = State::REQUEST_LINE;
    Request request_;
    std::string buffer_;
    std::string_view leftover_;
    size_t body_expected_ = 0;
    size_t body_received_ = 0;

    bool parse_request_line();
    bool parse_headers();
    bool check_body_complete();
};

// Build an HTTP response
struct Response {
    int status_code = 200;
    std::string status_text;
    std::vector<Header> headers;
    std::string body;

    // If set, the server will send this file via sendfile() after the headers.
    // body should be empty when this is used.
    std::optional<std::string> file_to_send;

    // Optional byte offset into file_to_send (for Range: requests)
    off_t file_offset = 0;

    void set_header(std::string_view name, std::string_view value);
    void set_content_type(std::string_view ct);
    void set_content_length(size_t len);

    // Read back the Content-Length header value (for sendfile logic)
    std::optional<size_t> content_length_opt() const;

    // Serialize to wire format (headers + body; body is omitted when file_to_send is set)
    std::string to_string() const;
};

// Parse method from string
Method parse_method(std::string_view s);

// Get status message for code
const char* status_message(int code);

} // namespace http
```

thumbnail.cpp
```
#include "thumbnail.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <fstream>
#include <array>
#include <algorithm>

#ifdef __linux__
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace thumbnail {

// ── Globals ──────────────────────────────────────────────────────────────────
static bool s_ffmpeg_available = false;
static fs::path s_cache_dir;

// ── Initialization ───────────────────────────────────────────────────────────

void init() {
    // Check if ffmpeg is available
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

    // Create cache directory in system temp (isolated from served files)
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
           ext == ".m4a"  || ext == ".wma";
}

bool is_video_file(std::string_view filename) {
    auto dot = filename.rfind('.');
    if (dot == std::string_view::npos) return false;
    std::string ext(filename.substr(dot));
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".mp4"  || ext == ".webm" || ext == ".ogv" ||
           ext == ".mkv"  || ext == ".avi"  || ext == ".mov";
}

// ── Cache key: hash of file path + mtime ─────────────────────────────────────

static std::string cache_key(const fs::path& filepath) {
    auto ftime = fs::last_write_time(filepath);
    auto t = std::chrono::duration_cast<std::chrono::seconds>(
        ftime.time_since_epoch()).count();
    // Simple hash: djb2 of canonical path + mtime
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

// ── Run ffmpeg to extract thumbnail ──────────────────────────────────────────

#ifdef __linux__
static std::string run_ffmpeg_thumbnail(const fs::path& filepath, int size, bool is_video) {
    if (!s_ffmpeg_available) return {};

    std::string tmp_output = (s_cache_dir / "tmp_thumb.jpg").string();

    std::string cmd;
    if (is_video) {
        // Extract frame at 2 seconds (to avoid black first frames)
        char buf[1024];
        snprintf(buf, sizeof(buf),
            "ffmpeg -y -ss 2 -i '%s' -vframes 1 -vf 'scale=%d:%d:force_original_aspect_ratio=decrease' "
            "-q:v 3 '%s' 2>/dev/null",
            filepath.c_str(), size, size, tmp_output.c_str());
        cmd = buf;
    } else {
        // Audio: try to extract embedded cover art
        char buf[1024];
        snprintf(buf, sizeof(buf),
            "ffmpeg -y -i '%s' -an -vcodec copy -vf 'scale=%d:%d:force_original_aspect_ratio=decrease' "
            "'%s' 2>/dev/null",
            filepath.c_str(), size, size, tmp_output.c_str());
        cmd = buf;
    }

    int ret = std::system(cmd.c_str());
    if (ret != 0 || !fs::exists(tmp_output)) {
        // Try alternative: extract frame at 0 seconds
        if (is_video) {
            char buf2[1024];
            snprintf(buf2, sizeof(buf2),
                "ffmpeg -y -ss 0 -i '%s' -vframes 1 -vf 'scale=%d:%d:force_original_aspect_ratio=decrease' "
                "-q:v 3 '%s' 2>/dev/null",
                filepath.c_str(), size, size, tmp_output.c_str());
            cmd = buf2;
            ret = std::system(cmd.c_str());
        }
    }

    if (ret != 0 || !fs::exists(tmp_output)) {
        return {};
    }

    // Read the generated file
    std::ifstream f(tmp_output, std::ios::binary | std::ios::ate);
    if (!f) return {};
    auto fsize = f.tellg();
    f.seekg(0);
    std::string data(static_cast<size_t>(fsize), '\0');
    f.read(data.data(), fsize);

    // Remove temp
    std::error_code ec;
    fs::remove(tmp_output, ec);

    return data;
}
#else
static std::string run_ffmpeg_thumbnail(const fs::path&, int, bool) {
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

    // 2. Try ffmpeg
    bool is_vid = is_video_file(filepath.filename().string());
    std::string result = run_ffmpeg_thumbnail(filepath, size, is_vid);

    if (!result.empty()) {
        // Cache it
        std::ofstream f(cache_file, std::ios::binary | std::ios::trunc);
        if (f) {
            f.write(result.data(), static_cast<std::streamsize>(result.size()));
        }
        return result;
    }

    // 3. Fallback: SVG icon
    // Cache SVG icon too (but as .svg so we know)
    fs::path svg_cache = s_cache_dir / (cache_key(filepath) + "_" + std::to_string(size) + ".svg");
    std::string svg = is_vid ? svg_video_icon(size) : svg_audio_icon(size);

    // Don't bother caching SVG — it's fast to generate
    return svg;
}

std::string_view mime_type(const std::string& data) {
    if (data.size() >= 5 && std::string_view(data.data(), 5) == "<?xml") {
        return "image/svg+xml";
    }
    // Check for JPEG magic
    if (data.size() >= 2 &&
        static_cast<unsigned char>(data[0]) == 0xFF &&
        static_cast<unsigned char>(data[1]) == 0xD8) {
        return "image/jpeg";
    }
    // Check for PNG magic
    if (data.size() >= 8 &&
        static_cast<unsigned char>(data[0]) == 0x89 &&
        data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
        return "image/png";
    }
    return "image/svg+xml";
}

} // namespace thumbnail
```

thumbnail.h
```
#pragma once

#include <string>
#include <string_view>
#include <filesystem>

namespace fs = std::filesystem;

namespace thumbnail {

// Initialize: check if ffmpeg is available, create cache directory
void init();

// Generate a thumbnail for a media file.
// Returns the thumbnail image data (JPEG/PNG) or an SVG icon as fallback.
// If size > 0, thumbnail is scaled to fit within size×size (maintains aspect ratio).
// If size == 0, uses default (256).
std::string generate(const fs::path& filepath, int size = 256);

// Get the MIME type for a generated thumbnail
std::string_view mime_type(const std::string& data);

// Check if a file is a supported media file by extension
bool is_media_file(std::string_view filename);

// Check if a file is a video file (as opposed to audio)
bool is_video_file(std::string_view filename);

} // namespace thumbnail
```

utils.cpp
```
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
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X",
                     static_cast<unsigned char>(c));
            result.append(hex, 3);
        }
    }
    return result;
}

// ── Time formatting (stack-based snprintf — zero heap allocation) ────────────

std::string rfc1123_time(const std::chrono::system_clock::time_point& tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_buf;
    gmtime_r(&t, &tm_buf);
    const char* days[]   = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                            "Jul","Aug","Sep","Oct","Nov","Dec"};
    char buf[40];
    // "Wkd, DD Mon YYYY HH:MM:SS GMT" = 29 chars + null
    int n = snprintf(buf, sizeof(buf), "%s, %02d %s %04d %02d:%02d:%02d GMT",
                     days[tm_buf.tm_wday],
                     tm_buf.tm_mday,
                     months[tm_buf.tm_mon],
                     tm_buf.tm_year + 1900,
                     tm_buf.tm_hour,
                     tm_buf.tm_min,
                     tm_buf.tm_sec);
    return std::string(buf, static_cast<size_t>(n));
}

std::string iso8601_time(const std::chrono::system_clock::time_point& tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_buf;
    gmtime_r(&t, &tm_buf);
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                     tm_buf.tm_year + 1900,
                     tm_buf.tm_mon + 1,
                     tm_buf.tm_mday,
                     tm_buf.tm_hour,
                     tm_buf.tm_min,
                     tm_buf.tm_sec);
    return std::string(buf, static_cast<size_t>(n));
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

    std::string_view ext_sv = path.substr(dot);

    // Lowercase into stack buffer (max extension in map is 5 chars: .woff2)
    char ext_lower[16];
    size_t n = std::min(ext_sv.size(), sizeof(ext_lower) - 1);
    for (size_t i = 0; i < n; ++i) {
        ext_lower[i] = static_cast<char>(
            std::tolower(static_cast<unsigned char>(ext_sv[i])));
    }
    ext_lower[n] = '\0';
    std::string_view ext_key(ext_lower, n);

    // Binary search (exact ASCII, all map entries are lowercase)
    auto it = std::lower_bound(kMimeMap.begin(), kMimeMap.end(), ext_key,
        [](const MimeEntry& e, std::string_view key) {
            return e.first < key;
        });

    if (it != kMimeMap.end() && it->first == ext_key) {
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

std::string format_size(uintmax_t size) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_idx = 0;
    double display = static_cast<double>(size);
    while (display >= 1024.0 && unit_idx < 4) {
        display /= 1024.0;
        ++unit_idx;
    }
    char buf[32];
    if (unit_idx == 0) {
        snprintf(buf, sizeof(buf), "%d %s", static_cast<int>(display), units[unit_idx]);
    } else {
        snprintf(buf, sizeof(buf), "%.1f %s", display, units[unit_idx]);
    }
    return std::string(buf);
}

} // namespace utils
```

utils.h
```
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

// Base64-decode a string (used for HTTP Basic auth)
std::string base64_decode(std::string_view str);

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

// Human-readable file size (e.g. "1.5 MB")
std::string format_size(uintmax_t size);

} // namespace utils
```

webdav_handler.cpp
```
#include "webdav_handler.h"
#include "file_ops.h"
#include "xml_utils.h"
#include "html_dir.h"
#include "thumbnail.h"
#include "utils.h"
#include <iostream>
#include <algorithm>
#include <charconv>
#include <unistd.h>

WebDavHandler::WebDavHandler(const fs::path& root_dir, bool allow_browser,
                             std::optional<std::string> username,
                             std::optional<std::string> password)
    : root_dir_(fs::absolute(root_dir).lexically_normal())
    , allow_browser_(allow_browser)
    , username_(std::move(username))
    , password_(std::move(password))
{
    if (!file_ops::exists(root_dir_)) {
        file_ops::create_directory(root_dir_);
    }
    std::cout << "[INFO] Serving directory: " << root_dir_ << std::endl;
    if (username_ && password_) {
        std::cout << "[INFO] Authentication enabled (user: " << *username_ << ")" << std::endl;
    }
}

bool WebDavHandler::check_auth(const http::Request& req, const fs::path& resolved_path) {
    // No credentials configured → skip auth
    if (!username_ || !password_) return true;

    // ── Media token check (for player page subresource requests) ─────────
    // Browsers' <video>/<audio> elements don't reliably send HTTP Basic auth
    // credentials on subresource requests. The player page embeds a temporary
    // token in the media URL so the file can be accessed without auth.
    auto token_start = req.query.find("mtoken=");
    if (token_start != std::string::npos) {
        std::string token = req.query.substr(token_start + 7); // len("mtoken=")
        auto amp = token.find('&');
        if (amp != std::string::npos) token.resize(amp);
        if (!token.empty() && verify_media_token(resolved_path, token)) {
            return true;
        }
    }

    // ── Standard HTTP Basic auth ─────────────────────────────────────────
    auto auth_hdr = req.header("Authorization");
    if (!auth_hdr) return false;

    std::string_view auth = *auth_hdr;
    if (auth.size() < 6 || !utils::iequals(auth.substr(0, 6), "Basic ")) {
        std::cerr << "[AUTH] Bad auth header: '" << auth << "'" << std::endl;
        return false;
    }

    std::string decoded = utils::base64_decode(auth.substr(6));
    auto colon = decoded.find(':');
    if (colon == std::string::npos) {
        std::cerr << "[AUTH] base64 decoded no colon: '" << decoded << "'" << std::endl;
        return false;
    }

    std::string user = decoded.substr(0, colon);
    std::string pass = decoded.substr(colon + 1);

    bool ok = (user == *username_ && pass == *password_);
    if (!ok) {
        std::cerr << "[AUTH] Credential mismatch: got '" << user << "'/'" << pass
                  << "' expected '" << *username_ << "'/'" << *password_ << "'" << std::endl;
    }
    return ok;
}

void WebDavHandler::add_common_headers(http::Response& resp) {
    resp.set_header("Server", "WebDAV-Server/1.1 (C++20)");
    resp.set_header("Date", utils::rfc1123_now());
    resp.set_header("Accept-Ranges", "bytes");
}

void WebDavHandler::add_dav_header(http::Response& resp) {
    resp.set_header("DAV", "1, 2");
    resp.set_header("Allow",
        "OPTIONS, GET, HEAD, PUT, DELETE, MKCOL, PROPFIND, PROPPATCH, MOVE, COPY, LOCK, UNLOCK");
}

http::Response WebDavHandler::error_response(int code, std::string_view message) {
    http::Response resp;
    resp.status_code = code;
    add_common_headers(resp);

    std::string msg(message);
    if (msg.empty()) msg = http::status_message(code);

    std::string body = "<!DOCTYPE html><html><head><title>" +
        std::to_string(code) + " " + msg + "</title></head>" +
        "<body><h1>" + std::to_string(code) + " " + msg + "</h1>" +
        "<hr><em>WebDAV Server</em></body></html>";

    resp.set_content_type("text/html; charset=utf-8");
    resp.set_content_length(body.size());
    resp.body = std::move(body);
    return resp;
}

bool WebDavHandler::is_browser_request(const http::Request& req) {
    auto accept = req.header("Accept");
    if (accept && accept->find("text/html") != std::string_view::npos) {
        return true;
    }
    auto ua = req.header("User-Agent");
    if (ua) {
        std::string_view uav = *ua;
        if (uav.find("Mozilla")   != std::string_view::npos ||
            uav.find("Chrome")    != std::string_view::npos ||
            uav.find("Safari")    != std::string_view::npos ||
            uav.find("Firefox")   != std::string_view::npos ||
            uav.find("Edge")      != std::string_view::npos ||
            uav.find("Edg")       != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

bool WebDavHandler::prefers_html(const http::Request& req) {
    auto accept = req.header("Accept");
    if (!accept) return false;
    return accept->find("text/html") != std::string_view::npos;
}

bool WebDavHandler::is_media_file(const fs::path& path) {
    auto ext = path.extension().string();
    // Convert to lowercase for comparison
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".mp4"  || ext == ".webm" || ext == ".ogv"  ||
           ext == ".mkv"  || ext == ".avi"  || ext == ".mov"  ||
           ext == ".mp3"  || ext == ".ogg"  || ext == ".opus" ||
           ext == ".flac" || ext == ".wav"  || ext == ".aac"  ||
           ext == ".m4a"  || ext == ".wma";
}

http::Response WebDavHandler::serve_media_player_page(const http::Request& req, const fs::path& resolved) {
    std::string filename = resolved.filename().string();
    std::string escaped_name = filename;
    // Basic HTML escaping for the filename
    auto escape = [](std::string_view s) -> std::string {
        std::string r;
        r.reserve(s.size() + 8);
        for (char c : s) {
            switch (c) {
            case '&': r += "&amp;"; break;
            case '<': r += "&lt;"; break;
            case '>': r += "&gt;"; break;
            case '"': r += "&quot;"; break;
            default:  r.push_back(c); break;
            }
        }
        return r;
    };
    escaped_name = escape(escaped_name);

    bool is_audio = !(resolved.extension() == ".mp4" ||
                      resolved.extension() == ".webm" ||
                      resolved.extension() == ".ogv" ||
                      resolved.extension() == ".mkv" ||
                      resolved.extension() == ".avi" ||
                      resolved.extension() == ".mov");

    // Build the media URL with appropriate query parameter:
    // - With auth:    include a temporary media token so the browser's <video>
    //                 element can access the file without sending the
    //                 Authorization header (which it doesn't reliably do)
    // - Without auth: simple ?raw=1 to bypass the player page
    std::string media_query;
    if (username_ && password_) {
        std::string token = generate_media_token(resolved);
        media_query = "?mtoken=" + token;
    } else {
        media_query = "?raw=1";
    }
    // Simple path encoding: replace spaces and special chars
    auto url_encode_path = [](std::string_view p) -> std::string {
        std::string r;
        r.reserve(p.size() * 3);
        for (char c : p) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                c == '.' || c == '~' || c == '/') {
                r.push_back(c);
            } else {
                char hex[4];
                snprintf(hex, sizeof(hex), "%%%02X", static_cast<unsigned char>(c));
                r.append(hex, 3);
            }
        }
        return r;
    };
    std::string encoded_path = url_encode_path(req.path);

    std::string mime = utils::mime_type(resolved.string());
    uintmax_t fsize = file_ops::file_size(resolved);

    std::string html;
    html.reserve(2048);
    html += "<!DOCTYPE html>\r\n<html lang=\"en\">\r\n<head>\r\n";
    html += "<meta charset=\"UTF-8\">\r\n";
    html += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\r\n";
    html += "<title>" + escaped_name + "</title>\r\n";
    html += "<style>\r\n";
    html += "*{box-sizing:border-box;margin:0;padding:0;}\r\n";
    html += "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;";
    html += "background:#1a1a2e;color:#eee;display:flex;flex-direction:column;";
    html += "align-items:center;justify-content:center;min-height:100vh;}\r\n";
    html += ".player-container{width:100%;max-width:960px;padding:20px;}\r\n";
    html += ".header{text-align:center;margin-bottom:20px;}\r\n";
    html += ".header h1{font-size:1.2em;font-weight:400;color:#ccc;word-break:break-all;}\r\n";
    html += ".header .info{font-size:0.85em;color:#888;margin-top:8px;}\r\n";
    html += "video,audio{width:100%;border-radius:8px;box-shadow:0 4px 24px rgba(0,0,0,0.5);";
    html += "background:#000;outline:none;}\r\n";
    html += "audio{min-height:60px;}\r\n";
    html += ".back-link{display:inline-block;margin-top:20px;color:#888;text-decoration:none;";
    html += "font-size:0.9em;transition:color 0.2s;}\r\n";
    html += ".back-link:hover{color:#fff;}\r\n";
    html += "</style>\r\n</head>\r\n<body>\r\n";
    html += "<div class=\"player-container\">\r\n";
    html += "<div class=\"header\">\r\n";
    html += "<h1>" + escaped_name + "</h1>\r\n";
    html += "<div class=\"info\">" + utils::format_size(fsize) + " &bull; " + mime + "</div>\r\n";
    html += "</div>\r\n";

    if (is_audio) {
        html += "<audio controls autoplay preload=\"auto\">\r\n";
        html += "<source src=\"" + encoded_path + media_query + "\" type=\"" + mime + "\">\r\n";
        html += "Your browser does not support the audio element.\r\n";
        html += "</audio>\r\n";
    } else {
        html += "<video controls autoplay preload=\"auto\" playsinline>\r\n";
        html += "<source src=\"" + encoded_path + media_query + "\" type=\"" + mime + "\">\r\n";
        html += "Your browser does not support the video element.\r\n";
        html += "</video>\r\n";
    }

    html += "<a class=\"back-link\" href=\"javascript:history.back()\">&larr; Back to directory</a>\r\n";
    html += "</div>\r\n</body>\r\n</html>\r\n";

    http::Response resp;
    resp.status_code = 200;
    add_common_headers(resp);
    resp.set_content_type("text/html; charset=utf-8");
    resp.set_content_length(html.size());
    resp.body = std::move(html);
    return resp;
}

// ── Media token helpers ──────────────────────────────────────────────────────
// When HTTP Basic auth is enabled, browser <video>/<audio> elements don't
// reliably include the Authorization header in subresource requests. To
// work around this, the player page embeds a short-lived token in the
// media URL. The token is a djb2 hash of (path + expiry + password) and
// is valid for 1 hour.

static uint64_t djb2_hash(const std::string& data) {
    uint64_t hash = 5381;
    for (char c : data) {
        hash = ((hash << 5) + hash) + static_cast<unsigned char>(c);
    }
    return hash;
}

std::string WebDavHandler::generate_media_token(const fs::path& filepath) const {
    auto now = std::chrono::system_clock::now();
    auto expiry = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count() + 3600;  // 1 hour

    std::string key = filepath.string() + "|" + std::to_string(expiry) + "|" + password_.value_or("");
    return std::to_string(expiry) + "_" + std::to_string(djb2_hash(key));
}

bool WebDavHandler::verify_media_token(const fs::path& filepath, std::string_view token) const {
    auto underscore = token.find('_');
    if (underscore == std::string_view::npos) return false;

    auto expiry_sv = token.substr(0, underscore);
    auto hash_sv   = token.substr(underscore + 1);

    // Parse expiry
    int64_t expiry = 0;
    auto res = std::from_chars(expiry_sv.data(), expiry_sv.data() + expiry_sv.size(), expiry);
    if (res.ec != std::errc{}) return false;

    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (now > expiry) return false;

    // Parse expected hash
    uint64_t expected_hash = 0;
    res = std::from_chars(hash_sv.data(), hash_sv.data() + hash_sv.size(), expected_hash);
    if (res.ec != std::errc{}) return false;

    // Recompute hash
    std::string key = filepath.string() + "|" + std::string(expiry_sv) + "|" + password_.value_or("");
    return djb2_hash(key) == expected_hash;
}

// ── Thumbnail endpoint ───────────────────────────────────────────────────────

http::Response WebDavHandler::handle_thumbnail(const http::Request& req) {
    // Extract the 'path' query parameter
    std::string file_path;
    auto path_start = req.query.find("path=");
    if (path_start != std::string::npos) {
        file_path = req.query.substr(path_start + 5); // len("path=")
        auto amp = file_path.find('&');
        if (amp != std::string::npos) file_path.resize(amp);
        file_path = utils::url_decode(file_path);
    }

    if (file_path.empty()) {
        return error_response(400, "Bad Request — missing path parameter");
    }

    // Resolve the file path against root_dir
    fs::path resolved = file_ops::resolve_path(root_dir_, file_path);
    if (resolved.empty() || !file_ops::exists(resolved)) {
        return error_response(404, "Not Found");
    }

    // Check auth for the target file (or use media token)
    if (!check_auth(req, resolved)) {
        http::Response resp;
        resp.status_code = 401;
        resp.status_text = "Unauthorized";
        add_common_headers(resp);
        resp.set_header("WWW-Authenticate", "Basic realm=\"WebDAV\"");
        resp.set_content_length(0);
        return resp;
    }

    // Extract optional size parameter
    int size = 256;
    auto size_start = req.query.find("size=");
    if (size_start != std::string::npos) {
        std::string size_str = req.query.substr(size_start + 5);
        auto amp = size_str.find('&');
        if (amp != std::string::npos) size_str.resize(amp);
        if (!size_str.empty()) {
            int parsed = 0;
            auto res = std::from_chars(size_str.data(), size_str.data() + size_str.size(), parsed);
            if (res.ec == std::errc{} && parsed > 0 && parsed <= 1024) {
                size = parsed;
            }
        }
    }

    // Generate thumbnail
    std::string thumb_data = thumbnail::generate(resolved, size);
    std::string mime(thumbnail::mime_type(thumb_data));

    http::Response resp;
    resp.status_code = 200;
    add_common_headers(resp);
    resp.set_content_type(mime);
    resp.set_content_length(thumb_data.size());
    // Cache for 1 hour
    resp.set_header("Cache-Control", "public, max-age=3600");
    resp.body = std::move(thumb_data);
    return resp;
}

http::Response WebDavHandler::handle(const http::Request& req) {
    // Log every request for debugging
    std::cerr << "[REQ] " << req.method_str << " " << req.path
              << " (Auth: " << (req.header("Authorization").has_value() ? "yes" : "no")
              << ", UA: ";
    auto ua = req.header("User-Agent");
    if (ua) {
        std::string_view uav = *ua;
        if (uav.size() > 60) uav = uav.substr(0, 60);
        std::cerr << uav;
    }
    std::cerr << ")" << std::endl;

    // ── Thumbnail endpoint (before auth check — uses own token/auth logic) ─
    if (req.method == http::Method::GET && req.path == "/__thumb__") {
        return handle_thumbnail(req);
    }

    // ── Resolve path (needed early for token-based auth) ──────────────────
    fs::path resolved = file_ops::resolve_path(root_dir_, req.path);
    if (resolved.empty()) {
        return error_response(403, "Forbidden");
    }

    // ── OPTIONS: allow without auth for WebDAV client discovery ───────────
    // Many WebDAV clients (Windows Explorer, macOS Finder, davfs2) send an
    // initial OPTIONS request WITHOUT credentials to discover server
    // capabilities. Returning 401 breaks their discovery flow. Apache and
    // nginx both allow unauthenticated OPTIONS — it's the standard approach.
    bool is_options = (req.method == http::Method::OPTIONS);

    // ── Auth check (skip for OPTIONS) ────────────────────────────────────
    if (!is_options && !check_auth(req, resolved)) {
        std::cerr << "[AUTH] 401 for " << req.method_str << " " << req.path
                  << " (Authorization: " << (req.header("Authorization").has_value() ? "present" : "missing") << ")"
                  << std::endl;
        http::Response resp;
        resp.status_code = 401;
        resp.status_text = "Unauthorized";
        add_common_headers(resp);
        resp.set_header("WWW-Authenticate", "Basic realm=\"WebDAV\"");
        resp.set_content_type("text/html; charset=utf-8");
        std::string body = "<!DOCTYPE html><html><body><h1>401 Unauthorized</h1></body></html>";
        resp.set_content_length(body.size());
        resp.body = std::move(body);
        return resp;
    }

    switch (req.method) {
    case http::Method::OPTIONS:  return handle_options(req, resolved);
    case http::Method::GET:      return handle_get(req, resolved);
    case http::Method::HEAD:     return handle_head(req, resolved);
    case http::Method::PUT:      return handle_put(req, resolved);
    case http::Method::DELETE:   return handle_delete(req, resolved);
    case http::Method::MKCOL:    return handle_mkcol(req, resolved);
    case http::Method::PROPFIND: return handle_propfind(req, resolved);
    case http::Method::MOVE:     return handle_move(req, resolved);
    case http::Method::COPY:     return handle_copy(req, resolved);
    case http::Method::LOCK:
    case http::Method::UNLOCK:   return handle_lock_unlock(req, resolved);
    default:
        return error_response(501, "Not Implemented");
    }
}

http::Response WebDavHandler::handle_options(const http::Request& req, const fs::path& resolved) {
    (void)req;
    (void)resolved;
    http::Response resp;
    resp.status_code = 200;
    add_common_headers(resp);
    add_dav_header(resp);
    resp.set_content_length(0);
    return resp;
}

http::Response WebDavHandler::handle_get(const http::Request& req, const fs::path& resolved) {
    if (!file_ops::exists(resolved)) {
        return error_response(404, "Not Found");
    }

    // ── Directory → HTML listing ──────────────────────────────────────────
    if (file_ops::is_directory(resolved)) {
        std::string path = req.path;
        if (path.empty()) path = "/";

        // Build server origin for thumbnail URLs
        std::string origin;
        auto host_hdr = req.header("Host");
        if (host_hdr) {
            origin = "http://" + std::string(*host_hdr);
        }

        std::string html = html_dir::generate(path, resolved, origin);

        http::Response resp;
        resp.status_code = 200;
        add_common_headers(resp);
        resp.set_content_type("text/html; charset=utf-8");
        resp.set_content_length(html.size());
        resp.body = std::move(html);
        return resp;
    }

    // ── Browser requesting a media file → serve player page ───────────────
    // When auth is enabled, the player page embeds a temporary media token
    // in the video/audio source URL so the browser's media element can
    // access the file without sending HTTP Basic auth credentials (which
    // <video>/<audio> elements don't reliably include in subresource requests).
    if (allow_browser_ && is_browser_request(req) && prefers_html(req) && is_media_file(resolved)) {
        // Check if this is a raw media request (from the player page itself)
        bool has_mtoken = req.query.find("mtoken=") != std::string::npos;
        if (!has_mtoken) {
            return serve_media_player_page(req, resolved);
        }
        // else: query contains a media token → fall through to serve raw file
    }

    // ── Regular file → sendfile (zero-copy) + Range support ───────────────
    uintmax_t fsize = file_ops::file_size(resolved);
    auto range = req.parse_range(fsize);

    http::Response resp;
    add_common_headers(resp);
    resp.set_header("Last-Modified", utils::rfc1123_time(file_ops::last_modified(resolved)));
    resp.set_content_type(utils::mime_type(resolved.string()));

    if (range && fsize > 0) {
        // Clamp range to file size
        size_t range_start = range->start;
        size_t range_end   = range->end.value_or(static_cast<size_t>(fsize) - 1);

        if (range_start >= fsize) {
            // Range Not Satisfiable
            resp.status_code = 416;
            resp.set_header("Content-Range", "bytes */" + std::to_string(fsize));
            resp.set_content_length(0);
            return resp;
        }

        if (range_end >= fsize) range_end = static_cast<size_t>(fsize) - 1;

        size_t content_len = range_end - range_start + 1;

        resp.status_code = 206;
        resp.set_header("Content-Range",
            "bytes " + std::to_string(range_start) + "-" +
            std::to_string(range_end) + "/" + std::to_string(fsize));
        resp.set_content_length(content_len);

        if (content_len > 0) {
            resp.file_to_send = resolved.string();
            resp.file_offset = static_cast<off_t>(range_start);
        }
    } else {
        // Full file
        resp.status_code = 200;
        resp.set_content_length(static_cast<size_t>(fsize));
        if (fsize > 0) {
            resp.file_to_send = resolved.string();
        }
    }
    return resp;
}

http::Response WebDavHandler::handle_head(const http::Request& req, const fs::path& resolved) {
    http::Response resp = handle_get(req, resolved);
    resp.body.clear();
    resp.file_to_send.reset();
    return resp;
}

http::Response WebDavHandler::handle_put(const http::Request& req, const fs::path& resolved) {
    auto parent = resolved.parent_path();
    if (!file_ops::exists(parent)) {
        file_ops::create_directory(parent);
    }

    bool existed = file_ops::exists(resolved);

    // If overwriting an existing file, check it's not in use
    if (existed && file_ops::is_regular_file(resolved)) {
        int lock_fd = file_ops::try_lock_exclusive(resolved);
        if (lock_fd == -1) {
            // File is currently being streamed — reject
            return error_response(423, "Locked — file is being read");
        }
        if (lock_fd >= 0) ::close(lock_fd);  // release lock, we'll write now
    }

    if (!file_ops::write_file(resolved, req.body)) {
        return error_response(507, "Insufficient Storage");
    }

    http::Response resp;
    resp.status_code = existed ? 200 : 201;
    add_common_headers(resp);
    resp.set_content_length(0);
    return resp;
}

http::Response WebDavHandler::handle_delete(const http::Request& req, const fs::path& resolved) {
    (void)req;
    if (!file_ops::exists(resolved)) {
        return error_response(404, "Not Found");
    }

    // For regular files: check if in use before deleting
    if (file_ops::is_regular_file(resolved)) {
        int lock_fd = file_ops::try_lock_exclusive(resolved);
        if (lock_fd == -1) {
            return error_response(423, "Locked — file is being read");
        }
        if (lock_fd >= 0) ::close(lock_fd);
    }

    bool ok = file_ops::is_directory(resolved)
                ? file_ops::remove_all(resolved)
                : file_ops::remove(resolved);

    if (!ok) {
        return error_response(500, "Internal Server Error");
    }

    http::Response resp;
    resp.status_code = 204;
    add_common_headers(resp);
    resp.set_content_length(0);
    return resp;
}

http::Response WebDavHandler::handle_mkcol(const http::Request& req, const fs::path& resolved) {
    (void)req;
    if (file_ops::exists(resolved)) {
        return error_response(405, "Method Not Allowed");
    }

    auto parent = resolved.parent_path();
    if (!file_ops::exists(parent)) {
        return error_response(409, "Conflict — parent does not exist");
    }

    if (!file_ops::create_directory(resolved)) {
        return error_response(507, "Insufficient Storage");
    }

    http::Response resp;
    resp.status_code = 201;
    add_common_headers(resp);
    resp.set_content_length(0);
    return resp;
}

http::Response WebDavHandler::handle_propfind(const http::Request& req, const fs::path& resolved) {
    int depth = 0;
    std::string depth_str = req.depth();
    if (depth_str == "1") depth = 1;
    else if (depth_str == "infinity") depth = 1;

    std::string xml = xml_utils::propfind_response("/", root_dir_, resolved, depth);

    http::Response resp;
    resp.status_code = 207;
    add_common_headers(resp);
    resp.set_content_type("application/xml; charset=utf-8");
    resp.set_content_length(xml.size());
    resp.body = std::move(xml);
    return resp;
}

http::Response WebDavHandler::handle_move(const http::Request& req, const fs::path& resolved) {
    auto dest_hdr = req.header("Destination");
    if (!dest_hdr) {
        return error_response(400, "Bad Request — missing Destination header");
    }

    std::string dest_path = utils::url_decode(std::string(*dest_hdr));

    auto scheme_pos = dest_path.find("://");
    if (scheme_pos != std::string::npos) {
        auto path_start = dest_path.find('/', scheme_pos + 3);
        if (path_start != std::string::npos) {
            dest_path = dest_path.substr(path_start);
        }
    }

    fs::path dest_resolved = file_ops::resolve_path(root_dir_, dest_path);
    if (dest_resolved.empty()) {
        return error_response(403, "Forbidden");
    }

    if (!file_ops::exists(resolved)) {
        return error_response(404, "Not Found");
    }

    // For regular files: check if source file is in use
    if (file_ops::is_regular_file(resolved)) {
        int lock_fd = file_ops::try_lock_exclusive(resolved);
        if (lock_fd == -1) {
            return error_response(423, "Locked — file is being read");
        }
        if (lock_fd >= 0) ::close(lock_fd);
    }

    auto overwrite = req.header("Overwrite");
    bool allow_overwrite = !overwrite || *overwrite != "F";

    if (file_ops::exists(dest_resolved) && !allow_overwrite) {
        return error_response(412, "Precondition Failed");
    }

    // Also check destination if overwriting
    if (file_ops::exists(dest_resolved) && file_ops::is_regular_file(dest_resolved)) {
        int lock_fd = file_ops::try_lock_exclusive(dest_resolved);
        if (lock_fd == -1) {
            return error_response(423, "Locked — destination file is being read");
        }
        if (lock_fd >= 0) ::close(lock_fd);
    }

    auto dest_parent = dest_resolved.parent_path();
    if (!file_ops::exists(dest_parent)) {
        file_ops::create_directory(dest_parent);
    }

    if (!file_ops::rename(resolved, dest_resolved)) {
        return error_response(500, "Internal Server Error");
    }

    http::Response resp;
    resp.status_code = file_ops::exists(resolved) ? 204 : 201;
    add_common_headers(resp);
    resp.set_content_length(0);
    return resp;
}

http::Response WebDavHandler::handle_copy(const http::Request& req, const fs::path& resolved) {
    auto dest_hdr = req.header("Destination");
    if (!dest_hdr) {
        return error_response(400, "Bad Request — missing Destination header");
    }

    std::string dest_path = utils::url_decode(std::string(*dest_hdr));

    auto scheme_pos = dest_path.find("://");
    if (scheme_pos != std::string::npos) {
        auto path_start = dest_path.find('/', scheme_pos + 3);
        if (path_start != std::string::npos) {
            dest_path = dest_path.substr(path_start);
        }
    }

    fs::path dest_resolved = file_ops::resolve_path(root_dir_, dest_path);
    if (dest_resolved.empty()) {
        return error_response(403, "Forbidden");
    }

    if (!file_ops::exists(resolved)) {
        return error_response(404, "Not Found");
    }

    auto overwrite = req.header("Overwrite");
    bool allow_overwrite = !overwrite || *overwrite != "F";

    if (file_ops::exists(dest_resolved) && !allow_overwrite) {
        return error_response(412, "Precondition Failed");
    }

    auto dest_parent = dest_resolved.parent_path();
    if (!file_ops::exists(dest_parent)) {
        file_ops::create_directory(dest_parent);
    }

    if (!file_ops::copy(resolved, dest_resolved)) {
        return error_response(500, "Internal Server Error");
    }

    http::Response resp;
    resp.status_code = file_ops::exists(dest_resolved) ? 204 : 201;
    add_common_headers(resp);
    resp.set_content_length(0);
    return resp;
}

http::Response WebDavHandler::handle_lock_unlock(const http::Request& req, const fs::path& resolved) {
    (void)resolved;

    if (req.method == http::Method::LOCK) {
        std::string lock_token = "urn:uuid:webdav-lock-" +
            std::to_string(std::chrono::system_clock::now().time_since_epoch().count());

        std::string body =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
            "<D:prop xmlns:D=\"DAV:\">\r\n"
            "  <D:lockdiscovery>\r\n"
            "    <D:activelock>\r\n"
            "      <D:locktype><D:write/></D:locktype>\r\n"
            "      <D:lockscope><D:exclusive/></D:lockscope>\r\n"
            "      <D:depth>infinity</D:depth>\r\n"
            "      <D:locktoken><D:href>" + lock_token + "</D:href></D:locktoken>\r\n"
            "      <D:timeout>Second-3600</D:timeout>\r\n"
            "    </D:activelock>\r\n"
            "  </D:lockdiscovery>\r\n"
            "</D:prop>\r\n";

        http::Response resp;
        resp.status_code = 200;
        add_common_headers(resp);
        resp.set_header("Lock-Token", "<" + lock_token + ">");
        resp.set_content_type("application/xml; charset=utf-8");
        resp.set_content_length(body.size());
        resp.body = std::move(body);
        return resp;
    }

    http::Response resp;
    resp.status_code = 204;
    add_common_headers(resp);
    resp.set_content_length(0);
    return resp;
}
```

webdav_handler.h
```
#pragma once

#include "http_parser.h"
#include <filesystem>
#include <string>
#include <optional>

namespace fs = std::filesystem;

class WebDavHandler {
public:
    WebDavHandler(const fs::path& root_dir, bool allow_browser = true,
                  std::optional<std::string> username = std::nullopt,
                  std::optional<std::string> password = std::nullopt);

    // Handle an HTTP request, return the response
    http::Response handle(const http::Request& req);

    // The root directory being served
    const fs::path& root_dir() const { return root_dir_; }

private:
    fs::path root_dir_;
    bool allow_browser_;
    std::optional<std::string> username_;
    std::optional<std::string> password_;

    // Check HTTP Basic auth; returns true if authorized (or auth not configured)
    // resolved_path is used to verify media tokens embedded in query string
    bool check_auth(const http::Request& req, const fs::path& resolved_path);

    // Check if the request is from a browser (Accept: text/html)

    // Generate a short-lived token for unauthenticated media access
    [[nodiscard]] std::string generate_media_token(const fs::path& filepath) const;

    // Verify a media token is valid for the given file
    [[nodiscard]] bool verify_media_token(const fs::path& filepath, std::string_view token) const;
    static bool is_browser_request(const http::Request& req);

    // Check if Accept header prefers text/html (page navigation vs media request)
    static bool prefers_html(const http::Request& req);

    // Check if file is a video or audio file (by extension)
    static bool is_media_file(const fs::path& path);

    // Serve an HTML5 media player page for video/audio files
    http::Response serve_media_player_page(const http::Request& req, const fs::path& resolved);

    // Serve a thumbnail for a media file
    http::Response handle_thumbnail(const http::Request& req);

    // WebDAV method handlers
    http::Response handle_options(const http::Request& req, const fs::path& resolved);
    http::Response handle_get(const http::Request& req, const fs::path& resolved);
    http::Response handle_head(const http::Request& req, const fs::path& resolved);
    http::Response handle_put(const http::Request& req, const fs::path& resolved);
    http::Response handle_delete(const http::Request& req, const fs::path& resolved);
    http::Response handle_mkcol(const http::Request& req, const fs::path& resolved);
    http::Response handle_propfind(const http::Request& req, const fs::path& resolved);
    http::Response handle_move(const http::Request& req, const fs::path& resolved);
    http::Response handle_copy(const http::Request& req, const fs::path& resolved);
    http::Response handle_lock_unlock(const http::Request& req, const fs::path& resolved);

    // Build common response headers
    void add_common_headers(http::Response& resp);
    void add_dav_header(http::Response& resp);

    // Generate error response
    http::Response error_response(int code, std::string_view message = {});
};
```

xml_utils.cpp
```
#include "xml_utils.h"
#include "utils.h"
#include <cstdio>

namespace xml_utils {

// ── XML escape with fast-path (no alloc if no special chars) ─────────────────

// Returns the original data if no escaping needed (to avoid copy).
// Caller must use correctly — if escaped is empty, use s directly.
static std::string_view escape_xml_fast(std::string_view s, std::string& buf) {
    // Fast scan: find first special char
    size_t i = 0;
    for (; i < s.size(); ++i) {
        if (s[i] == '&' || s[i] == '<' || s[i] == '>' ||
            s[i] == '"' || s[i] == '\'') break;
    }
    if (i == s.size()) return s;  // no escaping needed — fast path!

    // Need escaping — build into buf
    buf.clear();
    buf.reserve(s.size() + 16);
    buf.append(s.data(), i);
    for (; i < s.size(); ++i) {
        switch (s[i]) {
        case '&':  buf += "&amp;"; break;
        case '<':  buf += "&lt;"; break;
        case '>':  buf += "&gt;"; break;
        case '"':  buf += "&quot;"; break;
        case '\'': buf += "&apos;"; break;
        default:   buf.push_back(s[i]); break;
        }
    }
    return std::string_view(buf);
}

// ── Append a single <D:response> element directly to output buffer ───────────

static void append_file_xml(std::string& out,
                            std::string_view href,
                            const file_ops::DirEntry& entry,
                            std::string& esc_buf)
{
    auto href_safe = escape_xml_fast(href, esc_buf);
    auto name_safe = escape_xml_fast(entry.name, esc_buf);

    out += "    <D:response>\r\n";
    out += "      <D:href>";
    out += href_safe;
    out += "</D:href>\r\n";
    out += "      <D:propstat>\r\n";
    out += "        <D:prop>\r\n";

    if (entry.is_directory) {
        out += "          <D:resourcetype><D:collection/></D:resourcetype>\r\n";
    } else {
        out += "          <D:resourcetype/>\r\n";
    }

    out += "          <D:displayname>";
    out += name_safe;
    out += "</D:displayname>\r\n";

    if (!entry.is_directory) {
        out += "          <D:getcontentlength>";
        out += std::to_string(entry.size);
        out += "</D:getcontentlength>\r\n";
    }

    out += "          <D:getlastmodified>";
    out += utils::rfc1123_time(entry.last_modified);
    out += "</D:getlastmodified>\r\n";

    out += "          <D:creationdate>";
    out += utils::iso8601_time(entry.creation_time);
    out += "</D:creationdate>\r\n";

    if (!entry.is_directory) {
        auto mt = utils::mime_type(entry.name);
        auto mt_safe = escape_xml_fast(mt, esc_buf);
        out += "          <D:getcontenttype>";
        out += mt_safe;
        out += "</D:getcontenttype>\r\n";

        // ETag: size-mtime
        char etag_buf[64];
        int n = snprintf(etag_buf, sizeof(etag_buf), "\"%ju-%ld\"",
                         static_cast<uintmax_t>(entry.size),
                         static_cast<long>(entry.last_modified.time_since_epoch().count()));
        out += "          <D:getetag>";
        out.append(etag_buf, static_cast<size_t>(n));
        out += "</D:getetag>\r\n";
    }

    out += "        </D:prop>\r\n";
    out += "        <D:status>HTTP/1.1 200 OK</D:status>\r\n";
    out += "      </D:propstat>\r\n";
    out += "    </D:response>\r\n";
}

// ── Main PROPFIND response builder ───────────────────────────────────────────

std::string propfind_response(
    std::string_view href_prefix,
    const fs::path& root_dir,
    const fs::path& resolved_path,
    int depth)
{
    // ── href prefix ──────────────────────────────────────────────────────
    std::string prefix(href_prefix);
    if (!prefix.empty() && prefix.back() != '/') prefix += '/';

    // ── Relative path ────────────────────────────────────────────────────
    std::string rel_path = "/";
    bool is_dir = file_ops::is_directory(resolved_path);

    if (resolved_path != root_dir) {
        auto rel = fs::relative(resolved_path, root_dir).string();
        std::replace(rel.begin(), rel.end(), '\\', '/');
        if (!rel.empty() && rel != ".") {
            rel_path = prefix + rel;
            if (!rel_path.empty() && rel_path.back() != '/' && is_dir) {
                rel_path += '/';
            }
        }
    }

    // ── Estimate output size: header + ~400B per entry ──────────────────
    std::string out;
    out.reserve(1024);  // will grow if needed; most dirs < 50 files fit
    std::string esc_buf;  // reusable temp for escape_xml_fast

    out += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n";
    out += "<D:multistatus xmlns:D=\"DAV:\">\r\n";

    if (file_ops::exists(resolved_path)) {
        auto entry = file_ops::get_entry(resolved_path);
        append_file_xml(out, rel_path, entry, esc_buf);

        if (depth == 1 && entry.is_directory) {
            auto children = file_ops::list_directory(resolved_path);

            // Pre-build href prefix to avoid concat in loop
            std::string href_base = rel_path;
            if (href_base != "/" && !href_base.empty() && href_base.back() != '/') {
                href_base += '/';
            }

            for (const auto& child : children) {
                std::string child_href;
                child_href.reserve(href_base.size() + child.name.size() + 2);
                child_href = href_base;
                child_href += child.name;
                if (child.is_directory) child_href += '/';
                append_file_xml(out, child_href, child, esc_buf);
            }
        }
    } else {
        out += "    <D:response>\r\n";
        out += "      <D:href>";
        out += rel_path;
        out += "</D:href>\r\n";
        out += "      <D:propstat>\r\n";
        out += "        <D:prop/>\r\n";
        out += "        <D:status>HTTP/1.1 404 Not Found</D:status>\r\n";
        out += "      </D:propstat>\r\n";
        out += "    </D:response>\r\n";
    }

    out += "</D:multistatus>\r\n";
    return out;
}

} // namespace xml_utils
```

xml_utils.h
```
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
```
