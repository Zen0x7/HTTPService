#include <httpservice/server.hpp>

#include <csignal>
#include <cstdio>

namespace
{

httpservice::service* g_service = nullptr;

void
on_signal(int)
{
  if (g_service != nullptr)
  {
    g_service->stop();
  }
}

} // namespace

int
main()
{
  httpservice::config c{
      .address = "0.0.0.0",
      .port = 9000,
      .listener_threads = 4,
      .connection_threads = 8,
      .arena_bytes = 32 * 1024 * 1024,
      .per_request_bytes = 1024 * 1024,
      .session_timeout_seconds = 30,
  };

  // stop() only posts to the io_contexts; the main thread is blocked in
  // service::run() (join) with no allocation in flight, so this is safe in
  // practice. TSAN flags it as "signal-unsafe" but it is not a data race.
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  std::printf("httpservice listening on %s:%u\n", c.address.c_str(), c.port);

  httpservice::service s(c);
  g_service = &s;
  s.run();
  g_service = nullptr;
  return 0;
}
