#include "webdav_handler.h"
#include "file_ops.h"
#include "xml_utils.h"
#include "html_dir.h"
#include "thumbnail.h"
#include "utils.h"
#include <iostream>
#include <algorithm>
#include <charconv>
#include <unistd.h>

WebDavHandler::WebDavHandler(const fs::path& root_dir, bool allow_browser,
                             std::optional<std::string> username,
                             std::optional<std::string> password)
    : root_dir_(fs::absolute(root_dir).lexically_normal())
    , allow_browser_(allow_browser)
    , username_(std::move(username))
    , password_(std::move(password))
{
    if (!file_ops::exists(root_dir_)) {
        file_ops::create_directory(root_dir_);
    }
    std::cout << "[INFO] Serving directory: " << root_dir_ << std::endl;
    if (username_ && password_) {
        std::cout << "[INFO] Authentication enabled (user: " << *username_ << ")" << std::endl;
    }
}

bool WebDavHandler::check_auth(const http::Request& req, const fs::path& resolved_path) {
    // No credentials configured → skip auth
    if (!username_ || !password_) return true;

    // ── Media token check (for player page subresource requests) ─────────
    // Browsers' <video>/<audio> elements don't reliably send HTTP Basic auth
    // credentials on subresource requests. The player page embeds a temporary
    // token in the media URL so the file can be accessed without auth.
    auto token_start = req.query.find("mtoken=");
    if (token_start != std::string::npos) {
        std::string token = req.query.substr(token_start + 7); // len("mtoken=")
        auto amp = token.find('&');
        if (amp != std::string::npos) token.resize(amp);
        if (!token.empty() && verify_media_token(resolved_path, token)) {
            return true;
        }
    }

    // ── Standard HTTP Basic auth ─────────────────────────────────────────
    auto auth_hdr = req.header("Authorization");
    if (!auth_hdr) return false;

    std::string_view auth = *auth_hdr;
    if (auth.size() < 6 || !utils::iequals(auth.substr(0, 6), "Basic ")) {
        std::cerr << "[AUTH] Bad auth header: '" << auth << "'" << std::endl;
        return false;
    }

    std::string decoded = utils::base64_decode(auth.substr(6));
    auto colon = decoded.find(':');
    if (colon == std::string::npos) {
        std::cerr << "[AUTH] base64 decoded no colon: '" << decoded << "'" << std::endl;
        return false;
    }

    std::string user = decoded.substr(0, colon);
    std::string pass = decoded.substr(colon + 1);

    bool ok = (user == *username_ && pass == *password_);
    if (!ok) {
        std::cerr << "[AUTH] Credential mismatch: got '" << user << "'/'" << pass
                  << "' expected '" << *username_ << "'/'" << *password_ << "'" << std::endl;
    }
    return ok;
}

void WebDavHandler::add_common_headers(http::Response& resp) {
    resp.set_header("Server", "WebDAV-Server/1.1 (C++20)");
    resp.set_header("Date", utils::rfc1123_now());
    resp.set_header("Accept-Ranges", "bytes");
}

void WebDavHandler::add_dav_header(http::Response& resp) {
    resp.set_header("DAV", "1, 2");
    resp.set_header("Allow",
        "OPTIONS, GET, HEAD, PUT, DELETE, MKCOL, PROPFIND, PROPPATCH, MOVE, COPY, LOCK, UNLOCK");
}

http::Response WebDavHandler::error_response(int code, std::string_view message) {
    http::Response resp;
    resp.status_code = code;
    add_common_headers(resp);

    std::string msg(message);
    if (msg.empty()) msg = http::status_message(code);

    std::string body = "<!DOCTYPE html><html><head><title>" +
        std::to_string(code) + " " + msg + "</title></head>" +
        "<body><h1>" + std::to_string(code) + " " + msg + "</h1>" +
        "<hr><em>WebDAV Server</em></body></html>";

    resp.set_content_type("text/html; charset=utf-8");
    resp.set_content_length(body.size());
    resp.body = std::move(body);
    return resp;
}

