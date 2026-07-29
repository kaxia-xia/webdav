#include "server.h"
#include "http_parser.h"
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
        ssize_t n = ::recv(client_fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                // Timeout — keep-alive idle, close gracefully
                break;
            }
            break; // connection closed or error
        }

        std::string_view data(buf, static_cast<size_t>(n));
        std::string full_data;
        if (!pending.empty()) {
            full_data = std::move(pending);
            full_data.append(data);
            data = full_data;
        }

        // Parse and handle all complete requests in the buffer
        while (!data.empty() && running_) {
            if (!parser.parse(data)) {
                if (parser.needs_more()) {
                    pending = std::string(data);
                    break;
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

            // Check Connection header — replace, don't append
            keep_alive = true;
            auto conn = req.header("Connection");
            if (conn && utils::iequals(*conn, "close")) {
                keep_alive = false;
                resp.set_header("Connection", "close");
            } else {
                resp.set_header("Connection", "keep-alive");
            }

            // Build and send response headers (body is empty for file responses)
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

            // Get leftover data after this request
            data = parser.leftover();
            parser.reset();

            if (!keep_alive) {
                goto close_connection;
            }
        }

        if (data.empty()) {
            pending.clear();
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