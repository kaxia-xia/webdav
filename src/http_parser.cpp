#include "http_parser.h"
#include "utils.h"
#include <algorithm>
#include <charconv>

namespace http {

// ── Request ──────────────────────────────────────────────────────────────────

std::optional<std::string_view> Request::header(std::string_view name) const {
    for (const auto& h : headers) {
        if (utils::iequals(h.name, name)) return h.value;
    }
    return std::nullopt;
}

std::optional<size_t> Request::content_length() const {
    auto h = header("Content-Length");
    if (!h) return std::nullopt;
    size_t val = 0;
    auto result = std::from_chars(h->data(), h->data() + h->size(), val);
    if (result.ec != std::errc{}) return std::nullopt;
    return val;
}

std::string Request::depth() const {
    auto h = header("Depth");
    if (!h) return "infinity";
    return std::string(*h);
}

std::optional<ByteRange> Request::parse_range(std::optional<uintmax_t> file_size) const {
    auto h = header("Range");
    if (!h) return std::nullopt;

    std::string_view v = *h;
    if (v.size() < 6 || !utils::iequals(v.substr(0, 6), "bytes=")) return std::nullopt;

    std::string_view range_val = v.substr(6);
    auto comma = range_val.find(',');
    if (comma != std::string_view::npos) range_val = range_val.substr(0, comma);

    auto dash = range_val.find('-');
    if (dash == std::string_view::npos) return std::nullopt;

    std::string_view start_str = range_val.substr(0, dash);
    std::string_view end_str   = range_val.substr(dash + 1);

    ByteRange br;

    if (start_str.empty()) {
        if (!file_size || *file_size == 0) return std::nullopt;
        size_t suffix_count = 0;
        auto res = std::from_chars(end_str.data(), end_str.data() + end_str.size(), suffix_count);
        if (res.ec != std::errc{}) return std::nullopt;
        if (suffix_count == 0) return std::nullopt;
        if (suffix_count > *file_size) suffix_count = static_cast<size_t>(*file_size);
        br.start = static_cast<size_t>(*file_size - suffix_count);
        br.end   = static_cast<size_t>(*file_size - 1);
    } else {
        auto res = std::from_chars(start_str.data(), start_str.data() + start_str.size(), br.start);
        if (res.ec != std::errc{}) return std::nullopt;
        if (!end_str.empty()) {
            size_t end = 0;
            auto res2 = std::from_chars(end_str.data(), end_str.data() + end_str.size(), end);
            if (res2.ec != std::errc{}) return std::nullopt;
            br.end = end;
        }
    }

    if (br.end && *br.end < br.start) return std::nullopt;
    return br;
}

// ── Parser ───────────────────────────────────────────────────────────────────

Parser::Parser() { reset(); }

void Parser::reset() {
    state_ = State::REQUEST_LINE;
    request_ = Request{};
    buffer_.clear();
    leftover_ = {};
    body_expected_ = 0;
    body_received_ = 0;
}

bool Parser::parse(std::string_view data, size_t max_body) {
    buffer_.append(data);

    while (state_ != State::COMPLETE) {
        switch (state_) {
        case State::REQUEST_LINE: {
            auto pos = buffer_.find("\r\n");
            if (pos == std::string::npos) {
                if (buffer_.size() > 8192) return false;
                return false;
            }
            std::string line = buffer_.substr(0, pos);
            buffer_.erase(0, pos + 2);

            auto parts = utils::split(line, ' ');
            if (parts.size() < 3) return false;

            request_.method_str = std::string(parts[0]);
            request_.method = parse_method(request_.method_str);
            request_.uri = std::string(parts[1]);
            request_.version = std::string(parts[2]);

            auto qpos = request_.uri.find('?');
            if (qpos != std::string::npos) {
                request_.path = utils::url_decode(request_.uri.substr(0, qpos));
                request_.query = request_.uri.substr(qpos + 1);
            } else {
                request_.path = utils::url_decode(request_.uri);
            }

            state_ = State::HEADERS;
            break;
        }
        case State::HEADERS: {
            auto pos = buffer_.find("\r\n");
            if (pos == std::string::npos) {
                if (buffer_.size() > 65536) return false;
                return false;
            }
            std::string line = buffer_.substr(0, pos);
            buffer_.erase(0, pos + 2);

            if (line.empty()) {
                auto cl = request_.content_length();
                if (cl.has_value()) {
                    body_expected_ = *cl;
                    request_.expected_body_size = *cl;
                    if (body_expected_ > 0) {
                        if (body_expected_ > max_body) {
                            // Body too large — stop here, caller will stream
                            request_.body_truncated = true;
                            state_ = State::COMPLETE;
                            leftover_ = buffer_;
                        } else {
                            state_ = State::BODY;
                        }
                    } else {
                        state_ = State::COMPLETE;
                        leftover_ = buffer_;
                    }
                } else {
                    state_ = State::COMPLETE;
                    leftover_ = buffer_;
                }
            } else {
                auto colon = line.find(':');
                if (colon != std::string::npos) {
                    Header h;
                    h.name = std::string(utils::trim(line.substr(0, colon)));
                    h.value = std::string(utils::trim(line.substr(colon + 1)));
                    request_.headers.push_back(std::move(h));
                }
            }
            break;
        }
        case State::BODY: {
            if (buffer_.size() >= body_expected_) {
                request_.body = buffer_.substr(0, body_expected_);
                buffer_.erase(0, body_expected_);
                state_ = State::COMPLETE;
                leftover_ = buffer_;
            } else {
                return false;
            }
            break;
        }
        case State::COMPLETE:
            break;
        }
    }
    return true;
}

// ── Response ─────────────────────────────────────────────────────────────────

void Response::set_header(std::string_view name, std::string_view value) {
    headers.push_back(Header{std::string(name), std::string(value)});
}

void Response::set_content_type(std::string_view ct) {
    set_header("Content-Type", ct);
}

void Response::set_content_length(size_t len) {
    set_header("Content-Length", std::to_string(len));
}

std::optional<size_t> Response::content_length_opt() const {
    for (const auto& h : headers) {
        if (utils::iequals(h.name, "Content-Length")) {
            size_t val = 0;
            auto result = std::from_chars(h.value.data(), h.value.data() + h.value.size(), val);
            if (result.ec == std::errc{}) return val;
        }
    }
    return std::nullopt;
}

std::string Response::to_string() const {
    std::string result;

    // Pre-compute exact size to avoid reallocs
    size_t estimate = 20;
    for (const auto& h : headers) {
        estimate += h.name.size() + h.value.size() + 4;
    }
    estimate += 2;

    // Omit body from serialisation when sending a file or streaming body
    bool has_separate_body = file_to_send.has_value() || body_output_fd >= 0;
    if (!has_separate_body) {
        estimate += body.size();
    }
    result.reserve(estimate);

    // Status line
    result += "HTTP/1.1 ";
    result += std::to_string(status_code);
    result += " ";
    result += status_text.empty() ? std::string(status_message(status_code)) : status_text;
    result += "\r\n";

    // Headers
    for (const auto& h : headers) {
        result += h.name;
        result += ": ";
        result += h.value;
        result += "\r\n";
    }

    result += "\r\n";

    // Body (only for in-memory responses)
    if (!has_separate_body) {
        result += body;
    }
    return result;
}

// ── Helpers ──────────────────────────────────────────────────────────────────

Method parse_method(std::string_view s) {
    if (utils::iequals(s, "GET"))       return Method::GET;
    if (utils::iequals(s, "HEAD"))      return Method::HEAD;
    if (utils::iequals(s, "PUT"))       return Method::PUT;
    if (utils::iequals(s, "DELETE"))    return Method::DELETE;
    if (utils::iequals(s, "MKCOL"))     return Method::MKCOL;
    if (utils::iequals(s, "OPTIONS"))   return Method::OPTIONS;
    if (utils::iequals(s, "PROPFIND"))  return Method::PROPFIND;
    if (utils::iequals(s, "PROPPATCH")) return Method::PROPPATCH;
    if (utils::iequals(s, "MOVE"))      return Method::MOVE;
    if (utils::iequals(s, "COPY"))      return Method::COPY;
    if (utils::iequals(s, "LOCK"))      return Method::LOCK;
    if (utils::iequals(s, "UNLOCK"))    return Method::UNLOCK;
    if (utils::iequals(s, "POST"))      return Method::POST;
    return Method::UNKNOWN;
}

const char* status_message(int code) {
    switch (code) {
    case 100: return "Continue";
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 207: return "Multi-Status";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 412: return "Precondition Failed";
    case 415: return "Unsupported Media Type";
    case 423: return "Locked";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 507: return "Insufficient Storage";
    default:  return "Unknown";
    }
}

} // namespace http
