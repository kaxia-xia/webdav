#include "xml_utils.h"
#include "utils.h"
#include <sstream>

namespace xml_utils {

static std::string escape_xml(std::string_view s) {
    std::string result;
    result.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&':  result += "&amp;"; break;
        case '<':  result += "&lt;"; break;
        case '>':  result += "&gt;"; break;
        case '"':  result += "&quot;"; break;
        case '\'': result += "&apos;"; break;
        default:   result.push_back(c); break;
        }
    }
    return result;
}

std::string file_response_xml(
    std::string_view href,
    const file_ops::DirEntry& entry)
{
    std::ostringstream oss;
    oss << "    <D:response>\r\n";
    oss << "      <D:href>" << escape_xml(href) << "</D:href>\r\n";
    oss << "      <D:propstat>\r\n";
    oss << "        <D:prop>\r\n";

    // Resource type
    if (entry.is_directory) {
        oss << "          <D:resourcetype><D:collection/></D:resourcetype>\r\n";
    } else {
        oss << "          <D:resourcetype/>\r\n";
    }

    // Display name
    oss << "          <D:displayname>" << escape_xml(entry.name) << "</D:displayname>\r\n";

    // Content length
    if (!entry.is_directory) {
        oss << "          <D:getcontentlength>" << entry.size << "</D:getcontentlength>\r\n";
    }

    // Last modified
    oss << "          <D:getlastmodified>"
        << utils::rfc1123_time(entry.last_modified)
        << "</D:getlastmodified>\r\n";

    // Creation date
    oss << "          <D:creationdate>"
        << utils::iso8601_time(entry.creation_time)
        << "</D:creationdate>\r\n";

    // Content type (rough)
    if (!entry.is_directory) {
        oss << "          <D:getcontenttype>"
            << escape_xml(utils::mime_type(entry.name))
            << "</D:getcontenttype>\r\n";
    }

    // ETag (simple: use size + mtime)
    if (!entry.is_directory) {
        oss << "          <D:getetag>\"" << entry.size << "-"
            << entry.last_modified.time_since_epoch().count()
            << "\"</D:getetag>\r\n";
    }

    oss << "        </D:prop>\r\n";
    oss << "        <D:status>HTTP/1.1 200 OK</D:status>\r\n";
    oss << "      </D:propstat>\r\n";
    oss << "    </D:response>\r\n";
    return oss.str();
}

static std::string not_found_xml(std::string_view href) {
    std::ostringstream oss;
    oss << "    <D:response>\r\n";
    oss << "      <D:href>" << escape_xml(href) << "</D:href>\r\n";
    oss << "      <D:propstat>\r\n";
    oss << "        <D:prop/>\r\n";
    oss << "        <D:status>HTTP/1.1 404 Not Found</D:status>\r\n";
    oss << "      </D:propstat>\r\n";
    oss << "    </D:response>\r\n";
    return oss.str();
}

std::string propfind_response(
    std::string_view href_prefix,
    const fs::path& root_dir,
    const fs::path& resolved_path,
    int depth)
{
    // Ensure href_prefix ends with /
    std::string prefix(href_prefix);
    if (!prefix.empty() && prefix.back() != '/') prefix += '/';

    // Get the relative href path
    std::string rel_path = "/";
    if (resolved_path != root_dir) {
        auto rel = fs::relative(resolved_path, root_dir).string();
        // Convert backslashes to forward slashes (Windows)
        std::replace(rel.begin(), rel.end(), '\\', '/');
        if (!rel.empty() && rel != ".") {
            rel_path = prefix + rel;
            if (!rel_path.empty() && rel_path.back() != '/' &&
                file_ops::is_directory(resolved_path)) {
                rel_path += '/';
            }
        }
    }

    std::ostringstream oss;
    oss << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n";
    oss << "<D:multistatus xmlns:D=\"DAV:\">\r\n";

    if (file_ops::exists(resolved_path)) {
        auto entry = file_ops::get_entry(resolved_path);
        oss << file_response_xml(rel_path, entry);

        // Depth: 1 — also list children
        if (depth == 1 && entry.is_directory) {
            auto children = file_ops::list_directory(resolved_path);
            for (const auto& child : children) {
                std::string child_href = rel_path;
                if (child_href.empty() || child_href.back() != '/') {
                    if (child_href == "/") {
                        child_href += child.name;
                    } else {
                        child_href += '/' + child.name;
                    }
                } else {
                    child_href += child.name;
                }
                if (child.is_directory) child_href += '/';
                oss << file_response_xml(child_href, child);
            }
        }
    } else {
        oss << not_found_xml(rel_path);
    }

    oss << "</D:multistatus>\r\n";
    return oss.str();
}

} // namespace xml_utils
