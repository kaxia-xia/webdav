#include "webdav_handler.h"
#include "file_ops.h"
#include "xml_utils.h"
#include "html_dir.h"
#include "utils.h"
#include <iostream>
#include <algorithm>

WebDavHandler::WebDavHandler(const fs::path& root_dir, bool allow_browser)
    : root_dir_(fs::absolute(root_dir).lexically_normal())
    , allow_browser_(allow_browser)
{
    // Ensure root directory exists
    if (!file_ops::exists(root_dir_)) {
        file_ops::create_directory(root_dir_);
    }
    std::cout << "[INFO] Serving directory: " << root_dir_ << std::endl;
}

void WebDavHandler::add_common_headers(http::Response& resp) {
    resp.set_header("Server", "WebDAV-Server/1.0 (C++20)");
    resp.set_header("Connection", "keep-alive");
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
    // Also check User-Agent for common browsers
    auto ua = req.header("User-Agent");
    if (ua) {
        std::string_view uav = *ua;
        if (uav.find("Mozilla") != std::string_view::npos ||
            uav.find("Chrome") != std::string_view::npos ||
            uav.find("Safari") != std::string_view::npos ||
            uav.find("Firefox") != std::string_view::npos ||
            uav.find("Edge") != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

http::Response WebDavHandler::handle(const http::Request& req) {
    // Resolve path
    fs::path resolved = file_ops::resolve_path(root_dir_, req.path);
    if (resolved.empty()) {
        return error_response(403, "Forbidden");
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

    // Directory: check if browser wants HTML
    if (file_ops::is_directory(resolved)) {
        if (allow_browser_ && is_browser_request(req)) {
            // Generate HTML directory listing
            std::string path = req.path;
            if (path.empty()) path = "/";
            std::string html = html_dir::generate(path, resolved, root_dir_);

            http::Response resp;
            resp.status_code = 200;
            add_common_headers(resp);
            resp.set_content_type("text/html; charset=utf-8");
            resp.set_content_length(html.size());
            resp.body = std::move(html);
            return resp;
        } else {
            // Return directory as raw (some clients need this)
            // For WebDAV clients that don't accept HTML, return the dir listing as text
            http::Response resp;
            resp.status_code = 200;
            add_common_headers(resp);
            resp.set_content_type("text/html; charset=utf-8");
            std::string path = req.path;
            if (path.empty()) path = "/";
            std::string html = html_dir::generate(path, resolved, root_dir_);
            resp.set_content_length(html.size());
            resp.body = std::move(html);
            return resp;
        }
    }

    // Regular file
    std::string content = file_ops::read_file(resolved);
    // Check if file was readable
    if (content.empty() && file_ops::file_size(resolved) > 0) {
        return error_response(500, "Internal Server Error");
    }

    http::Response resp;
    resp.status_code = 200;
    add_common_headers(resp);
    resp.set_header("Last-Modified", utils::rfc1123_time(file_ops::last_modified(resolved)));
    resp.set_content_type(utils::mime_type(resolved.string()));
    resp.set_content_length(content.size());
    resp.body = std::move(content);
    return resp;
}

http::Response WebDavHandler::handle_head(const http::Request& req, const fs::path& resolved) {
    // Same as GET but without body
    http::Response resp = handle_get(req, resolved);
    resp.body.clear();
    return resp;
}

http::Response WebDavHandler::handle_put(const http::Request& req, const fs::path& resolved) {
    // Create parent directories if needed
    auto parent = resolved.parent_path();
    if (!file_ops::exists(parent)) {
        file_ops::create_directory(parent);
    }

    // Check if resource exists (for 200 vs 201)
    bool existed = file_ops::exists(resolved);

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

    bool ok;
    if (file_ops::is_directory(resolved)) {
        ok = file_ops::remove_all(resolved);
    } else {
        ok = file_ops::remove(resolved);
    }

    if (!ok) {
        return error_response(500, "Internal Server Error");
    }

    http::Response resp;
    resp.status_code = 204; // No Content
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
    else if (depth_str == "infinity") depth = 1; // Cap at 1 for safety

    // Generate XML
    std::string xml = xml_utils::propfind_response("/", root_dir_, resolved, depth);

    http::Response resp;
    resp.status_code = 207; // Multi-Status
    add_common_headers(resp);
    resp.set_content_type("application/xml; charset=utf-8");
    resp.set_content_length(xml.size());
    resp.body = std::move(xml);
    return resp;
}

http::Response WebDavHandler::handle_move(const http::Request& req, const fs::path& resolved) {
    // Destination header contains the target URI
    auto dest_hdr = req.header("Destination");
    if (!dest_hdr) {
        return error_response(400, "Bad Request — missing Destination header");
    }

    // Parse destination URL to get path
    std::string dest_path = utils::url_decode(std::string(*dest_hdr));

    // Remove scheme and host if absolute URL
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

    // Check Overwrite header
    auto overwrite = req.header("Overwrite");
    bool allow_overwrite = !overwrite || *overwrite != "F";

    if (file_ops::exists(dest_resolved) && !allow_overwrite) {
        return error_response(412, "Precondition Failed");
    }

    // Ensure parent exists
    auto dest_parent = dest_resolved.parent_path();
    if (!file_ops::exists(dest_parent)) {
        file_ops::create_directory(dest_parent);
    }

    if (!file_ops::rename(resolved, dest_resolved)) {
        return error_response(500, "Internal Server Error");
    }

    http::Response resp;
    resp.status_code = file_ops::exists(resolved) ? 204 : 201; // Moved to new location
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
    // Minimal LOCK/UNLOCK support: just return success
    // Real implementation would manage locks, but many clients work fine with this stub

    if (req.method == http::Method::LOCK) {
        // Return a simple lock token XML
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

    // UNLOCK
    http::Response resp;
    resp.status_code = 204;
    add_common_headers(resp);
    resp.set_content_length(0);
    return resp;
}
