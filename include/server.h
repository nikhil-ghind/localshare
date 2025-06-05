#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>

#include "thread_pool.h"

namespace localshare {

struct Connection {
    int fd;
    std::string in_buf;  // accumulated bytes from epoll edge-triggered reads
};

struct ServerOptions {
    std::string root;
    std::string bind = "0.0.0.0";
    int port = 8080;
    size_t worker_threads = 4;
    bool autoindex = true;  // generate HTML directory listing
};

class Server {
public:
    explicit Server(ServerOptions opts);
    ~Server();

    int run();
    void stop();

private:
    int setup_listen_socket();
    void accept_loop(int epfd, int listen_fd);
    void handle_readable(int epfd, int fd);
    void close_conn(int epfd, int fd);

    ServerOptions opts_;
    std::unique_ptr<ThreadPool> pool_;
    std::unordered_map<int, std::shared_ptr<Connection>> conns_;
    std::atomic<bool> stop_{false};
};

}  // namespace localshare
