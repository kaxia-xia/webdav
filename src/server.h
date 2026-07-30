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
        RECEIVING_BODY,    // streaming PUT body to disk
        SENDING_HEADERS,
        SENDING_FILE,      // sending file via io_uring read+send
        CLOSING,
    };

    static constexpr size_t READ_BUF_SIZE = 65536;
    static constexpr size_t FILE_BUF_SIZE = 65536;

    struct Connection {
        int fd = -1;
        State state = State::READING_REQUEST;
        bool keep_alive = true;

        // HTTP parsing
        http::Parser parser;
        char read_buf[READ_BUF_SIZE];
        size_t read_offset = 0;

        // Response (headers + small body, serialised)
        std::string response_data;
        size_t send_offset = 0;

        // File serving (GET)
        int file_fd = -1;
        off_t file_off = 0;
        size_t file_remaining = 0;
        char file_buf[FILE_BUF_SIZE];
        bool file_data_ready = false;

        // File receiving (PUT streaming)
        int output_fd = -1;
        std::string output_final_path;
        std::string output_tmp_path;
        size_t body_expected = 0;
        size_t body_received = 0;
        bool existed_before_put = false;
    };

    void worker_loop();
    static void set_socket_options(int fd);
    static void close_connection(Connection* conn);
    static void reset_connection(Connection* conn);

    // Helper: build minimal response headers for the streaming PUT case
    static std::string build_response_string(int status_code, bool keep_alive);
};
