# localshare — Implementation Plan

## Goal

A small, auditable HTTP/1.1 file server that exposes a local directory over
the LAN (or an SSH reverse tunnel) without any cloud or container
infrastructure. Demonstrates Linux event-driven I/O, thread-pool dispatch,
POSIX fd management, and MIME/chunked transfer semantics.

## Scope

- GET/HEAD only. No upload. No TLS. No HTTP/2.
- Static file serving + auto-generated directory listings.
- Linux only (uses `epoll`, `accept4`, `sendfile`, `MSG_NOSIGNAL`).

## Components

| Source           | Responsibility                                                  |
| ---------------- | --------------------------------------------------------------- |
| `thread_pool.cpp`| `std::function` worker pool with mutex/condvar queue.           |
| `http.cpp`       | Request parser, URL decoder, path resolver (with traversal guard), MIME table. |
| `server.cpp`     | Listening socket, epoll loop, per-conn state, handoff to pool.  |
| `main.cpp`       | Argv parsing, signal handlers (SIGINT/SIGTERM/SIGPIPE).         |

## Tricky parts

1. **epoll event loop only reads headers**. Workers serve the response.
   This keeps the accept/read loop non-blocking even when serving 1 GB files.
2. **Edge-triggered (`EPOLLET`)**: every readable event drains until
   `EAGAIN`, every accept loops until `EAGAIN`. Missing this is the classic
   epoll bug.
3. **Path traversal**: any `..` segment in the decoded path is rejected.
   Stricter than realpath comparisons, but unambiguous and easy to audit.
4. **`SIGPIPE` ignored, `MSG_NOSIGNAL` on `send`**: a peer that closes mid-
   transfer should fail the worker's send, not kill the process.
5. **Socket flips to blocking inside the worker** — sendfile semantics are
   simpler when the worker can rely on full sends without a write-readiness
   event from epoll.

## Out of scope

- Range requests (resumable downloads).
- HTTP basic auth.
- Per-connection rate limits.

These are doable but not part of the demo.
