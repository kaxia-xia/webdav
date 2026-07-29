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
