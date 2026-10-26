// 单例宏 vs 实例宏 的单条开销差值（同进程交替跑、取比值，抵消 CPU 频率漂移）
// README「单例开销：已归零」一节 0.949 比值数据的来源。
#include "Logger.h"
#include "affinity.h"

#include <spdlog/sinks/null_sink.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using clk = std::chrono::steady_clock;
constexpr int kIters = 1000000;
constexpr int kRounds = 9;

namespace
{
    mylog::LogConfig null_cfg(const char *name)
    {
        mylog::LogConfig cfg;
        cfg.logger_name = name;
        cfg.enable_console = false;
        cfg.enable_file = false;
        cfg.rotation = mylog::LogConfig::RotationPolicy::None;
        cfg.level = mylog::Level::Trace;
        cfg.flush_level = mylog::Level::Off;
        cfg.pattern = "%v";
        mylog::SinkConfig sink;
        sink.factory = [] { return std::make_unique<spdlog::sinks::null_sink_mt>(); };
        cfg.extra_sinks.push_back(sink);
        return cfg;
    }

    template <typename F>
    double time_ns(F &&f)
    {
        const auto t0 = clk::now();
        f();
        return std::chrono::duration<double, std::nano>(clk::now() - t0).count() / kIters;
    }
} // namespace

int main()
{
    bench::pin_process(0x4); // 钉在 CPU2

    mylog::SingletonLogger::init(null_cfg("gap_singleton"));
    mylog::Logger inst(null_cfg("gap_instance"));

    mylog::Logger *ptr = mylog::SingletonLogger::get();
    if (!ptr)
    {
        std::printf("singleton init failed\n");
        return 1;
    }

    std::vector<double> values; // LOGS_* 相对 LOG_INFO 的比值
    double best_s = 1e300, best_i = 1e300, best_r = 1e300;
    std::shared_ptr<spdlog::logger> raw = mylog::SingletonLogger::get()->native();
    if (!raw)
    {
        std::printf("native() returned null\n");
        return 1;
    }

    for (int r = 0; r < kRounds; ++r)
    {
        // 交替：单例宏 -> 实例宏 -> 直接 spdlog，排掉顺序偏差
        const double s = time_ns([&]
                                 {
                                     for (int i = 0; i < kIters; ++i)
                                         LOGS_INFO("tick {}", i);
                                 });
        const double i = time_ns([&]
                                 {
                                     for (int k = 0; k < kIters; ++k)
                                         LOG_INFO(*ptr, "tick {}", k);
                                 });
        // 直接调 spdlog（不经过 mylog 封装）：相当于“原生”基准
        const double rr = time_ns([&]
                                  {
                                      for (int k = 0; k < kIters; ++k)
                                          raw->info("tick {}", k);
                                  });
        values.push_back(s / i);
        best_s = std::min(best_s, s);
        best_i = std::min(best_i, i);
        best_r = std::min(best_r, rr);
        std::printf("round %d: LOGS %.1f  instance %.1f  raw-spdlog %.1f  (LOGS/instance %.3f)\n",
                    r + 1, s, i, rr, s / i);
    }

    std::sort(values.begin(), values.end());
    const double median = values[values.size() / 2];
    std::printf("\nmedian ratio (LOGS_* / LOG_INFO): %.3f\n", median);
    std::printf("best:  LOGS %.1f  instance %.1f  raw-spdlog %.1f\n", best_s, best_i, best_r);
    std::printf("singleton overhead = %+.1f ns;  wrapper overhead vs raw = %+.1f ns\n",
                best_s - best_i, best_i - best_r);

    mylog::SingletonLogger::shutdown();
    inst.shutdown();
    return 0;
}
