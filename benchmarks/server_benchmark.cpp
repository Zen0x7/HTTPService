#include <benchmark/benchmark.h>

#include <httpservice/server.hpp>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <cstddef>
#include <exception>
#include <string>
#include <thread>

namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace
{

// Connects, sends a request with Connection: close and reads until the server
// closes. Exercises the full accept -> session -> response -> shutdown path.
static void
BM_RequestRoundTrip(benchmark::State& state)
{
  std::string const req = "GET / HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Connection: close\r\n"
                          "\r\n";

  net::io_context ioc;
  for (auto _ : state)
  {
    tcp::socket sock(ioc);
    sock.open(tcp::v4());
    sock.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), 19000));
    net::write(sock, net::buffer(req));

    std::string resp;
    char buf[4096];
    boost::system::error_code ec;
    while (!ec)
    {
      std::size_t n = sock.read_some(net::buffer(buf), ec);
      resp.append(buf, n);
    }
    benchmark::DoNotOptimize(resp);
    sock.close();
  }
}
BENCHMARK(BM_RequestRoundTrip)->Threads(2)->Unit(benchmark::kMicrosecond);

} // namespace

int
main(int argc, char** argv)
{
  httpservice::config c;
  c.address = "127.0.0.1";
  c.port = 19000;
  c.listener_threads = 1;
  c.connection_threads = 2;
  c.arena_bytes = 1024 * 1024;
  c.per_request_bytes = 256 * 1024;

  static httpservice::service svc(c);
  std::thread server_thread([&] { svc.run(); });

  for (;;)
  {
    try
    {
      net::io_context ioc;
      tcp::socket probe(ioc);
      probe.open(tcp::v4());
      probe.connect(tcp::endpoint(net::ip::make_address(c.address), c.port));
      probe.close();
      break;
    }
    catch (std::exception const&)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();

  svc.stop();
  server_thread.join();
  return 0;
}
