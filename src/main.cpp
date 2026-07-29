#include "server.h"
#include <iostream>
#include <csignal>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

// Global server pointer for signal handling
Server* g_server = nullptr;

void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        std::cout << "\n[INFO] Received signal " << sig << ", shutting down..." << std::endl;
        if (g_server) {
            g_server->shutdown();
        }
    }
}

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [OPTIONS]\n"
              << "\n"
              << "  WebDAV Server — serve files over WebDAV + browse via browser\n"
              << "\n"
              << "Options:\n"
              << "  -d, --dir <path>    Directory to serve (default: current directory)\n"
              << "  -p, --port <port>   Port to listen on (default: 9000)\n"
              << "  --no-browser        Disable browser-friendly HTML directory listing\n"
              << "  -h, --help          Show this help\n"
              << std::endl;
}

int main(int argc, char* argv[]) {
    std::string root_dir = fs::current_path().string();
    int port = 9000;
    bool allow_browser = true;

    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "-d" || arg == "--dir") {
            if (i + 1 < argc) {
                root_dir = argv[++i];
            } else {
                std::cerr << "Missing argument for --dir" << std::endl;
                return 1;
            }
        } else if (arg == "-p" || arg == "--port") {
            if (i + 1 < argc) {
                port = std::stoi(argv[++i]);
            } else {
                std::cerr << "Missing argument for --port" << std::endl;
                return 1;
            }
        } else if (arg == "--no-browser") {
            allow_browser = false;
        } else {
            std::cerr << "Unknown option: " << arg << std::endl;
            print_usage(argv[0]);
            return 1;
        }
    }

    // Validate port
    if (port < 1 || port > 65535) {
        std::cerr << "Invalid port: " << port << std::endl;
        return 1;
    }

    // Validate root directory
    fs::path root_path = fs::absolute(root_dir);
    if (!fs::exists(root_path)) {
        std::cerr << "Directory does not exist: " << root_dir << std::endl;
        std::cerr << "Creating directory..." << std::endl;
        std::error_code ec;
        if (!fs::create_directories(root_path, ec)) {
            std::cerr << "Failed to create directory: " << ec.message() << std::endl;
            return 1;
        }
    }

    // Setup signal handling
    Server server(root_path, port, allow_browser);
    g_server = &server;

    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    // Ignore SIGPIPE — we use MSG_NOSIGNAL
    signal(SIGPIPE, SIG_IGN);

    server.run();

    return 0;
}
