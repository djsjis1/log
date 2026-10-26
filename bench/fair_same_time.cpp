// 同样时长下的对比：每个入口固定跑 N 秒，统计消息数 -> msg/s 与 ns/条
// 四个入口共用同一个 sink（null），格式化内容与参数也完全相同。
#include "Logger.h"
#include "affinity.h"

#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

using clk = std::chrono::steady_clock;

namespace
{
    constexpr double kSeconds = 3.0;
    constexpr int kRounds = 3;

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

    // 固定跑满 kSeconds，返回 (消息数, 实际耗时秒)
    template <typename F>
    std::pair<long long, double> run_for(F &&body)
    {
        long long count = 0;
        const auto start = clk::now();
        double elapsed = 0;
        do
        {
            for (int i = 0; i < 1024; ++i)
                body(i);
            count += 1024;
            elapsed = std::chrono::duration<double>(clk::now() - start).count();
        } while (elapsed < kSeconds);
        return {count, elapsed};
    }

    struct Result
    {
        double best_ns = 1e300;
        double best_rate = 0;
    };

    void report(const char *name, const Result &r)
    {
        std::printf("%-26s %10.1f ns/条   %12.0f 条/秒\n", name, r.best_ns, r.best_rate);
    }
} // namespace

int main()
{
    bench::pin_process(0x4); // 钉在 CPU2，单线程对比
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    mylog::SingletonLogger::init(null_cfg("fair_singleton"));
    mylog::Logger inst(null_cfg("fair_instance"));

    mylog::Logger *singleton = mylog::SingletonLogger::get();
    std::shared_ptr<spdlog::logger> raw = singleton ? singleton->native() : nullptr;
    if (!raw)
    {
        std::printf("init failed\n");
        return 1;
    }

    Result r_singleton, r_instance, r_raw, r_global;
    std::printf("每个入口各跑 %.0f 秒 x %d 轮，取最快\n\n", kSeconds, kRounds);

    for (int r = 0; r < kRounds; ++r)
    {
        // 1) mylog 单例宏
        {
            auto [n, sec] = run_for([](int i)
                                    { LOGS_INFO("tick {}", i); });
            const double ns = sec * 1e9 / double(n);
            r_singleton.best_ns = std::min(r_singleton.best_ns, ns);
            r_singleton.best_rate = std::max(r_singleton.best_rate, n / sec);
        }
        // 2) mylog 实例宏
        {
            auto [n, sec] = run_for([&](int i)
                                    { LOG_INFO(*singleton, "tick {}", i); });
            const double ns = sec * 1e9 / double(n);
            r_instance.best_ns = std::min(r_instance.best_ns, ns);
            r_instance.best_rate = std::max(r_instance.best_rate, n / sec);
        }
        // 3) 直接调同一个 spdlog logger（同一 sink、同一格式）
        {
            auto [n, sec] = run_for([&](int i)
                                    { raw->info("tick {}", i); });
            const double ns = sec * 1e9 / double(n);
            r_raw.best_ns = std::min(r_raw.best_ns, ns);
            r_raw.best_rate = std::max(r_raw.best_rate, n / sec);
        }
        // 4) spdlog 自己的全局入口（默认日志器）
        {
            static bool installed = false;
            if (!installed)
            {
                spdlog::set_default_logger(raw);
                spdlog::set_pattern("%v");
                installed = true;
            }
            auto [n, sec] = run_for([](int i)
                                    { spdlog::info("tick {}", i); });
            const double ns = sec * 1e9 / double(n);
            r_global.best_ns = std::min(r_global.best_ns, ns);
            r_global.best_rate = std::max(r_global.best_rate, n / sec);
        }
    }

    std::printf("%s\n", "--------------------------------------------------------------");
    report("mylog LOGS_* (单例宏)", r_singleton);
    report("mylog LOG_INFO (实例宏)", r_instance);
    report("spdlog raw->info()", r_raw);
    report("spdlog 全局 spdlog::info()", r_global);
    std::printf("%s\n", "--------------------------------------------------------------");
    std::printf("单例 vs spdlog 直接调:    %+.1f ns/条 (%.2fx)\n", r_singleton.best_ns - r_raw.best_ns,
                r_singleton.best_ns / r_raw.best_ns);
    std::printf("单例 vs spdlog 全局入口:  %+.1f ns/条 (%.2fx)\n", r_singleton.best_ns - r_global.best_ns,
                r_singleton.best_ns / r_global.best_ns);

    mylog::SingletonLogger::shutdown();
    inst.shutdown();
    return 0;
}
