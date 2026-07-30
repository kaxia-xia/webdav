#include "server.h"
#include "http_parser.h"
#include "thumbnail.h"
#include "file_ops.h"
#include "utils.h"
#include <iostream>
#include <cstring>
#include <cerrno>

#include <sys/socket.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <liburing.h>
#pragma GCC diagnostic pop

static constexpr unsigned RING_ENTRIES     = 512;
static constexpr unsigned IDLE_TIMEOUT_SEC = 30;
static constexpr int      SVR_TCP_KEEPIDLE  = 60;
static constexpr int      SVR_TCP_KEEPINTVL = 10;
static constexpr int      SVR_TCP_KEEPCNT   = 3;
static constexpr size_t   STREAM_THRESHOLD = 65536;
static constexpr size_t   BUF_SIZE         = Server::READ_BUF_SIZE;

static constexpr uintptr_t TAG_ACCEPT  = 1;
static constexpr uintptr_t TAG_TIMEOUT = 2;  // conn | TAG_TIMEOUT

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

Server::Server(const fs::path& root_dir, int port, bool allow_browser,
              std::optional<std::string> username,
              std::optional<std::string> password)
    : port_(port)
    , handler_(root_dir, allow_browser, std::move(username), std::move(password))
    , num_workers_(std::thread::hardware_concurrency())
{
    if (num_workers_ < 1) num_workers_ = 4;
    std::cout << "[INFO] " << num_workers_ << " workers, io_uring "
              << "(DEFER_TASKRUN + SO_REUSEPORT + async openat + pool + HARDLINK splice)" << std::endl;
    thumbnail::init();
}

Server::~Server() { shutdown(); }

void Server::shutdown() {
    if (!running_.exchange(false)) return;
    for (int fd : listen_fds_) if (fd >= 0) ::close(fd);
    listen_fds_.clear();
}

static thread_local struct io_uring* tls_ring = nullptr;

static void uring_async_close(int fd) {
    if (fd < 0) return;
    if (!tls_ring) { ::close(fd); return; }
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
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
    int idle = SVR_TCP_KEEPIDLE, intvl = SVR_TCP_KEEPINTVL, cnt = SVR_TCP_KEEPCNT;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &idle,  sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,   sizeof(cnt));
}

std::string Server::build_response_string(int code, bool keep_alive) {
    const char* st   = http::status_message(code);
    auto        d    = get_cached_date();
    const char* conn = keep_alive ? "keep-alive" : "close";
    std::string r; r.reserve(200);
    r = "HTTP/1.1 "; r += std::to_string(code); r += " "; r += st;
    r += "\r\nServer: WebDAV-Server/1.7\r\nDate: ";
    r.append(d.data(), d.size());
    r += "\r\nAccept-Ranges: bytes\r\nConnection: "; r += conn;
    r += "\r\nContent-Length: 0\r\n\r\n";
    return r;
}

static int create_listen_socket(int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { ::close(fd); return -1; }
    if (::listen(fd, SOMAXCONN) < 0) { ::close(fd); return -1; }
    return fd;
}

