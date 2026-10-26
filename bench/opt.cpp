// 定位"能省时间的开关"：pattern 复杂度 / flush_on / 单例快照，全部同库内对比
#include "Logger.h"
#include "affinity.h"

#include <spdlog/sinks/null_sink.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using clk = std::chrono::steady_clock;

namespace
{
    constexpr int kRounds = 5;
    constexpr std::size_t kN = 100000;

    bool pin_cpu(int cpu)
    {
        return bench::pin_process(std::uint64_t{1} << cpu);
    }

    double best_of(int rounds, const std::function<void()> &body)
    {
        double best = 1e300;
        for (int i = 0; i < rounds; ++i)
        {
            const auto t0 = clk::now();
            body();
            const double ns =
                std::chrono::duration<double, std::nano>(clk::now() - t0).count() / double(kN);
            best = std::min(best, ns);
        }
        return best;
    }

    mylog::LogConfig base_cfg(const char *name, const std::string &pattern, bool file_on)
    {
        mylog::LogConfig cfg;
        cfg.logger_name = name;
        cfg.enable_console = false;
        cfg.enable_file = file_on;
        cfg.log_dir = "bench/out";
        cfg.log_file = std::string(name) + ".log";
        cfg.rotation = mylog::LogConfig::RotationPolicy::None;
        cfg.level = mylog::Level::Trace;
        cfg.pattern = pattern;
        return cfg;
    }

    void line(const char *what, double ns, double ref)
    {
        std::printf("%-40s %8.1f ns  %+7.1f%%\n", what, ns, ref > 0 ? (ns - ref) / ref * 100.0 : 0.0);
    }

    // 真正的“无输出目标”基准：用 null sink，避免触发控制台回退
    mylog::LogConfig null_cfg(const char *name, const std::string &pattern)
    {
        mylog::LogConfig cfg;
        cfg.logger_name = name;
        cfg.enable_console = false;
        cfg.enable_file = false;
        cfg.rotation = mylog::LogConfig::RotationPolicy::None;
        cfg.level = mylog::Level::Trace;
        cfg.pattern = pattern;
        mylog::SinkConfig sink;
        sink.factory = [] { return std::make_unique<spdlog::sinks::null_sink_mt>(); };
        cfg.extra_sinks.push_back(sink);
        return cfg;
    }
} // namespace

int main()
{
    std::error_code ec;
    fs::create_directories("bench/out", ec);
    pin_cpu(2);

    const std::string kDefaultPattern = "[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] [%t] [%s:%#] %v";
    const std::string kNoTime = "[%n] [%^%l%$] [%t] [%s:%#] %v";
    const std::string kPlain = "%v";

    std::printf("== 1) pattern 复杂度（文件 sink, 同步, 1 线程, %zu 条/轮）==\n", kN);
    {
        auto mk = [&](const char *name, const std::string &pat)
        {
            auto cfg = base_cfg(name, pat, true);
            auto lg = std::make_unique<mylog::Logger>(cfg);
            return lg;
        };
        auto full = mk("o_full", kDefaultPattern);
        auto notime = mk("o_notime", kNoTime);
        auto plain = mk("o_plain", kPlain);
        const double t_full = best_of(kRounds, [&] { for (std::size_t i = 0; i < kN; ++i) LOG_INFO(*full, "hello {}", i); });
        const double t_notime = best_of(kRounds, [&] { for (std::size_t i = 0; i < kN; ++i) LOG_INFO(*notime, "hello {}", i); });
        const double t_plain = best_of(kRounds, [&] { for (std::size_t i = 0; i < kN; ++i) LOG_INFO(*plain, "hello {}", i); });
        line("默认 pattern（含 %t %s:%# 时间）", t_full, t_full);
        line("去掉时间戳 %Y-%m-%d %H:%M:%S.%e", t_notime, t_full);
        line("仅 %v（无名字/级别/线程/位置）", t_plain, t_full);
        full->shutdown();
        notime->shutdown();
        plain->shutdown();
    }

    std::printf("\n== 2) flush_on：打 Warn 日志时是否每条都刷盘（文件 sink）==\n");
    {
        auto off = std::make_unique<mylog::Logger>(base_cfg("o_flush_off", kPlain, true));
        auto warn = std::make_unique<mylog::Logger>(base_cfg("o_flush_warn", kPlain, true));
        off->set_flush_level(mylog::Level::Off);
        warn->set_flush_level(mylog::Level::Warn);
        const double t_off = best_of(kRounds, [&] { for (std::size_t i = 0; i < kN; ++i) LOG_WARN(*off, "warn {}", i); });
        const double t_warn = best_of(kRounds, [&] { for (std::size_t i = 0; i < kN; ++i) LOG_WARN(*warn, "warn {}", i); });
        line("flush_level = Off（不自动刷盘）", t_off, t_off);
        line("flush_level = Warn（每条 Warn 都刷盘）", t_warn, t_off);
        off->shutdown();
        warn->shutdown();
    }

    std::printf("\n== 3) 单例：每条取快照 vs 循环外取一次 ==\n");
    {
        mylog::SingletonLogger::init(null_cfg("o_single", kPlain));
        mylog::Logger inst(null_cfg("o_inst", kPlain));
        const double t_single = best_of(kRounds, [&] { for (std::size_t i = 0; i < kN; ++i) LOGS_INFO("hello {}", i); });
        const double t_hoist = best_of(kRounds, [&]
                                       {
                                           auto lg = mylog::SingletonLogger::acquire();
                                           if (lg)
                                               for (std::size_t i = 0; i < kN; ++i)
                                                   LOG_INFO(*lg, "hello {}", i);
                                       });
        const double t_inst = best_of(kRounds, [&] { for (std::size_t i = 0; i < kN; ++i) LOG_INFO(inst, "hello {}", i); });
        line("LOGS_INFO（每条取快照）", t_single, t_single);
        line("acquire 一次 + LOG_INFO 循环", t_hoist, t_single);
        line("纯实例 LOG_INFO（参照）", t_inst, t_single);
        mylog::SingletonLogger::shutdown();
        inst.shutdown();
    }

    std::printf("\n== 4) 同步 vs 异步（文件 sink, 关掉自动 flush）==\n");
    {
        auto sync = std::make_unique<mylog::Logger>(base_cfg("o_sync", kPlain, true));
        auto cfg_async = base_cfg("o_async", kPlain, true);
        cfg_async.async = true;
        cfg_async.async_queue_size = 1u << 20; // 大容量队列：不让生产端被磁盘反压
        auto async = std::make_unique<mylog::Logger>(cfg_async);
        sync->set_flush_level(mylog::Level::Off);
        async->set_flush_level(mylog::Level::Off);

        const double t_sync = best_of(1, [&] { for (std::size_t i = 0; i < kN; ++i) LOG_INFO(*sync, "hello {}", i); });
        const double t_async = best_of(1, [&] { for (std::size_t i = 0; i < kN; ++i) LOG_INFO(*async, "hello {}", i); });
        line("同步（调用线程直接写）", t_sync, t_sync);
        line("异步（入队即返回）", t_async, t_sync);
        sync->shutdown();
        async->shutdown(); // 含排空时间，未计入上方测量
    }

    std::printf("\n注：每档跑 %d 轮取最快；进程固定在 CPU2。\n", kRounds);
    return 0;
}
