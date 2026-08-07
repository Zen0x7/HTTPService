#include <httpservice/server.hpp>

#include "allocator.hpp"
#include "arena.hpp"

#include <boost/asio/bind_allocator.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core/bind_handler.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>
#include <boost/core/ignore_unused.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(SO_REUSEPORT)
#include <sys/socket.h>
#endif

namespace httpservice
{
namespace detail
{

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

// Each io_context runs on exactly one thread, so the reactor's per-io-object
// and registration locks are disabled, and the reactor I/O object states and
// timer heap are preallocated to avoid per-connection allocations. The
// scheduler lock stays on because cross-thread posts (accept handoff, stop)
// still occur.
inline net::config_from_string
io_context_config()
{
    return net::config_from_string{
        "scheduler.concurrency_hint=1\n"
        "scheduler.locking=1\n"
        // Reactor locks stay on: async_accept registers the accepted socket
        // with the connection reactor from the listener thread, so the
        // descriptor state is touched by two threads.
        "reactor.registration_locking=1\n"
        "reactor.io_locking=1\n"
        // Preallocate the reactor/timer state so connection churn performs no
        // per-connection allocations.
        "reactor.preallocated_io_objects=16\n"
        "timer.heap_reserve=16\n"};
}

using allocator_type = arena_allocator<char>;
using body_type = http::basic_string_body<char, std::char_traits<char>, allocator_type>;
using request_parser_type = http::request_parser<body_type, allocator_type>;
using response_type = http::response<body_type, http::basic_fields<allocator_type>>;
using arena_flat_buffer = beast::basic_flat_buffer<allocator_type>;

class connection_ctx;
class session;

// Per-session pool of fixed slots for the async operation machinery (the write
// composed op + the deferral post). Mirrors the canonical Asio
// allocation/server.cpp handler_memory pattern: a slot is borrowed for the
// duration of one operation and returned on deallocate. If a request exceeds a
// slot size (or all slots are busy) the pool falls back to the heap.
class handler_memory
{
public:
    static constexpr std::size_t slot_count = 4;
    static constexpr std::size_t slot_size = 1024;

    handler_memory()
    {
        for (std::size_t i = 0; i < slot_count; ++i)
        {
            slots_[i].next = free_;
            free_ = &slots_[i];
        }
    }

    handler_memory(handler_memory const&) = delete;
    handler_memory& operator=(handler_memory const&) = delete;

    void*
    allocate(std::size_t n)
    {
        if (n > slot_size || free_ == nullptr)
        {
            return ::operator new(n);
        }
        slot* s = free_;
        free_ = s->next;
        return &s->storage;
    }

    void
    deallocate(void* p) noexcept
    {
        for (auto& s : slots_)
        {
            if (p == &s.storage)
            {
                s.next = free_;
                free_ = &s;
                return;
            }
        }
        ::operator delete(p);
    }

private:
    struct slot
    {
        alignas(std::max_align_t) std::byte storage[slot_size];
        slot* next = nullptr;
    };

    slot slots_[slot_count];
    slot* free_ = nullptr;
};

// C++11 minimal allocator for use with net::bind_allocator.
template <class T>
class handler_allocator
{
public:
    using value_type = T;

    explicit handler_allocator(handler_memory& mem) noexcept
        : memory_(mem)
    {
    }

    template <class U>
    handler_allocator(handler_allocator<U> const& other) noexcept
        : memory_(other.memory_)
    {
    }

    T*
    allocate(std::size_t n) const
    {
        return static_cast<T*>(memory_.allocate(sizeof(T) * n));
    }

    void
    deallocate(T* p, std::size_t) const noexcept
    {
        memory_.deallocate(p);
    }

    friend bool
    operator==(handler_allocator const& a, handler_allocator const& b) noexcept
    {
        return &a.memory_ == &b.memory_;
    }

    friend bool
    operator!=(handler_allocator const& a, handler_allocator const& b) noexcept
    {
        return &a.memory_ != &b.memory_;
    }

private:
    template <class>
    friend class handler_allocator;

    handler_memory& memory_;
};

// One accepted connection. Confined to a single connection thread (its socket
// was bound to that thread's strand at accept time) and deleted (delete this)
// when the connection closes. Each session owns its own arena so a per-request
// reset never clobbers another session's in-flight data.
class session
{
public:
    session(tcp::socket&& socket, connection_ctx& ctx);

    void run();
    void shutdown_socket() noexcept;

    session* next_ = nullptr;

private:
    void do_read();
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    void handle_request(bool keep_alive);
    void send_response();
    void on_write(bool keep_alive, beast::error_code ec, std::size_t bytes_transferred);
    void do_close();

