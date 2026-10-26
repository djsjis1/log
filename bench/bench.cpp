// mylog vs 原生 spdlog 的严谨对照压测（README「和原生 spdlog 比」数据的来源）
// 严谨手段：进程亲和性固定到指定 CPU 核 + 每档跑 ROUNDS 轮取最快，同时输出波动范围。
#include "Logger.h"
#include "affinity.h"

#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using clk = std::chrono::steady_clock;

namespace
{
    constexpr int kRounds = 5;         // 每档跑几轮取最快
    constexpr std::size_t kSerial = 100000;
    constexpr std::size_t kThreads = 8;
    constexpr std::size_t kPerThread = 25000;

    bool pin_process(const std::vector<int> &cpus)
    {
        std::uint64_t mask = 0;
        for (int cpu : cpus)
            mask |= (std::uint64_t{1} << cpu);
        return bench::pin_process(mask);
    }

    double ns_per(std::size_t total, clk::time_point t0, clk::time_point t1)
    {
        return std::chrono::duration<double, std::nano>(t1 - t0).count() / double(total);
    }

    double serial_ns(std::size_t count, const std::function<void(std::size_t)> &body)
    {
        const auto t0 = clk::now();
        for (std::size_t i = 0; i < count; ++i)
            body(i);
        return ns_per(count, t0, clk::now());
    }

    double parallel_ns(std::size_t threads, std::size_t per_thread,
                       const std::function<void(std::size_t, std::size_t)> &body)
    {
        std::atomic<bool> start{false};
        std::vector<std::thread> pool;
        const auto t0 = clk::now();
        for (std::size_t t = 0; t < threads; ++t)
            pool.emplace_back([&, t]
                              {
                                  while (!start.load(std::memory_order_acquire))
                                      std::this_thread::yield();
                                  for (std::size_t i = 0; i < per_thread; ++i)
                                      body(t, i);
                              });
        start.store(true, std::memory_order_release);
        for (auto &th : pool)
            th.join();
        return ns_per(threads * per_thread, t0, clk::now());
    }

    struct Pair
    {
        double mylog_min = 0;
        double spdlog_min = 0;
        double mylog_spread = 0;
        double spdlog_spread = 0;
    };

    Pair measure_pair(const std::function<double()> &a, const std::function<double()> &b)
    {
        Pair p{1e300, 1e300, 0, 0};
        double a_max = 0;
        double b_max = 0;
        for (int i = 0; i < kRounds; ++i)
        {
            const double x = a();
            const double y = b();
            p.mylog_min = std::min(p.mylog_min, x);
            p.spdlog_min = std::min(p.spdlog_min, y);
            a_max = std::max(a_max, x);
            b_max = std::max(b_max, y);
        }
        p.mylog_spread = (a_max / p.mylog_min - 1.0) * 100.0;
        p.spdlog_spread = (b_max / p.spdlog_min - 1.0) * 100.0;
        return p;
    }

    void row(const char *name, const Pair &p)
    {
        std::printf("%-32s %8.1f %8.1f %+7.1f%%   %5.1f%% / %5.1f%%\n", name, p.mylog_min,
                    p.spdlog_min, (p.mylog_min - p.spdlog_min) / p.spdlog_min * 100.0,
                    p.mylog_spread, p.spdlog_spread);
    }

    mylog::LogConfig file_cfg(const char *name, bool async)
    {
        mylog::LogConfig cfg;
        cfg.logger_name = name;
        cfg.enable_console = false;
        cfg.enable_file = true;
        cfg.log_dir = "bench/out";
        cfg.log_file = std::string(name) + ".log";
        cfg.rotation = mylog::LogConfig::RotationPolicy::None;
        cfg.level = mylog::Level::Trace;
        cfg.flush_level = mylog::Level::Off;
        cfg.pattern = "%v";
        cfg.async = async;
        cfg.async_queue_size = 8192;
        cfg.async_thread_count = 1;
        return cfg;
    }

    mylog::LogConfig null_cfg(const char *name, bool async, mylog::Level level)
    {
        mylog::LogConfig cfg;
        cfg.logger_name = name;
        cfg.enable_console = false;
        cfg.enable_file = false;
        cfg.level = level;
        cfg.flush_level = mylog::Level::Off;
        cfg.pattern = "%v";
        cfg.async = async;
        cfg.async_queue_size = 8192;
        cfg.async_thread_count = 1;
        mylog::SinkConfig sink;
        sink.factory = []
        { return std::make_unique<spdlog::sinks::null_sink_mt>(); };
        cfg.extra_sinks.push_back(sink);
        return cfg;
    }
} // namespace

