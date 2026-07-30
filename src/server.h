#pragma once

#include "webdav_handler.h"
#include <filesystem>
#include <string>
#include <atomic>
#include <optional>
#include <thread>
#include <vector>
#include <chrono>

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

    // Exposed for tests / internal use
    static constexpr size_t PIPE_CAPACITY  = 1048576;  // 1 MiB
    static constexpr size_t READ_BUF_SIZE  = 65536;

private:
    int port_;
    WebDavHandler handler_;
    int shutdown_eventfd_ = -1;
    std::atomic<bool> running_{true};
    unsigned num_workers_;

    // One listen fd per worker (SO_REUSEPORT distributes at kernel level)
    std::vector<int> listen_fds_;
    std::vector<std::jthread> workers_;

    // ── Connection state machine ───────────────────────────────────────────

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

    struct Connection {
        int fd = -1;
        int listen_fd = -1;  // which listen socket this conn came from (for shutdown)
        State state = State::READING_REQUEST;
        bool keep_alive = true;
        bool idle_timeout_armed = false;  // linked timeout posted on recv

        // HTTP parsing
        http::Parser parser;
        char read_buf[READ_BUF_SIZE];
        size_t read_offset = 0;

        // Response
        std::string response_data;
        size_t send_offset = 0;

        // ── File serving ─────────────────────────────────────────────────
        int file_fd = -1;
        std::string file_path;
        off_t file_off = 0;
        size_t file_remaining = 0;
        int splice_pipe[2] = {-1, -1};
        SplicePhase splice_phase = SplicePhase::TO_PIPE;
        size_t splice_pending = 0;

        // ── File receiving ───────────────────────────────────────────────
        int output_fd = -1;
        std::string output_final_path;
        std::string output_tmp_path;
        size_t body_expected = 0;
        size_t body_received = 0;
        bool existed_before_put = false;
        bool put_write_pending = false;
        size_t put_write_size = 0;
    };

    void worker_loop(unsigned worker_id, int listen_fd);
    static void set_socket_options(int fd);
    static void close_connection(Connection* conn);
    static void reset_connection(Connection* conn);
    static std::string build_response_string(int status_code, bool keep_alive);
};
