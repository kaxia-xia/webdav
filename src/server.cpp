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
#include <pthread.h>

// liburing is a C library with kernel ABI structs that use C idioms
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <liburing.h>
#pragma GCC diagnostic pop

// ── Per-thread constants ─────────────────────────────────────────────────────
static constexpr unsigned RING_ENTRIES     = 512;
static constexpr unsigned CQE_BATCH        = 64;
static constexpr uintptr_t ACCEPT_MARKER   = 1;
static constexpr size_t   STREAM_THRESHOLD = 65536;
static constexpr size_t   BUF_SIZE         = 65536;

// ── Cached Date header (1-second granularity) ────────────────────────────────
static thread_local time_t tls_cached_epoch = 0;
static thread_local char   tls_cached_date[40];

static std::string_view get_cached_date() {
    time_t now = time(nullptr);
    if (now != tls_cached_epoch) {
        tls_cached_epoch = now;
        struct tm tm_buf;
        gmtime_r(&now, &tm_buf);
        static const char* days[]   = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
        static const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                       "Jul","Aug","Sep","Oct","Nov","Dec"};
        int n = snprintf(tls_cached_date, sizeof(tls_cached_date),
                         "%s, %02d %s %04d %02d:%02d:%02d GMT",
                         days[tm_buf.tm_wday], tm_buf.tm_mday, months[tm_buf.tm_mon],
                         tm_buf.tm_year + 1900, tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
        tls_cached_date[static_cast<size_t>(n)] = '\0';
    }
    return std::string_view(tls_cached_date);
}

// ── Constructor / Destructor ─────────────────────────────────────────────────

