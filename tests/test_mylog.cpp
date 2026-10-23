// mylog 回归测试：只依赖 C++17 标准库，用 CTest 驱动。
// 覆盖初始化失败回调重入、单例生命周期、异步排空 / 溢出统计、
// 降级与目录失败、重复初始化 / 关闭、移动语义、日轮转。
#include "Logger.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace mylog;

namespace
{
    int g_failures = 0;

    const fs::path kBase = "mylog_test_logs";

#define CHECK(cond)                                                                          \
    do                                                                                       \
    {                                                                                        \
        if (!(cond))                                                                         \
        {                                                                                    \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                                    \
        }                                                                                    \
    } while (false)

    std::size_t count_lines(const fs::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        std::size_t lines = 0;
        std::string line;
        while (std::getline(in, line))
            ++lines;
        return lines;
    }

    void test_empty_instance()
    {
        spdlog::drop("app");

        // 未初始化时写日志：静默丢弃，不能崩溃，也不能注册 "app"
        LOGS_INFO("this must be dropped silently");
        CHECK(spdlog::get("app") == nullptr);

        auto &borrowed = SingletonLogger::instance();
        CHECK(!borrowed.is_initialized());
        CHECK(borrowed.level() == Level::Off);
        CHECK(!borrowed.should_log(Level::Info));

        // 空实例上的设置操作必须无副作用，不能突然开始输出
        borrowed.set_level(Level::Info);
        LOGS_INFO("still dropped");
        CHECK(spdlog::get("app") == nullptr);
    }

    void test_init_failure_callback_reentry()
    {
        LogConfig cfg;
        cfg.logger_name = "bad";
        cfg.enable_console = false;
        cfg.enable_file = true;
        cfg.log_dir = (kBase / "bad").string();
        cfg.log_file = "bad.log";
        cfg.max_file_size = 0; // BySize 非法

        bool called = false;
        cfg.error_handler = [&called](const std::string &msg)
        {
            called = true;
            CHECK(!msg.empty());
            // 回调若在持锁路径上被调用，这里会永久阻塞并被 CTest 超时捕获
            LOGS_ERROR("error handler reentry: {}", msg);
        };

        auto result = SingletonLogger::init_checked(cfg);
        CHECK(!result.ok);
        CHECK(result.error == InitError::InvalidConfig);
        CHECK(called);
        CHECK(spdlog::get("bad") == nullptr);
    }

    void test_directory_failure()
    {
        const fs::path not_a_dir = kBase / "not_a_dir";
        std::ofstream(not_a_dir).put('x');

        LogConfig cfg;
        cfg.logger_name = "dir_bad";
        cfg.enable_console = false;
        cfg.enable_file = true;
        cfg.log_dir = not_a_dir.string();
        cfg.log_file = "x.log";

        auto result = SingletonLogger::init_checked(cfg);
        CHECK(!result.ok);
        CHECK(result.error == InitError::Directory);
    }

    void test_file_sink_fallback()
    {
        const fs::path dir = kBase / "fallback";
        std::error_code ec;
        fs::create_directories(dir / "not_a_file", ec); // 让 log_file 指向一个目录，打开必然失败

        LogConfig cfg;
        cfg.logger_name = "fallback";
        cfg.enable_console = true;
        cfg.enable_file = true;
        cfg.log_dir = dir.string();
        cfg.log_file = "not_a_file";
        cfg.fallback_to_console_on_file_error = true;

        bool notified = false;
        cfg.error_handler = [&notified](const std::string &msg)
        {
            notified = true;
            CHECK(!msg.empty());
        };

        auto result = SingletonLogger::init_checked(cfg);
        CHECK(result.ok);
        CHECK(result.degraded);
        CHECK(notified);

        auto logger = SingletonLogger::acquire();
        CHECK(logger && logger->is_initialized());
        logger.reset();
        SingletonLogger::shutdown();
    }

