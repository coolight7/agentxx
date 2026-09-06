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
    size_t      iterations;
    double      total_ns;
    double      mean_ns;
    double      min_ns;
    double      max_ns;
    double      stddev_ns;
    double      median_ns;
};

/// 资源占用基准测试结果 (内存、CPU、容器大小、Token 等)
struct ResourceResult {
    std::string mode;                 ///< 模式: cli | tui | split_cli | split_tui | ffi
    std::string side;                 ///< 侧: self | server | client
    std::string point;                ///< 采样时机: startup | ctx100k | ctx200k
    double      rssMB         = 0.0;  ///< 常驻物理内存 (MB)
    double      privateMB     = 0.0;  ///< 私有内存 (MB)
    double      cpuIdlePct    = -1.0; ///< 空闲期 CPU 占用百分比 (负数表示未测)
    double      cpuBusyPct    = -1.0; ///< 繁忙/生成期 CPU 占用百分比 (负数表示未测)
    size_t      viewCount     = 0;    ///< view 消息条数
    size_t      viewBytes     = 0;    ///< view 消息近似字节数
    size_t      llmCount      = 0;    ///< llm 消息条数
    size_t      llmBytes      = 0;    ///< llm 消息近似字节数
    size_t      tokens        = 0;    ///< 估算 token 数量
    size_t      pluginsAgent  = 0;    ///< agent 侧成功加载插件数
    size_t      pluginsClient = 0;    ///< client 侧成功加载插件数
    std::string note;                 ///< 备注 (如 headless, 允差, 降级说明等)
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

inline void printResult(const BenchResult& r) {
    std::cout << "  [" << r.name << "]\n"
              << "    iterations : " << r.iterations << "\n"
              << "    total      : " << fmtNs(r.total_ns) << "\n"
              << "    mean       : " << fmtNs(r.mean_ns) << "\n"
              << "    median     : " << fmtNs(r.median_ns) << "\n"
              << "    min        : " << fmtNs(r.min_ns) << "\n"
              << "    max        : " << fmtNs(r.max_ns) << "\n"
              << "    stddev     : " << fmtNs(r.stddev_ns) << "\n";
}

inline void printResourceResult(const ResourceResult& r) {
    std::cout << fmt::format(
        "  [{:<9}][{:<6}][{:<7}] rss={:>6.2f}MB priv={:>6.2f}MB cpuIdle={:>5.1f}% cpuBusy={:>5.1f}% "
        "view={}/{}KB llm={}/{}KB tok={:<6} plugins={}/{} note={}\n",
        r.mode,
        r.side,
        r.point,
        r.rssMB,
        r.privateMB,
        r.cpuIdlePct >= 0 ? r.cpuIdlePct : 0.0,
        r.cpuBusyPct >= 0 ? r.cpuBusyPct : 0.0,
        r.viewCount,
        r.viewBytes / 1024,
        r.llmCount,
        r.llmBytes / 1024,
        r.tokens,
        r.pluginsAgent,
        r.pluginsClient,
        r.note
    );
}

class BenchReporter {
public:

    static BenchReporter& instance() {
        static BenchReporter reporter;
        return reporter;
    }

    void setOutputDir(const std::string& dir) {
        outputDir_ = dir;
    }

    const std::string& getOutputDir() const {
        return outputDir_;
    }

    void addResult(const BenchResult& r) {
        results_.push_back(r);
    }

    void addResource(const ResourceResult& r) {
        resourceResults_.push_back(r);
    }

    const std::vector<ResourceResult>& getResourceResults() const {
        return resourceResults_;
    }