void Server::worker_loop(unsigned worker_id, int listen_fd) {
    {
        cpu_set_t cs; CPU_ZERO(&cs);
        unsigned nc = std::thread::hardware_concurrency();
        if (nc == 0) nc = 4;
        CPU_SET(worker_id % nc, &cs);
        pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
    }

    struct io_uring ring;
    struct io_uring_params params = {};
    params.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;

    int ret = io_uring_queue_init_params(RING_ENTRIES, &ring, &params);
    tls_ring = &ring;
    if (ret < 0) {
        std::cerr << "[W" << worker_id << "] uring: " << std::strerror(-ret) << std::endl;
        return;
    }

    // ── Connection pool ────────────────────────────────────────────────
    static thread_local Connection* tls_pool = nullptr;
    auto pool_alloc = [&]() -> Connection* {
        if (tls_pool) { auto* c = tls_pool; tls_pool = c->pool_next; return new (c) Connection; }
        auto* b = new Connection[POOL_BLOCK];
        for (unsigned i = 1; i < POOL_BLOCK; i++) { b[i].pool_next = tls_pool; tls_pool = &b[i]; }
        return &b[0];
    };
    auto pool_free = [&](Connection* c) { c->pool_next = tls_pool; tls_pool = c; };

    // ── kill_conn / reset_conn ─────────────────────────────────────────
    auto kill_conn = [&](Connection* c) {
        uring_async_close(c->fd);         c->fd = -1;
        uring_async_close(c->file_fd);    c->file_fd = -1;
        uring_async_close(c->output_fd);  c->output_fd = -1;
        uring_async_close(c->splice_pipe[0]); c->splice_pipe[0] = -1;
        uring_async_close(c->splice_pipe[1]); c->splice_pipe[1] = -1;
        if (!c->output_tmp_path.empty()) { ::unlink(c->output_tmp_path.c_str()); c->output_tmp_path.clear(); }
        pool_free(c);
    };

    auto reset_conn = [&](Connection* c) {
        c->state = State::READING_REQUEST;
        c->read_offset = 0; c->parser.reset();
        c->response_data.clear(); c->send_offset = 0;
        c->keep_alive = true; c->recv_has_timeout = false;
        uring_async_close(c->file_fd);    c->file_fd = -1;
        uring_async_close(c->splice_pipe[0]); c->splice_pipe[0] = -1;
        uring_async_close(c->splice_pipe[1]); c->splice_pipe[1] = -1;
        c->file_remaining = 0; c->splice_pending = 0;
        c->splice_phase = SplicePhase::TO_PIPE; c->file_path.clear();
        uring_async_close(c->output_fd);  c->output_fd = -1;
        c->body_expected = 0; c->body_received = 0;
        c->put_write_pending = false; c->put_write_size = 0;
        c->output_tmp_path.clear(); c->output_final_path.clear();
    };

    auto cleanup_rb = [&](Connection* c) {
        uring_async_close(c->output_fd); c->output_fd = -1;
        if (!c->output_tmp_path.empty()) ::unlink(c->output_tmp_path.c_str());
        kill_conn(c);
    };

    // ── Submission helpers ─────────────────────────────────────────────
    auto get_sqe = [&]() { return io_uring_get_sqe(&ring); };

    // recv + linked idle timeout
    auto submit_recv_to = [&](Connection* c) -> bool {
        struct io_uring_sqe* s1 = get_sqe(), *s2 = get_sqe();
        if (!s1) return false;
        if (!s2) { // no second slot → plain recv
            io_uring_prep_recv(s1, c->fd, c->read_buf + c->read_offset,
                               BUF_SIZE - c->read_offset, 0);
            io_uring_sqe_set_data(s1, c);
            c->recv_has_timeout = false;
            return true;
        }
        io_uring_prep_recv(s1, c->fd, c->read_buf + c->read_offset,
                           BUF_SIZE - c->read_offset, 0);
        io_uring_sqe_set_data(s1, c);
        s1->flags |= IOSQE_IO_LINK;
        struct __kernel_timespec ts = {.tv_sec = IDLE_TIMEOUT_SEC, .tv_nsec = 0};
        io_uring_prep_link_timeout(s2, &ts, 0);
        io_uring_sqe_set_data(s2,
            reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(c) | TAG_TIMEOUT));
        c->recv_has_timeout = true;
        return true;
    };

    // HARDLINK splice: file→pipe ⟹ pipe→socket, single CQE
    auto submit_splice_linked = [&](Connection* c) -> bool {
        size_t chunk = c->file_remaining;
        if (chunk > PIPE_CAPACITY) chunk = PIPE_CAPACITY;
        c->splice_pending = chunk;

        struct io_uring_sqe* s1 = get_sqe(), *s2 = get_sqe();
        if (!s1) return false;
        if (!s2) { // no second slot → unlinked fallback
            io_uring_prep_splice(s1, c->file_fd, c->file_off,
                                 c->splice_pipe[1], -1, static_cast<unsigned>(chunk),
                                 SPLICE_F_MOVE | SPLICE_F_MORE);
            io_uring_sqe_set_data(s1, c);
            c->splice_phase = SplicePhase::TO_PIPE;
            return true;
        }
        io_uring_prep_splice(s1, c->file_fd, c->file_off,
                             c->splice_pipe[1], -1, static_cast<unsigned>(chunk),
                             SPLICE_F_MOVE | SPLICE_F_MORE);
        io_uring_sqe_set_data(s1, c);
        s1->flags |= IOSQE_IO_HARDLINK;
        io_uring_prep_splice(s2, c->splice_pipe[0], -1,
                             c->fd, -1, static_cast<unsigned>(chunk),
                             SPLICE_F_MOVE | SPLICE_F_MORE);
        io_uring_sqe_set_data(s2, c);
        c->splice_phase = SplicePhase::TO_SOCKET;
        return true;
    };

    auto submit_splice_sock = [&](Connection* c) -> bool {
        struct io_uring_sqe* s = get_sqe();
        if (!s) return false;
        io_uring_prep_splice(s, c->splice_pipe[0], -1, c->fd, -1,
                             static_cast<unsigned>(c->splice_pending),
                             SPLICE_F_MOVE | SPLICE_F_MORE);
        io_uring_sqe_set_data(s, c);
        return true;
    };

    // ── Kick off multishot accept ─────────────────────────────────────
    {
        struct io_uring_sqe* s = get_sqe();
        if (s) {
            io_uring_prep_multishot_accept(s, listen_fd, nullptr, nullptr, 0);
            io_uring_sqe_set_data(s, reinterpret_cast<void*>(TAG_ACCEPT));
            io_uring_submit(&ring);
        }
    }

    while (running_) {
        struct io_uring_cqe* cqe;
        ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0) {
            if (ret == -EINTR) continue;
            if (ret == -EBADF || ret == -EINVAL) break;
            break;
        }

        uintptr_t ud = reinterpret_cast<uintptr_t>(io_uring_cqe_get_data(cqe));
        int      res = cqe->res;
        unsigned flg = cqe->flags;

        // ── Linked timeout CQE ───────────────────────────────────────
        if (ud & TAG_TIMEOUT) {
            auto* c = reinterpret_cast<Connection*>(ud & ~TAG_TIMEOUT);
            c->recv_has_timeout = false;
            if (res == -ETIME && c->state == State::READING_REQUEST) kill_conn(c);
            goto done;
        }

        // ── Multishot accept CQE ────────────────────────────────────
        if (ud == TAG_ACCEPT) {
            if (res >= 0) {
                set_socket_options(res);
                auto* c = pool_alloc();
                c->fd = res;
                struct io_uring_sqe* s = get_sqe();
                if (s) { io_uring_prep_recv(s, c->fd, c->read_buf, BUF_SIZE, 0); io_uring_sqe_set_data(s, c); }
                else kill_conn(c);
            }
            if (!(flg & IORING_CQE_F_MORE)) {
                struct io_uring_sqe* s = get_sqe();
                if (s) { io_uring_prep_multishot_accept(s, listen_fd, nullptr, nullptr, 0); io_uring_sqe_set_data(s, reinterpret_cast<void*>(TAG_ACCEPT)); }
            }
            goto done;
        }

        if (ud == 0) goto done;

        {
        auto* c = reinterpret_cast<Connection*>(ud);

        switch (c->state) {

        case State::READING_REQUEST: {
            if (res <= 0) { kill_conn(c); break; }
            c->recv_has_timeout = false;
            c->read_offset += static_cast<size_t>(res);
            std::string_view data(c->read_buf, c->read_offset);
            if (!c->parser.parse(data, STREAM_THRESHOLD)) {
                if (c->read_offset >= BUF_SIZE) { kill_conn(c); break; }
                struct io_uring_sqe* s = get_sqe();
                if (s) { io_uring_prep_recv(s, c->fd, c->read_buf + c->read_offset, BUF_SIZE - c->read_offset, 0); io_uring_sqe_set_data(s, c); }
                else kill_conn(c);
                break;
            }
            const auto& req = c->parser.request();

            if (req.body_truncated && req.method == http::Method::PUT) {
                http::Response resp = handler_.handle(req);
                if (resp.body_output_fd >= 0) {
                    c->state = State::RECEIVING_BODY;
                    c->output_fd = resp.body_output_fd;
                    c->output_final_path = std::move(resp.body_output_path);
                    c->output_tmp_path   = std::move(resp.body_tmp_path);
                    c->body_expected  = resp.body_expected;
                    c->body_received  = 0;
                    c->existed_before_put = (resp.status_code == 200);
                    c->put_write_pending  = false;

                    std::string_view lv = c->parser.leftover();
                    if (!lv.empty()) {
                        c->put_write_size = lv.size();
                        memmove(c->read_buf, lv.data(), lv.size());
                        c->body_received = lv.size();
                        c->put_write_pending = true;
                        struct io_uring_sqe* s = get_sqe();
                        if (s) { io_uring_prep_write(s, c->output_fd, c->read_buf, c->put_write_size, -1); io_uring_sqe_set_data(s, c); }
                        else cleanup_rb(c);
                        break;
                    }
                    c->read_offset = 0;
                    { struct io_uring_sqe* s = get_sqe(); if (s) { io_uring_prep_recv(s, c->fd, c->read_buf, BUF_SIZE, 0); io_uring_sqe_set_data(s, c); } else cleanup_rb(c); }
                    break;
                }
                goto send_resp;
            }

send_resp:
            {
                http::Response resp = c->state == State::SENDING_HEADERS ? http::Response{} : handler_.handle(req);
                if (c->state == State::READING_REQUEST) {
                    auto ch = req.header("Connection");
                    if (ch && utils::iequals(*ch, "close")) { c->keep_alive = false; resp.set_header("Connection", "close"); }
                    else { resp.set_header("Connection", "keep-alive"); }
                    if (resp.file_to_send) {
                        c->file_path = std::move(*resp.file_to_send);
                        c->file_off  = resp.file_offset;
                        c->file_remaining = resp.content_length_opt().value_or(0);
                        c->response_data = resp.to_string();
                        struct io_uring_sqe* s = get_sqe();
                        if (s) { io_uring_prep_openat(s, AT_FDCWD, c->file_path.c_str(), O_RDONLY, 0); io_uring_sqe_set_data(s, c); c->state = State::OPENING_FILE; break; }
                        // fallback sync
                        c->file_fd = ::open(c->file_path.c_str(), O_RDONLY);
                        if (c->file_fd < 0) { kill_conn(c); break; }
                        file_ops::lock_shared_nb(c->file_fd);
                        if (::pipe2(c->splice_pipe, O_NONBLOCK) < 0) { kill_conn(c); break; }
                        ::fcntl(c->splice_pipe[0], F_SETPIPE_SZ, PIPE_CAPACITY);
                        ::fcntl(c->splice_pipe[1], F_SETPIPE_SZ, PIPE_CAPACITY);
                    }
                    c->response_data = resp.to_string();
                }
                c->send_offset = 0;
                c->state = State::SENDING_HEADERS;
                struct io_uring_sqe* s = get_sqe();
                if (s) { io_uring_prep_send(s, c->fd, c->response_data.data(), c->response_data.size(), MSG_NOSIGNAL); io_uring_sqe_set_data(s, c); }
                else kill_conn(c);
            }
            break;
        }

        case State::OPENING_FILE: {
            if (res < 0) { c->state = State::SENDING_HEADERS; goto send_now; }
            c->file_fd = res;
            file_ops::lock_shared_nb(c->file_fd);
            if (::pipe2(c->splice_pipe, O_NONBLOCK) < 0) { kill_conn(c); break; }
            ::fcntl(c->splice_pipe[0], F_SETPIPE_SZ, PIPE_CAPACITY);
            ::fcntl(c->splice_pipe[1], F_SETPIPE_SZ, PIPE_CAPACITY);
        send_now:
            c->state = State::SENDING_HEADERS;
            struct io_uring_sqe* s = get_sqe();
            if (s) { io_uring_prep_send(s, c->fd, c->response_data.data(), c->response_data.size(), MSG_NOSIGNAL); io_uring_sqe_set_data(s, c); }
            else kill_conn(c);
            break;
        }

        case State::RECEIVING_BODY: {
            if (c->put_write_pending) {
                c->put_write_pending = false;
                if (res < 0) { cleanup_rb(c); break; }
                if (static_cast<size_t>(res) < c->put_write_size) {
                    size_t wrote = static_cast<size_t>(res);
                    c->body_received -= (c->put_write_size - wrote);
                    size_t r = c->put_write_size - wrote;
                    memmove(c->read_buf, c->read_buf + wrote, r);
                    c->put_write_size = r; c->put_write_pending = true;
                    struct io_uring_sqe* s = get_sqe();
                    if (s) { io_uring_prep_write(s, c->output_fd, c->read_buf, c->put_write_size, -1); io_uring_sqe_set_data(s, c); }
                    else cleanup_rb(c);
                    break;
                }
                if (c->body_received >= c->body_expected) {
                    uring_async_close(c->output_fd); c->output_fd = -1;
                    if (!c->output_tmp_path.empty() && !c->output_final_path.empty())
                        ::rename(c->output_tmp_path.c_str(), c->output_final_path.c_str());
                    c->output_tmp_path.clear(); c->output_final_path.clear();
                    c->response_data = build_response_string(c->existed_before_put ? 200 : 201, c->keep_alive);
                    c->state = State::SENDING_HEADERS;
                    struct io_uring_sqe* s = get_sqe();
                    if (s) { io_uring_prep_send(s, c->fd, c->response_data.data(), c->response_data.size(), MSG_NOSIGNAL); io_uring_sqe_set_data(s, c); }
                    else kill_conn(c);
                    break;
                }
                c->read_offset = 0;
                { struct io_uring_sqe* s = get_sqe(); if (s) { io_uring_prep_recv(s, c->fd, c->read_buf, BUF_SIZE, 0); io_uring_sqe_set_data(s, c); } else cleanup_rb(c); }
                break;
            }
            if (res <= 0) { cleanup_rb(c); break; }
            c->put_write_size = static_cast<size_t>(res);
            c->put_write_pending = true;
            c->body_received += static_cast<size_t>(res);
            { struct io_uring_sqe* s = get_sqe(); if (s) { io_uring_prep_write(s, c->output_fd, c->read_buf, c->put_write_size, -1); io_uring_sqe_set_data(s, c); } else cleanup_rb(c); }
            break;
        }

        case State::SENDING_HEADERS: {
            if (res <= 0) { kill_conn(c); break; }
            c->send_offset += static_cast<size_t>(res);
            if (c->send_offset < c->response_data.size()) {
                size_t len = c->response_data.size() - c->send_offset;
                struct io_uring_sqe* s = get_sqe();
                if (s) { io_uring_prep_send(s, c->fd, c->response_data.data() + c->send_offset, len, MSG_NOSIGNAL); io_uring_sqe_set_data(s, c); }
                else kill_conn(c);
                break;
            }
            if (c->file_fd >= 0 && c->file_remaining > 0) {
                c->state = State::SENDING_FILE;
                if (!submit_splice_linked(c)) kill_conn(c);
            } else if (c->keep_alive) {
                reset_conn(c);
                if (!submit_recv_to(c)) kill_conn(c);
            } else { kill_conn(c); }
            break;
        }

        case State::SENDING_FILE: {
            if (res <= 0) { kill_conn(c); break; }
            size_t sent = static_cast<size_t>(res);
            if (sent < c->splice_pending) {
                c->splice_pending -= sent;
                if (!submit_splice_sock(c)) kill_conn(c);
            } else {
                c->file_off += static_cast<off_t>(c->splice_pending);
                c->file_remaining -= c->splice_pending;
                c->splice_pending = 0;
                if (c->file_remaining > 0) {
                    if (!submit_splice_linked(c)) kill_conn(c);
                } else {
                    uring_async_close(c->file_fd); c->file_fd = -1;
                    uring_async_close(c->splice_pipe[0]); c->splice_pipe[0] = -1;
                    uring_async_close(c->splice_pipe[1]); c->splice_pipe[1] = -1;
                    if (c->keep_alive) {
                        reset_conn(c);
                        if (!submit_recv_to(c)) kill_conn(c);
                    } else kill_conn(c);
                }
            }
            break;
        }

        case State::CLOSING:
            kill_conn(c);
            break;
        }
        }

