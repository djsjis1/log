// mylog 回归测试：只依赖 C++17 标准库，用 CTest 驱动。
// 覆盖初始化失败回调重入、单例生命周期、异步排空 / 溢出统计、
// 降级与目录失败、重复初始化 / 关闭、移动语义、日轮转。
#include "Logger.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/ostream_sink.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace mylog;

// 同一源文件分别以 Trace、Info、Off 编译；宏裁剪只作用于调用点。
static int test_compile_levels()
{
    int errors = 0;
    int arguments[6]{};
    int instance_evaluations = 0;
    int callbacks = 0;
    LogConfig cfg;
    cfg.logger_name = "compile_instance";
    cfg.enable_console = false;
    cfg.enable_file = false;
    cfg.level = Level::Trace;
    cfg.callback_level = Level::Trace;
    cfg.log_callback = [&](const LogEvent &) { ++callbacks; };
    Logger logger(cfg);
    cfg.logger_name = "compile_singleton";
    if (!logger.is_initialized() || !SingletonLogger::init(cfg))
        return 1;
    auto evaluated = [&]() -> Logger & { ++instance_evaluations; return logger; };
    LOG_TRACE(evaluated(), "{}", ++arguments[0]);
    LOG_DEBUG(evaluated(), "{}", ++arguments[1]);
    LOG_INFO(evaluated(), "{}", ++arguments[2]);
    LOG_WARN(evaluated(), "{}", ++arguments[3]);
    LOG_ERROR(evaluated(), "{}", ++arguments[4]);
    LOG_CRITICAL(evaluated(), "{}", ++arguments[5]);
    LOGS_TRACE("{}", ++arguments[0]);
    LOGS_DEBUG("{}", ++arguments[1]);
    LOGS_INFO("{}", ++arguments[2]);
    LOGS_WARN("{}", ++arguments[3]);
    LOGS_ERROR("{}", ++arguments[4]);
    LOGS_CRITICAL("{}", ++arguments[5]);
    // 这些标识符刻意不存在，格式也刻意不合法；被裁剪后必须仍可编译。
#if MYLOG_ACTIVE_LEVEL > MYLOG_LEVEL_TRACE
    LOG_TRACE(no_such_logger, "{", no_such_value);
    LOGS_TRACE("{", no_such_value);
#endif
#if MYLOG_ACTIVE_LEVEL > MYLOG_LEVEL_DEBUG
    LOG_DEBUG(no_such_logger, "{", no_such_value);
    LOGS_DEBUG("{", no_such_value);
#endif
#if MYLOG_ACTIVE_LEVEL > MYLOG_LEVEL_INFO
    LOG_INFO(no_such_logger, "{", no_such_value);
    LOGS_INFO("{", no_such_value);
#endif
#if MYLOG_ACTIVE_LEVEL > MYLOG_LEVEL_WARN
    LOG_WARN(no_such_logger, "{", no_such_value);
    LOGS_WARN("{", no_such_value);
#endif
#if MYLOG_ACTIVE_LEVEL > MYLOG_LEVEL_ERROR
    LOG_ERROR(no_such_logger, "{", no_such_value);
    LOGS_ERROR("{", no_such_value);
#endif
#if MYLOG_ACTIVE_LEVEL > MYLOG_LEVEL_CRITICAL
    LOG_CRITICAL(no_such_logger, "{", no_such_value);
    LOGS_CRITICAL("{", no_such_value);
#endif
    int enabled = 0;
    for (int i = 0; i < 6; ++i)
    {
        const bool active = i >= MYLOG_ACTIVE_LEVEL;
        enabled += active ? 1 : 0;
        if (arguments[i] != (active ? 2 : 0))
            ++errors;
    }
    if (callbacks != enabled * 2 || instance_evaluations != enabled)
        ++errors;
    SingletonLogger::shutdown();
    return errors;
}

