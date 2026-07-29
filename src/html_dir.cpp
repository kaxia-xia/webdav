#include "html_dir.h"
#include "file_ops.h"
#include "utils.h"
#include <cstdio>
#include <algorithm>

namespace html_dir {

// ── Cached CSS (computed once, reused for all requests) ──────────────────────
static const std::string& cached_css() {
    static const std::string css = [] {
        std::string s;
        s.reserve(1500);
        s += "  * { box-sizing: border-box; margin: 0; padding: 0; }\r\n";
        s += "  body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; "
             "background: #f5f5f5; color: #333; }\r\n";
        s += "  .header { background: #2c3e50; color: white; padding: 16px 24px; }\r\n";
        s += "  .header h1 { font-size: 1.3em; font-weight: 500; }\r\n";
        s += "  .header .server { font-size: 0.85em; opacity: 0.7; margin-top: 4px; }\r\n";
        s += "  .container { max-width: 960px; margin: 24px auto; padding: 0 16px; }\r\n";
        s += "  .breadcrumb { background: white; padding: 12px 20px; border-radius: 8px; "
             "margin-bottom: 16px; box-shadow: 0 1px 3px rgba(0,0,0,0.08); font-size: 0.9em; }\r\n";
        s += "  .breadcrumb a { color: #3498db; text-decoration: none; }\r\n";
        s += "  .breadcrumb a:hover { text-decoration: underline; }\r\n";
        s += "  .breadcrumb span { color: #999; margin: 0 6px; }\r\n";
        s += "  table { width: 100%; background: white; border-radius: 8px; "
             "box-shadow: 0 1px 3px rgba(0,0,0,0.08); border-collapse: collapse; }\r\n";
        s += "  th { text-align: left; padding: 12px 20px; font-size: 0.8em; text-transform: uppercase; "
             "color: #888; border-bottom: 2px solid #eee; letter-spacing: 0.5px; }\r\n";
        s += "  td { padding: 10px 20px; border-bottom: 1px solid #f0f0f0; }\r\n";
        s += "  tr:hover { background: #f8f9fa; }\r\n";
        s += "  .icon { width: 24px; text-align: center; padding-right: 8px; }\r\n";
        s += "  .name a { color: #2c3e50; text-decoration: none; }\r\n";
        s += "  .name a:hover { color: #3498db; text-decoration: underline; }\r\n";
        s += "  .dir a { font-weight: 500; color: #2980b9; }\r\n";
        s += "  .size { color: #888; text-align: right; white-space: nowrap; }\r\n";
        s += "  .date { color: #888; text-align: right; white-space: nowrap; font-size: 0.9em; }\r\n";
        s += "  .footer { text-align: center; padding: 24px; color: #aaa; font-size: 0.85em; }\r\n";
        s += "  @media (max-width: 600px) {\r\n";
        s += "    .date { display: none; }\r\n";
        s += "    td { padding: 8px 12px; }\r\n";
        s += "  }\r\n";
        return s;
    }();
    return css;
}

// ── Helpers (stack-based, no heap allocation in hot path) ────────────────────

static std::string escape_html(std::string_view s) {
    std::string result;
    result.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '&':  result += "&amp;"; break;
        case '<':  result += "&lt;"; break;
        case '>':  result += "&gt;"; break;
        case '"':  result += "&quot;"; break;
        default:   result.push_back(c); break;
        }
    }
    return result;
}

// Check if file is a video or audio file (by extension)
static bool is_media_ext(const std::string& name) {
    auto dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".mp4"  || ext == ".webm" || ext == ".ogv"  ||
           ext == ".mkv"  || ext == ".avi"  || ext == ".mov"  ||
           ext == ".mp3"  || ext == ".ogg"  || ext == ".opus" ||
           ext == ".flac" || ext == ".wav"  || ext == ".aac"  ||
           ext == ".m4a"  || ext == ".wma";
}

// ── Main generator ───────────────────────────────────────────────────────────