    beast::tcp_stream stream_;
    connection_ctx& ctx_;
    arena arena_;
    handler_memory handler_mem_;
    arena_flat_buffer buffer_;
    std::optional<request_parser_type> parser_;
    response_type response_;
};

    // Everything owned by a connection thread: its io_context, the thread-local
    // chunk pool that backs the per-session arenas, and the intrusive list of
    // live sessions. No locks: the list is only touched on the connection
    // thread.
    class connection_ctx
    {
    public:
        // Session slots preallocated so connection churn never mallocs a session.
        static constexpr std::size_t pooled_sessions = 32;

        explicit connection_ctx(config const& cfg)
            : pool_(cfg.arena_bytes, cfg.per_request_bytes)
            , timeout_seconds_(cfg.session_timeout_seconds)
            , guard_(net::make_work_guard(ioc_))
        {
            // Preallocate the session slots so connection churn never mallocs a
            // session object. Beyond this cap new slots are grown on demand.
        free_sessions_.reserve(pooled_sessions);
        for (std::size_t i = 0; i < pooled_sessions; ++i)
        {
            free_sessions_.push_back(::operator new(sizeof(session)));
        }
    }

    ~connection_ctx()
    {
        // drain() runs before this (in the connection thread loop); free the
        // recycled session slots. Live sessions are gone by then.
        for (void* p : free_sessions_)
        {
            ::operator delete(p);
        }
    }

    chunk_pool& pool() noexcept
    {
        return pool_;
    }

    std::size_t
    timeout_seconds() const noexcept
    {
        return timeout_seconds_;
    }

    void
    add(session* s) noexcept
    {
        s->next_ = first_;
        first_ = s;
    }

    void
    remove(session* s) noexcept
    {
        if (first_ == s)
        {
            first_ = s->next_;
            return;
        }
        for (session* cur = first_; cur != nullptr && cur->next_ != nullptr; cur = cur->next_)
        {
            if (cur->next_ == s)
            {
                cur->next_ = s->next_;
                return;
            }
        }
    }

    void
    cancel_all() noexcept
    {
        for (session* s = first_; s != nullptr; s = s->next_)
        {
            s->shutdown_socket();
        }
    }

    void
    drain() noexcept
    {
        for (session* s = first_; s != nullptr;)
        {
            session* next = s->next_;
            destroy_session(s);
            s = next;
        }
        first_ = nullptr;
    }

    // Session objects come from a small recycling pool so connection churn does
    // not malloc/free a session per accept. Slots are grown on demand and
    // returned on close; only ever touched on this connection thread.
    void*
    acquire_session_memory()
    {
        if (free_sessions_.empty())
        {
            return ::operator new(sizeof(session));
        }
        void* p = free_sessions_.back();
        free_sessions_.pop_back();
        return p;
    }

    void
    release_session_memory(void* p)
    {
        free_sessions_.push_back(p);
    }

    session*
    make_session(tcp::socket&& socket)
    {
        return new (acquire_session_memory()) session(std::move(socket), *this);
    }

    void
    destroy_session(session* s)
    {
        s->~session();
        release_session_memory(s);
    }

