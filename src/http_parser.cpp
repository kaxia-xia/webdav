#include "http_parser.h"
#include "utils.h"
#include <algorithm>
#include <charconv>

namespace http {

// ---------- Request ----------

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

// ---------- Parser ----------

Parser::Parser() { reset(); }

void Parser::reset() {
    state_ = State::REQUEST_LINE;
    request_ = Request{};
    buffer_.clear();
    leftover_ = {};
    body_expected_ = 0;
    body_received_ = 0;
}

bool Parser::parse(std::string_view data) {
    buffer_.append(data);

    while (state_ != State::COMPLETE) {
        switch (state_) {
        case State::REQUEST_LINE: {
            // Find \r\n
            auto pos = buffer_.find("\r\n");
            if (pos == std::string::npos) {
                // Need more data, but protect against too-large first line
                if (buffer_.size() > 8192) return false;
                return false;
            }
            std::string line = buffer_.substr(0, pos);
            buffer_.erase(0, pos + 2);

            // Parse: METHOD URI HTTP/VERSION
            auto parts = utils::split(line, ' ');
            if (parts.size() < 3) return false;

            request_.method_str = std::string(parts[0]);
            request_.method = parse_method(request_.method_str);
            request_.uri = std::string(parts[1]);
            request_.version = std::string(parts[2]);

            // Parse path and query
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
            // Read headers line by line
            auto pos = buffer_.find("\r\n");
            if (pos == std::string::npos) {
                if (buffer_.size() > 65536) return false;
                return false;
            }
            std::string line = buffer_.substr(0, pos);
            buffer_.erase(0, pos + 2);

            if (line.empty()) {
                // End of headers
                auto cl = request_.content_length();
                if (cl.has_value()) {
                    body_expected_ = *cl;
                    if (body_expected_ > 0) {
                        state_ = State::BODY;
                    } else {
                        state_ = State::COMPLETE;
                        leftover_ = buffer_;
                    }
                } else {
                    // No Content-Length for GET/HEAD/etc — no body
                    state_ = State::COMPLETE;
                    leftover_ = buffer_;
                }
            } else {
                // Parse header: Name: Value
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
                return false; // Need more data
            }
            break;
        }
        case State::COMPLETE:
            break;
        }
    }
    return true;
}

// ---------- Response ----------

void Response::set_header(std::string_view name, std::string_view value) {
    headers.push_back(Header{std::string(name), std::string(value)});
}

void Response::set_content_type(std::string_view ct) {
    set_header("Content-Type", ct);
}

void Response::set_content_length(size_t len) {
    set_header("Content-Length", std::to_string(len));
}

std::string Response::to_string() const {
    std::string result;
    result.reserve(256 + body.size());

    result += "HTTP/1.1 ";
    result += std::to_string(status_code);
    result += " ";
    result += status_text.empty() ? std::string(status_message(status_code)) : status_text;
    result += "\r\n";

    for (const auto& h : headers) {
        result += h.name;
        result += ": ";
        result += h.value;
        result += "\r\n";
    }

    result += "\r\n";
    result += body;
    return result;
}

// ---------- Helpers ----------

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
