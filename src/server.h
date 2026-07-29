#pragma once

#include "webdav_handler.h"
#include <filesystem>
#include <string>
#include <atomic>
#include <thread>
#include <vector>

class Server {
public:
    Server(const fs::path& root_dir, int port = 9000, bool allow_browser = true);
    ~Server();

    // Start the server (blocking)
    void run();

    // Signal shutdown
    void shutdown();

    // Get the port
    int port() const { return port_; }

private:
    int port_;
    WebDavHandler handler_;
    int server_fd_ = -1;
    std::atomic<bool> running_{true};
    std::vector<std::jthread> client_threads_;

    void handle_client(int client_fd);
};
