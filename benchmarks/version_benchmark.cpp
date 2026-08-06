#include <benchmark/benchmark.h>

#include <httpservice/version.hpp>

static void
BM_GetVersion(benchmark::State &state)
{
  for (auto _ : state)
  {
    benchmark::DoNotOptimize(httpservice::get_version());
  }
}
BENCHMARK(BM_GetVersion);

BENCHMARK_MAIN();