    void flushToFile() const {
        if (outputDir_.empty()) {
            std::cerr << "[BenchReporter] output dir not set, skip writing file" << std::endl;
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
#if XX_IS_WIN_D || defined(_LIBCPP_VERSION)
        // Windows/MinGW 与 libc++ 的 zoned_time 缺失，回退为本地 tm
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm     tm{};
#if XX_IS_WIN_D
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
        std::string timestamp(buf);
#else
        std::chrono::zoned_time local_time{std::chrono::current_zone(), now};
        std::string             timestamp = std::format("{:%Y%m%d_%H%M%S}", local_time);
#endif

        fs::path filePath = dir / fmt::format("bench_{}.json", timestamp);

        std::ofstream ofs(filePath, std::ios::out | std::ios::trunc);
        if (!ofs.is_open()) {
            std::cerr << "[BenchReporter] failed to open file: " << filePath << std::endl;
            return;
        }

        ofs << "{\n";
        ofs << fmt::format("  \"timestamp\": \"{}\",\n", timestamp);
        ofs << "  \"results\": [\n";
        for (size_t i = 0; i < results_.size(); ++i) {
            const auto& r = results_[i];
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
            ofs << fmt::format("      \"median_human\": \"{}\",\n", fmtNs(r.median_ns));
            ofs << fmt::format("      \"min_human\": \"{}\",\n", fmtNs(r.min_ns));
            ofs << fmt::format("      \"max_human\": \"{}\",\n", fmtNs(r.max_ns));
            ofs << fmt::format("      \"stddev_human\": \"{}\"\n", fmtNs(r.stddev_ns));
            ofs << "    }";
            if (i + 1 < results_.size()) {
                ofs << ",";
            }
            ofs << "\n";
        }
        ofs << "  ]";

        if (!resourceResults_.empty()) {
            ofs << ",\n  \"resource\": [\n";
            for (size_t i = 0; i < resourceResults_.size(); ++i) {
                const auto& r = resourceResults_[i];
                ofs << "    {\n";
                ofs << fmt::format("      \"mode\": \"{}\",\n", r.mode);
                ofs << fmt::format("      \"side\": \"{}\",\n", r.side);
                ofs << fmt::format("      \"point\": \"{}\",\n", r.point);
                ofs << fmt::format("      \"rssMB\": {:.2f},\n", r.rssMB);
                ofs << fmt::format("      \"privateMB\": {:.2f},\n", r.privateMB);
                ofs << fmt::format("      \"cpuIdlePct\": {:.2f},\n", r.cpuIdlePct);
                ofs << fmt::format("      \"cpuBusyPct\": {:.2f},\n", r.cpuBusyPct);
                ofs << fmt::format("      \"viewCount\": {},\n", r.viewCount);
                ofs << fmt::format("      \"viewBytes\": {},\n", r.viewBytes);
                ofs << fmt::format("      \"llmCount\": {},\n", r.llmCount);
                ofs << fmt::format("      \"llmBytes\": {},\n", r.llmBytes);
                ofs << fmt::format("      \"tokens\": {},\n", r.tokens);
                ofs << fmt::format("      \"pluginsAgent\": {},\n", r.pluginsAgent);
                ofs << fmt::format("      \"pluginsClient\": {},\n", r.pluginsClient);
                ofs << fmt::format("      \"note\": \"{}\"\n", r.note);
                ofs << "    }";
                if (i + 1 < resourceResults_.size()) {
                    ofs << ",";
                }
                ofs << "\n";
            }
            ofs << "  ]\n";
        } else {
            ofs << "\n";
        }
        ofs << "}\n";

        ofs.close();
        std::cout << "\n[BenchReporter] results written to: " << filePath << std::endl;
    }

private:

    BenchReporter() = default;
    std::string                 outputDir_;
    std::vector<BenchResult>    results_;
    std::vector<ResourceResult> resourceResults_;
};

template<typename Fn>
BenchResult runBench(const std::string& name, size_t iterations, Fn&& fn) {
    std::vector<double> durations;
    durations.reserve(iterations);

    for (size_t i = 0; i < iterations; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        fn();
        auto   end = std::chrono::high_resolution_clock::now();
        double ns  = std::chrono::duration<double, std::nano>(end - start).count();
        durations.push_back(ns);
    }

    double total_ns = 0;
    double min_ns   = durations[0];
    double max_ns   = durations[0];
    for (auto d : durations) {
        total_ns += d;
        if (d < min_ns) {
            min_ns = d;
        }
        if (d > max_ns) {
            max_ns = d;
        }
    }
    double mean_ns = total_ns / static_cast<double>(iterations);

    double variance = 0;
    for (auto d : durations) {
        double diff  = d - mean_ns;
        variance    += diff * diff;
    }
    double stddev_ns = std::sqrt(variance / static_cast<double>(iterations));

    std::sort(durations.begin(), durations.end());
    double median_ns = durations[iterations / 2];

    BenchResult r;
    r.name       = name;
    r.iterations = iterations;
    r.total_ns   = total_ns;
    r.mean_ns    = mean_ns;
    r.min_ns     = min_ns;
    r.max_ns     = max_ns;
    r.stddev_ns  = stddev_ns;
    r.median_ns  = median_ns;
    BenchReporter::instance().addResult(r);
    return r;
}

template<typename SetupFn, typename Fn>
BenchResult
    runBenchWithSetup(const std::string& name, size_t iterations, SetupFn&& setup, Fn&& fn) {
    std::vector<double> durations;
    durations.reserve(iterations);

    for (size_t i = 0; i < iterations; ++i) {
        setup();
        auto start = std::chrono::high_resolution_clock::now();
        fn();
        auto   end = std::chrono::high_resolution_clock::now();
        double ns  = std::chrono::duration<double, std::nano>(end - start).count();
        durations.push_back(ns);
    }

    double total_ns = 0;
    double min_ns   = durations[0];
    double max_ns   = durations[0];
    for (auto d : durations) {
        total_ns += d;
        if (d < min_ns) {
            min_ns = d;
        }
        if (d > max_ns) {
            max_ns = d;
        }
    }
    double mean_ns = total_ns / static_cast<double>(iterations);

    double variance = 0;
    for (auto d : durations) {
        double diff  = d - mean_ns;
        variance    += diff * diff;
    }
    double stddev_ns = std::sqrt(variance / static_cast<double>(iterations));

    std::sort(durations.begin(), durations.end());
    double median_ns = durations[iterations / 2];

    BenchResult r;
    r.name       = name;
    r.iterations = iterations;
    r.total_ns   = total_ns;
    r.mean_ns    = mean_ns;
    r.min_ns     = min_ns;
    r.max_ns     = max_ns;
    r.stddev_ns  = stddev_ns;
    r.median_ns  = median_ns;
    BenchReporter::instance().addResult(r);
    return r;
}

} // namespace bench
} // namespace agentxx