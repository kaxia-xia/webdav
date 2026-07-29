#pragma once

#include "http_parser.h"
#include <filesystem>
#include <string>
#include <optional>

namespace fs = std::filesystem;

class WebDavHandler {
public:
    WebDavHandler(const fs::path& root_dir, bool allow_browser = true,
                  std::optional<std::string> username = std::nullopt,
                  std::optional<std::string> password = std::nullopt);

    // Handle an HTTP request, return the response
    http::Response handle(const http::Request& req);

    // The root directory being served
    const fs::path& root_dir() const { return root_dir_; }

private:
    fs::path root_dir_;
    bool allow_browser_;
    std::optional<std::string> username_;
    std::optional<std::string> password_;

    // Check HTTP Basic auth; returns true if authorized (or auth not configured)
    // resolved_path is used to verify media tokens embedded in query string
    bool check_auth(const http::Request& req, const fs::path& resolved_path);

    // Check if the request is from a browser (Accept: text/html)

    // Generate a short-lived token for unauthenticated media access
    [[nodiscard]] std::string generate_media_token(const fs::path& filepath) const;

    // Verify a media token is valid for the given file
    [[nodiscard]] bool verify_media_token(const fs::path& filepath, std::string_view token) const;
    static bool is_browser_request(const http::Request& req);

    // Check if Accept header prefers text/html (page navigation vs media request)
    static bool prefers_html(const http::Request& req);

    // Check if file is a video or audio file (by extension)
    static bool is_media_file(const fs::path& path);

    // Serve an HTML5 media player page for video/audio files
    http::Response serve_media_player_page(const http::Request& req, const fs::path& resolved);

    // Serve a thumbnail for a media file
    http::Response handle_thumbnail(const http::Request& req);

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
