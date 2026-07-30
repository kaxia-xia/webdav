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

private:
    int port_;
    WebDavHandler handler_;
    int server_fd_ = -1;
    int shutdown_eventfd_ = -1;
    std::atomic<bool> running_{true};
    unsigned num_workers_;
    std::vector<std::jthread> workers_;

    // ── Connection state machine (io_uring) ───────────────────────────────

    enum class State {
        READING_REQUEST,
        RECEIVING_BODY,    // streaming PUT body to disk (async io_uring write)
        OPENING_FILE,      // async openat for GET file serving
        SENDING_HEADERS,
        SENDING_FILE,      // zero-copy file send via splice(2)
        CLOSING,
    };

    // Phase within SENDING_FILE (splice pipeline)
    enum class SplicePhase { TO_PIPE, TO_SOCKET };

    static constexpr size_t READ_BUF_SIZE  = 65536;
    static constexpr size_t PIPE_CAPACITY  = 1048576;  // 1 MiB — fewer splice rounds

    struct Connection {
        int fd = -1;
        State state = State::READING_REQUEST;
        bool keep_alive = true;

        // HTTP parsing
        http::Parser parser;
        char read_buf[READ_BUF_SIZE];
        size_t read_offset = 0;

        // Response (headers + small body, serialised)
        // Per-connection buffer reused across keep-alive requests.
        // Cleared on reset but capacity retained to avoid realloc.
        std::string response_data;
        size_t send_offset = 0;

        // ── File serving (GET) — zero-copy via splice(2) ───────────────
        int file_fd = -1;
        std::string file_path;   // for async openat
        off_t file_off = 0;
        size_t file_remaining = 0;
        int splice_pipe[2] = {-1, -1};
        SplicePhase splice_phase = SplicePhase::TO_PIPE;
        size_t splice_pending = 0;       // bytes waiting in pipe → socket

        // ── File receiving (PUT streaming) — async io_uring write ──────
        int output_fd = -1;
        std::string output_final_path;
        std::string output_tmp_path;
        size_t body_expected = 0;
        size_t body_received = 0;
        bool existed_before_put = false;
        bool put_write_pending = false;  // true = next CQE is write completion
        size_t put_write_size = 0;
    };

    void worker_loop(unsigned worker_id);
    static void set_socket_options(int fd);
    static void close_connection(Connection* conn);
    static void reset_connection(Connection* conn);

    // Helper: build minimal response headers for the streaming PUT case
    static std::string build_response_string(int status_code, bool keep_alive);
};