    net::io_context ioc_{io_context_config()};
    chunk_pool pool_;
    std::size_t timeout_seconds_;
    net::executor_work_guard<net::io_context::executor_type> guard_;
    session* first_ = nullptr;
    std::vector<void*> free_sessions_;
};

// Accepts connections on one io_context with SO_REUSEPORT. Each accepted
// socket is bound to the strand of a connection thread at accept time, so the
// connection's async operations run on that thread. The accept loop itself is
// re-armed on this listener's io_context.
 class listener
{
public:
    listener(config const& cfg,
             connection_ctx** conns,
             std::size_t num_conns,
             std::atomic<std::size_t>& next_conn)
        : conns_(conns)
        , num_conns_(num_conns)
        , next_conn_(next_conn)
        , guard_(net::make_work_guard(ioc_))
    {
        beast::error_code ec;
        auto const address = net::ip::make_address(cfg.address, ec);
        if (ec)
        {
            throw std::runtime_error("invalid address: " + cfg.address);
        }

        acceptor_.open(address.is_v4() ? tcp::v4() : tcp::v6(), ec);
        if (ec)
        {
            throw std::runtime_error(ec.message());
        }

        acceptor_.set_option(net::socket_base::reuse_address(true), ec);
        if (ec)
        {
            throw std::runtime_error(ec.message());
        }

#if defined(SO_REUSEPORT)
        {
            int const reuse = 1;
            ::setsockopt(acceptor_.native_handle(), SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
        }
#endif

        acceptor_.bind(tcp::endpoint{address, cfg.port}, ec);
        if (ec)
        {
            throw std::runtime_error(ec.message());
        }

        acceptor_.listen(net::socket_base::max_listen_connections, ec);
        if (ec)
        {
            throw std::runtime_error(ec.message());
        }
    }

    void
    run()
    {
        ioc_.run();
    }

    void
    start()
    {
        do_accept();
    }

    void
    stop() noexcept
    {
        stopped_.store(true, std::memory_order_release);
        net::post(ioc_, [this] {
            beast::error_code ec;
            acceptor_.close(ec);
        });

        // Wait for the in-flight accept completions (which run on connection
        // strands) to drain so no handler touches `this` after stop() returns.
        while (pending_accepts_.load(std::memory_order_acquire) != 0)
        {
            std::this_thread::yield();
        }
        guard_.reset();
    }

private:
    void
    do_accept()
    {
        if (stopped_.load(std::memory_order_acquire))
        {
            return;
        }

        std::size_t const conn_idx = next_conn_.fetch_add(1, std::memory_order_relaxed) % num_conns_;
        connection_ctx& ctx = *conns_[conn_idx];
        pending_accepts_.fetch_add(1, std::memory_order_relaxed);

        // Bind the accepted socket to the connection thread's strand: the
        // connection keeps this executor for the whole session. The accept
        // handler and the re-arm post allocate from this listener's slot pool.
        acceptor_.async_accept(
            net::make_strand(ctx.ioc_),
            net::bind_allocator(
                handler_allocator<int>{handler_mem_},
                [this, ctx = &ctx](beast::error_code ec, tcp::socket socket) {
                    on_accept(ctx, ec, std::move(socket));
                }));
    }

    void
    on_accept(connection_ctx* ctx, beast::error_code ec, tcp::socket socket)
    {
        if (!ec)
        {
            beast::error_code noec;
            socket.set_option(tcp::no_delay(true), noec);

            // Construct the session on the connection thread: its object comes
            // from that thread's recycling pool (no heap), and the session is
            // then started there. This handoff crosses threads, so the posted
            // task itself allocates on the heap (one per connection).
            net::post(ctx->ioc_, [ctx, socket = std::move(socket)]() mutable {
                session* s = ctx->make_session(std::move(socket));
                ctx->add(s);
                s->run();
            });
        }
        else if (ec != net::error::operation_aborted)
        {
            std::fprintf(stderr, "accept: %s\n", ec.message().c_str());
        }

        pending_accepts_.fetch_sub(1, std::memory_order_relaxed);

        if (stopped_.load(std::memory_order_acquire) || ec == net::error::operation_aborted)
        {
            return;
        }

        net::post(
            ioc_,
            net::bind_allocator(
                handler_allocator<int>{handler_mem_},
                [this] { do_accept(); }));
    }

    net::io_context ioc_{io_context_config()};
    tcp::acceptor acceptor_{ioc_};
    connection_ctx** conns_;
    std::size_t num_conns_;
    std::atomic<std::size_t>& next_conn_;
    std::atomic<std::size_t> pending_accepts_{0};
    std::atomic<bool> stopped_{false};
    net::executor_work_guard<net::io_context::executor_type> guard_;
    handler_memory handler_mem_;
};

// --- session --------------------------------------------------------------

session::session(tcp::socket&& socket, connection_ctx& ctx)
    : stream_(std::move(socket))
    , ctx_(ctx)
    , arena_(ctx_.pool())
    , buffer_(allocator_type{&arena_})
{
}

void
session::run()
{
    // The accept handler runs on the listener thread; hop to the connection
    // thread's io_context so do_read runs where the session arena is active.
    net::post(
        ctx_.ioc_,
        net::bind_allocator(
            handler_allocator<int>{handler_mem_},
            beast::bind_front_handler(&session::do_read, this)));
}

void
session::do_read()
{
    current_arena = &arena_;

    // Destroy this cycle's arena-backed objects BEFORE resetting the arena so
    // their destructors (e.g. the parser's intrusive header list) walk valid
    // memory. Only this session's chunks are rewound; other sessions keep
    // theirs untouched.
    buffer_ = {};
    parser_.reset();
    arena_.reset();
    parser_.emplace();

    stream_.expires_after(std::chrono::seconds(ctx_.timeout_seconds()));

    http::async_read(
        stream_,
        buffer_,
        *parser_,
        net::bind_allocator(
            handler_allocator<int>{handler_mem_},
            beast::bind_front_handler(&session::on_read, this)));
}

void
session::on_read(beast::error_code ec, std::size_t bytes_transferred)
{
    current_arena = &arena_;
    boost::ignore_unused(bytes_transferred);

    if (ec)
    {
        do_close();
        return;
    }

    handle_request(parser_->get().keep_alive());
}

void
session::handle_request(bool keep_alive)
{
    response_ = response_type{http::status::ok, parser_->get().version()};
    response_.set(http::field::server, "httpservice");
    response_.set(http::field::content_type, "text/plain");
    response_.body() = "ok";
    response_.content_length(response_.body().size());
    response_.keep_alive(keep_alive);
    send_response();
}

void
session::send_response()
{
    bool const keep_alive = response_.keep_alive();
    http::async_write(
        stream_,
        std::move(response_),
        net::bind_allocator(
            handler_allocator<int>{handler_mem_},
            beast::bind_front_handler(&session::on_write, this, keep_alive)));
}

void
session::on_write(bool keep_alive, beast::error_code ec, std::size_t bytes_transferred)
{
    current_arena = &arena_;
    boost::ignore_unused(bytes_transferred);

    if (ec || !keep_alive)
    {
        do_close();
        return;
    }

    // Post instead of calling do_read inline: do_read resets the arena, and
    // the write op that just completed (holding the previous response) is
    // still unwinding above us. Running after this stack unwinds keeps the
    // arena reset from clobbering live objects.
    net::post(
        ctx_.ioc_,
        net::bind_allocator(
            handler_allocator<int>{handler_mem_},
            beast::bind_front_handler(&session::do_read, this)));
}

void
session::shutdown_socket() noexcept
{
    beast::error_code ec;
    stream_.socket().cancel(ec);
}

void
session::do_close()
{
    current_arena = &arena_;

    beast::error_code ec;
    stream_.socket().shutdown(tcp::socket::shutdown_send, ec);
    stream_.socket().close(ec);
    ctx_.remove(this);
    ctx_.destroy_session(this);
}

} // namespace detail
} // namespace httpservice

namespace httpservice
{

// pimpl: owns raw pointers to every listener and connection context. Sessions
// are owned by their connection thread and delete themselves on close.
struct service::impl
{
    explicit impl(config cfg_)
        : cfg(std::move(cfg_))
    {
    }