Server::Server(const fs::path& root_dir, int port, bool allow_browser,
              std::optional<std::string> username,
              std::optional<std::string> password)
    : port_(port)
    , handler_(root_dir, allow_browser, std::move(username), std::move(password))
    , num_workers_(std::thread::hardware_concurrency())
{
    if (num_workers_ < 1) num_workers_ = 4;
    std::cout << "[INFO] " << num_workers_ << " workers, io_uring backend "
              << "(DEFER_TASKRUN + multishot accept + batch CQE + async openat)" << std::endl;
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

// Thread-local ring pointer for fire-and-forget async close
static thread_local struct io_uring* tls_ring = nullptr;

static void uring_async_close(int fd) {
    if (fd < 0 || !tls_ring) { if (fd >= 0) ::close(fd); return; }
    struct io_uring_sqe* sqe = io_uring_get_sqe(tls_ring);
    if (!sqe) { ::close(fd); return; }
    io_uring_prep_close(sqe, fd);
    sqe->flags |= IOSQE_CQE_SKIP_SUCCESS;
}

void Server::set_socket_options(int fd) {
    struct timeval tv{30, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}

void Server::close_connection(Connection* conn) {
    uring_async_close(conn->fd);         conn->fd = -1;
    uring_async_close(conn->file_fd);    conn->file_fd = -1;
    uring_async_close(conn->output_fd);  conn->output_fd = -1;
    uring_async_close(conn->splice_pipe[0]); conn->splice_pipe[0] = -1;
    uring_async_close(conn->splice_pipe[1]); conn->splice_pipe[1] = -1;
    if (!conn->output_tmp_path.empty()) {
        ::unlink(conn->output_tmp_path.c_str());
        conn->output_tmp_path.clear();
    }
    delete conn;
}

void Server::reset_connection(Connection* conn) {
    conn->state = State::READING_REQUEST;
    conn->read_offset = 0;
    conn->parser.reset();
    conn->response_data.clear();
    conn->send_offset = 0;
    conn->keep_alive = true;
    uring_async_close(conn->file_fd);    conn->file_fd = -1;
    uring_async_close(conn->splice_pipe[0]); conn->splice_pipe[0] = -1;
    uring_async_close(conn->splice_pipe[1]); conn->splice_pipe[1] = -1;
    conn->file_remaining = 0;
    conn->splice_pending = 0;
    conn->splice_phase = SplicePhase::TO_PIPE;
    conn->file_path.clear();
    uring_async_close(conn->output_fd);  conn->output_fd = -1;
    conn->body_expected = 0;
    conn->body_received = 0;
    conn->put_write_pending = false;
    conn->put_write_size = 0;
    conn->output_tmp_path.clear();
    conn->output_final_path.clear();
}

std::string Server::build_response_string(int code, bool keep_alive) {
    const char* status_text = http::status_message(code);
    auto date = get_cached_date();
    const char* conn = keep_alive ? "keep-alive" : "close";
    std::string result;
    result.reserve(200);
    result  = "HTTP/1.1 ";
    result += std::to_string(code);
    result += " ";
    result += status_text;
    result += "\r\nServer: WebDAV-Server/1.5 (C++20)\r\nDate: ";
    result.append(date.data(), date.size());
    result += "\r\nAccept-Ranges: bytes\r\nConnection: ";
    result += conn;
    result += "\r\nContent-Length: 0\r\n\r\n";
    return result;
}

// ── Worker thread ────────────────────────────────────────────────────────────

void Server::worker_loop(unsigned worker_id) {
    // ── CPU affinity — pin each worker to a dedicated core ─────────────────
    {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        unsigned num_cpus = std::thread::hardware_concurrency();
        if (num_cpus == 0) num_cpus = 4;
        CPU_SET(worker_id % num_cpus, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    }

    struct io_uring ring;
    struct io_uring_params params = {};
    params.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;

    int ret = io_uring_queue_init_params(RING_ENTRIES, &ring, &params);
    tls_ring = &ring;
    if (ret < 0) {
        std::cerr << "[ERROR] io_uring init: " << std::strerror(-ret) << std::endl;
        return;
    }

    // ── Inline helpers (capture &ring implicitly via local scope) ──────────

    auto get_sqe = [&]() -> struct io_uring_sqe* { return io_uring_get_sqe(&ring); };

    auto submit_recv = [&](Connection* conn) {
        struct io_uring_sqe* sqe = get_sqe();
        if (!sqe) { close_connection(conn); return false; }
        size_t space = BUF_SIZE - conn->read_offset;
        io_uring_prep_recv(sqe, conn->fd, conn->read_buf + conn->read_offset, space, 0);
        io_uring_sqe_set_data(sqe, conn);
        return true;
    };

    auto submit_send = [&](Connection* conn) {
        struct io_uring_sqe* sqe = get_sqe();
        if (!sqe) { close_connection(conn); return false; }
        size_t len = conn->response_data.size() - conn->send_offset;
        io_uring_prep_send(sqe, conn->fd,
                           conn->response_data.data() + conn->send_offset,
                           len > 0 ? len : 0, MSG_NOSIGNAL);
        io_uring_sqe_set_data(sqe, conn);
        return true;
    };

    auto submit_splice_to_pipe = [&](Connection* conn) {
        struct io_uring_sqe* sqe = get_sqe();
        if (!sqe) { close_connection(conn); return false; }
        size_t chunk = conn->file_remaining;
        if (chunk > PIPE_CAPACITY) chunk = PIPE_CAPACITY;
        io_uring_prep_splice(sqe, conn->file_fd, conn->file_off,
                             conn->splice_pipe[1], -1,
                             static_cast<unsigned>(chunk),
                             SPLICE_F_MOVE | SPLICE_F_MORE);
        io_uring_sqe_set_data(sqe, conn);
        return true;
    };

    auto submit_splice_to_socket = [&](Connection* conn) {
        struct io_uring_sqe* sqe = get_sqe();
        if (!sqe) { close_connection(conn); return false; }
        io_uring_prep_splice(sqe, conn->splice_pipe[0], -1,
                             conn->fd, -1,
                             static_cast<unsigned>(conn->splice_pending),
                             SPLICE_F_MOVE | SPLICE_F_MORE);
        io_uring_sqe_set_data(sqe, conn);
        return true;
    };

    auto submit_write_body = [&](Connection* conn) {
        struct io_uring_sqe* sqe = get_sqe();
        if (!sqe) return false;
        io_uring_prep_write(sqe, conn->output_fd,
                            conn->read_buf, conn->put_write_size, -1);
        io_uring_sqe_set_data(sqe, conn);
        return true;
    };

    auto submit_openat = [&](Connection* conn) {
        struct io_uring_sqe* sqe = get_sqe();
        if (!sqe) return false;
        io_uring_prep_openat(sqe, AT_FDCWD, conn->file_path.c_str(), O_RDONLY, 0);
        io_uring_sqe_set_data(sqe, conn);
        return true;
    };

    auto resubmit_accept = [&]() {
        struct io_uring_sqe* sqe = get_sqe();
        if (sqe) {
            io_uring_prep_multishot_accept(sqe, server_fd_, nullptr, nullptr, 0);
            io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(ACCEPT_MARKER));
        }
    };

    // ── Kick off multishot accept ─────────────────────────────────────────
    resubmit_accept();
    io_uring_submit(&ring);

    // ── CQE batch buffer ──────────────────────────────────────────────────
    struct io_uring_cqe* cqe_batch[CQE_BATCH];

    while (running_) {
        // Submit pending SQEs + wait for ≥1 CQE
        ret = io_uring_submit_and_wait(&ring, 1);
        if (ret < 0) {
            if (ret == -EINTR) continue;
            if (ret == -EBADF || ret == -EINVAL) break;
            break;
        }

        // Drain all ready CQEs in one batch
        unsigned batch = io_uring_peek_batch_cqe(&ring, cqe_batch, CQE_BATCH);
        for (unsigned i = 0; i < batch; i++) {
            struct io_uring_cqe* cqe = cqe_batch[i];
            uintptr_t ud = reinterpret_cast<uintptr_t>(io_uring_cqe_get_data(cqe));
            int res       = cqe->res;
            unsigned flags = cqe->flags;

            // ── Accept (multishot) ────────────────────────────────────
            if (ud == ACCEPT_MARKER) {
                if (res >= 0) {
                    set_socket_options(res);
                    auto* conn = new Connection;
                    conn->fd = res;
                    if (!submit_recv(conn)) continue;
                }
                if (!(flags & IORING_CQE_F_MORE)) resubmit_accept();
                continue;
            }

            if (ud == 0) continue;

            auto* conn = reinterpret_cast<Connection*>(ud);

        #define CHECK_CLOSE if (!ok) continue
            bool ok = true;

            switch (conn->state) {

            case State::READING_REQUEST: {
                if (res <= 0) { close_connection(conn); continue; }

                conn->read_offset += static_cast<size_t>(res);
                std::string_view data(conn->read_buf, conn->read_offset);
                bool done = conn->parser.parse(data, STREAM_THRESHOLD);

                if (!done) {
                    if (conn->read_offset >= BUF_SIZE) { close_connection(conn); continue; }
                    ok = submit_recv(conn); CHECK_CLOSE;
                    continue;
                }

                const auto& req = conn->parser.request();

                // ── Streaming PUT ──────────────────────────────────
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
                            conn->put_write_size = lv.size();
                            memmove(conn->read_buf, lv.data(), lv.size());
                            conn->body_received = lv.size();
                            conn->put_write_pending = true;
                            ok = submit_write_body(conn); CHECK_CLOSE;
                            continue;
                        }
                        conn->read_offset = 0;
                        ok = submit_recv(conn); CHECK_CLOSE;
                        continue;
                    }

                    conn->state = State::SENDING_HEADERS;
                    conn->response_data = resp.to_string();
                    conn->send_offset = 0;
                    auto ch = req.header("Connection");
                    if (ch && utils::iequals(*ch, "close")) conn->keep_alive = false;
                    ok = submit_send(conn); CHECK_CLOSE;
                    continue;
                }

                // ── Normal request ─────────────────────────────────
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

                    // ── File to send → async openat (never blocks) ─────
                    if (resp.file_to_send) {
                        conn->file_path = *resp.file_to_send;
                        conn->file_off = resp.file_offset;
                        conn->file_remaining = resp.content_length_opt().value_or(0);
                        conn->state = State::OPENING_FILE;
                        if (submit_openat(conn)) continue;
                        // SQE full — fallback to sync open
                        conn->file_fd = ::open(conn->file_path.c_str(), O_RDONLY);
                        if (conn->file_fd < 0) { close_connection(conn); continue; }
                        file_ops::lock_shared_nb(conn->file_fd);
                        if (::pipe2(conn->splice_pipe, O_NONBLOCK) < 0) {
                            close_connection(conn); continue;
                        }
                        ::fcntl(conn->splice_pipe[0], F_SETPIPE_SZ, PIPE_CAPACITY);
                        ::fcntl(conn->splice_pipe[1], F_SETPIPE_SZ, PIPE_CAPACITY);
                        conn->splice_phase = SplicePhase::TO_PIPE;
                        conn->splice_pending = 0;
                        conn->state = State::SENDING_HEADERS;
                        ok = submit_send(conn); CHECK_CLOSE;
                        continue;
                    }

                    conn->state = State::SENDING_HEADERS;
                    ok = submit_send(conn); CHECK_CLOSE;
                }
                continue;
            } // READING_REQUEST

            case State::OPENING_FILE: {
                if (res < 0) {
                    // Open failed → send response as-is (may contain error body)
                    conn->state = State::SENDING_HEADERS;
                    ok = submit_send(conn); CHECK_CLOSE;
                    continue;
                }
                conn->file_fd = res;
                file_ops::lock_shared_nb(conn->file_fd);
                if (::pipe2(conn->splice_pipe, O_NONBLOCK) < 0) {
                    close_connection(conn); continue;
                }
                ::fcntl(conn->splice_pipe[0], F_SETPIPE_SZ, PIPE_CAPACITY);
                ::fcntl(conn->splice_pipe[1], F_SETPIPE_SZ, PIPE_CAPACITY);
                conn->splice_phase = SplicePhase::TO_PIPE;
                conn->splice_pending = 0;
                conn->state = State::SENDING_HEADERS;
                ok = submit_send(conn); CHECK_CLOSE;
                continue;
            }

            case State::RECEIVING_BODY: {
                if (conn->put_write_pending) {
                    conn->put_write_pending = false;
                    if (res < 0) {
                        uring_async_close(conn->output_fd); conn->output_fd = -1;
                        if (!conn->output_tmp_path.empty()) ::unlink(conn->output_tmp_path.c_str());
                        close_connection(conn); continue;
                    }
                    if (static_cast<size_t>(res) < conn->put_write_size) {
                        size_t wrote = static_cast<size_t>(res);
                        conn->body_received -= (conn->put_write_size - wrote);
                        size_t remain = conn->put_write_size - wrote;
                        memmove(conn->read_buf, conn->read_buf + wrote, remain);
                        conn->put_write_size = remain;
                        conn->put_write_pending = true;
                        ok = submit_write_body(conn); if (!ok) {
                            uring_async_close(conn->output_fd); conn->output_fd = -1;
                            if (!conn->output_tmp_path.empty()) ::unlink(conn->output_tmp_path.c_str());
                            close_connection(conn);
                        }
                        continue;
                    }
                    if (conn->body_received >= conn->body_expected) {
                        uring_async_close(conn->output_fd); conn->output_fd = -1;
                        if (!conn->output_tmp_path.empty() && !conn->output_final_path.empty())
                            ::rename(conn->output_tmp_path.c_str(), conn->output_final_path.c_str());
                        conn->output_tmp_path.clear();
                        conn->output_final_path.clear();
                        conn->response_data = build_response_string(
                            conn->existed_before_put ? 200 : 201, conn->keep_alive);
                        conn->send_offset = 0;
                        conn->state = State::SENDING_HEADERS;
                        ok = submit_send(conn); CHECK_CLOSE;
                        continue;
                    }
                    conn->read_offset = 0;
                    ok = submit_recv(conn); CHECK_CLOSE;
                    continue;
                }

                if (res <= 0) {
                    uring_async_close(conn->output_fd); conn->output_fd = -1;
                    if (!conn->output_tmp_path.empty()) ::unlink(conn->output_tmp_path.c_str());
                    close_connection(conn); continue;
                }

                conn->put_write_size = static_cast<size_t>(res);
                conn->put_write_pending = true;
                conn->body_received += static_cast<size_t>(res);
                ok = submit_write_body(conn); if (!ok) {
                    uring_async_close(conn->output_fd); conn->output_fd = -1;
                    if (!conn->output_tmp_path.empty()) ::unlink(conn->output_tmp_path.c_str());
                    close_connection(conn);
                }
                continue;
            } // RECEIVING_BODY

            case State::SENDING_HEADERS: {
                if (res <= 0) { close_connection(conn); continue; }

                conn->send_offset += static_cast<size_t>(res);
                if (conn->send_offset < conn->response_data.size()) {
                    ok = submit_send(conn); CHECK_CLOSE;
                    continue;
                }

                if (conn->file_fd >= 0 && conn->file_remaining > 0) {
                    conn->state = State::SENDING_FILE;
                    conn->splice_phase = SplicePhase::TO_PIPE;
                    ok = submit_splice_to_pipe(conn); CHECK_CLOSE;
                } else if (conn->keep_alive) {
                    reset_connection(conn);
                    ok = submit_recv(conn); CHECK_CLOSE;
                } else {
                    close_connection(conn);
                }
                continue;
            }

            case State::SENDING_FILE: {
                if (res <= 0) { close_connection(conn); continue; }

                if (conn->splice_phase == SplicePhase::TO_PIPE) {
                    conn->splice_pending = static_cast<size_t>(res);
                    conn->splice_phase = SplicePhase::TO_SOCKET;
                    ok = submit_splice_to_socket(conn); CHECK_CLOSE;
                } else {
                    size_t sent = static_cast<size_t>(res);
                    if (sent < conn->splice_pending) {
                        conn->splice_pending -= sent;
                        ok = submit_splice_to_socket(conn); CHECK_CLOSE;
                    } else {
                        conn->file_off += static_cast<off_t>(conn->splice_pending);
                        conn->file_remaining -= conn->splice_pending;
                        conn->splice_pending = 0;

                        if (conn->file_remaining > 0) {
                            conn->splice_phase = SplicePhase::TO_PIPE;
                            ok = submit_splice_to_pipe(conn); CHECK_CLOSE;
                        } else {
                            uring_async_close(conn->file_fd); conn->file_fd = -1;
                            uring_async_close(conn->splice_pipe[0]); conn->splice_pipe[0] = -1;
                            uring_async_close(conn->splice_pipe[1]); conn->splice_pipe[1] = -1;
                            if (conn->keep_alive) {
                                reset_connection(conn);
                                ok = submit_recv(conn); CHECK_CLOSE;
                            } else {
                                close_connection(conn);
                            }
                        }
                    }
                }
                continue;
            }

            case State::CLOSING:
                close_connection(conn);
                continue;
            }
        #undef CHECK_CLOSE
        }

        io_uring_cq_advance(&ring, batch);
        // DEFER_TASKRUN: submit_and_wait already submitted.
        // New SQEs will be flushed on the next submit_and_wait.
    }

    tls_ring = nullptr;
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
    std::cout << "[INFO] I/O: DEFER_TASKRUN + multishot accept + batch CQE + async openat + splice" << std::endl;
    std::cout << "[INFO] Kernel " << utils::kernel_version() << std::endl;

    workers_.reserve(num_workers_);
    for (unsigned i = 0; i < num_workers_; ++i) {
        workers_.emplace_back([this, i] { worker_loop(i); });
    }
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();

    if (shutdown_eventfd_ >= 0) { ::close(shutdown_eventfd_); shutdown_eventfd_ = -1; }
    if (server_fd_ >= 0)        { ::close(server_fd_); server_fd_ = -1; }

    std::cout << "[INFO] Server stopped." << std::endl;
}
