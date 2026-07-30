#include "server.h"
#include "http_parser.h"
#include "thumbnail.h"
#include "file_ops.h"
#include "utils.h"
#include <iostream>
#include <cstring>
#include <cerrno>

#include <sys/socket.h>
#include <sys/eventfd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

// liburing is a C library with kernel ABI structs that use C idioms
// (anonymous structs, zero-length arrays, flexible array members).
// Suppress C++ pedantic warnings for the inclusion.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <liburing.h>
#pragma GCC diagnostic pop

// ── Per-thread constants ─────────────────────────────────────────────────────
static constexpr unsigned RING_ENTRIES     = 512;
static constexpr unsigned MAX_ACCEPTS      = 16;
static constexpr uintptr_t ACCEPT_MARKER   = 1;
static constexpr size_t   STREAM_THRESHOLD = 65536;
static constexpr size_t   BUF_SIZE         = 65536;

// ── Constructor / Destructor ─────────────────────────────────────────────────

Server::Server(const fs::path& root_dir, int port, bool allow_browser,
              std::optional<std::string> username,
              std::optional<std::string> password)
    : port_(port)
    , handler_(root_dir, allow_browser, std::move(username), std::move(password))
    , num_workers_(std::thread::hardware_concurrency())
{
    if (num_workers_ < 1) num_workers_ = 4;
    std::cout << "[INFO] " << num_workers_ << " workers, io_uring backend" << std::endl;
    thumbnail::init();
}

Server::~Server() { shutdown(); }

void Server::shutdown() {
    if (!running_.exchange(false)) return;
    if (shutdown_eventfd_ >= 0) {
        uint64_t val = 1;
        ssize_t n = write(shutdown_eventfd_, &val, sizeof(val));
        (void)n;
    }
    if (server_fd_ >= 0) {
        ::close(server_fd_);
        server_fd_ = -1;
    }
}

// ── Helpers ──────────────────────────────────────────────────────────────────