    ~impl()
    {
        for (detail::connection_ctx* c : conns)
        {
            delete c;
        }
        for (detail::listener* l : listeners)
        {
            delete l;
        }
    }

    config cfg;
    std::vector<detail::listener*> listeners;
    std::vector<detail::connection_ctx*> conns;
    std::vector<std::thread> threads;
    std::atomic<bool> stop_requested{false};
    std::atomic<std::size_t> next_conn{0};
};

service::service(config cfg)
    : impl_(new impl(std::move(cfg)))
{
    config const& c = impl_->cfg;

    impl_->conns.reserve(c.connection_threads);
    for (std::size_t i = 0; i < c.connection_threads; ++i)
    {
        impl_->conns.push_back(new detail::connection_ctx(c));
    }

    try
    {
        impl_->listeners.reserve(c.listener_threads);
        for (std::size_t i = 0; i < c.listener_threads; ++i)
        {
            impl_->listeners.push_back(
                new detail::listener(c, impl_->conns.data(), impl_->conns.size(), impl_->next_conn));
        }
    }
    catch (...)
    {
        // A second socket cannot bind the same port; impl's destructor frees
        // the connection contexts and any listeners created so far.
        delete impl_;
        impl_ = nullptr;
        throw;
    }
}

service::~service()
{
    stop();

    for (std::thread& t : impl_->threads)
    {
        if (t.joinable())
        {
            t.join();
        }
    }

    delete impl_;
}

void
service::run()
{
    for (detail::listener* l : impl_->listeners)
    {
        impl_->threads.emplace_back([l] {
            l->start();
            l->run();
        });
    }

    for (detail::connection_ctx* c : impl_->conns)
    {
        impl_->threads.emplace_back([c] {
            c->ioc_.run();
            c->drain();
        });
    }

    for (std::thread& t : impl_->threads)
    {
        t.join();
    }
}

void
service::stop()
{
    bool expected = false;
    if (!impl_->stop_requested.compare_exchange_strong(expected, true))
    {
        return;
    }

    for (detail::listener* l : impl_->listeners)
    {
        l->stop();
    }

    for (detail::connection_ctx* c : impl_->conns)
    {
        boost::asio::post(c->ioc_, [c] { c->cancel_all(); });
        c->guard_.reset();
    }
}

} // namespace httpservice