    void test_async_drain()
    {
        const fs::path dir = kBase / "async";
        LogConfig cfg;
        cfg.logger_name = "drain";
        cfg.enable_console = false;
        cfg.enable_file = true;
        cfg.log_dir = dir.string();
        cfg.log_file = "drain.log";
        cfg.async = true;
        cfg.async_queue_size = 4096;
        cfg.async_thread_count = 2;
        cfg.level = Level::Trace;
        cfg.pattern = "%v";

        auto result = SingletonLogger::init_checked(cfg);
        CHECK(result.ok);

        const int kCount = 5000;
        for (int i = 0; i < kCount; ++i)
            LOGS_INFO("line {}", i);

        SingletonLogger::shutdown();

        // 队列里的消息必须在关闭时全部落盘
        CHECK(count_lines(dir / "drain.log") == static_cast<std::size_t>(kCount));
    }

    void test_borrowed_snapshot()
    {
        const fs::path dir = kBase / "snapshot";
        LogConfig cfg;
        cfg.logger_name = "snapshot";
        cfg.enable_console = false;
        cfg.enable_file = true;
        cfg.log_dir = dir.string();
        cfg.log_file = "snapshot.log";
        cfg.pattern = "%v";

        CHECK(SingletonLogger::init(cfg));

        auto snapshot = SingletonLogger::acquire();
        CHECK(snapshot && snapshot->is_initialized());

        // 关闭全局持有后，快照仍然可用（生命周期由快照延长）
        SingletonLogger::shutdown();
        CHECK(snapshot->is_initialized());
        snapshot->log(MYLOG_SRC_LOC, Level::Info, "after global shutdown");
        snapshot->flush();

        snapshot.reset(); // 最后一个引用释放，此时才真正关闭并写盘
        CHECK(count_lines(dir / "snapshot.log") == 1);
    }

    void test_overflow_discard()
    {
        const fs::path dir = kBase / "overflow";
        LogConfig cfg;
        cfg.logger_name = "overflow";
        cfg.enable_console = false;
        cfg.enable_file = true;
        cfg.log_dir = dir.string();
        cfg.log_file = "overflow.log";
        cfg.async = true;
        cfg.async_queue_size = 1;
        cfg.async_thread_count = 1;
        cfg.async_overflow = OverflowPolicy::DiscardNew;
        cfg.pattern = "%v";

        CHECK(SingletonLogger::init(cfg));

        for (int i = 0; i < 20000; ++i)
            LOGS_INFO("overflow {}", i);

        auto snapshot = SingletonLogger::acquire();
        CHECK(snapshot != nullptr);
        const AsyncStats stats = snapshot->stats();
        CHECK(stats.discard_count > 0);

        SingletonLogger::shutdown();
        snapshot.reset();
    }

    void test_reinit_and_repeat_shutdown()
    {
        const fs::path dir = kBase / "repeat";
        LogConfig cfg;
        cfg.logger_name = "repeat";
        cfg.enable_console = false;
        cfg.enable_file = true;
        cfg.log_dir = dir.string();
        cfg.log_file = "repeat.log";
        cfg.pattern = "%v";

        CHECK(SingletonLogger::init(cfg));
        CHECK(SingletonLogger::init(cfg)); // 同名重复初始化

        SingletonLogger::shutdown();
        SingletonLogger::shutdown(); // 重复关闭必须安全

        CHECK(SingletonLogger::init(cfg));
        // 旧实例关闭时不能把新实例从 registry 里误删
        CHECK(spdlog::get("repeat") != nullptr);
        SingletonLogger::shutdown();
    }

    void test_level_and_stats()
    {
        const fs::path dir = kBase / "level";
        LogConfig cfg;
        cfg.logger_name = "level";
        cfg.enable_console = false;
        cfg.enable_file = true;
        cfg.log_dir = dir.string();
        cfg.log_file = "level.log";
        cfg.level = Level::Warn;

        CHECK(SingletonLogger::init(cfg));

        auto snapshot = SingletonLogger::acquire();
        CHECK(snapshot != nullptr);
        CHECK(snapshot->level() == Level::Warn);
        CHECK(snapshot->should_log(Level::Error));
        CHECK(!snapshot->should_log(Level::Info));

        const AsyncStats stats = snapshot->stats();
        CHECK(stats.queue_size == 0);
        CHECK(stats.overrun_count == 0);
        CHECK(stats.discard_count == 0);

        snapshot->set_level(Level::Info);
        CHECK(snapshot->should_log(Level::Info));

        snapshot.reset();
        SingletonLogger::shutdown();
    }

