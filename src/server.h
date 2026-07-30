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

    void run();
    void shutdown();
    int port() const { return port_; }

    static constexpr size_t PIPE_CAPACITY  = 1048576;  // 1 MiB
    static constexpr size_t READ_BUF_SIZE  = 16384;    // 16 KiB — HTTP headers fit

private:
    int port_;
    WebDavHandler handler_;
    std::atomic<bool> running_{true};
    unsigned num_workers_;
    std::vector<int> listen_fds_;
    std::vector<std::jthread> workers_;

    enum class State {
        READING_REQUEST,
        RECEIVING_BODY,
        OPENING_FILE,
        SENDING_HEADERS,
        SENDING_FILE,
        CLOSING,
    };

    enum class SplicePhase { TO_PIPE, TO_SOCKET };

    static constexpr unsigned MAX_CONNS_PER_WORKER = 4096;
    static constexpr unsigned POOL_BLOCK           = 64;   // alloc 64 conns at once

    struct Connection {
        int fd = -1;
        State state = State::READING_REQUEST;
        bool keep_alive = true;

        // ── Parsing (16 KiB stack buffer) ───────────────────────────────
        http::Parser parser;
        char read_buf[READ_BUF_SIZE];
        size_t read_offset = 0;
        bool recv_has_timeout = false;  // linked timeout armed on recv

        // ── Response ────────────────────────────────────────────────────
        std::string response_data;
        size_t send_offset = 0;
        bool send_is_zc = false;  // true if current send was via send_zc

        // ── File serving (zero-copy splice) ─────────────────────────────
        int file_fd = -1;
        std::string file_path;
        off_t file_off = 0;
        size_t file_remaining = 0;
        int splice_pipe[2] = {-1, -1};
        SplicePhase splice_phase = SplicePhase::TO_PIPE;
        size_t splice_pending = 0;

        // ── File receiving ──────────────────────────────────────────────
        int output_fd = -1;
        std::string output_final_path;
        std::string output_tmp_path;
        size_t body_expected = 0;
        size_t body_received = 0;
        bool existed_before_put = false;
        bool put_write_pending = false;
        size_t put_write_size = 0;

        // ── Free list (for object pool) ─────────────────────────────────
        Connection* pool_next = nullptr;
    };

    void worker_loop(unsigned worker_id, int listen_fd);
    static void set_socket_options(int fd);
    static std::string build_response_string(int code, bool keep_alive);
};
