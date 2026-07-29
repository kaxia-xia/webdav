#include "server.h"
#include "http_parser.h"
#include "utils.h"
#include <iostream>
#include <cstring>
#include <cerrno>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

Server::Server(const fs::path& root_dir, int port, bool allow_browser)
    : port_(port)
    , handler_(root_dir, allow_browser)
{
}

Server::~Server() {
    shutdown();
}

void Server::shutdown() {
    running_ = false;
    if (server_fd_ >= 0) {
        ::close(server_fd_);
        server_fd_ = -1;
    }
}

void Server::handle_client(int client_fd) {
    http::Parser parser;

    // Set read timeout via SO_RCVTIMEO (30 seconds)
    struct timeval tv;
    tv.tv_sec = 30;
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    // Set TCP_NODELAY
    int optval = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));

    char buf[65536];
    std::string pending; // leftover data from last parse

    while (running_) {
        ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                // Timeout — continue waiting
                continue;
            }
            break; // Connection closed or error
        }

        std::string_view data(buf, static_cast<size_t>(n));

        // If we have pending data, prepend it
        std::string full_data;
        if (!pending.empty()) {
            full_data = std::move(pending);
            full_data.append(data);
            data = full_data;
        }

        // Parse all complete requests in the buffer
        while (!data.empty() && running_) {
            if (!parser.parse(data)) {
                if (parser.needs_more()) {
                    // Save leftover for next recv
                    pending = std::string(data);
                    break;
                }
                // Parse error — send 400 and close
                http::Response resp;
                resp.status_code = 400;
                resp.set_header("Connection", "close");
                resp.set_header("Content-Length", "0");
                std::string resp_str = resp.to_string();
                send(client_fd, resp_str.data(), resp_str.size(), MSG_NOSIGNAL);
                goto close_connection;
            }

            // Handle the request
            const auto& req = parser.request();
            http::Response resp = handler_.handle(req);

            // Check Connection header
            bool keep_alive = true;
            auto conn = req.header("Connection");
            if (conn && utils::iequals(*conn, "close")) {
                keep_alive = false;
                resp.set_header("Connection", "close");
            }

            std::string resp_str = resp.to_string();
            ssize_t sent = send(client_fd, resp_str.data(), resp_str.size(), MSG_NOSIGNAL);
            if (sent < 0) {
                goto close_connection;
            }

            // Get leftover data after this request
            data = parser.leftover();
            parser.reset();

            if (!keep_alive) {
                goto close_connection;
            }
        }

        // If we consumed everything, clear pending
        if (data.empty()) {
            pending.clear();
        }
    }

close_connection:
    ::close(client_fd);
}

void Server::run() {
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        std::cerr << "[ERROR] Failed to create socket: " << std::strerror(errno) << std::endl;
        return;
    }

    // Allow address reuse
    int reuse = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port_));

    if (bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[ERROR] Failed to bind port " << port_ << ": "
                  << std::strerror(errno) << std::endl;
        ::close(server_fd_);
        server_fd_ = -1;
        return;
    }

    if (listen(server_fd_, SOMAXCONN) < 0) {
        std::cerr << "[ERROR] Failed to listen: " << std::strerror(errno) << std::endl;
        ::close(server_fd_);
        server_fd_ = -1;
        return;
    }

    std::cout << "[INFO] WebDAV server listening on http://0.0.0.0:" << port_ << std::endl;
    std::cout << "[INFO] Serving: " << handler_.root_dir() << std::endl;
    std::cout << "[INFO] Browser access: http://localhost:" << port_ << "/" << std::endl;

    while (running_) {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd_, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);

        if (client_fd < 0) {
            if (!running_) break;
            std::cerr << "[WARN] accept failed: " << std::strerror(errno) << std::endl;
            continue;
        }

        // Get client IP for logging
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        std::cout << "[CONNECT] " << client_ip << ":" << ntohs(client_addr.sin_port) << std::endl;

        // Spawn thread for this client
        client_threads_.emplace_back(&Server::handle_client, this, client_fd);

        // Clean up finished threads
        for (auto it = client_threads_.begin(); it != client_threads_.end(); ) {
            if (it->joinable()) {
                auto st = it->get_stop_token();
                // jthread: just check if we can join (thread is done)
                // Simple cleanup: try join, if it throws (still running), skip it
            }
            ++it;
        }
        // Erase finished threads
        std::erase_if(client_threads_, [](std::jthread& t) {
            return !t.joinable();
        });
    }

    // Cleanup: close server socket and wait for clients
    if (server_fd_ >= 0) {
        ::close(server_fd_);
        server_fd_ = -1;
    }

    // Join all client threads
    for (auto& t : client_threads_) {
        if (t.joinable()) {
            t.request_stop();
        }
    }
    client_threads_.clear();

    std::cout << "[INFO] Server stopped." << std::endl;
}