    void test_move_semantics()
    {
        const fs::path dir = kBase / "move";
        LogConfig cfg;
        cfg.logger_name = "move";
        cfg.enable_console = false;
        cfg.enable_file = true;
        cfg.log_dir = dir.string();
        cfg.log_file = "move.log";
        cfg.pattern = "%v";

        Logger first(cfg);
        CHECK(first.is_initialized());

        Logger second(std::move(first));
        CHECK(second.is_initialized());
        CHECK(!first.is_initialized());

        LOG_INFO(second, "moved-line");
        second.flush();
        second.shutdown();
        CHECK(count_lines(dir / "move.log") == 1);
    }

    void test_worker_thread_flag()
    {
        const fs::path dir = kBase / "worker";
        LogConfig cfg;
        cfg.logger_name = "worker";
        cfg.enable_console = false;
        cfg.enable_file = true;
        cfg.log_dir = dir.string();
        cfg.log_file = "worker.log";
        cfg.async = true;
        cfg.pattern = "%v";

        CHECK(SingletonLogger::init(cfg));

        auto snapshot = SingletonLogger::acquire();
        CHECK(snapshot != nullptr);
        CHECK(!snapshot->is_on_worker_thread());

        snapshot.reset();
        SingletonLogger::shutdown();
    }

    void test_concurrent_log_and_shutdown()
    {
        const fs::path dir = kBase / "race";

        auto make_config = [&dir](int round)
        {
            LogConfig cfg;
            cfg.logger_name = "race" + std::to_string(round);
            cfg.enable_console = false;
            cfg.enable_file = true;
            cfg.log_dir = dir.string();
            cfg.log_file = "race" + std::to_string(round) + ".log";
            cfg.pattern = "%v";
            return cfg;
        };

        CHECK(SingletonLogger::init(make_config(0)));

        std::atomic<bool> stop{false};
        std::vector<std::thread> writers;
        for (int i = 0; i < 4; ++i)
        {
            writers.emplace_back([&stop, i]
                                 {
                                     while (!stop.load(std::memory_order_relaxed))
                                     {
                                         LOGS_INFO("race {} {}", i, 0);
                                         std::this_thread::sleep_for(std::chrono::microseconds(50));
                                     }
                                 });
        }

        // 日志线程正在写的同时反复切换单例：快照必须保证不会悬空
        for (int round = 1; round <= 30; ++round)
        {
            SingletonLogger::shutdown();
            CHECK(SingletonLogger::init(make_config(round)));
        }

        stop = true;
        for (auto &writer : writers)
            writer.join();
        SingletonLogger::shutdown();
    }

    void test_daily_rotation()
    {
        const fs::path dir = kBase / "daily";
        LogConfig cfg;
        cfg.logger_name = "daily";
        cfg.enable_console = false;
        cfg.enable_file = true;
        cfg.log_dir = dir.string();
        cfg.log_file = "daily.log";
        cfg.rotation = LogConfig::RotationPolicy::Daily;
        cfg.daily_max_files = 2;

        CHECK(SingletonLogger::init(cfg));
        LOGS_INFO("daily line");
        SingletonLogger::shutdown();

        std::time_t now = std::time(nullptr);
        std::tm tm_now{};
#ifdef _WIN32
        localtime_s(&tm_now, &now);
#else
        localtime_r(&now, &tm_now);
#endif

        char filename[64] = {};
        std::snprintf(filename, sizeof(filename), "daily_%04d-%02d-%02d.log",
                      tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday);
        CHECK(fs::exists(dir / filename));
    }

} // namespace

int main()
{
    std::error_code ec;
    fs::remove_all(kBase, ec);
    fs::create_directories(kBase, ec);

    test_empty_instance();
    test_init_failure_callback_reentry();
    test_directory_failure();
    test_file_sink_fallback();
    test_async_drain();
    test_borrowed_snapshot();
    test_overflow_discard();
    test_reinit_and_repeat_shutdown();
    test_level_and_stats();
    test_move_semantics();
    test_worker_thread_flag();
    test_concurrent_log_and_shutdown();
    test_daily_rotation();

    fs::remove_all(kBase, ec);

    if (g_failures == 0)
    {
        std::printf("all mylog tests passed\n");
        return 0;
    }

    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
}