void Server::set_socket_options(int fd) {
    struct timeval tv{30, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}

void Server::close_connection(Connection* conn) {
    if (conn->fd >= 0)         { ::close(conn->fd); conn->fd = -1; }
    if (conn->file_fd >= 0)    { ::close(conn->file_fd); conn->file_fd = -1; }
    if (conn->output_fd >= 0)  { ::close(conn->output_fd); conn->output_fd = -1; }
    if (conn->splice_pipe[0] >= 0) { ::close(conn->splice_pipe[0]); conn->splice_pipe[0] = -1; }
    if (conn->splice_pipe[1] >= 0) { ::close(conn->splice_pipe[1]); conn->splice_pipe[1] = -1; }
    // Clean up incomplete upload temp file
    if (!conn->output_tmp_path.empty()) {
        ::unlink(conn->output_tmp_path.c_str());
        conn->output_tmp_path.clear();
    }
    // Clean up renamed but abandoned file (write completed, rename done,
    // but connection died before response sent — file is valid but orphaned
    // from the client's perspective; we leave it since rename already happened)
    delete conn;
}

void Server::reset_connection(Connection* conn) {
    conn->state = State::READING_REQUEST;
    conn->read_offset = 0;
    conn->parser.reset();
    conn->response_data.clear();
    conn->send_offset = 0;
    conn->keep_alive = true;
    if (conn->file_fd >= 0)    { ::close(conn->file_fd); conn->file_fd = -1; }
    if (conn->splice_pipe[0] >= 0) { ::close(conn->splice_pipe[0]); conn->splice_pipe[0] = -1; }
    if (conn->splice_pipe[1] >= 0) { ::close(conn->splice_pipe[1]); conn->splice_pipe[1] = -1; }
    conn->file_remaining = 0;
    conn->splice_pending = 0;
    conn->splice_phase = SplicePhase::TO_PIPE;
    if (conn->output_fd >= 0)  { ::close(conn->output_fd); conn->output_fd = -1; }
    conn->body_expected = 0;
    conn->body_received = 0;
    conn->put_write_pending = false;
    conn->put_write_size = 0;
    conn->output_tmp_path.clear();
    conn->output_final_path.clear();
}

std::string Server::build_response_string(int code, bool keep_alive) {
    http::Response resp;
    resp.status_code = code;
    resp.set_header("Server", "WebDAV-Server/1.3 (C++20)");
    resp.set_header("Date", utils::rfc1123_now());
    resp.set_header("Accept-Ranges", "bytes");
    resp.set_header("Connection", keep_alive ? "keep-alive" : "close");
    resp.set_content_length(0);
    return resp.to_string();
}

// ── ETag generator (weak, from mtime + size) ─────────────────────────────────

// ── Worker thread (io_uring event loop) ──────────────────────────────────────

void Server::worker_loop() {
    struct io_uring ring;
    struct io_uring_params params = {};
    params.flags = IORING_SETUP_SINGLE_ISSUER;

    int ret = io_uring_queue_init_params(RING_ENTRIES, &ring, &params);
    if (ret < 0) {
        std::cerr << "[ERROR] io_uring init: " << std::strerror(-ret) << std::endl;
        return;
    }

    // ── Submission lambdas ──────────────────────────────────────────────────

    auto uring_accept = [&]() -> bool {
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
        if (!sqe) return false;
        io_uring_prep_accept(sqe, server_fd_, nullptr, nullptr, 0);
        io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(ACCEPT_MARKER));
        return true;
    };

    auto uring_recv = [&](Connection* conn) -> bool {
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
        if (!sqe) return false;
        size_t space = BUF_SIZE - conn->read_offset;
        if (space == 0) return false;
        io_uring_prep_recv(sqe, conn->fd, conn->read_buf + conn->read_offset, space, 0);
        io_uring_sqe_set_data(sqe, conn);
        return true;
    };

    auto uring_send = [&](Connection* conn) -> bool {
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
        if (!sqe) return false;
        size_t len = conn->response_data.size() - conn->send_offset;
        io_uring_prep_send(sqe, conn->fd,
                           conn->response_data.data() + conn->send_offset, len, 0);
        io_uring_sqe_set_data(sqe, conn);
        return true;
    };

    // ── Zero-copy splice helpers (file ↔ pipe ↔ socket) ──────────────────

    auto uring_splice_to_pipe = [&](Connection* conn) -> bool {
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
        if (!sqe) return false;
        size_t chunk = conn->file_remaining;
        if (chunk > PIPE_CAPACITY) chunk = PIPE_CAPACITY;
        io_uring_prep_splice(sqe, conn->file_fd, conn->file_off,
                             conn->splice_pipe[1], -1,
                             static_cast<unsigned>(chunk),
                             SPLICE_F_MOVE | SPLICE_F_MORE);
        io_uring_sqe_set_data(sqe, conn);
        return true;
    };

    auto uring_splice_to_socket = [&](Connection* conn) -> bool {
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
        if (!sqe) return false;
        io_uring_prep_splice(sqe, conn->splice_pipe[0], -1,
                             conn->fd, -1,
                             static_cast<unsigned>(conn->splice_pending),
                             SPLICE_F_MOVE | SPLICE_F_MORE);
        io_uring_sqe_set_data(sqe, conn);
        return true;
    };

    // ── Async write helper (PUT streaming) ───────────────────────────────

    auto uring_write_body = [&](Connection* conn) -> bool {
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
        if (!sqe) return false;
        io_uring_prep_write(sqe, conn->output_fd,
                            conn->read_buf, conn->put_write_size, 0);
        io_uring_sqe_set_data(sqe, conn);
        return true;
    };

    int accept_in_flight = 0;
    for (unsigned i = 0; i < MAX_ACCEPTS; i++) {
        if (uring_accept()) accept_in_flight++;
    }
    io_uring_submit(&ring);

    while (running_) {
        struct io_uring_cqe* cqe;
        ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0) {
            if (ret == -EINTR) continue;
            break;
        }

        uintptr_t ud = reinterpret_cast<uintptr_t>(io_uring_cqe_get_data(cqe));
        int res = cqe->res;

        // ── Accept completion ─────────────────────────────────────────────
        if (ud == ACCEPT_MARKER) {
            accept_in_flight--;
            if (res >= 0) {
                int client_fd = res;
                set_socket_options(client_fd);

                auto* conn = new Connection;
                conn->fd = client_fd;

                if (!uring_recv(conn)) close_connection(conn);
            }
            while (accept_in_flight < static_cast<int>(MAX_ACCEPTS)) {
                if (!uring_accept()) break;
                accept_in_flight++;
            }
        }
        // ── Connection I/O completion ─────────────────────────────────────
        else if (ud != 0) {
            auto* conn = reinterpret_cast<Connection*>(ud);

            switch (conn->state) {

            // ══════════════════════════════════════════════════════════════
            case State::READING_REQUEST: {
                if (res <= 0) { close_connection(conn); break; }

                conn->read_offset += static_cast<size_t>(res);
                std::string_view data(conn->read_buf, conn->read_offset);
                bool done = conn->parser.parse(data, STREAM_THRESHOLD);

                if (!done) {
                    if (conn->read_offset >= BUF_SIZE) { close_connection(conn); break; }
                    if (!uring_recv(conn)) close_connection(conn);
                    break;
                }

                const auto& req = conn->parser.request();

                // ── Streaming PUT ──────────────────────────────────────
                if (req.body_truncated && req.method == http::Method::PUT) {
                    http::Response resp = handler_.handle(req);

                    if (resp.body_output_fd >= 0) {
                        conn->state = State::RECEIVING_BODY;
                        conn->output_fd = resp.body_output_fd;
                        conn->output_final_path = resp.body_output_path;
                        conn->output_tmp_path = resp.body_tmp_path;
                        conn->body_expected = resp.body_expected;
                        conn->body_received = 0;
                        conn->existed_before_put = (resp.status_code == 200);
                        conn->put_write_pending = false;

                        std::string_view lv = conn->parser.leftover();
                        if (!lv.empty()) {
                            // Submit async write for leftover — always,
                            // even if it's the entire body. Completion
                            // handled in RECEIVING_BODY.
                            conn->put_write_size = lv.size();
                            memmove(conn->read_buf, lv.data(), lv.size());
                            conn->body_received = lv.size();
                            conn->put_write_pending = true;
                            if (!uring_write_body(conn)) close_connection(conn);
                            break;
                        }

                        conn->read_offset = 0;
                        if (!uring_recv(conn)) close_connection(conn);
                        break;
                    }

                    // Fall through: no streaming, use response directly
                    conn->state = State::SENDING_HEADERS;
                    conn->response_data = resp.to_string();
                    conn->send_offset = 0;
                    auto ch = req.header("Connection");
                    if (ch && utils::iequals(*ch, "close")) conn->keep_alive = false;
                    if (!uring_send(conn)) close_connection(conn);
                    break;
                }

                // ── Normal request ─────────────────────────────────────
                {
                    http::Response resp = handler_.handle(req);
                    auto ch = req.header("Connection");
                    if (ch && utils::iequals(*ch, "close")) {
                        conn->keep_alive = false;
                        resp.set_header("Connection", "close");
                    } else {
                        resp.set_header("Connection", "keep-alive");
                    }

                    conn->response_data = resp.to_string();
                    conn->send_offset = 0;

                    // ── File send: set up splice pipe ────────────────
                    if (resp.file_to_send) {
                        conn->file_fd = ::open(resp.file_to_send->c_str(), O_RDONLY);
                        if (conn->file_fd < 0) { close_connection(conn); break; }
                        file_ops::lock_shared(conn->file_fd);
                        conn->file_off = resp.file_offset;
                        conn->file_remaining = resp.content_length_opt().value_or(0);

                        // Create pipe + expand to 1 MiB for fewer splice rounds
                        if (::pipe2(conn->splice_pipe, O_NONBLOCK) < 0) {
                            close_connection(conn); break;
                        }
                        ::fcntl(conn->splice_pipe[0], F_SETPIPE_SZ, PIPE_CAPACITY);
                        ::fcntl(conn->splice_pipe[1], F_SETPIPE_SZ, PIPE_CAPACITY);
                        conn->splice_phase = SplicePhase::TO_PIPE;
                        conn->splice_pending = 0;
                    }

                    conn->state = State::SENDING_HEADERS;
                    if (!uring_send(conn)) close_connection(conn);
                }
                break;
            }

            // ══════════════════════════════════════════════════════════════
            case State::RECEIVING_BODY: {
                if (conn->put_write_pending) {
                    // ── Async write completion ─────────────────────────
                    conn->put_write_pending = false;
                    if (res < 0) {
                        if (conn->output_fd >= 0) { ::close(conn->output_fd); conn->output_fd = -1; }
                        if (!conn->output_tmp_path.empty()) ::unlink(conn->output_tmp_path.c_str());
                        close_connection(conn);
                        break;
                    }
                    // Handle short writes (disk full, quota, etc.)
                    if (static_cast<size_t>(res) < conn->put_write_size) {
                        size_t wrote = static_cast<size_t>(res);
                        conn->body_received -= (conn->put_write_size - wrote);
                        size_t remaining = conn->put_write_size - wrote;
                        memmove(conn->read_buf, conn->read_buf + wrote, remaining);
                        conn->put_write_size = remaining;
                        conn->put_write_pending = true;
                        if (!uring_write_body(conn)) close_connection(conn);
                        break;
                    }
                    // Full write: check if done
                    if (conn->body_received >= conn->body_expected) {
                        ::close(conn->output_fd); conn->output_fd = -1;
                        if (!conn->output_tmp_path.empty() && !conn->output_final_path.empty()) {
                            ::rename(conn->output_tmp_path.c_str(), conn->output_final_path.c_str());
                        }
                        conn->output_tmp_path.clear();
                        conn->output_final_path.clear();
                        conn->response_data = build_response_string(
                            conn->existed_before_put ? 200 : 201, conn->keep_alive);
                        conn->send_offset = 0;
                        conn->state = State::SENDING_HEADERS;
                        if (!uring_send(conn)) close_connection(conn);
                        break;
                    }
                    // Submit next recv
                    conn->read_offset = 0;
                    if (!uring_recv(conn)) close_connection(conn);
                    break;
                }

                // ── Recv completion → submit async write ──────────────
                if (res <= 0) {
                    if (conn->output_fd >= 0) { ::close(conn->output_fd); conn->output_fd = -1; }
                    if (!conn->output_tmp_path.empty()) ::unlink(conn->output_tmp_path.c_str());
                    close_connection(conn);
                    break;
                }

                conn->put_write_size = static_cast<size_t>(res);
                conn->put_write_pending = true;
                conn->body_received += static_cast<size_t>(res);

                if (!uring_write_body(conn)) {
                    if (conn->output_fd >= 0) { ::close(conn->output_fd); conn->output_fd = -1; }
                    if (!conn->output_tmp_path.empty()) ::unlink(conn->output_tmp_path.c_str());
                    close_connection(conn);
                }
                break;
            }

            // ══════════════════════════════════════════════════════════════
            case State::SENDING_HEADERS: {
                if (res <= 0) { close_connection(conn); break; }

                conn->send_offset += static_cast<size_t>(res);
                if (conn->send_offset < conn->response_data.size()) {
                    if (!uring_send(conn)) close_connection(conn);
                    break;
                }

                if (conn->file_fd >= 0 && conn->file_remaining > 0) {
                    // Kick off zero-copy splice: file → pipe
                    conn->state = State::SENDING_FILE;
                    conn->splice_phase = SplicePhase::TO_PIPE;
                    if (!uring_splice_to_pipe(conn)) close_connection(conn);
                } else if (conn->keep_alive) {
                    reset_connection(conn);
                    if (!uring_recv(conn)) close_connection(conn);
                } else {
                    close_connection(conn);
                }
                break;
            }

            // ══════════════════════════════════════════════════════════════
            //  Zero-copy file send via splice(2):
            //    file_fd → pipe → socket   (all in kernel, no userspace copy)
            //  Pipe is 1 MiB — reduces syscall count ~16× vs default 64 KiB.
            // ══════════════════════════════════════════════════════════════
            case State::SENDING_FILE: {
                if (res <= 0) { close_connection(conn); break; }

                if (conn->splice_phase == SplicePhase::TO_PIPE) {
                    // Just completed splice(file → pipe)
                    conn->splice_pending = static_cast<size_t>(res);
                    conn->splice_phase = SplicePhase::TO_SOCKET;

                    if (!uring_splice_to_socket(conn)) close_connection(conn);
                } else {
                    // Just completed splice(pipe → socket)
                    size_t sent = static_cast<size_t>(res);

                    if (sent < conn->splice_pending) {
                        // Short write — socket buffer full, retry remainder
                        conn->splice_pending -= sent;
                        if (!uring_splice_to_socket(conn)) close_connection(conn);
                    } else {
                        // All pipe data consumed → advance file cursor
                        conn->file_off += static_cast<off_t>(conn->splice_pending);
                        conn->file_remaining -= conn->splice_pending;
                        conn->splice_pending = 0;

                        if (conn->file_remaining > 0) {
                            // More file data → next chunk
                            conn->splice_phase = SplicePhase::TO_PIPE;
                            if (!uring_splice_to_pipe(conn)) close_connection(conn);
                        } else {
                            // Done!
                            ::close(conn->file_fd); conn->file_fd = -1;
                            ::close(conn->splice_pipe[0]); conn->splice_pipe[0] = -1;
                            ::close(conn->splice_pipe[1]); conn->splice_pipe[1] = -1;
                            if (conn->keep_alive) {
                                reset_connection(conn);
                                if (!uring_recv(conn)) close_connection(conn);
                            } else {
                                close_connection(conn);
                            }
                        }
                    }
                }
                break;
            }

            case State::CLOSING:
                close_connection(conn);
                break;
            }
        }

        io_uring_cqe_seen(&ring, cqe);
        io_uring_submit(&ring);
    }

    io_uring_queue_exit(&ring);
}

