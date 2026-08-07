#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace httpservice
{

// Runtime configuration for the HTTP service. All fields have defaults and
// may be overridden with designated initializers.
struct config
{
    std::string address = "0.0.0.0";
    std::uint16_t port = 9000;

    // One thread per io_context. Listener threads accept with SO_REUSEPORT;
    // connection threads handle the full lifecycle of each connection.
    std::size_t listener_threads = 4;
    std::size_t connection_threads = 8;

    // Thread-local arena for request/response buffers. arena_bytes is the
    // initial reserve per connection thread; per_request_bytes is the growth
    // chunk used when a single request exceeds the current capacity.
    std::size_t arena_bytes = 32 * 1024 * 1024;
    std::size_t per_request_bytes = 1024 * 1024;

    std::size_t session_timeout_seconds = 30;
};

// Low-contention HTTP service: one io_context per listener/connection thread,
// thread-local bump arenas, and no heap allocations on the request path.
//
// Run it on a background thread or from main; call stop() to shut down.
class service
{
public:
    explicit service(config cfg);
    ~service();

    service(service const&) = delete;
    service& operator=(service const&) = delete;

    // Starts listener/connection threads and blocks until stop() is called.
    void run();

    // Graceful shutdown: closes acceptors, cancels pending operations and
    // joins all worker threads. Safe to call from a signal handler.
    void stop();

private:
    struct impl;
    impl* impl_;
};

} // namespace httpservice
