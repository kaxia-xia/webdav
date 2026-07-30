#include "html_dir.h"
#include "file_ops.h"
#include "thumbnail.h"
#include "utils.h"
#include <cstdio>
#include <algorithm>

namespace html_dir {

// ── Cached CSS (computed once, reused for all requests) ──────────────────────
static const std::string& cached_css() {
    static const std::string css = [] {
        std::string s;
        s.reserve(4000);
        s += "  * { box-sizing: border-box; margin: 0; padding: 0; }\r\n";
        s += "  body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; "
             "background: #f5f5f5; color: #333; }\r\n";
        s += "  .header { background: #2c3e50; color: white; padding: 16px 24px; }\r\n";
        s += "  .header h1 { font-size: 1.3em; font-weight: 500; }\r\n";
        s += "  .header .server { font-size: 0.85em; opacity: 0.7; margin-top: 4px; }\r\n";
        s += "  .container { max-width: 1200px; margin: 24px auto; padding: 0 16px; }\r\n";
        s += "  .breadcrumb { background: white; padding: 12px 20px; border-radius: 8px; "
             "margin-bottom: 16px; box-shadow: 0 1px 3px rgba(0,0,0,0.08); font-size: 0.9em; }\r\n";
        s += "  .breadcrumb a { color: #3498db; text-decoration: none; }\r\n";
        s += "  .breadcrumb a:hover { text-decoration: underline; }\r\n";
        s += "  .breadcrumb span { color: #999; margin: 0 6px; }\r\n";
        s += "  .section-title { font-size: 1.1em; font-weight: 600; color: #2c3e50; "
             "padding: 12px 0 8px 0; margin-top: 8px; border-bottom: 2px solid #eee; }\r\n";
        s += "  .section-title .count { font-weight: 400; color: #888; font-size: 0.85em; }\r\n";

        // ── Table styles (for directories and other files) ──────────────
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

        // ── Grid styles (for media files) ────────────────────────────────
        s += "  .media-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(180px, 1fr)); "
             "gap: 16px; margin: 12px 0 20px 0; }\r\n";
        s += "  .media-card { background: white; border-radius: 10px; overflow: hidden; "
             "box-shadow: 0 2px 8px rgba(0,0,0,0.08); transition: transform 0.15s, box-shadow 0.15s; "
             "cursor: pointer; }\r\n";
        s += "  .media-card:hover { transform: translateY(-2px); box-shadow: 0 4px 16px rgba(0,0,0,0.15); }\r\n";
        s += "  .media-card a { text-decoration: none; color: inherit; display: block; }\r\n";
        s += "  .media-thumb { width: 100%; aspect-ratio: 1; background: #1a1a2e; "
             "display: flex; align-items: center; justify-content: center; overflow: hidden; position: relative; }\r\n";
        s += "  .media-thumb img { width: 100%; height: 100%; object-fit: cover; }\r\n";
        s += "  .media-thumb .fallback-icon { display: flex; align-items: center; justify-content: center; "
             "width: 100%; height: 100%; }\r\n";
        s += "  .media-thumb .fallback-icon svg { width: 64px; height: 64px; opacity: 0.7; }\r\n";
        s += "  .media-badge { position: absolute; top: 8px; left: 8px; background: rgba(0,0,0,0.7); "
             "color: #fff; font-size: 0.7em; padding: 2px 8px; border-radius: 4px; "
             "text-transform: uppercase; letter-spacing: 0.5px; }\r\n";
        s += "  .media-duration { position: absolute; bottom: 8px; right: 8px; background: rgba(0,0,0,0.7); "
             "color: #fff; font-size: 0.7em; padding: 2px 6px; border-radius: 4px; }\r\n";
        s += "  .media-info { padding: 10px 12px; }\r\n";
        s += "  .media-name { font-size: 0.85em; font-weight: 500; color: #2c3e50; "
             "white-space: nowrap; overflow: hidden; text-overflow: ellipsis; margin-bottom: 4px; }\r\n";
        s += "  .media-meta { font-size: 0.75em; color: #888; }\r\n";

        // ── Footer ────────────────────────────────────────────────────────
        s += "  .footer { text-align: center; padding: 24px; color: #aaa; font-size: 0.85em; }\r\n";

        // ── Responsive ────────────────────────────────────────────────────
        s += "  @media (max-width: 600px) {\r\n";
        s += "    .date { display: none; }\r\n";
        s += "    td { padding: 8px 12px; }\r\n";
        s += "    .media-grid { grid-template-columns: repeat(auto-fill, minmax(140px, 1fr)); gap: 10px; }\r\n";
        s += "  }\r\n";

        return s;
    }();
    return css;
}

// ── Helpers ──────────────────────────────────────────────────────────────────

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

// ── SVG fallback icons (inline, base64-encoded for use in <img> tags) ──────

static std::string video_icon_svg() {
    return "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 128 128\" "
           "width=\"64\" height=\"64\">"
           "<rect width=\"128\" height=\"128\" rx=\"12\" fill=\"#2c3e50\"/>"
           "<polygon points=\"48,32 48,96 100,64\" fill=\"#3498db\" opacity=\"0.8\"/>"
           "</svg>";
}

static std::string audio_icon_svg() {
    return "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 128 128\" "
           "width=\"64\" height=\"64\">"
           "<rect width=\"128\" height=\"128\" rx=\"12\" fill=\"#2c3e50\"/>"
           "<circle cx=\"64\" cy=\"64\" r=\"32\" fill=\"none\" stroke=\"#e74c3c\" stroke-width=\"6\" opacity=\"0.8\"/>"
           "<circle cx=\"64\" cy=\"64\" r=\"14\" fill=\"#e74c3c\" opacity=\"0.8\"/>"
           "</svg>";
}

static std::string image_icon_svg() {
    return "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 128 128\" "
           "width=\"64\" height=\"64\">"
           "<rect width=\"128\" height=\"128\" rx=\"12\" fill=\"#2c3e50\"/>"
           "<rect x=\"24\" y=\"20\" width=\"80\" height=\"70\" rx=\"4\" fill=\"none\" "
           "stroke=\"#27ae60\" stroke-width=\"4\" opacity=\"0.8\"/>"
           "<circle cx=\"48\" cy=\"44\" r=\"10\" fill=\"#27ae60\" opacity=\"0.7\"/>"
           "<polygon points=\"24,90 50,60 68,76 88,50 104,70 104,90\" "
           "fill=\"#27ae60\" opacity=\"0.5\"/>"
           "</svg>";
}

// ── Main generator ───────────────────────────────────────────────────────────

std::string generate(std::string_view path, const fs::path& resolved_path,
                     std::string_view server_origin,
                     const std::unordered_map<std::string, std::string>* media_tokens) {
    auto entries = file_ops::list_directory(resolved_path);

    // ── Separate entries by type ──────────────────────────────────────────
    std::vector<file_ops::DirEntry> dirs;
    std::vector<file_ops::DirEntry> media_files;
    std::vector<file_ops::DirEntry> other_files;

    for (const auto& e : entries) {
        if (e.is_directory) {
            dirs.push_back(e);
        } else if (thumbnail::is_media_file(e.name)) {
            media_files.push_back(e);
        } else {
            other_files.push_back(e);
        }
    }

    // Pre-allocate
    size_t est_size = 4000 + entries.size() * 300;
    std::string h;
    h.reserve(est_size);

    // ── Parent path ───────────────────────────────────────────────────────
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

    // Build thumb prefix for the thumbnail endpoint
    std::string thumb_prefix(server_origin);
    thumb_prefix += "/__thumb__?path=";

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

    // ── Header ────────────────────────────────────────────────────────────
    h += "<div class=\"header\">\r\n";
    h += "  <h1>📁 Index of ";
    h += escape_html(display_path);
    h += "</h1>\r\n";
    h += "  <div class=\"server\">WebDAV Server &bull; ";
    h += std::to_string(entries.size());
    h += " items";
    if (!media_files.empty()) {
        h += " (";
        h += std::to_string(media_files.size());
        h += " media)";
    }
    h += "</div>\r\n";
    h += "</div>\r\n";

    h += "<div class=\"container\">\r\n";

    // ── Breadcrumb ────────────────────────────────────────────────────────
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

    // ══════════════════════════════════════════════════════════════════════
    // ── Media Grid Section ───────────────────────────────────────────────
    // ══════════════════════════════════════════════════════════════════════
    if (!media_files.empty()) {
        h += "<div class=\"section-title\">🎬 Media <span class=\"count\">(";
        h += std::to_string(media_files.size());
        h += " files)</span></div>\r\n";
        h += "<div class=\"media-grid\">\r\n";

        for (const auto& entry : media_files) {
            bool is_vid = thumbnail::is_video_file(entry.name);
            bool is_img = thumbnail::is_image_file(entry.name);
            std::string badge = is_vid ? "VIDEO" : (is_img ? "IMAGE" : "AUDIO");

            // Encode the file path for the thumbnail URL
            std::string file_url = utils::url_encode(display_path) + utils::url_encode(entry.name);
            std::string thumb_url = thumb_prefix + utils::url_encode(display_path + entry.name);

            // Append media token for auth-bypassed thumbnails
            if (media_tokens) {
                auto it = media_tokens->find(entry.name);
                if (it != media_tokens->end()) {
                    thumb_url += "&mtoken=" + it->second;
                }
            }

            h += "<div class=\"media-card\">\r\n";
            h += "  <a href=\"";
            h += escape_html(file_url);
            h += "\">\r\n";
            h += "    <div class=\"media-thumb\">\r\n";
            // Use <img> tag pointing to thumbnail endpoint; onerror shows fallback
            h += "      <img src=\"";
            h += escape_html(thumb_url);
            h += "\" alt=\"";
            h += escape_html(entry.name);
            h += "\" loading=\"lazy\"";
            h += " onerror=\"this.style.display='none';this.nextElementSibling.style.display='flex';\">\r\n";
            // Fallback icon (hidden until img fails)
            h += "      <div class=\"fallback-icon\" style=\"display:none;\">\r\n";
            if (is_vid) {
                h += "        " + video_icon_svg() + "\r\n";
            } else if (is_img) {
                h += "        " + image_icon_svg() + "\r\n";
            } else {
                h += "        " + audio_icon_svg() + "\r\n";
            }
            h += "      </div>\r\n";
            h += "      <span class=\"media-badge\">" + badge + "</span>\r\n";
            h += "    </div>\r\n";
            h += "    <div class=\"media-info\">\r\n";
            h += "      <div class=\"media-name\" title=\"";
            h += escape_html(entry.name);
            h += "\">";
            h += escape_html(entry.name);
            h += "</div>\r\n";
            h += "      <div class=\"media-meta\">";
            h += utils::format_size(entry.size);
            h += "</div>\r\n";
            h += "    </div>\r\n";
            h += "  </a>\r\n";
            h += "</div>\r\n";
        }

        h += "</div>\r\n";
    }

    // ══════════════════════════════════════════════════════════════════════
    // ── Directory Table Section ──────────────────────────────────────────
    // ══════════════════════════════════════════════════════════════════════
    bool has_dirs_or_other = !dirs.empty() || !other_files.empty();
    bool has_parent = !parent_path.empty() || path == "/";

    if (has_dirs_or_other || has_parent) {
        if (!media_files.empty()) {
            h += "<div class=\"section-title\">📂 Files &amp; Folders</div>\r\n";
        }

        h += "<table>\r\n";
        h += "<thead><tr>"
             "<th></th><th>Name</th><th class=\"size\">Size</th><th class=\"date\">Modified</th>"
             "</tr></thead>\r\n";
        h += "<tbody>\r\n";

        // Parent ".." link
        if (has_parent) {
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

        // Directory entries
        for (const auto& entry : dirs) {
            h += "<tr>";
            h += "<td class=\"icon\">📁</td>";
            h += "<td class=\"name dir\"><a href=\"";
            h += escape_html(utils::url_encode(display_path) + utils::url_encode(entry.name));
            h += "/\">";
            h += escape_html(entry.name);
            h += "</a></td>";
            h += "<td class=\"size\">—</td>";
            h += "<td class=\"date\">";
            h += utils::rfc1123_time(entry.last_modified);
            h += "</td>";
            h += "</tr>\r\n";
        }

        // Other file entries
        for (const auto& entry : other_files) {
            h += "<tr>";
            h += "<td class=\"icon\">📄</td>";
            h += "<td class=\"name\"><a href=\"";
            h += escape_html(utils::url_encode(display_path) + utils::url_encode(entry.name));
            h += "\">";
            h += escape_html(entry.name);
            h += "</a></td>";
            h += "<td class=\"size\">";
            h += utils::format_size(entry.size);
            h += "</td>";
            h += "<td class=\"date\">";
            h += utils::rfc1123_time(entry.last_modified);
            h += "</td>";
            h += "</tr>\r\n";
        }

        h += "</tbody>\r\n";
        h += "</table>\r\n";
    }

    // Footer
    h += "<div class=\"footer\">WebDAV Server / C++20</div>\r\n";
    h += "</div>\r\n";
    h += "</body>\r\n";
    h += "</html>\r\n";

    return h;
}

} // namespace html_dir
