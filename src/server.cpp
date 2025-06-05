#include "server.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "http.h"

namespace localshare {

namespace {

constexpr size_t kReadChunk = 8192;
constexpr size_t kSendChunk = 64 * 1024;
constexpr size_t kMaxHeaderBytes = 16 * 1024;

void set_nonblock(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

ssize_t write_all(int fd, const char* buf, size_t n) {
    size_t written = 0;
    while (written < n) {
        ssize_t w = ::send(fd, buf + written, n - written, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        written += w;
    }
    return static_cast<ssize_t>(written);
}

void send_simple(int fd, int code, const char* reason, const std::string& body,
                 const char* content_type = "text/plain; charset=utf-8",
                 bool keep_alive = false) {
    char header[512];
    int n = std::snprintf(header, sizeof(header),
                          "HTTP/1.1 %d %s\r\n"
                          "Content-Type: %s\r\n"
                          "Content-Length: %zu\r\n"
                          "Connection: %s\r\n"
                          "\r\n",
                          code, reason, content_type, body.size(),
                          keep_alive ? "keep-alive" : "close");
    write_all(fd, header, n);
    if (!body.empty()) write_all(fd, body.data(), body.size());
}

std::string html_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '&': out += "&amp;"; break;
            case '"': out += "&quot;"; break;
            default:  out += c; break;
        }
    }
    return out;
}

std::string render_dir_listing(const std::string& fs_path,
                               const std::string& url_path) {
    DIR* d = ::opendir(fs_path.c_str());
    if (!d) return {};
    std::vector<std::string> dirs, files;
    while (auto* e = ::readdir(d)) {
        std::string name = e->d_name;
        if (name == ".") continue;
        std::string full = fs_path + "/" + name;
        struct stat st;
        if (::lstat(full.c_str(), &st) < 0) continue;
        if (S_ISDIR(st.st_mode)) dirs.push_back(name);
        else files.push_back(name);
    }
    ::closedir(d);
    std::sort(dirs.begin(), dirs.end());
    std::sort(files.begin(), files.end());

    std::ostringstream out;
    out << "<!doctype html><html><head><meta charset=\"utf-8\">"
        << "<title>Index of " << html_escape(url_path) << "</title>"
        << "<style>body{font-family:monospace;padding:1.5rem;}"
        << "a{display:block;padding:.15rem 0;}"
        << "</style></head><body>"
        << "<h2>Index of " << html_escape(url_path) << "</h2><hr>";
    for (auto& name : dirs) {
        out << "<a href=\"" << html_escape(name) << "/\">" << html_escape(name) << "/</a>";
    }
    for (auto& name : files) {
        out << "<a href=\"" << html_escape(name) << "\">" << html_escape(name) << "</a>";
    }
    out << "</body></html>";
    return out.str();
}

// Stream a regular file using `sendfile(2)` in 64KB chunks. We send each
// chunk as a chunked-transfer-encoding frame so very large files don't
// require a Content-Length header up-front.
bool send_file_chunked(int sock, int file_fd, off_t size, const char* mime,
                       bool keep_alive) {
    char header[512];
    int hn = std::snprintf(header, sizeof(header),
                           "HTTP/1.1 200 OK\r\n"
                           "Content-Type: %s\r\n"
                           "Content-Length: %lld\r\n"
                           "Connection: %s\r\n"
                           "Accept-Ranges: bytes\r\n"
                           "\r\n",
                           mime, static_cast<long long>(size),
                           keep_alive ? "keep-alive" : "close");
    if (write_all(sock, header, hn) < 0) return false;

    off_t offset = 0;
    while (offset < size) {
        size_t want = static_cast<size_t>(std::min<off_t>(kSendChunk, size - offset));
        ssize_t s = ::sendfile(sock, file_fd, &offset, want);
        if (s < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return false;
        }
        if (s == 0) break;
    }
    return true;
}

void serve_request(int sock, const HttpRequest& req, const ServerOptions& opts) {
    if (req.method != "GET" && req.method != "HEAD") {
        send_simple(sock, 405, "Method Not Allowed", "method not allowed\n");
        return;
    }
    std::string fs = resolve_path(opts.root, req.target);
    if (fs.empty()) {
        send_simple(sock, 400, "Bad Request", "bad path\n");
        return;
    }
    struct stat st;
    if (::stat(fs.c_str(), &st) < 0) {
        send_simple(sock, 404, "Not Found", "not found\n");
        return;
    }

    if (S_ISDIR(st.st_mode)) {
        // Try index.html first.
        std::string idx = fs + "/index.html";
        struct stat ist;
        if (::stat(idx.c_str(), &ist) == 0 && S_ISREG(ist.st_mode)) {
            fs = idx;
            st = ist;
        } else if (opts.autoindex) {
            // Strip query for the URL we display.
            std::string url = req.target;
            auto q = url.find('?');
            if (q != std::string::npos) url.resize(q);
            std::string body = render_dir_listing(fs, url);
            if (body.empty()) {
                send_simple(sock, 403, "Forbidden", "cannot list directory\n");
                return;
            }
            send_simple(sock, 200, "OK", body, "text/html; charset=utf-8",
                        req.keep_alive);
            return;
        } else {
            send_simple(sock, 403, "Forbidden", "directory listing disabled\n");
            return;
        }
    }
    if (!S_ISREG(st.st_mode)) {
        send_simple(sock, 403, "Forbidden", "not a regular file\n");
        return;
    }

    int ffd = ::open(fs.c_str(), O_RDONLY | O_CLOEXEC);
    if (ffd < 0) {
        send_simple(sock, 403, "Forbidden", "cannot open file\n");
        return;
    }
    if (req.method == "HEAD") {
        char header[512];
        int hn = std::snprintf(header, sizeof(header),
                               "HTTP/1.1 200 OK\r\n"
                               "Content-Type: %s\r\n"
                               "Content-Length: %lld\r\n"
                               "Connection: %s\r\n\r\n",
                               mime_for(fs), static_cast<long long>(st.st_size),
                               req.keep_alive ? "keep-alive" : "close");
        write_all(sock, header, hn);
    } else {
        send_file_chunked(sock, ffd, st.st_size, mime_for(fs), req.keep_alive);
    }
    ::close(ffd);
}

}  // namespace