bool WebDavHandler::is_browser_request(const http::Request& req) {
    auto accept = req.header("Accept");
    if (accept && accept->find("text/html") != std::string_view::npos) {
        return true;
    }
    auto ua = req.header("User-Agent");
    if (ua) {
        std::string_view uav = *ua;
        if (uav.find("Mozilla")   != std::string_view::npos ||
            uav.find("Chrome")    != std::string_view::npos ||
            uav.find("Safari")    != std::string_view::npos ||
            uav.find("Firefox")   != std::string_view::npos ||
            uav.find("Edge")      != std::string_view::npos ||
            uav.find("Edg")       != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

bool WebDavHandler::prefers_html(const http::Request& req) {
    auto accept = req.header("Accept");
    if (!accept) return false;
    return accept->find("text/html") != std::string_view::npos;
}

bool WebDavHandler::is_media_file(const fs::path& path) {
    auto ext = path.extension().string();
    // Convert to lowercase for comparison
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".mp4"  || ext == ".webm" || ext == ".ogv"  ||
           ext == ".mkv"  || ext == ".avi"  || ext == ".mov"  ||
           ext == ".mp3"  || ext == ".ogg"  || ext == ".opus" ||
           ext == ".flac" || ext == ".wav"  || ext == ".aac"  ||
           ext == ".m4a"  || ext == ".wma";
}

http::Response WebDavHandler::serve_media_player_page(const http::Request& req, const fs::path& resolved) {
    std::string filename = resolved.filename().string();
    std::string escaped_name = filename;
    // Basic HTML escaping for the filename
    auto escape = [](std::string_view s) -> std::string {
        std::string r;
        r.reserve(s.size() + 8);
        for (char c : s) {
            switch (c) {
            case '&': r += "&amp;"; break;
            case '<': r += "&lt;"; break;
            case '>': r += "&gt;"; break;
            case '"': r += "&quot;"; break;
            default:  r.push_back(c); break;
            }
        }
        return r;
    };
    escaped_name = escape(escaped_name);

    bool is_audio = !(resolved.extension() == ".mp4" ||
                      resolved.extension() == ".webm" ||
                      resolved.extension() == ".ogv" ||
                      resolved.extension() == ".mkv" ||
                      resolved.extension() == ".avi" ||
                      resolved.extension() == ".mov");

    // Build the media URL with appropriate query parameter:
    // - With auth:    include a temporary media token so the browser's <video>
    //                 element can access the file without sending the
    //                 Authorization header (which it doesn't reliably do)
    // - Without auth: simple ?raw=1 to bypass the player page
    std::string media_query;
    if (username_ && password_) {
        std::string token = generate_media_token(resolved);
        media_query = "?mtoken=" + token;
    } else {
        media_query = "?raw=1";
    }
    // Simple path encoding: replace spaces and special chars
    auto url_encode_path = [](std::string_view p) -> std::string {
        std::string r;
        r.reserve(p.size() * 3);
        for (char c : p) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                c == '.' || c == '~' || c == '/') {
                r.push_back(c);
            } else {
                char hex[4];
                snprintf(hex, sizeof(hex), "%%%02X", static_cast<unsigned char>(c));
                r.append(hex, 3);
            }
        }
        return r;
    };
    std::string encoded_path = url_encode_path(req.path);

    std::string mime = utils::mime_type(resolved.string());
    uintmax_t fsize = file_ops::file_size(resolved);

    std::string html;
    html.reserve(2048);
    html += "<!DOCTYPE html>\r\n<html lang=\"en\">\r\n<head>\r\n";
    html += "<meta charset=\"UTF-8\">\r\n";
    html += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\r\n";
    html += "<title>" + escaped_name + "</title>\r\n";
    html += "<style>\r\n";
    html += "*{box-sizing:border-box;margin:0;padding:0;}\r\n";
    html += "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;";
    html += "background:#1a1a2e;color:#eee;display:flex;flex-direction:column;";
    html += "align-items:center;justify-content:center;min-height:100vh;}\r\n";
    html += ".player-container{width:100%;max-width:960px;padding:20px;}\r\n";
    html += ".header{text-align:center;margin-bottom:20px;}\r\n";
    html += ".header h1{font-size:1.2em;font-weight:400;color:#ccc;word-break:break-all;}\r\n";
    html += ".header .info{font-size:0.85em;color:#888;margin-top:8px;}\r\n";
    html += "video,audio{width:100%;border-radius:8px;box-shadow:0 4px 24px rgba(0,0,0,0.5);";
    html += "background:#000;outline:none;}\r\n";
    html += "audio{min-height:60px;}\r\n";
    html += ".back-link{display:inline-block;margin-top:20px;color:#888;text-decoration:none;";
    html += "font-size:0.9em;transition:color 0.2s;}\r\n";
    html += ".back-link:hover{color:#fff;}\r\n";
    html += "</style>\r\n</head>\r\n<body>\r\n";
    html += "<div class=\"player-container\">\r\n";
    html += "<div class=\"header\">\r\n";
    html += "<h1>" + escaped_name + "</h1>\r\n";
    html += "<div class=\"info\">" + utils::format_size(fsize) + " &bull; " + mime + "</div>\r\n";
    html += "</div>\r\n";

    if (is_audio) {
        html += "<audio controls autoplay preload=\"auto\">\r\n";
        html += "<source src=\"" + encoded_path + media_query + "\" type=\"" + mime + "\">\r\n";
        html += "Your browser does not support the audio element.\r\n";
        html += "</audio>\r\n";
    } else {
        html += "<video controls autoplay preload=\"auto\" playsinline>\r\n";
        html += "<source src=\"" + encoded_path + media_query + "\" type=\"" + mime + "\">\r\n";
        html += "Your browser does not support the video element.\r\n";
        html += "</video>\r\n";
    }

    html += "<a class=\"back-link\" href=\"javascript:history.back()\">&larr; Back to directory</a>\r\n";
    html += "</div>\r\n</body>\r\n</html>\r\n";

    http::Response resp;
    resp.status_code = 200;
    add_common_headers(resp);
    resp.set_content_type("text/html; charset=utf-8");
    resp.set_content_length(html.size());
    resp.body = std::move(html);
    return resp;
}

// ── Media token helpers ──────────────────────────────────────────────────────
// When HTTP Basic auth is enabled, browser <video>/<audio> elements don't
// reliably include the Authorization header in subresource requests. To
// work around this, the player page embeds a short-lived token in the
// media URL. The token is a djb2 hash of (path + expiry + password) and
// is valid for 1 hour.

static uint64_t djb2_hash(const std::string& data) {
    uint64_t hash = 5381;
    for (char c : data) {
        hash = ((hash << 5) + hash) + static_cast<unsigned char>(c);
    }
    return hash;
}

std::string WebDavHandler::generate_media_token(const fs::path& filepath) const {
    auto now = std::chrono::system_clock::now();
    auto expiry = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count() + 3600;  // 1 hour

    std::string key = filepath.string() + "|" + std::to_string(expiry) + "|" + password_.value_or("");
    return std::to_string(expiry) + "_" + std::to_string(djb2_hash(key));
}

bool WebDavHandler::verify_media_token(const fs::path& filepath, std::string_view token) const {
    auto underscore = token.find('_');
    if (underscore == std::string_view::npos) return false;

    auto expiry_sv = token.substr(0, underscore);
    auto hash_sv   = token.substr(underscore + 1);

    // Parse expiry
    int64_t expiry = 0;
    auto res = std::from_chars(expiry_sv.data(), expiry_sv.data() + expiry_sv.size(), expiry);
    if (res.ec != std::errc{}) return false;

    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (now > expiry) return false;

    // Parse expected hash
    uint64_t expected_hash = 0;
    res = std::from_chars(hash_sv.data(), hash_sv.data() + hash_sv.size(), expected_hash);
    if (res.ec != std::errc{}) return false;

    // Recompute hash
    std::string key = filepath.string() + "|" + std::string(expiry_sv) + "|" + password_.value_or("");
    return djb2_hash(key) == expected_hash;
}

// ── Thumbnail endpoint ───────────────────────────────────────────────────────

http::Response WebDavHandler::handle_thumbnail(const http::Request& req) {
    // Extract the 'path' query parameter
    std::string file_path;
    auto path_start = req.query.find("path=");
    if (path_start != std::string::npos) {
        file_path = req.query.substr(path_start + 5); // len("path=")
        auto amp = file_path.find('&');
        if (amp != std::string::npos) file_path.resize(amp);
        file_path = utils::url_decode(file_path);
    }

    if (file_path.empty()) {
        return error_response(400, "Bad Request — missing path parameter");
    }

    // Resolve the file path against root_dir
    fs::path resolved = file_ops::resolve_path(root_dir_, file_path);
    if (resolved.empty() || !file_ops::exists(resolved)) {
        return error_response(404, "Not Found");
    }

    // Check auth for the target file (or use media token)
    if (!check_auth(req, resolved)) {
        http::Response resp;
        resp.status_code = 401;
        resp.status_text = "Unauthorized";
        add_common_headers(resp);
        resp.set_header("WWW-Authenticate", "Basic realm=\"WebDAV\"");
        resp.set_content_length(0);
        return resp;
    }

    // Extract optional size parameter
    int size = 256;
    auto size_start = req.query.find("size=");
    if (size_start != std::string::npos) {
        std::string size_str = req.query.substr(size_start + 5);
        auto amp = size_str.find('&');
        if (amp != std::string::npos) size_str.resize(amp);
        if (!size_str.empty()) {
            int parsed = 0;
            auto res = std::from_chars(size_str.data(), size_str.data() + size_str.size(), parsed);
            if (res.ec == std::errc{} && parsed > 0 && parsed <= 1024) {
                size = parsed;
            }
        }
    }

    // Generate thumbnail
    std::string thumb_data = thumbnail::generate(resolved, size);
    std::string mime(thumbnail::mime_type(thumb_data));

    http::Response resp;
    resp.status_code = 200;
    add_common_headers(resp);
    resp.set_content_type(mime);
    resp.set_content_length(thumb_data.size());
    // Cache for 1 hour
    resp.set_header("Cache-Control", "public, max-age=3600");
    resp.body = std::move(thumb_data);
    return resp;
}

http::Response WebDavHandler::handle(const http::Request& req) {
    // Log every request for debugging
    std::cerr << "[REQ] " << req.method_str << " " << req.path
              << " (Auth: " << (req.header("Authorization").has_value() ? "yes" : "no")
              << ", UA: ";
    auto ua = req.header("User-Agent");
    if (ua) {
        std::string_view uav = *ua;
        if (uav.size() > 60) uav = uav.substr(0, 60);
        std::cerr << uav;
    }
    std::cerr << ")" << std::endl;

    // ── Thumbnail endpoint (before auth check — uses own token/auth logic) ─
    if (req.method == http::Method::GET && req.path == "/__thumb__") {
        return handle_thumbnail(req);
    }

    // ── Resolve path (needed early for token-based auth) ──────────────────
    fs::path resolved = file_ops::resolve_path(root_dir_, req.path);
    if (resolved.empty()) {
        return error_response(403, "Forbidden");
    }

    // ── OPTIONS: allow without auth for WebDAV client discovery ───────────
    // Many WebDAV clients (Windows Explorer, macOS Finder, davfs2) send an
    // initial OPTIONS request WITHOUT credentials to discover server
    // capabilities. Returning 401 breaks their discovery flow. Apache and
    // nginx both allow unauthenticated OPTIONS — it's the standard approach.
    bool is_options = (req.method == http::Method::OPTIONS);

    // ── Auth check (skip for OPTIONS) ────────────────────────────────────
    if (!is_options && !check_auth(req, resolved)) {
        std::cerr << "[AUTH] 401 for " << req.method_str << " " << req.path
                  << " (Authorization: " << (req.header("Authorization").has_value() ? "present" : "missing") << ")"
                  << std::endl;
        http::Response resp;
        resp.status_code = 401;
        resp.status_text = "Unauthorized";
        add_common_headers(resp);
        resp.set_header("WWW-Authenticate", "Basic realm=\"WebDAV\"");
        resp.set_content_type("text/html; charset=utf-8");
        std::string body = "<!DOCTYPE html><html><body><h1>401 Unauthorized</h1></body></html>";
        resp.set_content_length(body.size());
        resp.body = std::move(body);
        return resp;
    }

    switch (req.method) {
    case http::Method::OPTIONS:  return handle_options(req, resolved);
    case http::Method::GET:      return handle_get(req, resolved);
    case http::Method::HEAD:     return handle_head(req, resolved);
    case http::Method::PUT:      return handle_put(req, resolved);
    case http::Method::DELETE:   return handle_delete(req, resolved);
    case http::Method::MKCOL:    return handle_mkcol(req, resolved);
    case http::Method::PROPFIND: return handle_propfind(req, resolved);
    case http::Method::MOVE:     return handle_move(req, resolved);
    case http::Method::COPY:     return handle_copy(req, resolved);
    case http::Method::LOCK:
    case http::Method::UNLOCK:   return handle_lock_unlock(req, resolved);
    default:
        return error_response(501, "Not Implemented");
    }
}

http::Response WebDavHandler::handle_options(const http::Request& req, const fs::path& resolved) {
    (void)req;
    (void)resolved;
    http::Response resp;
    resp.status_code = 200;
    add_common_headers(resp);
    add_dav_header(resp);
    resp.set_content_length(0);
    return resp;
}

http::Response WebDavHandler::handle_get(const http::Request& req, const fs::path& resolved) {
    if (!file_ops::exists(resolved)) {
        return error_response(404, "Not Found");
    }

    // ── Directory → HTML listing ──────────────────────────────────────────
    if (file_ops::is_directory(resolved)) {
        std::string path = req.path;
        if (path.empty()) path = "/";

        // Build server origin for thumbnail URLs
        std::string origin;
        auto host_hdr = req.header("Host");
        if (host_hdr) {
            origin = "http://" + std::string(*host_hdr);
        }

        std::string html = html_dir::generate(path, resolved, origin);

        http::Response resp;
        resp.status_code = 200;
        add_common_headers(resp);
        resp.set_content_type("text/html; charset=utf-8");
        resp.set_content_length(html.size());
        resp.body = std::move(html);
        return resp;
    }

    // ── Browser requesting a media file → serve player page ───────────────
    // When auth is enabled, the player page embeds a temporary media token
    // in the video/audio source URL so the browser's media element can
    // access the file without sending HTTP Basic auth credentials (which
    // <video>/<audio> elements don't reliably include in subresource requests).
    if (allow_browser_ && is_browser_request(req) && prefers_html(req) && is_media_file(resolved)) {
        // Check if this is a raw media request (from the player page itself)
        bool has_mtoken = req.query.find("mtoken=") != std::string::npos;
        if (!has_mtoken) {
            return serve_media_player_page(req, resolved);
        }
        // else: query contains a media token → fall through to serve raw file
    }

    // ── Regular file → sendfile (zero-copy) + Range support ───────────────
    uintmax_t fsize = file_ops::file_size(resolved);
    auto range = req.parse_range(fsize);

    http::Response resp;
    add_common_headers(resp);
    resp.set_header("Last-Modified", utils::rfc1123_time(file_ops::last_modified(resolved)));
    resp.set_content_type(utils::mime_type(resolved.string()));

    if (range && fsize > 0) {
        // Clamp range to file size
        size_t range_start = range->start;
        size_t range_end   = range->end.value_or(static_cast<size_t>(fsize) - 1);

        if (range_start >= fsize) {
            // Range Not Satisfiable
            resp.status_code = 416;
            resp.set_header("Content-Range", "bytes */" + std::to_string(fsize));
            resp.set_content_length(0);
            return resp;
        }

        if (range_end >= fsize) range_end = static_cast<size_t>(fsize) - 1;

        size_t content_len = range_end - range_start + 1;

        resp.status_code = 206;
        resp.set_header("Content-Range",
            "bytes " + std::to_string(range_start) + "-" +
            std::to_string(range_end) + "/" + std::to_string(fsize));
        resp.set_content_length(content_len);

        if (content_len > 0) {
            resp.file_to_send = resolved.string();
            resp.file_offset = static_cast<off_t>(range_start);
        }
    } else {
        // Full file
        resp.status_code = 200;
        resp.set_content_length(static_cast<size_t>(fsize));
        if (fsize > 0) {
            resp.file_to_send = resolved.string();
        }
    }
    return resp;
}

http::Response WebDavHandler::handle_head(const http::Request& req, const fs::path& resolved) {
    http::Response resp = handle_get(req, resolved);
    resp.body.clear();
    resp.file_to_send.reset();
    return resp;
}

http::Response WebDavHandler::handle_put(const http::Request& req, const fs::path& resolved) {
    auto parent = resolved.parent_path();
    if (!file_ops::exists(parent)) {
        file_ops::create_directory(parent);
    }

    bool existed = file_ops::exists(resolved);

    // If overwriting an existing file, check it's not in use
    if (existed && file_ops::is_regular_file(resolved)) {
        int lock_fd = file_ops::try_lock_exclusive(resolved);
        if (lock_fd == -1) {
            // File is currently being streamed — reject
            return error_response(423, "Locked — file is being read");
        }
        if (lock_fd >= 0) ::close(lock_fd);  // release lock, we'll write now
    }

    if (!file_ops::write_file(resolved, req.body)) {
        return error_response(507, "Insufficient Storage");
    }

    http::Response resp;
    resp.status_code = existed ? 200 : 201;
    add_common_headers(resp);
    resp.set_content_length(0);
    return resp;
}

http::Response WebDavHandler::handle_delete(const http::Request& req, const fs::path& resolved) {
    (void)req;
    if (!file_ops::exists(resolved)) {
        return error_response(404, "Not Found");
    }

    // For regular files: check if in use before deleting
    if (file_ops::is_regular_file(resolved)) {
        int lock_fd = file_ops::try_lock_exclusive(resolved);
        if (lock_fd == -1) {
            return error_response(423, "Locked — file is being read");
        }
        if (lock_fd >= 0) ::close(lock_fd);
    }

    bool ok = file_ops::is_directory(resolved)
                ? file_ops::remove_all(resolved)
                : file_ops::remove(resolved);

    if (!ok) {
        return error_response(500, "Internal Server Error");
    }

    http::Response resp;
    resp.status_code = 204;
    add_common_headers(resp);
    resp.set_content_length(0);
    return resp;
}

http::Response WebDavHandler::handle_mkcol(const http::Request& req, const fs::path& resolved) {
    (void)req;
    if (file_ops::exists(resolved)) {
        return error_response(405, "Method Not Allowed");
    }

    auto parent = resolved.parent_path();
    if (!file_ops::exists(parent)) {
        return error_response(409, "Conflict — parent does not exist");
    }

    if (!file_ops::create_directory(resolved)) {
        return error_response(507, "Insufficient Storage");
    }

    http::Response resp;
    resp.status_code = 201;
    add_common_headers(resp);
    resp.set_content_length(0);
    return resp;
}

http::Response WebDavHandler::handle_propfind(const http::Request& req, const fs::path& resolved) {
    int depth = 0;
    std::string depth_str = req.depth();
    if (depth_str == "1") depth = 1;
    else if (depth_str == "infinity") depth = 1;

    std::string xml = xml_utils::propfind_response("/", root_dir_, resolved, depth);

    http::Response resp;
    resp.status_code = 207;
    add_common_headers(resp);
    resp.set_content_type("application/xml; charset=utf-8");
    resp.set_content_length(xml.size());
    resp.body = std::move(xml);
    return resp;
}

http::Response WebDavHandler::handle_move(const http::Request& req, const fs::path& resolved) {
    auto dest_hdr = req.header("Destination");
    if (!dest_hdr) {
        return error_response(400, "Bad Request — missing Destination header");
    }

    std::string dest_path = utils::url_decode(std::string(*dest_hdr));

    auto scheme_pos = dest_path.find("://");
    if (scheme_pos != std::string::npos) {
        auto path_start = dest_path.find('/', scheme_pos + 3);
        if (path_start != std::string::npos) {
            dest_path = dest_path.substr(path_start);
        }
    }

    fs::path dest_resolved = file_ops::resolve_path(root_dir_, dest_path);
    if (dest_resolved.empty()) {
        return error_response(403, "Forbidden");
    }

    if (!file_ops::exists(resolved)) {
        return error_response(404, "Not Found");
    }

    // For regular files: check if source file is in use
    if (file_ops::is_regular_file(resolved)) {
        int lock_fd = file_ops::try_lock_exclusive(resolved);
        if (lock_fd == -1) {
            return error_response(423, "Locked — file is being read");
        }
        if (lock_fd >= 0) ::close(lock_fd);
    }

    auto overwrite = req.header("Overwrite");
    bool allow_overwrite = !overwrite || *overwrite != "F";

    if (file_ops::exists(dest_resolved) && !allow_overwrite) {
        return error_response(412, "Precondition Failed");
    }

    // Also check destination if overwriting
    if (file_ops::exists(dest_resolved) && file_ops::is_regular_file(dest_resolved)) {
        int lock_fd = file_ops::try_lock_exclusive(dest_resolved);
        if (lock_fd == -1) {
            return error_response(423, "Locked — destination file is being read");
        }
        if (lock_fd >= 0) ::close(lock_fd);
    }

    auto dest_parent = dest_resolved.parent_path();
    if (!file_ops::exists(dest_parent)) {
        file_ops::create_directory(dest_parent);
    }

    if (!file_ops::rename(resolved, dest_resolved)) {
        return error_response(500, "Internal Server Error");
    }

    http::Response resp;
    resp.status_code = file_ops::exists(resolved) ? 204 : 201;
    add_common_headers(resp);
    resp.set_content_length(0);
    return resp;
}

http::Response WebDavHandler::handle_copy(const http::Request& req, const fs::path& resolved) {
    auto dest_hdr = req.header("Destination");
    if (!dest_hdr) {
        return error_response(400, "Bad Request — missing Destination header");
    }

    std::string dest_path = utils::url_decode(std::string(*dest_hdr));

    auto scheme_pos = dest_path.find("://");
    if (scheme_pos != std::string::npos) {
        auto path_start = dest_path.find('/', scheme_pos + 3);
        if (path_start != std::string::npos) {
            dest_path = dest_path.substr(path_start);
        }
    }

    fs::path dest_resolved = file_ops::resolve_path(root_dir_, dest_path);
    if (dest_resolved.empty()) {
        return error_response(403, "Forbidden");
    }

    if (!file_ops::exists(resolved)) {
        return error_response(404, "Not Found");
    }

    auto overwrite = req.header("Overwrite");
    bool allow_overwrite = !overwrite || *overwrite != "F";

    if (file_ops::exists(dest_resolved) && !allow_overwrite) {
        return error_response(412, "Precondition Failed");
    }

    auto dest_parent = dest_resolved.parent_path();
    if (!file_ops::exists(dest_parent)) {
        file_ops::create_directory(dest_parent);
    }

    if (!file_ops::copy(resolved, dest_resolved)) {
        return error_response(500, "Internal Server Error");
    }

    http::Response resp;
    resp.status_code = file_ops::exists(dest_resolved) ? 204 : 201;
    add_common_headers(resp);
    resp.set_content_length(0);
    return resp;
}

http::Response WebDavHandler::handle_lock_unlock(const http::Request& req, const fs::path& resolved) {
    (void)resolved;

    if (req.method == http::Method::LOCK) {
        std::string lock_token = "urn:uuid:webdav-lock-" +
            std::to_string(std::chrono::system_clock::now().time_since_epoch().count());

        std::string body =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
            "<D:prop xmlns:D=\"DAV:\">\r\n"
            "  <D:lockdiscovery>\r\n"
            "    <D:activelock>\r\n"
            "      <D:locktype><D:write/></D:locktype>\r\n"
            "      <D:lockscope><D:exclusive/></D:lockscope>\r\n"
            "      <D:depth>infinity</D:depth>\r\n"
            "      <D:locktoken><D:href>" + lock_token + "</D:href></D:locktoken>\r\n"
            "      <D:timeout>Second-3600</D:timeout>\r\n"
            "    </D:activelock>\r\n"
            "  </D:lockdiscovery>\r\n"
            "</D:prop>\r\n";

        http::Response resp;
        resp.status_code = 200;
        add_common_headers(resp);
        resp.set_header("Lock-Token", "<" + lock_token + ">");
        resp.set_content_type("application/xml; charset=utf-8");
        resp.set_content_length(body.size());
        resp.body = std::move(body);
        return resp;
    }

    http::Response resp;
    resp.status_code = 204;
    add_common_headers(resp);
    resp.set_content_length(0);
    return resp;
}