// ── Run server ───────────────────────────────────────────────────────────────

void Server::run() {
    server_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (server_fd_ < 0) {
        std::cerr << "[ERROR] socket: " << std::strerror(errno) << std::endl;
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
        std::cerr << "[ERROR] bind: " << std::strerror(errno) << std::endl;
        ::close(server_fd_); server_fd_ = -1; return;
    }
    if (::listen(server_fd_, SOMAXCONN) < 0) {
        std::cerr << "[ERROR] listen: " << std::strerror(errno) << std::endl;
        ::close(server_fd_); server_fd_ = -1; return;
    }

    shutdown_eventfd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    std::cout << "[INFO] http://0.0.0.0:" << port_ << " — " << handler_.root_dir() << std::endl;
    std::cout << "[INFO] I/O: io_uring + splice (1 MiB pipe) — kernel 6.6, liburing 2.1" << std::endl;

    workers_.reserve(num_workers_);
    for (unsigned i = 0; i < num_workers_; ++i) {
        workers_.emplace_back(&Server::worker_loop, this);
    }
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();

    if (shutdown_eventfd_ >= 0) { ::close(shutdown_eventfd_); shutdown_eventfd_ = -1; }
    if (server_fd_ >= 0)        { ::close(server_fd_); server_fd_ = -1; }

    std::cout << "[INFO] Server stopped." << std::endl;
}
