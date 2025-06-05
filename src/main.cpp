#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "server.h"

namespace {

std::atomic<localshare::Server*> g_server{nullptr};

void on_sigint(int) {
    if (auto* s = g_server.load()) s->stop();
}

void usage(const char* prog) {
    std::fprintf(stderr,
        "usage: %s [options] [root-dir]\n"
        "  -p, --port N         port to bind (default 8080)\n"
        "  -b, --bind ADDR      bind address (default 0.0.0.0)\n"
        "  -w, --workers N      worker threads (default 4)\n"
        "      --no-autoindex   disable directory listings\n"
        "  -h, --help           print this help\n",
        prog);
}

}  // namespace

int main(int argc, char** argv) {
    localshare::ServerOptions opts;
    opts.root = ".";

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else if (a == "-p" || a == "--port") {
            if (auto* v = next("--port")) opts.port = std::atoi(v); else return 2;
        } else if (a == "-b" || a == "--bind") {
            if (auto* v = next("--bind")) opts.bind = v; else return 2;
        } else if (a == "-w" || a == "--workers") {
            if (auto* v = next("--workers")) opts.worker_threads = std::atoi(v);
            else return 2;
        } else if (a == "--no-autoindex") {
            opts.autoindex = false;
        } else if (a.size() > 0 && a[0] != '-') {
            opts.root = a;
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            usage(argv[0]);
            return 2;
        }
    }
    if (opts.worker_threads == 0) opts.worker_threads = 1;

    localshare::Server srv(std::move(opts));
    g_server.store(&srv);

    struct sigaction sa{};
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);

    try {
        return srv.run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
}
