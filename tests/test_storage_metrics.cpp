#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>
#include "observability/storage_metrics.h"

int main() {
  using namespace TLSS::OBSERVABILITY;
  StorageMetrics metrics;
  if (metrics.get(Metric::connections) != 0)
    throw std::runtime_error("initial gauge");
  metrics.subtract(Metric::connections);
  if (metrics.get(Metric::connections) != 0)
    throw std::runtime_error("gauge underflow");
  std::vector<std::thread> writers;
  for (int n = 0; n < 4; ++n)
    writers.emplace_back([&] {
      for (int i = 0; i < 10000; ++i)
        metrics.add(Metric::upload_committed);
    });
  for (auto& writer : writers)
    writer.join();
  if (metrics.get(Metric::upload_committed) != 40000)
    throw std::runtime_error("lost increment");
  metrics.set(Metric::connections, std::numeric_limits<std::uint64_t>::max());
  metrics.add(Metric::connections);
  if (metrics.get(Metric::connections) != std::numeric_limits<std::uint64_t>::max())
    throw std::runtime_error("counter wrap");
  if (metrics.snapshot()[static_cast<std::size_t>(Metric::upload_committed)] != 40000)
    throw std::runtime_error("snapshot");
  std::cout << "PASS metrics concurrency, saturation and gauges\n";
}
