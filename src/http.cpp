#include "http.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace localshare {

namespace {

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

}  // namespace

bool url_decode(std::string_view in, std::string* out) {
    out->clear();
    out->reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '%') {
            if (i + 2 >= in.size()) return false;
            int hi = hex_val(in[i + 1]);
            int lo = hex_val(in[i + 2]);
            if (hi < 0 || lo < 0) return false;
            char dec = static_cast<char>((hi << 4) | lo);
            if (dec == '\0') return false;
            out->push_back(dec);
            i += 2;
        } else if (c == '+') {
            out->push_back(' ');
        } else {
            out->push_back(c);
        }
    }
    return true;
}

bool parse_request(std::string_view buf, HttpRequest* out, size_t* header_len) {
    auto end = buf.find("\r\n\r\n");
    if (end == std::string_view::npos) return false;
    *header_len = end + 4;
    auto headers = buf.substr(0, end);

    auto first_eol = headers.find("\r\n");
    if (first_eol == std::string_view::npos) return false;
    auto line = headers.substr(0, first_eol);

    auto sp1 = line.find(' ');
    if (sp1 == std::string_view::npos) return false;
    auto sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos) return false;

    out->method = std::string(line.substr(0, sp1));
    out->target = std::string(line.substr(sp1 + 1, sp2 - sp1 - 1));
    out->version = std::string(line.substr(sp2 + 1));

    out->keep_alive = (out->version == "HTTP/1.1");

    size_t pos = first_eol + 2;
    while (pos < headers.size()) {
        auto eol = headers.find("\r\n", pos);
        if (eol == std::string_view::npos) eol = headers.size();
        auto h = headers.substr(pos, eol - pos);
        auto colon = h.find(':');
        if (colon != std::string_view::npos) {
            auto name = h.substr(0, colon);
            auto val = h.substr(colon + 1);
            while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.remove_prefix(1);
            while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.remove_suffix(1);
            if (ieq(name, "host")) {
                out->host = std::string(val);
            } else if (ieq(name, "connection")) {
                if (ieq(val, "close")) out->keep_alive = false;
                else if (ieq(val, "keep-alive")) out->keep_alive = true;
            }
        }
        pos = eol + 2;
    }
    return true;
}

std::string resolve_path(const std::string& root, const std::string& target) {
    // Strip query string.
    std::string path = target;
    auto q = path.find('?');
    if (q != std::string::npos) path.resize(q);

    std::string decoded;
    if (!url_decode(path, &decoded)) return {};
    if (decoded.empty() || decoded[0] != '/') return {};

    // Reject `..` segments — simpler and stricter than realpath comparisons.
    size_t i = 0;
    while (i < decoded.size()) {
        if (decoded[i] == '/') {
            if (decoded.compare(i, 4, "/../") == 0) return {};
            if (decoded.compare(i, 3, "/..") == 0 && i + 3 == decoded.size()) return {};
        }
        ++i;
    }

    std::string full = root;
    if (!full.empty() && full.back() == '/') full.pop_back();
    full += decoded;
    return full;
}

const char* mime_for(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
    if (ext == "css")  return "text/css; charset=utf-8";
    if (ext == "js")   return "application/javascript; charset=utf-8";
    if (ext == "json") return "application/json; charset=utf-8";
    if (ext == "xml")  return "application/xml; charset=utf-8";
    if (ext == "txt" || ext == "log" || ext == "md") return "text/plain; charset=utf-8";
    if (ext == "png")  return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif")  return "image/gif";
    if (ext == "svg")  return "image/svg+xml";
    if (ext == "ico")  return "image/x-icon";
    if (ext == "webp") return "image/webp";
    if (ext == "pdf")  return "application/pdf";
    if (ext == "zip")  return "application/zip";
    if (ext == "tar")  return "application/x-tar";
    if (ext == "gz")   return "application/gzip";
    if (ext == "mp3")  return "audio/mpeg";
    if (ext == "mp4")  return "video/mp4";
    if (ext == "wasm") return "application/wasm";
    return "application/octet-stream";
}

}  // namespace localshare