done:
        io_uring_cqe_seen(&ring, cqe);
        io_uring_submit(&ring);
    }

    tls_ring = nullptr;
    io_uring_queue_exit(&ring);
}

void Server::run() {
    {
        struct rlimit rl;
        if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
            rl.rlim_cur = rl.rlim_max;
            if (setrlimit(RLIMIT_NOFILE, &rl) != 0) { rl.rlim_cur = 65536; setrlimit(RLIMIT_NOFILE, &rl); }
            std::cout << "[INFO] FD limit " << rl.rlim_cur << std::endl;
        }
    }

    listen_fds_.reserve(num_workers_);
    for (unsigned i = 0; i < num_workers_; ++i) {
        int fd = create_listen_socket(port_);
        if (fd < 0) {
            std::cerr << "[ERROR] listen " << i << ": " << std::strerror(errno) << std::endl;
            for (int f : listen_fds_) ::close(f);
            listen_fds_.clear(); return;
        }
        listen_fds_.push_back(fd);
    }

    std::cout << "[INFO] http://0.0.0.0:" << port_ << " — " << handler_.root_dir() << std::endl;
    std::cout << "[INFO] SO_REUSEPORT ×" << num_workers_ << " | keepalive " << SVR_TCP_KEEPIDLE
              << "/" << SVR_TCP_KEEPINTVL << "/" << SVR_TCP_KEEPCNT << " | idle " << IDLE_TIMEOUT_SEC << "s" << std::endl;

    workers_.reserve(num_workers_);
    for (unsigned i = 0; i < num_workers_; ++i)
        workers_.emplace_back([this, i, fd = listen_fds_[i]] { worker_loop(i, fd); });
    for (auto& t : workers_) if (t.joinable()) t.join();
    workers_.clear();

    for (int fd : listen_fds_) if (fd >= 0) ::close(fd);
    listen_fds_.clear();
    std::cout << "[INFO] Server stopped." << std::endl;
}
