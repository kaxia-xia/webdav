#include "xml_utils.h"
#include "utils.h"
#include <cstdio>
#include <chrono>

namespace xml_utils {

// ── XML escape with fast-path (no alloc if no special chars) ─────────────────

// Returns the original data if no escaping needed (to avoid copy).
// Caller must use correctly — if escaped is empty, use s directly.
static std::string_view escape_xml_fast(std::string_view s, std::string& buf) {
    // Fast scan: find first special char
    size_t i = 0;
    for (; i < s.size(); ++i) {
        if (s[i] == '&' || s[i] == '<' || s[i] == '>' ||
            s[i] == '"' || s[i] == '\'') break;
    }
    if (i == s.size()) return s;  // no escaping needed — fast path!

    // Need escaping — build into buf
    buf.clear();
    buf.reserve(s.size() + 16);
    buf.append(s.data(), i);
    for (; i < s.size(); ++i) {
        switch (s[i]) {
        case '&':  buf += "&amp;"; break;
        case '<':  buf += "&lt;"; break;
        case '>':  buf += "&gt;"; break;
        case '"':  buf += "&quot;"; break;
        case '\'': buf += "&apos;"; break;
        default:   buf.push_back(s[i]); break;
        }
    }
    return std::string_view(buf);
}

// ── Append a single <D:response> element directly to output buffer ───────────

static void append_file_xml(std::string& out,
                            std::string_view href,
                            const file_ops::DirEntry& entry,
                            std::string& esc_buf)
{
    // href must be percent-encoded per RFC 4918
    std::string encoded_href = utils::url_encode(href);
    auto href_safe = escape_xml_fast(encoded_href, esc_buf);
    auto name_safe = escape_xml_fast(entry.name, esc_buf);

    out += "    <D:response>\r\n";
    out += "      <D:href>";
    out += href_safe;
    out += "</D:href>\r\n";
    out += "      <D:propstat>\r\n";
    out += "        <D:prop>\r\n";

    if (entry.is_directory) {
        out += "          <D:resourcetype><D:collection/></D:resourcetype>\r\n";
        out += "          <D:getcontentlength>0</D:getcontentlength>\r\n";
        out += "          <D:getcontenttype>httpd/unix-directory</D:getcontenttype>\r\n";
    } else {
        out += "          <D:resourcetype/>\r\n";
    }

    out += "          <D:displayname>";
    out += name_safe;
    out += "</D:displayname>\r\n";

    if (!entry.is_directory) {
        out += "          <D:getcontentlength>";
        out += std::to_string(entry.size);
        out += "</D:getcontentlength>\r\n";
    }

    out += "          <D:getlastmodified>";
    out += utils::rfc1123_time(entry.last_modified);
    out += "</D:getlastmodified>\r\n";

    out += "          <D:creationdate>";
    out += utils::iso8601_time(entry.creation_time);
    out += "</D:creationdate>\r\n";

    if (!entry.is_directory) {
        auto mt = utils::mime_type(entry.name);
        auto mt_safe = escape_xml_fast(mt, esc_buf);
        out += "          <D:getcontenttype>";
        out += mt_safe;
        out += "</D:getcontenttype>\r\n";
    }

    // ETag for both files and directories
    auto mt_dur = entry.last_modified.time_since_epoch();
    auto mt_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(mt_dur).count();
    char etag_buf[64];
    int n = snprintf(etag_buf, sizeof(etag_buf), "\"%ld-%ju\"",
                     static_cast<long>(mt_ns),
                     static_cast<uintmax_t>(entry.size));
    out += "          <D:getetag>";
    out.append(etag_buf, static_cast<size_t>(n));
    out += "</D:getetag>\r\n";

    out += "        </D:prop>\r\n";
    out += "        <D:status>HTTP/1.1 200 OK</D:status>\r\n";
    out += "      </D:propstat>\r\n";
    out += "    </D:response>\r\n";
}

// ── Main PROPFIND response builder ───────────────────────────────────────────

std::string propfind_response(
    std::string_view href_prefix,
    const fs::path& root_dir,
    const fs::path& resolved_path,
    int depth)
{
    // ── href prefix ──────────────────────────────────────────────────────
    std::string prefix(href_prefix);
    if (!prefix.empty() && prefix.back() != '/') prefix += '/';

    // ── Relative path ────────────────────────────────────────────────────
    std::string rel_path = "/";
    bool is_dir = file_ops::is_directory(resolved_path);

    if (resolved_path != root_dir) {
        auto rel = fs::relative(resolved_path, root_dir).string();
        std::replace(rel.begin(), rel.end(), '\\', '/');
        if (!rel.empty() && rel != ".") {
            rel_path = prefix + rel;
            if (!rel_path.empty() && rel_path.back() != '/' && is_dir) {
                rel_path += '/';
            }
        }
    }

    // ── Estimate output size: header + ~400B per entry ──────────────────
    std::string out;
    out.reserve(1024);  // will grow if needed; most dirs < 50 files fit
    std::string esc_buf;  // reusable temp for escape_xml_fast

    out += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n";
    out += "<D:multistatus xmlns:D=\"DAV:\">\r\n";

    if (file_ops::exists(resolved_path)) {
        auto entry = file_ops::get_entry(resolved_path);
        append_file_xml(out, rel_path, entry, esc_buf);

        if (depth == 1 && entry.is_directory) {
            auto children = file_ops::list_directory(resolved_path);

            // Pre-build href prefix to avoid concat in loop
            std::string href_base = rel_path;
            if (href_base != "/" && !href_base.empty() && href_base.back() != '/') {
                href_base += '/';
            }

            for (const auto& child : children) {
                std::string child_href;
                child_href.reserve(href_base.size() + child.name.size() + 2);
                child_href = href_base;
                child_href += child.name;
                if (child.is_directory) child_href += '/';
                append_file_xml(out, child_href, child, esc_buf);
            }
        }
    } else {
        out += "    <D:response>\r\n";
        out += "      <D:href>";
        out += rel_path;
        out += "</D:href>\r\n";
        out += "      <D:propstat>\r\n";
        out += "        <D:prop/>\r\n";
        out += "        <D:status>HTTP/1.1 404 Not Found</D:status>\r\n";
        out += "      </D:propstat>\r\n";
        out += "    </D:response>\r\n";
    }

    out += "</D:multistatus>\r\n";
    return out;
}

} // namespace xml_utils
