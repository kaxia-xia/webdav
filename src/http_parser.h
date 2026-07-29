#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <optional>
#include <cstddef>
#include <cstdint>

namespace http {

// HTTP method
enum class Method {
    UNKNOWN,
    GET,
    HEAD,
    PUT,
    DELETE,
    MKCOL,
    OPTIONS,
    PROPFIND,
    PROPPATCH,
    MOVE,
    COPY,
    LOCK,
    UNLOCK,
    POST,
};

struct Header {
    std::string name;
    std::string value;
};

// Parsed Range: bytes=START-END
// start and end are inclusive. If end is nullopt → from start to EOF.
// If both nullopt → no range.
struct ByteRange {
    size_t start = 0;
    std::optional<size_t> end;  // inclusive
};

struct Request {
    Method method = Method::UNKNOWN;
    std::string method_str;
    std::string uri;
    std::string path;   // decoded path
    std::string query;  // query string (after ?)
    std::string version;
    std::vector<Header> headers;
    std::string body;

    // Get header value by name (case-insensitive)
    std::optional<std::string_view> header(std::string_view name) const;

    // Content-Length as integer
    std::optional<size_t> content_length() const;

    // Get Depth header value
    std::string depth() const;

    // Parse Range: bytes= header. Returns nullopt if missing or invalid.
    // file_size is required to resolve suffix ranges (bytes=-N).
    // If file_size is nullopt, suffix ranges return nullopt.
    std::optional<ByteRange> parse_range(std::optional<uintmax_t> file_size = std::nullopt) const;
};

// Parser states for streaming parse
class Parser {
public:
    Parser();

    // Feed data to parser. Returns true when a complete request has been parsed.
    // Data beyond the request is stored in leftover.
    bool parse(std::string_view data);

    // Get the parsed request
    const Request& request() const { return request_; }

    // Reset for next request
    void reset();

    // Check if parser needs more data
    bool needs_more() const { return state_ != State::COMPLETE; }

    // Get leftover data after parsing
    std::string_view leftover() const { return leftover_; }

private:
    enum class State {
        REQUEST_LINE,
        HEADERS,
        BODY,
        COMPLETE,
    };

    State state_ = State::REQUEST_LINE;
    Request request_;
    std::string buffer_;
    std::string_view leftover_;
    size_t body_expected_ = 0;
    size_t body_received_ = 0;

    bool parse_request_line();
    bool parse_headers();
    bool check_body_complete();
};

// Build an HTTP response
struct Response {
    int status_code = 200;
    std::string status_text;
    std::vector<Header> headers;
    std::string body;

    // If set, the server will send this file via sendfile() after the headers.
    // body should be empty when this is used.
    std::optional<std::string> file_to_send;

    // Optional byte offset into file_to_send (for Range: requests)
    off_t file_offset = 0;

    void set_header(std::string_view name, std::string_view value);
    void set_content_type(std::string_view ct);
    void set_content_length(size_t len);

    // Read back the Content-Length header value (for sendfile logic)
    std::optional<size_t> content_length_opt() const;

    // Serialize to wire format (headers + body; body is omitted when file_to_send is set)
    std::string to_string() const;
};

// Parse method from string
Method parse_method(std::string_view s);

// Get status message for code
const char* status_message(int code);

} // namespace http