#ifdef MYLOG_COMPILE_LEVEL_TEST
int main()
{
    const int errors = test_compile_levels();
    std::printf("compile level %d: %d failures\n", MYLOG_ACTIVE_LEVEL, errors);
    return errors != 0;
}
#else
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

    // 受控 sink：可以让后台线程停在指定条数的消息上，从而确定性地构造
    // “后台正在写 A、队列里已有一条 B”这种状态，而不依赖大量压测碰概率。
    class gated_sink final : public spdlog::sinks::base_sink<std::mutex>
    {
    public:
        explicit gated_sink(std::size_t gate_count) : gate_count_(gate_count) {}

        bool wait_until_entered()
        {
            std::unique_lock<std::mutex> lock(mutex_);
            const bool entered = cv_.wait_for(lock, std::chrono::seconds(5), [this]
                                              { return entered_ > 0; });
            if (!entered)
            {
                gate_open_ = true; // 失败后也让析构能够排空线程池，避免测试卡死
                cv_.notify_all();
            }
            return entered;
        }

        void open_gate()
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                gate_open_ = true;
            }
            cv_.notify_all();
        }

        std::vector<std::string> messages()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return messages_;
        }

        std::size_t flush_count()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return flush_count_;
        }

    protected:
        void sink_it_(const spdlog::details::log_msg &msg) override
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (entered_ < gate_count_)
            {
                ++entered_;
                cv_.notify_all();
                cv_.wait(lock, [this]
                         { return gate_open_; });
            }
            messages_.emplace_back(msg.payload.data(), msg.payload.size());
        }

        void flush_() override
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++flush_count_;
        }

    private:
        std::mutex mutex_;
        std::condition_variable cv_;
        std::vector<std::string> messages_;
        std::size_t entered_ = 0;
        std::size_t flush_count_ = 0;
        std::size_t gate_count_;
        bool gate_open_ = false;
    };

    // 观测数据独立于 sink 所有权，销毁 sink 后仍能安全检查结果。
    struct Probe
    {
        std::mutex mutex;
        std::condition_variable cv;
        std::vector<std::string> messages;
        std::vector<std::string> formatted;
        std::size_t flushes = 0;
        std::size_t destroyed = 0;
        bool throw_flush = false;
        std::function<void()> on_flush;

        bool wait_flush(std::size_t count = 1)
        {
            std::unique_lock<std::mutex> lock(mutex);
            return cv.wait_for(lock, std::chrono::seconds(5), [&] { return flushes >= count; });
        }
        bool wait_destroyed()
        {
            std::unique_lock<std::mutex> lock(mutex);
            return cv.wait_for(lock, std::chrono::seconds(5), [&] { return destroyed > 0; });
        }
    };

    class probe_sink final : public spdlog::sinks::base_sink<std::mutex>
    {
    public:
        explicit probe_sink(std::shared_ptr<Probe> probe) : probe_(std::move(probe)) {}
        ~probe_sink()
        {
            std::lock_guard<std::mutex> lock(probe_->mutex);
            ++probe_->destroyed;
            probe_->cv.notify_all();
        }
    protected:
        void sink_it_(const spdlog::details::log_msg &msg) override
        {
            spdlog::memory_buf_t text;
            formatter_->format(msg, text);
            std::lock_guard<std::mutex> lock(probe_->mutex);
            probe_->messages.emplace_back(msg.payload.data(), msg.payload.size());
            probe_->formatted.emplace_back(text.data(), text.size());
            probe_->cv.notify_all();
        }
        void flush_() override
        {
            if (probe_->on_flush)
                probe_->on_flush();
            {
                std::lock_guard<std::mutex> lock(probe_->mutex);
                ++probe_->flushes;
                probe_->cv.notify_all();
            }
            if (probe_->throw_flush)
                throw std::runtime_error("injected flush failure");
        }
    private:
        std::shared_ptr<Probe> probe_;
    };

    LogConfig probe_config(const std::string &name, const std::shared_ptr<Probe> &probe)
    {
        LogConfig cfg;
        cfg.logger_name = name;
        cfg.enable_console = false;
        cfg.enable_file = false;
        cfg.level = Level::Trace;
        cfg.flush_level = Level::Off;
        cfg.pattern = "%v";
        SinkConfig sink;
        sink.factory = [probe] { return std::make_unique<probe_sink>(probe); };
        cfg.extra_sinks.push_back(sink);
        return cfg;
    }

    class prefix_formatter final : public spdlog::formatter
    {
    public:
        explicit prefix_formatter(std::shared_ptr<int> clones) : clones_(std::move(clones)) {}
        void format(const spdlog::details::log_msg &msg, spdlog::memory_buf_t &dest) override
        {
            const std::string text = "custom:" + std::string(msg.payload.data(), msg.payload.size()) + "\n";
            dest.append(text.data(), text.data() + text.size());
        }
        std::unique_ptr<spdlog::formatter> clone() const override
        {
            ++*clones_;
            return std::make_unique<prefix_formatter>(clones_);
        }
    private:
        std::shared_ptr<int> clones_;
    };

    std::string file_text(const fs::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
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
        LOGS_INFO("first");

        // 旧实例仍持有同一文件时，重新初始化必须被拒绝，
        // 而不是并行开第二个轮转器
        auto blocked = SingletonLogger::init_checked(cfg);
        CHECK(!blocked.ok);
        CHECK(blocked.error == InitError::FileSink);
        CHECK(blocked.message.find("still in use") != std::string::npos);
        LOGS_INFO("old instance still writable");

        SingletonLogger::shutdown();
        SingletonLogger::shutdown(); // 重复关闭必须安全

        // 完全释放后可以重新占用同一文件，旧内容不会被截断
        CHECK(SingletonLogger::init(cfg));
        // 旧实例关闭时不能把新实例从 registry 里误删
        CHECK(spdlog::get("repeat") != nullptr);
        LOGS_INFO("second");
        SingletonLogger::shutdown();

        CHECK(count_lines(dir / "repeat.log") == 3);
    }

    void test_same_file_held_by_snapshot()
    {
        const fs::path dir = kBase / "held";
        LogConfig cfg;
        cfg.logger_name = "held";
        cfg.enable_console = false;
        cfg.enable_file = true;
        cfg.log_dir = dir.string();
        cfg.log_file = "held.log";
        cfg.pattern = "%v";

        CHECK(SingletonLogger::init(cfg));
        LOGS_INFO("before-reinit");

        auto snapshot = SingletonLogger::acquire();
        CHECK(snapshot != nullptr);
        SingletonLogger::shutdown(); // 只解除全局持有，快照仍占用文件

        auto blocked = SingletonLogger::init_checked(cfg);
        CHECK(!blocked.ok);
        CHECK(blocked.error == InitError::FileSink);

        // 旧快照仍然可写，已有内容没有被截断
        snapshot->log(MYLOG_SRC_LOC, Level::Info, "still alive");
        snapshot->flush();
        CHECK(count_lines(dir / "held.log") == 2);

        snapshot.reset(); // 文件句柄在此关闭，占用归还

        CHECK(SingletonLogger::init(cfg));
        LOGS_INFO("reopened");
        SingletonLogger::shutdown();
        CHECK(count_lines(dir / "held.log") == 3);
    }

    void test_same_file_aliases()
    {
        const fs::path dir = kBase / "alias";
        LogConfig first;
        first.logger_name = "alias_a";
        first.enable_console = false;
        first.enable_file = true;
        first.log_dir = dir.string();
        first.log_file = "alias.log";
        CHECK(SingletonLogger::init(first));

        // 不同实例名、不同写法指向同一个文件时，必须一样被拒绝
        LogConfig other_name = first;
        other_name.logger_name = "alias_b";
        auto r1 = SingletonLogger::init_checked(other_name);
        CHECK(!r1.ok);
        CHECK(r1.error == InitError::FileSink);

        LogConfig dotted = first;
        dotted.logger_name = "alias_c";
        dotted.log_dir = (kBase / "." / "alias" / ".." / "alias").string();
        CHECK(!SingletonLogger::init_checked(dotted).ok);

        LogConfig absolute = first;
        absolute.logger_name = "alias_d";
        absolute.log_dir = fs::absolute(dir).string();
        CHECK(!SingletonLogger::init_checked(absolute).ok);

#ifdef _WIN32
        LogConfig upper = first;
        upper.logger_name = "alias_e";
        std::string upper_dir = fs::absolute(dir).string();
        for (char &c : upper_dir)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        upper.log_dir = upper_dir;
        CHECK(!SingletonLogger::init_checked(upper).ok);
#endif

        SingletonLogger::shutdown();

        // 释放后可以重新占用
        CHECK(SingletonLogger::init(other_name));
        SingletonLogger::shutdown();
    }

    void test_concurrent_same_file_init()
    {
        const fs::path dir = kBase / "contend";
        LogConfig cfg;
        cfg.logger_name = "contend";
        cfg.enable_console = false;
        cfg.enable_file = true;
        cfg.log_dir = dir.string();
        cfg.log_file = "contend.log";

        std::atomic<bool> start{false};
        std::mutex winners_mutex;
        std::vector<std::unique_ptr<Logger>> winners;

        std::vector<std::thread> threads;
        for (int i = 0; i < 8; ++i)
        {
            threads.emplace_back([&]
                                 {
                                     while (!start.load(std::memory_order_acquire))
                                         std::this_thread::yield();
                                     Logger candidate(cfg);
                                     if (!candidate.is_initialized())
                                         return;
                                     // 保留胜出的实例，保证所有线程尝试期间它一直占着文件
                                     std::lock_guard<std::mutex> lock(winners_mutex);
                                     winners.push_back(std::make_unique<Logger>(std::move(candidate)));
                                 });
        }
        start.store(true, std::memory_order_release);
        for (auto &t : threads)
            t.join();

        CHECK(winners.size() == 1);

        winners.clear(); // 释放后可以重新占用
        CHECK(SingletonLogger::init(cfg));
        SingletonLogger::shutdown();
    }

    void test_size_rotation()
    {
        const fs::path dir = kBase / "rotate";
        LogConfig cfg;
        cfg.logger_name = "rotate";
        cfg.enable_console = false;
        cfg.enable_file = true;
        cfg.log_dir = dir.string();
        cfg.log_file = "rotate.log";
        cfg.pattern = "%v";
        cfg.max_file_size = 64;
        cfg.max_files = 2;

        CHECK(SingletonLogger::init(cfg));
        for (int i = 0; i < 50; ++i)
            LOGS_INFO("rotation line {:04d}", i);
        SingletonLogger::shutdown();

        CHECK(fs::exists(dir / "rotate.1.log"));
    }

    // 关闭不应在覆盖策略下额外挤掉已入队的日志（P2 回归）。
    // 判定依据：修复前关闭的 flush 会立即覆盖 B（overrun 增加且 B 丢失）。
    void test_close_does_not_overwrite_queued_log(OverflowPolicy policy, const char *name,
                                                  bool expect_flush)
    {
        LogConfig cfg;
        cfg.logger_name = name;
        cfg.enable_console = false;
        cfg.enable_file = false; // 只用测试 sink，避免文件 I/O
        cfg.async = true;
        cfg.async_queue_size = 1;
        cfg.async_thread_count = 1;
        cfg.async_overflow = policy;
        cfg.level = Level::Trace;
        cfg.pattern = "%v";

        // 在初始化时注入受控 sink，不再修改运行中的目标容器。
        gated_sink *sink = nullptr;
        SinkConfig extra;
        extra.factory = [&sink]
        {
            auto owned = std::make_unique<gated_sink>(1);
            sink = owned.get();
            return owned;
        };
        cfg.extra_sinks.push_back(extra);
        CHECK(SingletonLogger::init(cfg));
        auto registered = spdlog::get(name);
        CHECK(registered != nullptr);
        if (!registered)
            return;

        auto snapshot = SingletonLogger::acquire();
        CHECK(snapshot != nullptr);
        if (!snapshot)
            return;

        LOGS_INFO("A");
        CHECK(sink->wait_until_entered()); // 后台线程已停在 A 上

        LOGS_INFO("B"); // 队列容量为 1，B 留在队列里
        CHECK(snapshot->stats().queue_size == 1);

        // 直接调用实例的 shutdown()：它走的正是“关闭时 flush”的 release() 路径
        auto holder = snapshot;
        std::thread closer([holder]
                           { holder->shutdown(); });

        // 给关闭线程足够时间走到“投递 flush”。这一等待只影响测试能否抓住旧行为，
        // 不影响最终判定方向：若 flush 以覆盖/丢弃方式入队，B 会永久丢失。
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        sink->open_gate();
        closer.join();
        snapshot.reset();
        SingletonLogger::shutdown();

        CHECK(sink->messages().size() >= 2);
        CHECK(sink->flush_count() > 0 || !expect_flush);

        bool saw_b = false;
        for (const auto &text : sink->messages())
        {
            if (text == "B")
                saw_b = true;
        }
        CHECK(saw_b);
    }

    // 正常过载时 OverrunOldest 依然丢最旧的日志：关闭修正不能把运行期策略改成阻塞
    void test_normal_overrun_still_drops_oldest()
    {
        LogConfig cfg;
        cfg.logger_name = "normal_overrun";
        cfg.enable_console = false;
        cfg.enable_file = false;
        cfg.async = true;
        cfg.async_queue_size = 1;
        cfg.async_thread_count = 1;
        cfg.async_overflow = OverflowPolicy::OverrunOldest;
        cfg.level = Level::Trace;
        cfg.pattern = "%v";

        gated_sink *sink = nullptr;
        SinkConfig extra;
        extra.factory = [&sink]
        {
            auto owned = std::make_unique<gated_sink>(1);
            sink = owned.get();
            return owned;
        };
        cfg.extra_sinks.push_back(extra);
        CHECK(SingletonLogger::init(cfg));
        auto registered = spdlog::get("normal_overrun");
        CHECK(registered != nullptr);
        if (!registered)
        {
            SingletonLogger::shutdown();
            return;
        }

        auto snapshot = SingletonLogger::acquire();
        CHECK(snapshot != nullptr);
        if (!snapshot)
        {
            SingletonLogger::shutdown();
            return;
        }

        LOGS_INFO("A");
        CHECK(sink->wait_until_entered());
        LOGS_INFO("B"); // 入队
        LOGS_INFO("C"); // 容量为 1，旧策略下 B 被覆盖
        CHECK(snapshot->stats().overrun_count == 1);

        sink->open_gate();
        SingletonLogger::shutdown(); // 关闭不应再额外丢
        snapshot.reset();            // 最后一个引用释放时才真正 flush 并回收

        bool saw_b = false;
        bool saw_c = false;
        for (const auto &text : sink->messages())
        {
            if (text == "B")
                saw_b = true;
            if (text == "C")
                saw_c = true;
        }
        CHECK(!saw_b); // 正常过载造成的丢失仍然存在
        CHECK(saw_c);  // 关闭没有挤掉关闭前已入队的那条
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

    void test_failed_init_releases_file()
    {
        const fs::path dir = kBase / "reclaim";
        std::error_code ec;
        fs::create_directories(dir, ec);
        // 先把日志文件位置占成目录，文件 sink 必然打开失败
        fs::create_directories(dir / "blocked.log", ec);

        LogConfig cfg;
        cfg.logger_name = "reclaim";
        cfg.enable_console = false;
        cfg.enable_file = true;
        cfg.log_dir = dir.string();
        cfg.log_file = "blocked.log";

        auto failed = SingletonLogger::init_checked(cfg);
        CHECK(!failed.ok);
        CHECK(failed.error == InitError::FileSink);

        // 构造失败必须通过 RAII 归还路径占用，否则这里会一直报 “still in use”
        fs::remove(dir / "blocked.log", ec);
        auto retried = SingletonLogger::init_checked(cfg);
        CHECK(retried.ok);
        CHECK(retried.error == InitError::None);

        LOGS_INFO("reclaimed");
        SingletonLogger::shutdown();
        CHECK(count_lines(dir / "blocked.log") == 1);
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

        // 按天轮转同样遵守同一基础路径占用规则
        auto blocked = SingletonLogger::init_checked(cfg);
        CHECK(!blocked.ok);
        CHECK(blocked.error == InitError::FileSink);

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

    // CTest 捕获真实 stdout，检查门槛和格式；不替换内置控制台 sink。
    int test_console_output()
    {
        LogConfig cfg;
        cfg.logger_name = "console_probe";
        cfg.enable_file = false;
        cfg.level = Level::Debug;
        cfg.console_level = Level::Warn;
        cfg.pattern = "inherit:%v";
        Logger logger(cfg);
        CHECK(logger.is_initialized());
        LOG_DEBUG(logger, "forbidden-console");
        LOG_TRACE(logger, "forbidden-total");
        LOG_WARN(logger, "first");
        logger.set_console_pattern("console:%l:%v");
        LOG_WARN(logger, "dedicated");
        logger.set_level(Level::Error);
        LOG_WARN(logger, "forbidden-total-after-change");
        logger.set_console_level(Level::Trace);
        LOG_WARN(logger, "forbidden-total-after-sink-lowered");
        logger.set_level(Level::Trace);
        LOG_DEBUG(logger, "lowered");
        logger.set_formatter(std::make_unique<prefix_formatter>(std::make_shared<int>(0)));
        LOG_INFO(logger, "formatter");
        logger.set_console_pattern("local:%v");
        LOG_INFO(logger, "override-formatter");
        logger.set_pattern("all:%v");
        LOG_INFO(logger, "unified");
        logger.set_file_pattern("forbidden-absent-file");
        LOG_INFO(logger, "still-unified");
        logger.set_console_pattern("");
        LOG_ERROR(logger, "forbidden-empty-runtime");
        logger.set_console_pattern("final:%v");
        logger.set_console_level(Level::Off);
        LOG_CRITICAL(logger, "forbidden-off");
        logger.set_console_level(Level::Error);
        LOG_ERROR(logger, "restored");
        logger.shutdown();

        cfg.console_pattern = "";
        cfg.pattern = "forbidden-inheritance:%v";
        {
            Logger blank(cfg);
            CHECK(blank.is_initialized());
            LOG_WARN(blank, "forbidden-empty-initial");
        }
        cfg.console_pattern = "configured:%v";
        {
            Logger configured(cfg);
            CHECK(configured.is_initialized());
            LOG_WARN(configured, "initial-specific");
        }
        if (g_failures == 0)
            std::printf("console-probe-passed\n");
        return g_failures != 0;
    }

    void test_independent_outputs()
    {
        auto probe = std::make_shared<Probe>();
        auto cfg = probe_config("independent", probe);
        cfg.enable_console = true;
        cfg.enable_file = true;
        cfg.log_dir = (kBase / "independent").string();
        cfg.log_file = "file.log";
        cfg.rotation = LogConfig::RotationPolicy::None;
        cfg.level = Level::Debug;
        cfg.console_level = Level::Warn;
        cfg.file_level = Level::Debug;
        cfg.console_pattern = "C:%v";
        cfg.file_pattern = "F:%v";
        cfg.extra_sinks[0].pattern = "P:%v";
        Logger logger(cfg);
        CHECK(logger.is_initialized());
        auto raw = spdlog::get(cfg.logger_name);
        CHECK(raw && raw->sinks().size() == 3);
        if (!raw) return;
        CHECK(!raw->sinks()[0]->should_log(spdlog::level::debug));
        CHECK(raw->sinks()[1]->should_log(spdlog::level::debug));
        LOG_TRACE(logger, "filtered-by-total");
        LOG_DEBUG(logger, "debug");
        logger.set_file_level(Level::Error);
        logger.set_console_level(Level::Off);
        logger.set_console_pattern("changed:%v");
        CHECK(raw->sinks()[0]->level() == spdlog::level::off);
        LOG_INFO(logger, "probe-only");
        logger.set_pattern("ALL:%v");
        LOG_ERROR(logger, "all");
        logger.set_file_pattern("FILE:%v");
        LOG_ERROR(logger, "file-specific");
        logger.set_file_pattern("");
        LOG_ERROR(logger, "empty-pattern");
        logger.flush();
        const auto text = file_text(kBase / "independent" / "file.log");
        CHECK(text.find("F:debug") != std::string::npos);
        CHECK(text.find("probe-only") == std::string::npos);
        CHECK(text.find("ALL:all") != std::string::npos);
        CHECK(text.find("FILE:file-specific") != std::string::npos);
        CHECK(text.find("empty-pattern") == std::string::npos);
        CHECK(probe->messages.size() == 5);
        CHECK(probe->formatted[0].find("P:debug") == 0);
        CHECK(probe->formatted.back().find("ALL:empty-pattern") == 0);
        logger.shutdown();
    }

    void test_formatter_and_extra_only()
    {
        auto first = std::make_shared<Probe>();
        auto second = std::make_shared<Probe>();
        auto cfg = probe_config("formatter", first);
        cfg.extra_sinks.push_back(probe_config("unused", second).extra_sinks[0]);
        cfg.extra_sinks[1].pattern = "second:%v";
        Logger logger(cfg);
        CHECK(logger.is_initialized());
        CHECK(!logger.init_result().degraded);
        CHECK(spdlog::get(cfg.logger_name)->sinks().size() == 2);
        LOG_INFO(logger, "initial");
        auto clones = std::make_shared<int>(0);
        logger.set_formatter(std::make_unique<prefix_formatter>(clones));
        CHECK(*clones == 1);
        logger.set_formatter(nullptr);
        LOG_INFO(logger, "custom");
        logger.set_pattern("reset:%v");
        logger.set_console_pattern("absent");
        logger.set_file_pattern("absent");
        logger.set_console_level(Level::Off);
        logger.set_file_level(Level::Off);
        LOG_INFO(logger, "normal");
        logger.shutdown();
        CHECK(first->formatted.size() == 3 && second->formatted.size() == 3);
        CHECK(first->formatted[1] == "custom:custom\n");
        CHECK(second->formatted[1] == first->formatted[1]);
        CHECK(second->formatted[0].find("second:initial") == 0);
        CHECK(first->formatted[2].find("reset:normal") == 0);
        CHECK(first->destroyed == 1 && second->destroyed == 1);
    }

    SinkConfig file_target(const fs::path &path)
    {
        SinkConfig sink;
        sink.file_paths.push_back(path.string());
        sink.factory = [path]
        {
            return std::make_unique<spdlog::sinks::basic_file_sink_mt>(path.string(), false);
        };
        return sink;
    }

    void test_extra_file_leases_and_failures()
    {
        const auto first = kBase / "extra_files" / "first.log";
        const auto second = kBase / "extra_files" / "second.log";
        LogConfig cfg;
        cfg.logger_name = "extra_files";
        cfg.enable_console = false;
        cfg.enable_file = false;
        cfg.pattern = "%v";
        cfg.extra_sinks = {file_target(first), file_target(second)};
        Logger logger(cfg);
        CHECK(logger.is_initialized());
        LOG_INFO(logger, "both");
        logger.flush();
        CHECK(file_text(first).find("both") != std::string::npos);
        CHECK(file_text(second).find("both") != std::string::npos);

        auto conflict = cfg;
        conflict.logger_name = "extra_conflict";
        conflict.fallback_to_console_on_file_error = true;
        Logger blocked(conflict);
        CHECK(!blocked.is_initialized());
        CHECK(blocked.init_result().error == InitError::FileSink);
        CHECK(!blocked.init_result().degraded);
        auto raw = spdlog::get(cfg.logger_name);
        logger.shutdown();
        Logger held(conflict);
        CHECK(!held.is_initialized());
        raw.reset();
        Logger reopened(conflict);
        CHECK(reopened.is_initialized());
        reopened.shutdown();

        int factories = 0;
        auto duplicate = cfg;
        duplicate.logger_name = "duplicate_extra";
        duplicate.extra_sinks[1].file_paths = {(kBase / "extra_files" / "." / "first.log").string()};
        duplicate.extra_sinks[0].factory = [&]() -> std::unique_ptr<spdlog::sinks::sink>
        {
            ++factories;
            return std::make_unique<spdlog::sinks::basic_file_sink_mt>(first.string(), false);
        };
        Logger dup(duplicate);
        CHECK(dup.init_result().error == InitError::FileSink);
        CHECK(factories == 0);
        duplicate = cfg;
        duplicate.enable_file = true;
        duplicate.log_dir = first.parent_path().string();
        duplicate.log_file = first.filename().string();
        Logger primary_dup(duplicate);
        CHECK(primary_dup.init_result().error == InitError::FileSink);

        auto bad = cfg;
        bad.logger_name = "bad_factory";
        bad.extra_sinks[1].factory = []() -> std::unique_ptr<spdlog::sinks::sink>
        { throw std::runtime_error("factory failure"); };
        Logger failed(bad);
        CHECK(failed.init_result().error == InitError::Unknown);
        // 失败实例仍然活着，也必须已归还所有路径。
        Logger recovered(cfg);
        CHECK(recovered.is_initialized());
        recovered.shutdown();
        bad.extra_sinks[1].factory = []() -> std::unique_ptr<spdlog::sinks::sink> { return nullptr; };
        Logger null_sink(bad);
        CHECK(null_sink.init_result().error == InitError::InvalidConfig);
        bad.extra_sinks[1].factory = {};
        Logger no_factory(bad);
        CHECK(no_factory.init_result().error == InitError::InvalidConfig);
        bad = cfg;
        bad.extra_sinks[1].file_paths = {""};
        Logger empty_path(bad);
        CHECK(empty_path.init_result().error == InitError::InvalidConfig);
        bad = cfg;
        bad.extra_sinks[1].factory = []() -> std::unique_ptr<spdlog::sinks::sink> { throw 42; };
        Logger unknown(bad);
        CHECK(unknown.init_result().error == InitError::Unknown);
        Logger after_all(cfg);
        CHECK(after_all.is_initialized());
    }

    void test_new_file_policies()
    {
        LogConfig cfg;
        cfg.logger_name = "append";
        cfg.log_dir = (kBase / "append").string();
        cfg.log_file = "append.log";
        cfg.enable_console = false;
        cfg.rotation = LogConfig::RotationPolicy::None;
        cfg.pattern = "%v";
        for (int i = 0; i < 2; ++i)
        {
            Logger logger(cfg);
            CHECK(logger.is_initialized());
            LOG_INFO(logger, "append {}", i);
            auto conflict = cfg;
            conflict.logger_name += "_blocked";
            Logger blocked(conflict);
            CHECK(blocked.init_result().error == InitError::FileSink);
        }
        CHECK(count_lines(kBase / "append" / "append.log") == 2);

        cfg.logger_name = "hourly";
        cfg.log_dir = (kBase / "hourly").string();
        cfg.log_file = "hourly.log";
        cfg.rotation = LogConfig::RotationPolicy::Hourly;
        cfg.hourly_max_files = 2;
        Logger logger(cfg);
        CHECK(logger.is_initialized());
        auto raw = spdlog::get(cfg.logger_name);
        CHECK(raw != nullptr);
        if (!raw) return;
        const auto now = spdlog::log_clock::now();
        for (int i = 0; i < 4; ++i)
            raw->log(now + std::chrono::hours(i), {}, spdlog::level::info, "hourly-line");
        auto conflict = cfg;
        conflict.logger_name += "_blocked";
        Logger blocked(conflict);
        CHECK(blocked.init_result().error == InitError::FileSink);
        logger.shutdown();
        raw.reset();
        std::size_t files = 0;
        for (const auto &entry : fs::directory_iterator(kBase / "hourly"))
        {
            CHECK(entry.path().filename().string().find("hourly_") == 0);
            CHECK(file_text(entry.path()).find("hourly-line") != std::string::npos);
            ++files;
        }
        CHECK(files == 2);
        for (int i = 2; i < 4; ++i)
        {
            const auto timestamp = spdlog::log_clock::to_time_t(now + std::chrono::hours(i));
            std::tm local{};
#ifdef _WIN32
            localtime_s(&local, &timestamp);
#else
            localtime_r(&timestamp, &local);
#endif
            char filename[80]{};
            std::strftime(filename, sizeof(filename), "hourly_%Y-%m-%d_%H.log", &local);
            CHECK(fs::exists(kBase / "hourly" / filename));
        }
        cfg.hourly_max_files = 65536;
        Logger invalid(cfg);
        CHECK(invalid.init_result().error == InitError::InvalidConfig);
    }

    void test_backtrace(bool async)
    {
        auto probe = std::make_shared<Probe>();
        auto cfg = probe_config(async ? "backtrace_async" : "backtrace_sync", probe);
        cfg.level = Level::Info;
        cfg.async = async;
        cfg.backtrace_capacity = 3;
        Logger logger(cfg);
        CHECK(logger.is_initialized());
        for (int i = 0; i < 5; ++i)
            LOG_DEBUG(logger, "trace{}", i);
        logger.dump_backtrace();
        logger.dump_backtrace(); // 空缓冲不重复导出
        logger.disable_backtrace();
        LOG_DEBUG(logger, "disabled");
        logger.dump_backtrace();
        logger.enable_backtrace(1);
        LOG_DEBUG(logger, "new");
        logger.dump_backtrace();
        logger.enable_backtrace(0);
        LOG_DEBUG(logger, "zero-disabled");
        logger.dump_backtrace();
        logger.shutdown();
        CHECK(probe->messages.size() == 8); // 两组开始/结束标记 + 4 条正文
        CHECK(std::count(probe->messages.begin(), probe->messages.end(), "trace2") == 1);
        CHECK(std::count(probe->messages.begin(), probe->messages.end(), "trace4") == 1);
        CHECK(std::count(probe->messages.begin(), probe->messages.end(), "new") == 1);
        CHECK(std::count(probe->messages.begin(), probe->messages.end(), "trace0") == 0);
        CHECK(std::count(probe->messages.begin(), probe->messages.end(), "disabled") == 0);

        auto limited = std::make_shared<Probe>();
        cfg = probe_config("backtrace_limited", limited);
        cfg.level = Level::Info;
        cfg.backtrace_capacity = 2;
        cfg.extra_sinks[0].level = Level::Warn;
        Logger filtered(cfg);
        LOG_DEBUG(filtered, "sink-filtered");
        filtered.dump_backtrace();
        CHECK(limited->messages.empty());
    }

    void test_callback_safety(bool async)
    {
        auto probe = std::make_shared<Probe>();
        auto cfg = probe_config(async ? "callback_async" : "callback_sync", probe);
        cfg.async = async;
        cfg.level = Level::Warn;
        cfg.callback_level = Level::Error;
        cfg.enable_console = true;
        cfg.console_level = Level::Off;
        cfg.enable_file = true;
        cfg.file_level = Level::Off;
        cfg.log_dir = (kBase / cfg.logger_name).string();
        cfg.log_file = "muted.log";
        Logger *same = nullptr;
        std::atomic<int> calls{0};
        std::atomic<int> errors{0};
        std::atomic<bool> correct_thread{true};
        const auto caller = std::this_thread::get_id();
        std::mutex events_mutex;
        std::vector<LogEvent> events;
        cfg.log_callback = [&](const LogEvent &event)
        {
            ++calls;
            if (same->is_on_worker_thread() != async ||
                ((std::this_thread::get_id() == caller) == async))
                correct_thread = false;
            {
                std::lock_guard<std::mutex> lock(events_mutex);
                events.push_back(event);
            }
            LOG_ERROR(*same, "forbidden-recursion");
            same->log(MYLOG_SRC_LOC, Level::Error, std::string_view("forbidden-string-view"));
            same->flush();
            same->set_level(Level::Off);
            same->set_flush_level(Level::Trace);
            same->set_pattern("forbidden-pattern");
            same->set_file_level(Level::Off);
            same->set_console_level(Level::Off);
            same->set_file_pattern("forbidden");
            same->set_console_pattern("forbidden");
            same->set_formatter(std::make_unique<prefix_formatter>(std::make_shared<int>(0)));
            same->enable_backtrace(1);
            same->disable_backtrace();
            same->dump_backtrace();
            same->shutdown();
            throw std::runtime_error("callback failure");
        };
        cfg.error_handler = [&](const std::string &)
        {
            ++errors;
            LOG_ERROR(*same, "forbidden-error-handler-recursion");
        };
        Logger logger(cfg);
        same = &logger;
        CHECK(logger.is_initialized());
        LOG_INFO(logger, "below-total");
        LOG_WARN(logger, "no-callback");
        LOG_ERROR(logger, "event{}", 1);
        LOG_CRITICAL(logger, "event2");
        logger.shutdown();
        CHECK(calls == 2 && errors == 2 && correct_thread);
        CHECK(events.size() == 2);
        CHECK(events[0].message == "event1" && events[0].logger_name == cfg.logger_name);
        CHECK(events[0].level == Level::Error);
        CHECK(probe->messages.size() == 3);
        CHECK(probe->formatted.back().find("event2") == 0);
        CHECK(file_text(fs::path(cfg.log_dir) / cfg.log_file).empty());

        auto filtered_probe = std::make_shared<Probe>();
        cfg = probe_config("callback_total", filtered_probe);
        cfg.async = async;
        cfg.level = Level::Critical;
        cfg.callback_level = Level::Error;
        std::atomic<int> filtered_calls{0};
        cfg.log_callback = [&](const LogEvent &) { ++filtered_calls; };
        Logger filtered(cfg);
        LOG_ERROR(filtered, "below-total-but-above-callback");
        LOG_CRITICAL(filtered, "passes-both");
        filtered.shutdown();
        CHECK(filtered_calls == 1);
        CHECK(filtered_probe->messages == std::vector<std::string>({"passes-both"}));
    }

    void test_nested_callbacks_and_backtrace()
    {
        auto a = std::make_shared<Probe>();
        auto b = std::make_shared<Probe>();
        auto ca = probe_config("nested_a", a);
        auto cb = probe_config("nested_b", b);
        Logger *first = nullptr;
        Logger *second = nullptr;
        int a_calls = 0, b_calls = 0;
        ca.callback_level = Level::Trace;
        cb.callback_level = Level::Trace;
        ca.level = Level::Info;
        ca.backtrace_capacity = 1;
        ca.log_callback = [&](const LogEvent &)
        {
            ++a_calls;
            LOG_ERROR(*second, "nested-b");
        };
        cb.log_callback = [&](const LogEvent &)
        {
            ++b_calls;
            LOG_ERROR(*first, "must-not-reenter-a");
            first->dump_backtrace();
        };
        Logger first_logger(ca);
        Logger second_logger(cb);
        first = &first_logger;
        second = &second_logger;
        LOG_DEBUG(first_logger, "cached");
        first_logger.dump_backtrace(); // 底层回溯锁被持有时也不能回调重入
        CHECK(a_calls == 3 && b_calls == 3);
        CHECK(a->messages.size() == 3);
        CHECK(std::count(a->messages.begin(), a->messages.end(), "cached") == 1);
    }

    void test_discarded_messages_have_no_callback()
    {
        LogConfig cfg;
        cfg.logger_name = "discard_callback";
        cfg.enable_console = false;
        cfg.enable_file = false;
        cfg.async = true;
        cfg.async_queue_size = 1;
        cfg.async_overflow = OverflowPolicy::DiscardNew;
        cfg.callback_level = Level::Trace;
        std::vector<std::string> events;
        cfg.log_callback = [&](const LogEvent &event) { events.push_back(event.message); };
        gated_sink *sink = nullptr;
        SinkConfig extra;
        extra.factory = [&]
        {
            auto owned = std::make_unique<gated_sink>(1);
            sink = owned.get();
            return owned;
        };
        cfg.extra_sinks.push_back(extra);
        Logger logger(cfg);
        LOG_INFO(logger, "A");
        CHECK(sink->wait_until_entered());
        LOG_INFO(logger, "B");
        LOG_INFO(logger, "C");
        CHECK(logger.stats().discard_count == 1);
        sink->open_gate();
        logger.shutdown();
        CHECK(events == std::vector<std::string>({"A", "B"}));
    }

    void test_periodic_flush(bool async)
    {
        auto probe = std::make_shared<Probe>();
        auto cfg = probe_config(async ? "periodic_async" : "periodic_sync", probe);
        cfg.async = async;
        cfg.flush_interval = std::chrono::milliseconds(20);
        Logger original(cfg);
        CHECK(original.is_initialized());
        CHECK(probe->wait_flush());
        Logger moved(std::move(original));
        CHECK(!original.is_initialized());
        auto untouched = std::make_shared<Probe>();
        Logger other(probe_config("no_periodic", untouched));
        std::size_t next;
        {
            std::lock_guard<std::mutex> lock(probe->mutex);
            next = probe->flushes + 1;
        }
        CHECK(probe->wait_flush(next));
        CHECK(untouched->flushes == 0);
        LOG_INFO(moved, "after-move");
        moved.shutdown();
        CHECK(probe->destroyed == 1);
        CHECK(probe->messages == std::vector<std::string>({"after-move"}));
        const auto previous = probe->flushes;
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        CHECK(probe->flushes == previous);

        auto long_probe = std::make_shared<Probe>();
        cfg = probe_config("long_period", long_probe);
        cfg.flush_interval = std::chrono::minutes(5);
        Logger long_period(cfg);
        const auto start = std::chrono::steady_clock::now();
        long_period.shutdown();
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
        CHECK(long_probe->destroyed == 1);
        cfg.flush_interval = std::chrono::milliseconds(-1);
        Logger negative(cfg);
        CHECK(negative.init_result().error == InitError::InvalidConfig);
    }

    void test_periodic_full_queue(OverflowPolicy policy)
    {
        LogConfig cfg;
        cfg.logger_name = "periodic_full";
        cfg.enable_console = false;
        cfg.enable_file = false;
        cfg.async = true;
        cfg.async_queue_size = 1;
        cfg.async_overflow = policy;
        cfg.flush_interval = std::chrono::milliseconds(100);
        gated_sink *sink = nullptr;
        SinkConfig extra;
        extra.factory = [&]
        {
            auto owned = std::make_unique<gated_sink>(1);
            sink = owned.get();
            return owned;
        };
        cfg.extra_sinks.push_back(extra);
        Logger logger(cfg);
        auto raw = spdlog::get(cfg.logger_name); // 仅保持 sink 存活，便于关闭后检查
        LOG_INFO(logger, "A");
        CHECK(sink->wait_until_entered());
        LOG_INFO(logger, "B");
        // 后台停在 A、队列被 B 占满；定时刷新不能覆盖 B 或增加丢弃统计。
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(logger.stats().overrun_count == 0);
        CHECK(logger.stats().discard_count == 0);
        sink->open_gate();
        logger.shutdown();
        CHECK(sink->messages() == std::vector<std::string>({"A", "B"}));
        CHECK(sink->flush_count() >= 2); // 已触发的周期刷新 + 关闭刷新
    }

    void test_background_self_shutdown(bool timer)
    {
        auto probe = std::make_shared<Probe>();
        auto cfg = probe_config(timer ? "timer_self_close" : "worker_self_close", probe);
        cfg.async = !timer;
        std::atomic<bool> acted{false};
        std::atomic<bool> correct{false};
        auto close_from_background = [&]
        {
            auto &borrowed = SingletonLogger::instance();
            if (!borrowed.is_initialized() || acted.exchange(true))
                return;
            correct = (borrowed.is_on_worker_thread() == !timer);
            borrowed.shutdown(); // 必须拒绝自关闭
            if (!borrowed.is_initialized())
                correct = false;
            SingletonLogger::shutdown(); // 实际回收由独立线程执行
        };
        if (timer)
        {
            probe->throw_flush = true;
            cfg.flush_interval = std::chrono::milliseconds(20);
            cfg.error_handler = [&](const std::string &) { close_from_background(); };
        }
        else
        {
            cfg.callback_level = Level::Info;
            cfg.log_callback = [&](const LogEvent &) { close_from_background(); };
        }
        CHECK(SingletonLogger::init(cfg));
        if (!timer)
            LOGS_INFO("self-close");
        CHECK(probe->wait_destroyed());
        CHECK(acted && correct);
        CHECK(!SingletonLogger::acquire());
        SingletonLogger::shutdown();
    }

    void test_new_empty_and_failure_paths()
    {
        auto &empty = SingletonLogger::instance();
        empty.set_console_level(Level::Debug);
        empty.set_file_level(Level::Debug);
        empty.set_console_pattern("x");
        empty.set_file_pattern("x");
        empty.set_formatter(std::make_unique<prefix_formatter>(std::make_shared<int>(0)));
        empty.enable_backtrace(2);
        empty.dump_backtrace();
        empty.disable_backtrace();
        CHECK(!empty.is_initialized());
        auto probe = std::make_shared<Probe>();
        auto cfg = probe_config("failure_with_timer", probe);
        cfg.flush_interval = std::chrono::milliseconds(1);
        SinkConfig bad;
        bad.factory = []() -> std::unique_ptr<spdlog::sinks::sink> { throw 1; };
        cfg.extra_sinks.push_back(bad);
        Logger failed(cfg);
        CHECK(!failed.is_initialized());
        CHECK(probe->destroyed == 1 && probe->flushes == 0);
        cfg.extra_sinks.pop_back();
        cfg.console_level = static_cast<Level>(99);
        Logger invalid_level(cfg);
        CHECK(invalid_level.init_result().error == InitError::InvalidConfig);
    }

    void test_late_init_failure_cleanup(bool async)
    {
        auto probe = std::make_shared<Probe>();
        auto cfg = probe_config(async ? "late_failure_async" : "late_failure_sync", probe);
        cfg.async = async;
        cfg.enable_file = true;
        cfg.rotation = LogConfig::RotationPolicy::None;
        cfg.log_dir = (kBase / cfg.logger_name).string();
        cfg.log_file = "reusable.log";
        cfg.flush_interval = std::chrono::milliseconds(10);
        // vector 容量必然超限：在 sink/logger（以及异步池）创建后确定性失败。
        cfg.backtrace_capacity = std::numeric_limits<std::size_t>::max() / 2;
        cfg.error_handler = [](const std::string &) {};
        Logger failed(cfg);
        CHECK(!failed.is_initialized());
        CHECK(failed.init_result().error == InitError::Unknown);
        CHECK(probe->destroyed == 1 && probe->flushes == 0);
        CHECK(spdlog::get(cfg.logger_name) == nullptr);
        CHECK(failed.stats().queue_size == 0);
        cfg.backtrace_capacity = 0;
        Logger recovered(cfg); // 失败对象仍存活时，文件租约也必须已经归还
        CHECK(recovered.is_initialized());
        CHECK(probe->wait_flush());
        recovered.shutdown();
        CHECK(probe->destroyed == 2);
    }

    void test_callback_concurrency(bool async)
    {
        LogConfig cfg;
        cfg.logger_name = async ? "concurrent_callback_async" : "concurrent_callback_sync";
        cfg.enable_console = false;
        cfg.enable_file = false;
        cfg.async = async;
        cfg.async_thread_count = 3;
        cfg.callback_level = Level::Info;
        std::atomic<int> notifications{0};
        cfg.log_callback = [&](const LogEvent &) { ++notifications; };
        Logger logger(cfg);
        CHECK(logger.is_initialized() && !logger.init_result().degraded);
        std::vector<std::thread> writers;
        for (int t = 0; t < 4; ++t)
            writers.emplace_back([&logger]
            {
                for (int i = 0; i < 200; ++i)
                    LOG_INFO(logger, "message {}", i);
            });
        for (auto &writer : writers)
            writer.join();
        logger.shutdown();
        CHECK(notifications == 800);
    }

    void test_periodic_snapshot_and_failed_reinit()
    {
        auto probe = std::make_shared<Probe>();
        auto cfg = probe_config("periodic_snapshot", probe);
        cfg.flush_interval = std::chrono::milliseconds(20);
        cfg.async = true;
        CHECK(SingletonLogger::init(cfg));
        auto snapshot = SingletonLogger::acquire();
        auto failed_cfg = cfg;
        failed_cfg.logger_name = "failed_reinit";
        failed_cfg.extra_sinks[0].factory = []() -> std::unique_ptr<spdlog::sinks::sink>
        { throw std::runtime_error("expected reinit failure"); };
        CHECK(!SingletonLogger::init_checked(failed_cfg));
        CHECK(SingletonLogger::acquire() == snapshot);
        LOGS_INFO("still-original");
        SingletonLogger::shutdown();
        CHECK(probe->wait_flush());
        {
            std::lock_guard<std::mutex> lock(probe->mutex);
            CHECK(probe->destroyed == 0);
        }
        snapshot.reset();
        CHECK(probe->destroyed == 1);
        CHECK(probe->messages == std::vector<std::string>({"still-original"}));
    }

    void test_explicit_console_fallback_with_extra()
    {
        auto probe = std::make_shared<Probe>();
        auto cfg = probe_config("extra_with_fallback", probe);
        cfg.enable_file = true;
        cfg.log_dir = (kBase / "extra_fallback").string();
        cfg.log_file = "directory";
        fs::create_directories(fs::path(cfg.log_dir) / cfg.log_file);
        cfg.fallback_to_console_on_file_error = true;
        cfg.console_level = Level::Off;
        Logger logger(cfg);
        CHECK(logger.is_initialized() && logger.init_result().degraded);
        auto raw = spdlog::get(cfg.logger_name);
        CHECK(raw && raw->sinks().size() == 2);
        LOG_INFO(logger, "extra-still-works");
        CHECK(probe->messages.size() == 1);
    }

} // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && std::string_view(argv[1]) == "--console-probe")
        return test_console_output();

    std::error_code ec;
    fs::remove_all(kBase, ec);
    fs::create_directories(kBase, ec);

    CHECK(test_compile_levels() == 0);
    test_independent_outputs();
    test_formatter_and_extra_only();
    test_extra_file_leases_and_failures();
    test_new_file_policies();
    test_backtrace(false);
    test_backtrace(true);
    test_callback_safety(false);
    test_callback_safety(true);
    test_nested_callbacks_and_backtrace();
    test_discarded_messages_have_no_callback();
    test_periodic_flush(false);
    test_periodic_flush(true);
    test_periodic_full_queue(OverflowPolicy::Block);
    test_periodic_full_queue(OverflowPolicy::OverrunOldest);
    test_periodic_full_queue(OverflowPolicy::DiscardNew);
    test_background_self_shutdown(false);
    test_background_self_shutdown(true);
    test_new_empty_and_failure_paths();
    test_late_init_failure_cleanup(false);
    test_late_init_failure_cleanup(true);
    test_callback_concurrency(false);
    test_callback_concurrency(true);
    test_periodic_snapshot_and_failed_reinit();
    test_explicit_console_fallback_with_extra();
    test_empty_instance();
    test_init_failure_callback_reentry();
    test_directory_failure();
    test_file_sink_fallback();
    test_async_drain();
    test_borrowed_snapshot();
    test_overflow_discard();
    test_reinit_and_repeat_shutdown();
    test_same_file_held_by_snapshot();
    test_same_file_aliases();
    test_concurrent_same_file_init();
    test_size_rotation();
    test_failed_init_releases_file();
    test_level_and_stats();
    test_move_semantics();
    test_worker_thread_flag();
    test_close_does_not_overwrite_queued_log(OverflowPolicy::Block, "close_block", true);
    test_close_does_not_overwrite_queued_log(OverflowPolicy::OverrunOldest, "close_overrun", true);
    test_close_does_not_overwrite_queued_log(OverflowPolicy::DiscardNew, "close_discard", true);
    test_normal_overrun_still_drops_oldest();
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
#endif
