// 线程扩展性测试：判断单例快照的"引用计数竞争"是不是瓶颈
// 变体：singleton（每条取快照） / hoisted（线程内取一次） / instance（实例日志器）
// 三者下游完全相同（同一个 null sink 类型），差值只反映快照路径。
#include "Logger.h"
#include "affinity.h"

#include <spdlog/sinks/null_sink.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using clk = std::chrono::steady_clock;

namespace
{
    constexpr int kRounds = 3;
    constexpr std::size_t kPerThread = 50000;
    const int kThreadCounts[] = {1, 2, 4, 8};

    double run_threads(int threads, const std::function<void()> &body)
    {
        std::atomic<bool> start{false};
        std::atomic<int> ready{0};
        std::vector<std::thread> pool;
        for (int t = 0; t < threads; ++t)
            pool.emplace_back([&]
                              {
                                  ready.fetch_add(1);
                                  while (!start.load(std::memory_order_acquire))
                                      std::this_thread::yield();
                                  body();
                              });
        while (ready.load() < threads)
            std::this_thread::yield();
        const auto t0 = clk::now();
        start.store(true, std::memory_order_release);
        for (auto &th : pool)
            th.join();
        const double total = std::chrono::duration<double, std::nano>(clk::now() - t0).count();
        return total / double(threads * kPerThread);
    }

    double best(int threads, const std::function<void()> &body)
    {
        double b = 1e300;
        for (int i = 0; i < kRounds; ++i)
            b = std::min(b, run_threads(threads, body));
        return b;
    }

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
} // namespace

int main()
{
    bench::pin_process(0x3FC); // CPU2..9

    mylog::SingletonLogger::init(null_cfg("sc_single"));
    mylog::Logger inst(null_cfg("sc_inst"));

    std::printf("%-26s", "variant (ns/msg)");
    for (int t : kThreadCounts)
        std::printf(" %8dT", t);
    std::printf("   %8s\n", "8T/1T");
    std::printf("%s\n", "--------------------------------------------------------------");

    double s1 = 0, s8 = 0, h1 = 0, h8 = 0, i1 = 0, i8 = 0;

    std::printf("%-26s", "singleton (snapshot per call)");
    for (int t : kThreadCounts)
    {
        const double v = best(t, [&]
                              {
                                  for (std::size_t i = 0; i < kPerThread; ++i)
                                      LOGS_INFO("m {}", i);
                              });
        std::printf(" %8.1f", v);
        if (t == 1) s1 = v;
        if (t == 8) s8 = v;
    }
    std::printf("   %+7.1f%%\n", (s8 / s1 - 1.0) * 100.0);

    std::printf("%-26s", "hoisted (get once)");
    for (int t : kThreadCounts)
    {
        const double v = best(t, [&]
                              {
                                  auto lg = mylog::SingletonLogger::get();
                                  if (!lg)
                                      return;
                                  for (std::size_t i = 0; i < kPerThread; ++i)
                                      LOG_INFO(*lg, "m {}", i);
                              });
        std::printf(" %8.1f", v);
        if (t == 1) h1 = v;
        if (t == 8) h8 = v;
    }
    std::printf("   %+7.1f%%\n", (h8 / h1 - 1.0) * 100.0);

    std::printf("%-26s", "instance (LOG_INFO)");
    for (int t : kThreadCounts)
    {
        const double v = best(t, [&]
                              {
                                  for (std::size_t i = 0; i < kPerThread; ++i)
                                      LOG_INFO(inst, "m {}", i);
                              });
        std::printf(" %8.1f", v);
        if (t == 1) i1 = v;
        if (t == 8) i8 = v;
    }
    std::printf("   %+7.1f%%\n", (i8 / i1 - 1.0) * 100.0);

    std::printf("%s\n", "--------------------------------------------------------------");
    std::printf("snapshot gap (singleton - hoisted): 1T %+.1f ns, 8T %+.1f ns\n", s1 - h1, s8 - h8);
    std::printf("hoisted vs instance (8T):           %+.1f ns\n", h8 - i8);

    mylog::SingletonLogger::shutdown();
    inst.shutdown();
    return 0;
}
