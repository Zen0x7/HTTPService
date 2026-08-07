#include <gtest/gtest.h>

#include <httpservice/server.hpp>

#include "../src/allocator.hpp"
#include "../src/arena.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <string>
#include <thread>

namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace
{

// Reads until `delim` appears or the socket errors out, bounded by `timeout`.
// On timeout the socket is closed to unblock the reader thread.
std::string
read_until(tcp::socket& sock, std::string const& delim, std::chrono::milliseconds timeout)
{
  std::future<std::string> fut = std::async(std::launch::async, [&] {
    std::string buf;
    char tmp[4096];
    while (buf.find(delim) == std::string::npos)
    {
      boost::system::error_code ec;
      std::size_t n = sock.read_some(net::buffer(tmp), ec);
      if (ec)
      {
        break;
      }
      buf.append(tmp, n);
    }
    return buf;
  });

  if (fut.wait_for(timeout) == std::future_status::timeout)
  {
    boost::system::error_code ec;
    sock.close(ec);
    return fut.get();
  }
  return fut.get();
}

} // namespace

TEST(Server, ConfigDefaults)
{
  httpservice::config c;
  EXPECT_EQ(c.address, "0.0.0.0");
  EXPECT_EQ(c.port, 9000);
  EXPECT_EQ(c.listener_threads, 4u);
  EXPECT_EQ(c.connection_threads, 8u);
  EXPECT_EQ(c.arena_bytes, 32u * 1024 * 1024);
  EXPECT_EQ(c.per_request_bytes, 1024u * 1024);
  EXPECT_EQ(c.session_timeout_seconds, 30u);
}

TEST(Arena, AllocatesAndReusesAfterReset)
{
  httpservice::detail::chunk_pool pool(4096, 1024);
  httpservice::detail::arena a(pool);
  void* p1 = a.allocate(64, alignof(std::max_align_t));
  ASSERT_NE(p1, nullptr);
  void* p2 = a.allocate(64, alignof(std::max_align_t));
  ASSERT_NE(p2, nullptr);
  EXPECT_NE(p1, p2);

  a.reset();
  void* p3 = a.allocate(64, alignof(std::max_align_t));
  ASSERT_NE(p3, nullptr);
  EXPECT_EQ(p1, p3);
}

TEST(Arena, AutoGrowsPastInitialReserve)
{
  httpservice::detail::chunk_pool pool(1024, 512);
  httpservice::detail::arena a(pool);
  void* big = a.allocate(2048, 16);
  ASSERT_NE(big, nullptr);
  void* small = a.allocate(128, 16);
  ASSERT_NE(small, nullptr);
  EXPECT_GT(a.capacity(), 1024u);
}

TEST(Arena, ChunksAreReusedAcrossArenas)
{
  httpservice::detail::chunk_pool pool(1024, 512);
  std::byte* first_chunk = nullptr;
  {
    httpservice::detail::arena a(pool);
    first_chunk = static_cast<std::byte*>(a.allocate(64, 16));
  }
  {
    httpservice::detail::arena b(pool);
    void* p = b.allocate(64, 16);
    EXPECT_EQ(first_chunk, p);
  }
}

TEST(ArenaAllocator, BindsToCurrentThreadArena)
{
  httpservice::detail::chunk_pool pool(1024, 512);
  httpservice::detail::arena a(pool);
  httpservice::detail::current_arena = &a;

  httpservice::detail::arena_allocator<char> alloc;
  char* p = alloc.allocate(100);
  ASSERT_NE(p, nullptr);
  alloc.deallocate(p, 100);

  httpservice::detail::current_arena = nullptr;
}

#ifndef HTTPSERVICE_DISABLE_NETWORK_TESTS

TEST(Server, RespondsOkAndHonorsKeepAlive)
{
  httpservice::config c;
  c.address = "127.0.0.1";
  c.port = 19877;
  c.listener_threads = 1;
  c.connection_threads = 2;
  c.arena_bytes = 1024 * 1024;
  c.per_request_bytes = 64 * 1024;

  httpservice::service s(c);
  std::thread server_thread([&] { s.run(); });

  bool ready = false;
  auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline)
  {
    try
    {
      net::io_context ioc;
      tcp::socket probe(ioc);
      probe.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), c.port));
      probe.close();
      ready = true;
      break;
    }
    catch (...)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  ASSERT_TRUE(ready) << "server did not become ready on port " << c.port;

  {
    net::io_context ioc;
    tcp::socket sock(ioc);
    sock.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), c.port));

    std::string const req = "GET / HTTP/1.1\r\n"
                            "Host: localhost\r\n"
                            "Connection: keep-alive\r\n"
                            "\r\n";

    net::write(sock, net::buffer(req));
    std::string const resp1 = read_until(sock, "\r\n\r\n", std::chrono::seconds(5));
    EXPECT_NE(resp1.find("200 OK"), std::string::npos) << resp1;

    net::write(sock, net::buffer(req));
    std::string const resp2 = read_until(sock, "\r\n\r\n", std::chrono::seconds(5));
    EXPECT_NE(resp2.find("200 OK"), std::string::npos) << resp2;

    sock.close();
  }

  s.stop();
  server_thread.join();
}

#endif