std::string generate(std::string_view path, const fs::path& resolved_path) {
    auto entries = file_ops::list_directory(resolved_path);

    // Pre-allocate: ~2.8KB fixed HTML + ~160 bytes per entry
    std::string h;
    h.reserve(2800 + entries.size() * 160);

    // Parent path — strip trailing slashes first
    std::string parent_path;
    std::string_view clean_path(path);
    while (clean_path.size() > 1 && clean_path.back() == '/')
        clean_path.remove_suffix(1);
    if (!clean_path.empty() && clean_path != "/") {
        auto last_slash = clean_path.rfind('/');
        if (last_slash == 0) {
            parent_path = "/";
        } else if (last_slash != std::string_view::npos) {
            parent_path = std::string(clean_path.substr(0, last_slash));
        }
    }

    std::string display_path(path);
    if (display_path.empty() || display_path.back() != '/') {
        display_path += '/';
    }

    // ── HTML head ────────────────────────────────────────────────────────
    h += "<!DOCTYPE html>\r\n";
    h += "<html lang=\"en\">\r\n";
    h += "<head>\r\n";
    h += "<meta charset=\"UTF-8\">\r\n";
    h += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\r\n";
    h += "<title>Index of ";
    h += escape_html(display_path);
    h += "</title>\r\n";
    h += "<style>\r\n";
    h += cached_css();
    h += "</style>\r\n";
    h += "</head>\r\n";
    h += "<body>\r\n";

    // Header
    h += "<div class=\"header\">\r\n";
    h += "  <h1>📁 Index of ";
    h += escape_html(display_path);
    h += "</h1>\r\n";
    h += "  <div class=\"server\">WebDAV Server &bull; ";
    h += std::to_string(entries.size());
    h += " items</div>\r\n";
    h += "</div>\r\n";

    h += "<div class=\"container\">\r\n";

    // Breadcrumb
    h += "<div class=\"breadcrumb\">\r\n";
    h += "  <a href=\"/\">🏠 Home</a>\r\n";
    if (!display_path.empty() && display_path != "/") {
        std::string accum;
        size_t start = 0;
        while (start < display_path.size() && display_path[start] == '/') ++start;
        auto remaining = std::string_view(display_path).substr(start);
        size_t pos = 0;
        while (pos < remaining.size()) {
            auto slash = remaining.find('/', pos);
            if (slash == std::string_view::npos) break;
            accum += '/' + std::string(remaining.substr(pos, slash - pos));
            h += "  <span>/</span>\r\n";
            h += "  <a href=\"";
            h += escape_html(utils::url_encode(accum));
            h += "/\">";
            h += escape_html(remaining.substr(pos, slash - pos));
            h += "</a>\r\n";
            pos = slash + 1;
        }
    }
    h += "</div>\r\n";

    // Table header
    h += "<table>\r\n";
    h += "<thead><tr>"
         "<th></th><th>Name</th><th class=\"size\">Size</th><th class=\"date\">Modified</th>"
         "</tr></thead>\r\n";
    h += "<tbody>\r\n";

    // Parent ".." link
    if (!parent_path.empty() || path == "/") {
        h += "<tr>";
        h += "<td class=\"icon\">📂</td>";
        std::string parent_href;
        if (parent_path.empty() || parent_path == "/")
            parent_href = "/";
        else
            parent_href = utils::url_encode(parent_path) + '/';
        h += "<td class=\"name dir\"><a href=\"";
        h += escape_html(parent_href);
        h += "\">..</a></td>";
        h += "<td class=\"size\">—</td>";
        h += "<td class=\"date\">—</td>";
        h += "</tr>\r\n";
    }

    // ── Directory entries ────────────────────────────────────────────────
    for (const auto& entry : entries) {
        h += "<tr>";
        h += "<td class=\"icon\">";
        if (entry.is_directory) {
            h += "📁";
        } else if (is_media_ext(entry.name)) {
            h += "▶️";
        } else {
            h += "📄";
        }
        h += "</td>";

        h += "<td class=\"name";
        if (entry.is_directory) h += " dir";
        h += "\"><a href=\"";
        h += escape_html(utils::url_encode(display_path) + utils::url_encode(entry.name));
        if (entry.is_directory) h += '/';
        h += "\">";
        h += escape_html(entry.name);
        h += "</a></td>";

        if (entry.is_directory) {
            h += "<td class=\"size\">—</td>";
        } else {
            h += "<td class=\"size\">";
            h += utils::format_size(entry.size);
            h += "</td>";
        }

        h += "<td class=\"date\">";
        h += utils::rfc1123_time(entry.last_modified);
        h += "</td>";

        h += "</tr>\r\n";
    }

    // Footer
    h += "</tbody>\r\n";
    h += "</table>\r\n";
    h += "<div class=\"footer\">WebDAV Server / C++20</div>\r\n";
    h += "</div>\r\n";
    h += "</body>\r\n";
    h += "</html>\r\n";

    return h;
}

} // namespace html_dir
