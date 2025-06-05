#pragma once
#include <string>
#include <string_view>

namespace localshare {

struct HttpRequest {
    std::string method;
    std::string target;     // raw path, may contain query
    std::string version;
    std::string host;
    bool keep_alive = true; // HTTP/1.1 default
};

// Returns true and populates `out` if `buf` contains a complete request line +
// headers terminated by CRLF CRLF. `header_len` is set to the byte length of
// the header section.
bool parse_request(std::string_view buf, HttpRequest* out, size_t* header_len);

// Resolves an HTTP path under `root`. Returns absolute filesystem path on
// success, or empty string for any path that escapes root or contains
// percent-encoded NULs / `..` traversal.
std::string resolve_path(const std::string& root, const std::string& target);

const char* mime_for(const std::string& path);

// URL-decode a path. Returns false if malformed (bad %xx, embedded NUL).
bool url_decode(std::string_view in, std::string* out);

}  // namespace localshare
