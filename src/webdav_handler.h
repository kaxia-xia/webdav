#pragma once

#include "http_parser.h"
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

class WebDavHandler {
public:
    WebDavHandler(const fs::path& root_dir, bool allow_browser = true);

    // Handle an HTTP request, return the response
    http::Response handle(const http::Request& req);

    // The root directory being served
    const fs::path& root_dir() const { return root_dir_; }

private:
    fs::path root_dir_;
    bool allow_browser_;

    // Check if the request is from a browser (Accept: text/html)
    static bool is_browser_request(const http::Request& req);

    // WebDAV method handlers
    http::Response handle_options(const http::Request& req, const fs::path& resolved);
    http::Response handle_get(const http::Request& req, const fs::path& resolved);
    http::Response handle_head(const http::Request& req, const fs::path& resolved);
    http::Response handle_put(const http::Request& req, const fs::path& resolved);
    http::Response handle_delete(const http::Request& req, const fs::path& resolved);
    http::Response handle_mkcol(const http::Request& req, const fs::path& resolved);
    http::Response handle_propfind(const http::Request& req, const fs::path& resolved);
    http::Response handle_move(const http::Request& req, const fs::path& resolved);
    http::Response handle_copy(const http::Request& req, const fs::path& resolved);
    http::Response handle_lock_unlock(const http::Request& req, const fs::path& resolved);

    // Build common response headers
    void add_common_headers(http::Response& resp);
    void add_dav_header(http::Response& resp);

    // Generate error response
    http::Response error_response(int code, std::string_view message = {});
};