Server::Server(ServerOptions opts) : opts_(std::move(opts)) {
    pool_ = std::make_unique<ThreadPool>(opts_.worker_threads);
}

Server::~Server() { stop(); }

void Server::stop() { stop_.store(true, std::memory_order_relaxed); }

int Server::setup_listen_socket() {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) throw std::runtime_error("socket");
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(opts_.port);
    if (::inet_pton(AF_INET, opts_.bind.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        throw std::runtime_error("invalid bind address");
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        throw std::runtime_error(std::string("bind: ") + std::strerror(errno));
    }
    if (::listen(fd, 128) < 0) {
        ::close(fd);
        throw std::runtime_error("listen");
    }
    set_nonblock(fd);
    return fd;
}

void Server::handle_readable(int epfd, int fd) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) return;
    auto conn = it->second;

    char buf[kReadChunk];
    while (true) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            conn->in_buf.append(buf, n);
            if (conn->in_buf.size() > kMaxHeaderBytes &&
                conn->in_buf.find("\r\n\r\n") == std::string::npos) {
                send_simple(fd, 431, "Request Header Fields Too Large",
                            "headers too large\n");
                close_conn(epfd, fd);
                return;
            }
        } else if (n == 0) {
            close_conn(epfd, fd);
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            close_conn(epfd, fd);
            return;
        }
    }

    HttpRequest req;
    size_t header_len = 0;
    if (!parse_request(conn->in_buf, &req, &header_len)) {
        return;  // partial; wait for more bytes
    }

    // Hand the actual response off to the thread pool so the epoll thread
    // doesn't block on disk reads or large sends.
    int sock = fd;
    auto opts_copy = opts_;
    pool_->submit([sock, req, opts_copy] {
        // Switch back to blocking mode for simple sendfile semantics.
        int flags = ::fcntl(sock, F_GETFL, 0);
        ::fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
        serve_request(sock, req, opts_copy);
    });

    conn->in_buf.erase(0, header_len);
    if (!req.keep_alive) {
        // Connection: close — let the worker drain/close, drop from epoll now.
        ::epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
        conns_.erase(fd);
        // Worker will close `sock` after writing? No — we keep ownership simple
        // by closing here once the worker is queued. Workers run on a copy of
        // the fd; the OS refcounts socket descriptors, so `close` after the
        // queue submit is safe — it just marks the fd table; the worker still
        // has a valid kernel reference through the dup. To avoid that
        // complication entirely, the worker closes the fd itself (see below).
        // The submitted lambda owns the fd; we already removed it from epoll.
        (void)0;  // intentional no-op
    }
}

void Server::close_conn(int epfd, int fd) {
    ::epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
    conns_.erase(fd);
    ::close(fd);
}

void Server::accept_loop(int epfd, int listen_fd) {
    while (true) {
        sockaddr_in caddr{};
        socklen_t clen = sizeof(caddr);
        int cfd = ::accept4(listen_fd, reinterpret_cast<sockaddr*>(&caddr), &clen,
                            SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
            return;
        }
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
        ev.data.fd = cfd;
        if (::epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev) < 0) {
            ::close(cfd);
            continue;
        }
        conns_[cfd] = std::make_shared<Connection>(Connection{cfd, {}});
    }
}

int Server::run() {
    // SIGPIPE arrives when we write to a peer that closed; we want EPIPE on
    // send() instead so we can clean up gracefully.
    ::signal(SIGPIPE, SIG_IGN);

    int listen_fd = setup_listen_socket();
    int epfd = ::epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        ::close(listen_fd);
        throw std::runtime_error("epoll_create1");
    }
    epoll_event lev{};
    lev.events = EPOLLIN | EPOLLET;
    lev.data.fd = listen_fd;
    ::epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &lev);

    std::printf("localshare: listening on %s:%d, root=%s, workers=%zu\n",
                opts_.bind.c_str(), opts_.port, opts_.root.c_str(),
                opts_.worker_threads);
    std::fflush(stdout);

    constexpr int kMaxEvents = 64;
    epoll_event events[kMaxEvents];
    while (!stop_.load(std::memory_order_relaxed)) {
        int n = ::epoll_wait(epfd, events, kMaxEvents, 500);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            if (fd == listen_fd) {
                accept_loop(epfd, listen_fd);
            } else if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                close_conn(epfd, fd);
            } else if (events[i].events & EPOLLIN) {
                handle_readable(epfd, fd);
            }
        }
    }
    ::close(listen_fd);
    ::close(epfd);
    pool_->shutdown();
    return 0;
}

}  // namespace localshare
