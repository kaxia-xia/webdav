#include "html_dir.h"
#include "file_ops.h"
#include "utils.h"
#include <sstream>
#include <algorithm>

namespace html_dir {

static std::string escape_html(std::string_view s) {
    std::string result;
    result.reserve(s.size());
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

static std::string format_size(uintmax_t size) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_idx = 0;
    double display = static_cast<double>(size);
    while (display >= 1024.0 && unit_idx < 4) {
        display /= 1024.0;
        ++unit_idx;
    }
    std::ostringstream oss;
    if (unit_idx == 0) {
        oss << static_cast<int>(display) << ' ' << units[unit_idx];
    } else {
        oss << std::fixed << std::setprecision(1) << display << ' ' << units[unit_idx];
    }
    return oss.str();
}

std::string generate(std::string_view path, const fs::path& resolved_path, const fs::path& root_dir) {
    auto entries = file_ops::list_directory(resolved_path);

    // Compute parent path
    std::string parent_path;
    if (path != "/") {
        auto last_slash = path.rfind('/');
        if (last_slash == 0) {
            parent_path = "/";
        } else if (last_slash != std::string_view::npos) {
            parent_path = std::string(path.substr(0, last_slash));
        }
    }

    // Ensure path ends with /
    std::string display_path(path);
    if (display_path.empty() || display_path.back() != '/') {
        display_path += '/';
    }

    std::ostringstream html;
    html << "<!DOCTYPE html>\r\n";
    html << "<html lang=\"en\">\r\n";
    html << "<head>\r\n";
    html << "<meta charset=\"UTF-8\">\r\n";
    html << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\r\n";
    html << "<title>Index of " << escape_html(display_path) << "</title>\r\n";
    html << "<style>\r\n";
    html << "  * { box-sizing: border-box; margin: 0; padding: 0; }\r\n";
    html << "  body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; "
            "background: #f5f5f5; color: #333; }\r\n";
    html << "  .header { background: #2c3e50; color: white; padding: 16px 24px; }\r\n";
    html << "  .header h1 { font-size: 1.3em; font-weight: 500; }\r\n";
    html << "  .header .server { font-size: 0.85em; opacity: 0.7; margin-top: 4px; }\r\n";
    html << "  .container { max-width: 960px; margin: 24px auto; padding: 0 16px; }\r\n";
    html << "  .breadcrumb { background: white; padding: 12px 20px; border-radius: 8px; "
            "margin-bottom: 16px; box-shadow: 0 1px 3px rgba(0,0,0,0.08); font-size: 0.9em; }\r\n";
    html << "  .breadcrumb a { color: #3498db; text-decoration: none; }\r\n";
    html << "  .breadcrumb a:hover { text-decoration: underline; }\r\n";
    html << "  .breadcrumb span { color: #999; margin: 0 6px; }\r\n";
    html << "  table { width: 100%; background: white; border-radius: 8px; "
            "box-shadow: 0 1px 3px rgba(0,0,0,0.08); border-collapse: collapse; }\r\n";
    html << "  th { text-align: left; padding: 12px 20px; font-size: 0.8em; text-transform: uppercase; "
            "color: #888; border-bottom: 2px solid #eee; letter-spacing: 0.5px; }\r\n";
    html << "  td { padding: 10px 20px; border-bottom: 1px solid #f0f0f0; }\r\n";
    html << "  tr:hover { background: #f8f9fa; }\r\n";
    html << "  .icon { width: 24px; text-align: center; padding-right: 8px; }\r\n";
    html << "  .name a { color: #2c3e50; text-decoration: none; }\r\n";
    html << "  .name a:hover { color: #3498db; text-decoration: underline; }\r\n";
    html << "  .dir a { font-weight: 500; color: #2980b9; }\r\n";
    html << "  .size { color: #888; text-align: right; white-space: nowrap; }\r\n";
    html << "  .date { color: #888; text-align: right; white-space: nowrap; font-size: 0.9em; }\r\n";
    html << "  .footer { text-align: center; padding: 24px; color: #aaa; font-size: 0.85em; }\r\n";
    html << "  @media (max-width: 600px) {\r\n";
    html << "    .date { display: none; }\r\n";
    html << "    td { padding: 8px 12px; }\r\n";
    html << "  }\r\n";
    html << "</style>\r\n";
    html << "</head>\r\n";
    html << "<body>\r\n";

    // Header
    html << "<div class=\"header\">\r\n";
    html << "  <h1>📁 Index of " << escape_html(display_path) << "</h1>\r\n";
    html << "  <div class=\"server\">WebDAV Server &bull; " << entries.size() << " items</div>\r\n";
    html << "</div>\r\n";

    html << "<div class=\"container\">\r\n";

    // Breadcrumb
    html << "<div class=\"breadcrumb\">\r\n";
    html << "  <a href=\"/\">🏠 Home</a>\r\n";
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
            html << "  <span>/</span>\r\n";
            html << "  <a href=\"" << escape_html(accum) << "/\">"
                 << escape_html(remaining.substr(pos, slash - pos)) << "</a>\r\n";
            pos = slash + 1;
        }
    }
    html << "</div>\r\n";

    // Table
    html << "<table>\r\n";
    html << "<thead><tr>"
            "<th></th><th>Name</th><th class=\"size\">Size</th><th class=\"date\">Modified</th>"
            "</tr></thead>\r\n";
    html << "<tbody>\r\n";

    // Parent directory link
    if (!parent_path.empty() || path == "/") {
        html << "<tr>";
        html << "<td class=\"icon\">📂</td>";
        html << "<td class=\"name dir\"><a href=\""
             << escape_html(parent_path.empty() ? "/" : parent_path)
             << "\">..</a></td>";
        html << "<td class=\"size\">—</td>";
        html << "<td class=\"date\">—</td>";
        html << "</tr>\r\n";
    }

    // Entries
    for (const auto& entry : entries) {
        std::string href = display_path + utils::url_encode(entry.name);
        std::string icon = entry.is_directory ? "📁" : "📄";

        html << "<tr>";
        html << "<td class=\"icon\">" << icon << "</td>";
        html << "<td class=\"name" << (entry.is_directory ? " dir" : "") << "\">"
             << "<a href=\"" << escape_html(href) << "\">"
             << escape_html(entry.name)
             << "</a></td>";

        if (entry.is_directory) {
            html << "<td class=\"size\">—</td>";
        } else {
            html << "<td class=\"size\">" << format_size(entry.size) << "</td>";
        }

        html << "<td class=\"date\">"
             << utils::rfc1123_time(entry.last_modified)
             << "</td>";

        html << "</tr>\r\n";
    }

    html << "</tbody>\r\n";
    html << "</table>\r\n";

    html << "<div class=\"footer\">WebDAV Server / C++20</div>\r\n";
    html << "</div>\r\n";
    html << "</body>\r\n";
    html << "</html>\r\n";

    return html.str();
}

} // namespace html_dir
