#pragma once

#include "fmt/format.h"
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

namespace agentxx {
namespace bench {

struct BenchResult {
  std::string name;
  size_t iterations;
  double total_ns;
  double mean_ns;
  double min_ns;
  double max_ns;
  double stddev_ns;
  double median_ns;
};

inline void printResult(const BenchResult &r) {
  auto fmtNs = [](double ns) -> std::string {
    if (ns < 1000.0) {
      return fmt::format("{:.1f} ns", ns);
    } else if (ns < 1'000'000.0) {
      return fmt::format("{:.2f} us", ns / 1000.0);
    } else if (ns < 1'000'000'000.0) {
      return fmt::format("{:.2f} ms", ns / 1'000'000.0);
    } else {
      return fmt::format("{:.3f} s", ns / 1'000'000'000.0);
    }
  };

  std::cout << "  [" << r.name << "]\n"
            << "    iterations : " << r.iterations << "\n"
            << "    total      : " << fmtNs(r.total_ns) << "\n"
            << "    mean       : " << fmtNs(r.mean_ns) << "\n"
            << "    median     : " << fmtNs(r.median_ns) << "\n"
            << "    min        : " << fmtNs(r.min_ns) << "\n"
            << "    max        : " << fmtNs(r.max_ns) << "\n"
            << "    stddev     : " << fmtNs(r.stddev_ns) << "\n";
}

template <typename Fn>
BenchResult runBench(const std::string &name, size_t iterations, Fn &&fn) {
  std::vector<double> durations;
  durations.reserve(iterations);

  for (size_t i = 0; i < iterations; ++i) {
    auto start = std::chrono::high_resolution_clock::now();
    fn();
    auto end = std::chrono::high_resolution_clock::now();
    double ns = std::chrono::duration<double, std::nano>(end - start).count();
    durations.push_back(ns);
  }

  double total_ns = 0;
  double min_ns = durations[0];
  double max_ns = durations[0];
  for (auto d : durations) {
    total_ns += d;
    if (d < min_ns)
      min_ns = d;
    if (d > max_ns)
      max_ns = d;
  }
  double mean_ns = total_ns / static_cast<double>(iterations);

  double variance = 0;
  for (auto d : durations) {
    double diff = d - mean_ns;
    variance += diff * diff;
  }
  double stddev_ns = std::sqrt(variance / static_cast<double>(iterations));

  std::sort(durations.begin(), durations.end());
  double median_ns = durations[iterations / 2];

  BenchResult r;
  r.name = name;
  r.iterations = iterations;
  r.total_ns = total_ns;
  r.mean_ns = mean_ns;
  r.min_ns = min_ns;
  r.max_ns = max_ns;
  r.stddev_ns = stddev_ns;
  r.median_ns = median_ns;
  return r;
}

template <typename SetupFn, typename Fn>
BenchResult runBenchWithSetup(const std::string &name, size_t iterations,
                              SetupFn &&setup, Fn &&fn) {
  std::vector<double> durations;
  durations.reserve(iterations);

  for (size_t i = 0; i < iterations; ++i) {
    setup();
    auto start = std::chrono::high_resolution_clock::now();
    fn();
    auto end = std::chrono::high_resolution_clock::now();
    double ns = std::chrono::duration<double, std::nano>(end - start).count();
    durations.push_back(ns);
  }

  double total_ns = 0;
  double min_ns = durations[0];
  double max_ns = durations[0];
  for (auto d : durations) {
    total_ns += d;
    if (d < min_ns)
      min_ns = d;
    if (d > max_ns)
      max_ns = d;
  }
  double mean_ns = total_ns / static_cast<double>(iterations);

  double variance = 0;
  for (auto d : durations) {
    double diff = d - mean_ns;
    variance += diff * diff;
  }
  double stddev_ns = std::sqrt(variance / static_cast<double>(iterations));

  std::sort(durations.begin(), durations.end());
  double median_ns = durations[iterations / 2];

  BenchResult r;
  r.name = name;
  r.iterations = iterations;
  r.total_ns = total_ns;
  r.mean_ns = mean_ns;
  r.min_ns = min_ns;
  r.max_ns = max_ns;
  r.stddev_ns = stddev_ns;
  r.median_ns = median_ns;
  return r;
}

} // namespace bench
} // namespace agentxx