int main()
{
    std::error_code ec;
    fs::create_directories("bench/out", ec);

    std::printf("rounds=%d, serial=%zu, mt=%zux%zu, affinity: serial=CPU2, mt=CPU2..9\n", kRounds,
                kSerial, kThreads, kPerThread);
    std::printf("%-32s %8s %8s %8s   %s\n", "scenario (ns/msg, best of N)", "mylog", "spdlog",
                "delta", "spread mylog/spdlog");
    std::printf("%s\n", "--------------------------------------------------------------------------"
                       "------------");

    // 1) 空 sink + 同步：纯封装开销
    {
        pin_process({2});
        auto cfg = null_cfg("b_null", false, mylog::Level::Trace);
        mylog::Logger lg(cfg);
        auto sp = std::make_shared<spdlog::logger>("b_null_sp",
                                                   std::make_shared<spdlog::sinks::null_sink_mt>());
        sp->set_level(spdlog::level::trace);
        sp->set_pattern("%v");
        row("null sink, sync, 1 thread",
            measure_pair([&] { return serial_ns(kSerial, [&](std::size_t i) { LOG_INFO(lg, "hello {}", i); }); },
                         [&] { return serial_ns(kSerial, [&](std::size_t i) { sp->info("hello {}", i); }); }));
        lg.shutdown();
    }

    // 2) 文件 + 同步，1 线程
    {
        pin_process({2});
        auto cfg = file_cfg("b_file", false);
        mylog::Logger lg(cfg);
        auto sp = spdlog::basic_logger_mt("b_file_sp", "bench/out/b_file_sp.log", false);
        sp->set_level(spdlog::level::trace);
        sp->set_pattern("%v");
        row("file sink, sync, 1 thread",
            measure_pair([&] { return serial_ns(kSerial, [&](std::size_t i) { LOG_INFO(lg, "hello {}", i); }); },
                         [&] { return serial_ns(kSerial, [&](std::size_t i) { sp->info("hello {}", i); }); }));
        lg.shutdown();
        spdlog::drop("b_file_sp");
    }

    // 3) 文件 + 同步，8 线程
    {
        pin_process({2, 3, 4, 5, 6, 7, 8, 9});
        auto cfg = file_cfg("b_file_mt", false);
        mylog::Logger lg(cfg);
        auto sp = spdlog::basic_logger_mt("b_file_mt_sp", "bench/out/b_file_mt_sp.log", false);
        sp->set_level(spdlog::level::trace);
        sp->set_pattern("%v");
        row("file sink, sync, 8 threads",
            measure_pair(
                [&] { return parallel_ns(kThreads, kPerThread, [&](std::size_t, std::size_t i) { LOG_INFO(lg, "hello {}", i); }); },
                [&] { return parallel_ns(kThreads, kPerThread, [&](std::size_t, std::size_t i) { sp->info("hello {}", i); }); }));
        lg.shutdown();
        spdlog::drop("b_file_mt_sp");
    }

    // 4) 文件 + 异步(8192,1)，1 线程
    {
        pin_process({2});
        auto cfg = file_cfg("b_async", true);
        mylog::Logger lg(cfg);
        spdlog::init_thread_pool(8192, 1);
        auto sp = spdlog::basic_logger_mt<spdlog::async_factory>("b_async_sp", "bench/out/b_async_sp.log", false);
        sp->set_level(spdlog::level::trace);
        sp->set_pattern("%v");
        row("file sink, async, 1 thread",
            measure_pair([&] { return serial_ns(kSerial, [&](std::size_t i) { LOG_INFO(lg, "hello {}", i); }); },
                         [&] { return serial_ns(kSerial, [&](std::size_t i) { sp->info("hello {}", i); }); }));
        lg.shutdown();
        spdlog::shutdown();
    }

    // 5) 文件 + 异步(8192,1)，8 线程
    {
        pin_process({2, 3, 4, 5, 6, 7, 8, 9});
        auto cfg = file_cfg("b_async_mt", true);
        mylog::Logger lg(cfg);
        spdlog::init_thread_pool(8192, 1);
        auto sp = spdlog::basic_logger_mt<spdlog::async_factory>("b_async_mt_sp", "bench/out/b_async_mt_sp.log", false);
        sp->set_level(spdlog::level::trace);
        sp->set_pattern("%v");
        row("file sink, async, 8 threads",
            measure_pair(
                [&] { return parallel_ns(kThreads, kPerThread, [&](std::size_t, std::size_t i) { LOG_INFO(lg, "hello {}", i); }); },
                [&] { return parallel_ns(kThreads, kPerThread, [&](std::size_t, std::size_t i) { sp->info("hello {}", i); }); }));
        lg.shutdown();
        spdlog::shutdown();
    }

    // 6) 被过滤的日志（level=Warn 打 Info）：只测门禁成本
    {
        pin_process({2});
        auto cfg = null_cfg("b_filtered", false, mylog::Level::Warn);
        mylog::Logger lg(cfg);
        auto sp = std::make_shared<spdlog::logger>("b_filtered_sp",
                                                   std::make_shared<spdlog::sinks::null_sink_mt>());
        sp->set_level(spdlog::level::warn);
        row("filtered (level=warn), 1 thread",
            measure_pair([&] { return serial_ns(500000, [&](std::size_t i) { LOG_INFO(lg, "hello {}", i); }); },
                         [&] { return serial_ns(500000, [&](std::size_t i) { sp->info("hello {}", i); }); }));
        lg.shutdown();
    }

    // 7) 单例宏 vs spdlog 直接调用
    {
        pin_process({2});
        mylog::SingletonLogger::init(null_cfg("b_singleton", false, mylog::Level::Trace));
        auto sp = std::make_shared<spdlog::logger>("b_singleton_sp",
                                                   std::make_shared<spdlog::sinks::null_sink_mt>());
        sp->set_level(spdlog::level::trace);
        sp->set_pattern("%v");
        row("singleton macro vs direct call",
            measure_pair([&] { return serial_ns(kSerial, [&](std::size_t i) { LOGS_INFO("hello {}", i); }); },
                         [&] { return serial_ns(kSerial, [&](std::size_t i) { sp->info("hello {}", i); }); }));
        mylog::SingletonLogger::shutdown();
    }

    // 8) 同一库内：实例宏 vs 单例宏，差值即快照成本
    {
        pin_process({2});
        mylog::Logger lg(null_cfg("b_inst", false, mylog::Level::Trace));
        mylog::SingletonLogger::init(null_cfg("b_single2", false, mylog::Level::Trace));
        const Pair p = measure_pair(
            [&] { return serial_ns(kSerial, [&](std::size_t i) { LOG_INFO(lg, "hello {}", i); }); },
            [&] { return serial_ns(kSerial, [&](std::size_t i) { LOGS_INFO("hello {}", i); }); });
        std::printf("%-32s %8.1f %8.1f %+6.1f ns  %5.1f%% / %5.1f%%\n", "instance macro vs singleton",
                    p.mylog_min, p.spdlog_min, p.spdlog_min - p.mylog_min, p.mylog_spread,
                    p.spdlog_spread);
        mylog::SingletonLogger::shutdown();
        lg.shutdown();
    }

    // 9) 异步提交成本隔离：大容量队列 + 空 sink，不会因队列满而阻塞
    {
        pin_process({2});
        auto cfg = null_cfg("b_async_null", true, mylog::Level::Trace);
        cfg.async_queue_size = 1u << 20;
        mylog::Logger lg(cfg);
        spdlog::init_thread_pool(1u << 20, 1);
        auto sp = std::make_shared<spdlog::async_logger>(
            "b_async_null_sp", std::make_shared<spdlog::sinks::null_sink_mt>(), spdlog::thread_pool(),
            spdlog::async_overflow_policy::block);
        sp->set_level(spdlog::level::trace);
        sp->set_pattern("%v");
        row("null sink, async(1M q), 1 thread",
            measure_pair([&] { return serial_ns(kSerial, [&](std::size_t i) { LOG_INFO(lg, "hello {}", i); }); },
                         [&] { return serial_ns(kSerial, [&](std::size_t i) { sp->info("hello {}", i); }); }));
        lg.shutdown();
        spdlog::shutdown();
    }

    std::printf("%s\n", "--------------------------------------------------------------------------"
                       "------------");

    pin_process({2, 3, 4, 5, 6, 7, 8, 9});
    std::printf("\n[stress] writer threads + singleton churn ...\n");
    {
        mylog::SingletonLogger::init(null_cfg("s_churn", false, mylog::Level::Trace));
        std::atomic<bool> stop{false};
        std::atomic<std::size_t> attempts{0};
        std::vector<std::thread> writers;
        for (int t = 0; t < 4; ++t)
        {
            writers.emplace_back([&]
                                 {
                                     while (!stop.load(std::memory_order_relaxed))
                                     {
                                         LOGS_INFO("churn {}", attempts.fetch_add(1));
                                         std::this_thread::sleep_for(std::chrono::microseconds(20));
                                     }
                                 });
        }
        for (int round = 0; round < 500; ++round)
        {
            mylog::SingletonLogger::shutdown();
            if (!mylog::SingletonLogger::init(null_cfg("s_churn", false, mylog::Level::Trace)))
                std::printf("  init failed at round %d\n", round);
        }
        stop = true;
        for (auto &w : writers)
            w.join();
        mylog::SingletonLogger::shutdown();
        std::printf("  ok, %zu attempts across 500 swizzles\n", attempts.load());
    }

    std::printf("[stress] async + flush_interval + concurrent flush_and_wait ...\n");
    {
        auto cfg = null_cfg("s_async", true, mylog::Level::Trace);
        cfg.flush_interval = std::chrono::milliseconds(20);
        mylog::Logger lg(cfg);
        std::atomic<bool> stop{false};
        std::atomic<std::size_t> flushed{0};
        std::vector<std::thread> writers;
        for (int t = 0; t < 4; ++t)
        {
            writers.emplace_back([&]
                                 {
                                     std::size_t i = 0;
                                     while (!stop.load(std::memory_order_relaxed))
                                     {
                                         LOG_INFO(lg, "async {}", i++);
                                         if (i % 500 == 0 && lg.flush_and_wait(std::chrono::seconds(2)))
                                             flushed.fetch_add(1);
                                     }
                                 });
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
        stop = true;
        for (auto &w : writers)
            w.join();
        std::printf("  ok, flush_and_wait succeeded %zu times\n", flushed.load());
        lg.shutdown();
    }

    std::printf("[stress] 8 instances sharing flush scheduler, concurrent close ...\n");
    {
        std::vector<std::unique_ptr<mylog::Logger>> loggers;
        for (int i = 0; i < 8; ++i)
        {
            char name[32];
            std::snprintf(name, sizeof(name), "s_sched_%d", i);
            auto cfg = null_cfg(name, false, mylog::Level::Trace);
            cfg.flush_interval = std::chrono::milliseconds(10 + i * 5);
            loggers.push_back(std::make_unique<mylog::Logger>(cfg));
        }
        std::vector<std::thread> writers;
        for (std::size_t i = 0; i < loggers.size(); ++i)
        {
            writers.emplace_back([&, i]
                                 {
                                     for (int k = 0; k < 20000; ++k)
                                         LOG_INFO(*loggers[i], "sched {}", k);
                                 });
        }
        for (auto &w : writers)
            w.join();
        std::vector<std::thread> closers;
        for (std::size_t i = 0; i < loggers.size(); ++i)
            closers.emplace_back([&, i]
                                 { loggers[i]->shutdown(); });
        for (auto &c : closers)
            c.join();
        std::printf("  ok, %zu instances closed concurrently\n", loggers.size());
    }

    std::printf("[stress] 16 threads race for the same file ...\n");
    {
        mylog::LogConfig cfg;
        cfg.logger_name = "s_race";
        cfg.enable_console = false;
        cfg.enable_file = true;
        cfg.log_dir = "bench/out";
        cfg.log_file = "race.log";
        cfg.pattern = "%v";
        std::atomic<int> winners{0};
        std::vector<std::thread> threads;
        std::vector<std::unique_ptr<mylog::Logger>> keep;
        std::mutex keep_mutex;
        for (int i = 0; i < 16; ++i)
        {
            threads.emplace_back([&]
                                 {
                                     auto candidate = std::make_unique<mylog::Logger>(cfg);
                                     if (candidate->is_initialized())
                                     {
                                         std::lock_guard<std::mutex> lock(keep_mutex);
                                         keep.push_back(std::move(candidate));
                                         winners.fetch_add(1);
                                     }
                                 });
        }
        for (auto &t : threads)
            t.join();
        std::printf("  ok, winners=%d (expect 1)\n", winners.load());
        keep.clear();
    }

    std::printf("\nall stress steps finished without crash\n");
    return 0;
}
