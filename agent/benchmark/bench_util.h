#pragma once

#include "fmt/format.h"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
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

inline std::string fmtNs(double ns) {
  if (ns < 1000.0) {
    return fmt::format("{:.1f} ns", ns);
  } else if (ns < 1'000'000.0) {
    return fmt::format("{:.2f} us", ns / 1000.0);
  } else if (ns < 1'000'000'000.0) {
    return fmt::format("{:.2f} ms", ns / 1'000'000.0);
  } else {
    return fmt::format("{:.3f} s", ns / 1'000'000'000.0);
  }
}

inline void printResult(const BenchResult &r) {
  std::cout << "  [" << r.name << "]\n"
            << "    iterations : " << r.iterations << "\n"
            << "    total      : " << fmtNs(r.total_ns) << "\n"
            << "    mean       : " << fmtNs(r.mean_ns) << "\n"
            << "    median     : " << fmtNs(r.median_ns) << "\n"
            << "    min        : " << fmtNs(r.min_ns) << "\n"
            << "    max        : " << fmtNs(r.max_ns) << "\n"
            << "    stddev     : " << fmtNs(r.stddev_ns) << "\n";
}

class BenchReporter {
public:
  static BenchReporter &instance() {
    static BenchReporter reporter;
    return reporter;
  }

  void setOutputDir(const std::string &dir) { outputDir_ = dir; }

  const std::string &getOutputDir() const { return outputDir_; }

  void addResult(const BenchResult &r) { results_.push_back(r); }

  void flushToFile() const {
    if (outputDir_.empty()) {
      std::cerr << "[BenchReporter] output dir not set, skip writing file"
                << std::endl;
      return;
    }

    namespace fs = std::filesystem;
    fs::path dir(outputDir_);
    if (!fs::exists(dir)) {
      std::error_code ec;
      fs::create_directories(dir, ec);
      if (ec) {
        std::cerr << "[BenchReporter] failed to create dir: " << dir
                  << ", error: " << ec.message() << std::endl;
        return;
      }
    }

    auto now = std::chrono::system_clock::now();
    std::chrono::zoned_time local_time{std::chrono::current_zone(), now};
    std::string timestamp = std::format("{:%Y%m%d_%H%M%S}", local_time);

    fs::path filePath = dir / fmt::format("bench_{}.json", timestamp);

    std::ofstream ofs(filePath, std::ios::out | std::ios::trunc);
    if (!ofs.is_open()) {
      std::cerr << "[BenchReporter] failed to open file: " << filePath
                << std::endl;
      return;
    }

    ofs << "{\n";
    ofs << fmt::format("  \"timestamp\": \"{}\",\n", timestamp);
    ofs << "  \"results\": [\n";
    for (size_t i = 0; i < results_.size(); ++i) {
      const auto &r = results_[i];
      ofs << "    {\n";
      ofs << fmt::format("      \"name\": \"{}\",\n", r.name);
      ofs << fmt::format("      \"iterations\": {},\n", r.iterations);
      ofs << fmt::format("      \"total_ns\": {:.2f},\n", r.total_ns);
      ofs << fmt::format("      \"mean_ns\": {:.2f},\n", r.mean_ns);
      ofs << fmt::format("      \"median_ns\": {:.2f},\n", r.median_ns);
      ofs << fmt::format("      \"min_ns\": {:.2f},\n", r.min_ns);
      ofs << fmt::format("      \"max_ns\": {:.2f},\n", r.max_ns);
      ofs << fmt::format("      \"stddev_ns\": {:.2f},\n", r.stddev_ns);
      ofs << fmt::format("      \"total_human\": \"{}\",\n", fmtNs(r.total_ns));
      ofs << fmt::format("      \"mean_human\": \"{}\",\n", fmtNs(r.mean_ns));
      ofs << fmt::format("      \"median_human\": \"{}\",\n",
                         fmtNs(r.median_ns));
      ofs << fmt::format("      \"min_human\": \"{}\",\n", fmtNs(r.min_ns));
      ofs << fmt::format("      \"max_human\": \"{}\",\n", fmtNs(r.max_ns));
      ofs << fmt::format("      \"stddev_human\": \"{}\"\n",
                         fmtNs(r.stddev_ns));
      ofs << "    }";
      if (i + 1 < results_.size()) {
        ofs << ",";
      }
      ofs << "\n";
    }
    ofs << "  ]\n";
    ofs << "}\n";

    ofs.close();
    std::cout << "\n[BenchReporter] results written to: " << filePath
              << std::endl;
  }

private:
  BenchReporter() = default;
  std::string outputDir_;
  std::vector<BenchResult> results_;
};

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
  BenchReporter::instance().addResult(r);
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
  BenchReporter::instance().addResult(r);
  return r;
}

} // namespace bench
} // namespace agentxx