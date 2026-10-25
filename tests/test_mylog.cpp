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

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

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

    // 受控 sink 的观测数据独立于 sink 所有权：日志器销毁后仍能安全读结果。
    struct GatedProbe
    {
        std::mutex mutex;
        std::condition_variable cv;
        std::vector<std::string> messages;
        std::size_t flushes = 0;
        std::size_t entered = 0;
        bool gate_open = false;

        bool wait_entered()
        {
            std::unique_lock<std::mutex> lock(mutex);
            const bool ok = cv.wait_for(lock, std::chrono::seconds(5), [this]
                                        { return entered > 0; });
            if (!ok)
            {
                gate_open = true; // 失败后开闸，避免关闭流程被卡死
                cv.notify_all();
            }
            return ok;
        }

        void open_gate()
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                gate_open = true;
            }
            cv.notify_all();
        }

        std::vector<std::string> taken_messages()
        {
            std::lock_guard<std::mutex> lock(mutex);
            return messages;
        }

        std::size_t taken_flushes()
        {
            std::lock_guard<std::mutex> lock(mutex);
            return flushes;
        }
    };

    // 受控 sink：可以让后台线程停在指定条数的消息上，从而确定性地构造
    // “后台正在写 A、队列里已有一条 B”这种状态，而不依赖大量压测碰概率。
    class gated_sink final : public spdlog::sinks::base_sink<std::mutex>
    {
    public:
        gated_sink(std::shared_ptr<GatedProbe> probe, std::size_t gate_count)
            : probe_(std::move(probe)), gate_count_(gate_count) {}

    protected:
        void sink_it_(const spdlog::details::log_msg &msg) override
        {
            std::unique_lock<std::mutex> lock(probe_->mutex);
            if (probe_->entered < gate_count_)
            {
                ++probe_->entered;
                probe_->cv.notify_all();
                probe_->cv.wait(lock, [this]
                                { return probe_->gate_open; });
            }
            probe_->messages.emplace_back(msg.payload.data(), msg.payload.size());
        }

        void flush_() override
        {
            std::lock_guard<std::mutex> lock(probe_->mutex);
            ++probe_->flushes;
        }

    private:
        std::shared_ptr<GatedProbe> probe_;
        std::size_t gate_count_;
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

    // 观测数据独立于 sink 所有权：日志器销毁后仍能安全检查“写入 / 刷新”的先后顺序。
    struct OrderedProbe
    {
        std::mutex mutex;
        std::condition_variable cv;
        std::vector<std::string> events;
        bool entered = false;
        bool gate_open = false;
        bool flushed = false;

        bool wait_entered()
        {
            std::unique_lock<std::mutex> lock(mutex);
            const bool ok = cv.wait_for(lock, std::chrono::seconds(5), [this] { return entered; });
            if (!ok)
            {
                gate_open = true; // 失败后开闸，避免关闭流程被卡死
                cv.notify_all();
            }
            return ok;
        }

        void open_gate()
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                gate_open = true;
            }
            cv.notify_all();
        }

        bool wait_flush(std::chrono::milliseconds timeout)
        {
            std::unique_lock<std::mutex> lock(mutex);
            return cv.wait_for(lock, timeout, [this] { return flushed; });
        }
    };

    // 记录“写入 / 刷新”先后顺序的 sink。log() 里可以把消费线程停在写入之前，
    // 用来确定性地构造“另一条消费线程已经刷新、最后一条日志还没写”的时序。
    class ordered_sink final : public spdlog::sinks::sink
    {
    public:
        explicit ordered_sink(std::shared_ptr<OrderedProbe> probe) : probe_(std::move(probe)) {}

        void log(const spdlog::details::log_msg &msg) override
        {
            const std::string text(msg.payload.data(), msg.payload.size());
            if (text == "hold")
            {
                // 卡在“已从队列取出、还没写入”这一步，且不持有观测锁
                std::unique_lock<std::mutex> lock(probe_->mutex);
                probe_->entered = true;
                probe_->cv.notify_all();
                probe_->cv.wait(lock, [this] { return probe_->gate_open; });
            }
            push("w:" + text);
        }

        void flush() override
        {
            {
                std::lock_guard<std::mutex> lock(probe_->mutex);
                probe_->flushed = true;
            }
            probe_->cv.notify_all();
            push("f");
        }

        void set_pattern(const std::string &) override {}
        void set_formatter(std::unique_ptr<spdlog::formatter>) override {}

    private:
        void push(std::string event)
        {
            std::lock_guard<std::mutex> lock(probe_->mutex);
            probe_->events.push_back(std::move(event));
            probe_->cv.notify_all();
        }

        std::shared_ptr<OrderedProbe> probe_;
    };

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

    // 未初始化时的全局入口必须静默丢弃：不崩溃、不注册名字、不影响后续初始化。
    // （旧版这里还测了已删除的 SingletonLogger::instance() 借用引用）
    void test_uninitialized_is_silent()
    {
        spdlog::drop("app");
        LOGS_INFO("this must be dropped silently");
        CHECK(spdlog::get("app") == nullptr);
        CHECK(SingletonLogger::get() == nullptr || !SingletonLogger::get()->is_initialized());

        auto probe = std::make_shared<Probe>();
        auto cfg = probe_config("uninitialized_then_init", probe);
        CHECK(SingletonLogger::init(cfg));
        LOGS_INFO("after init");
        CHECK(spdlog::get(cfg.logger_name) != nullptr);

        SingletonLogger::shutdown();
        // 关闭后：对象仍存在（指针稳定），但内部资源已释放、名字也从 registry 摘掉
        CHECK(SingletonLogger::get() != nullptr);
        CHECK(!SingletonLogger::get()->is_initialized());
        CHECK(spdlog::get(cfg.logger_name) == nullptr);
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

        auto *logger = SingletonLogger::acquire();
        CHECK(logger && logger->is_initialized());
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

    // 单例对象常驻：shutdown() 只释放内部资源，指针始终有效，写日志被静默丢弃。
    void test_pointer_stable_across_shutdown()
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
        Logger *logger = SingletonLogger::get();
        CHECK(logger && logger->is_initialized());
        LOGS_INFO("before-shutdown");

        SingletonLogger::shutdown();
        // 指针不变，但已经不可用：后续写入静默丢弃，不会悬空
        CHECK(SingletonLogger::get() == logger);
        CHECK(!logger->is_initialized());
        logger->log(MYLOG_SRC_LOC, Level::Info, "after global shutdown");
        logger->flush();
        CHECK(count_lines(dir / "snapshot.log") == 1);

        // 同一文件可以重新初始化，指针仍是同一个
        CHECK(SingletonLogger::init(cfg));
        CHECK(SingletonLogger::get() == logger);
        LOGS_INFO("after-reopen");
        SingletonLogger::shutdown();
        CHECK(count_lines(dir / "snapshot.log") == 2);
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

        auto *snapshot = SingletonLogger::acquire();
        CHECK(snapshot != nullptr);
        const AsyncStats stats = snapshot->stats();
        CHECK(stats.discard_count > 0);

        SingletonLogger::shutdown();
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

        // 单例改成"原地重建"后，同一文件重复 init 等价于重启：
        // 先归还租约再重新打开，内容只追加不截断。
        CHECK(SingletonLogger::init(cfg));
        LOGS_INFO("after-inplace-reinit");

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

    // 同一个日志文件已被别的实例占着时，单例 init 必须被拒（进程级租约仍是唯一所有权）。
    void test_same_file_held_by_instance()
    {
        const fs::path dir = kBase / "held";
        LogConfig cfg;
        cfg.logger_name = "held";
        cfg.enable_console = false;
        cfg.enable_file = true;
        cfg.log_dir = dir.string();
        cfg.log_file = "held.log";
        cfg.pattern = "%v";

        Logger holder(cfg);
        CHECK(holder.is_initialized());
        LOG_INFO(holder, "holder-line");

        auto blocked = SingletonLogger::init_checked(cfg);
        CHECK(!blocked.ok);
        CHECK(blocked.error == InitError::FileSink);
        CHECK(blocked.message.find("still in use") != std::string::npos);

        holder.shutdown();
        CHECK(SingletonLogger::init(cfg));
        LOGS_INFO("reopened");
        SingletonLogger::shutdown();
        CHECK(count_lines(dir / "held.log") == 2);
    }

    // 同一个文件的多种写法（不同名字 / 相对路径 / 绝对路径 / 大小写）仍视为同一资源：
    // 用两个实例验证（单例是原地重建，会先释放再重开，不参与这个竞态）。
    void test_same_file_aliases()
    {
        const fs::path dir = kBase / "alias";
        LogConfig first;
        first.logger_name = "alias_a";
        first.enable_console = false;
        first.enable_file = true;
        first.log_dir = dir.string();
        first.log_file = "alias.log";
        Logger owner(first);
        CHECK(owner.is_initialized());

        auto rejected = [&](LogConfig other)
        {
            Logger second(other);
            return !second.is_initialized();
        };

        LogConfig other_name = first;
        other_name.logger_name = "alias_b";
        CHECK(rejected(other_name));

        LogConfig dotted = first;
        dotted.logger_name = "alias_c";
        dotted.log_dir = (kBase / "." / "alias" / ".." / "alias").string();
        CHECK(rejected(dotted));

        LogConfig absolute = first;
        absolute.logger_name = "alias_d";
        absolute.log_dir = fs::absolute(dir).string();
        CHECK(rejected(absolute));

#ifdef _WIN32
        LogConfig upper = first;
        upper.logger_name = "alias_e";
        std::string upper_dir = fs::absolute(dir).string();
        for (char &c : upper_dir)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        upper.log_dir = upper_dir;
        CHECK(rejected(upper));
#endif

        owner.shutdown();

        // 释放后同一个文件可以重新占用
        Logger again(other_name);
        CHECK(again.is_initialized());
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
        auto probe = std::make_shared<GatedProbe>();
        SinkConfig extra;
        extra.factory = [probe]
        {
            return std::make_unique<gated_sink>(probe, 1);
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
        CHECK(probe->wait_entered()); // 后台线程已停在 A 上

        LOGS_INFO("B"); // 队列容量为 1，B 留在队列里
        CHECK(snapshot->stats().queue_size == 1);

        // 直接调用实例的 shutdown()：它走的正是关闭时的排空 + 最终刷新路径
        auto holder = snapshot;
        std::thread closer([holder]
                           { holder->shutdown(); });

        // 不需要 sleep：closer 会卡在“join 消费线程”上，直到开闸且 A、B 都写完。
        probe->open_gate();
        closer.join();
        SingletonLogger::shutdown();

        const auto messages = probe->taken_messages();
        CHECK(messages.size() >= 2);
        CHECK(probe->taken_flushes() > 0 || !expect_flush);

        bool saw_b = false;
        for (const auto &text : messages)
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

        auto probe = std::make_shared<GatedProbe>();
        SinkConfig extra;
        extra.factory = [probe]
        {
            return std::make_unique<gated_sink>(probe, 1);
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
        CHECK(probe->wait_entered());
        LOGS_INFO("B"); // 入队
        LOGS_INFO("C"); // 容量为 1，旧策略下 B 被覆盖
        CHECK(snapshot->stats().overrun_count == 1);

        probe->open_gate();
        SingletonLogger::shutdown(); // 关闭不应再额外丢
        registered.reset();

        bool saw_b = false;
        bool saw_c = false;
        for (const auto &text : probe->taken_messages())
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

        auto *snapshot = SingletonLogger::acquire();
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

        auto *snapshot = SingletonLogger::acquire();
        CHECK(snapshot != nullptr);
        CHECK(!snapshot->is_on_background_thread());

        SingletonLogger::shutdown();
    }

    // 写作线程与 init/shutdown 并发在常驻单例模型下属于未定义行为（与 spdlog 同款约束），
    // 因此这里只验证“先停写、再切换”的常规重建路径。
    void test_reinit_under_quiet_period()
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
        LOGS_INFO("round 0");

        for (int round = 1; round <= 5; ++round)
        {
            CHECK(SingletonLogger::init(make_config(round))); // 原地重建，指针不变
            LOGS_INFO("round {}", round);
        }

        SingletonLogger::shutdown();
        CHECK(count_lines(dir / "race5.log") == 1);
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

        // 单例原地重建：同一路径再次 init 会先释放租约再重开，不再报 still in use
        CHECK(SingletonLogger::init(cfg));

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

    // 把 stdout 临时重定向到文件：控制台输出可以在进程内逐条断言，
    // CTest 那边只需要匹配一行成功哨兵，不再硬编码整段输出。
    class stdout_redirect
    {
    public:
        explicit stdout_redirect(const fs::path &file)
        {
            std::fflush(stdout);
#ifdef _WIN32
            saved_ = _dup(_fileno(stdout));
            FILE *redirected = nullptr;
            freopen_s(&redirected, file.string().c_str(), "w", stdout);
#else
            saved_ = dup(fileno(stdout));
            (void)std::freopen(file.string().c_str(), "w", stdout);
#endif
        }
        ~stdout_redirect() { restore(); }
        stdout_redirect(const stdout_redirect &) = delete;
        stdout_redirect &operator=(const stdout_redirect &) = delete;

        void restore()
        {
            if (saved_ < 0)
                return;
            std::fflush(stdout);
#ifdef _WIN32
            _dup2(saved_, _fileno(stdout));
            _close(saved_);
#else
            dup2(saved_, fileno(stdout));
            close(saved_);
#endif
            saved_ = -1;
        }

    private:
        int saved_ = -1;
    };

    // 真实控制台 sink（不替换它）：先跑一遍，再对抓到文本逐项断言。
    int test_console_output()
    {
        std::error_code ec;
        fs::create_directories(kBase, ec);
        const fs::path captured = kBase / "console_output.txt";

        {
            stdout_redirect redirect(captured);

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
        }

        const std::string text = file_text(captured);
        const char *expected[] = {
            "inherit:first",
            "console:warning:dedicated",
            "console:debug:lowered",
            "custom:formatter",
            "local:override-formatter",
            "all:unified",
            "all:still-unified",
            "final:restored",
            "configured:initial-specific",
        };
        for (const char *line : expected)
            CHECK(text.find(line) != std::string::npos);
        // 任何一个被门槛/空格式拦下的正文都不能出现
        CHECK(text.find("forbidden") == std::string::npos);

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
            if (same->is_on_background_thread() != async ||
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
        auto probe = std::make_shared<GatedProbe>();
        SinkConfig extra;
        extra.factory = [probe]
        {
            return std::make_unique<gated_sink>(probe, 1);
        };
        cfg.extra_sinks.push_back(extra);
        Logger logger(cfg);
        LOG_INFO(logger, "A");
        CHECK(probe->wait_entered());
        LOG_INFO(logger, "B");
        LOG_INFO(logger, "C");
        CHECK(logger.stats().discard_count == 1);
        probe->open_gate();
        logger.shutdown();
        CHECK(events == std::vector<std::string>({"A", "B"}));
    }

    // 定时刷新是进程级共享调度：多个实例（同间隔/不同间隔）都能被刷到，
    // 关闭其中一个只停它自己的刷新，其余不受影响。
    void test_shared_flush_scheduler()
    {
        auto probe_a = std::make_shared<Probe>();
        auto probe_b = std::make_shared<Probe>();
        auto probe_c = std::make_shared<Probe>();

        auto cfg_a = probe_config("sched_a", probe_a);
        cfg_a.flush_interval = std::chrono::milliseconds(20);
        auto cfg_b = probe_config("sched_b", probe_b);
        cfg_b.flush_interval = std::chrono::milliseconds(20); // 同间隔：共用一个桶
        auto cfg_c = probe_config("sched_c", probe_c);
        cfg_c.flush_interval = std::chrono::milliseconds(40); // 不同间隔：另一个桶

        Logger a(cfg_a);
        Logger b(cfg_b);
        Logger c(cfg_c);
        CHECK(a.is_initialized() && b.is_initialized() && c.is_initialized());

        CHECK(probe_a->wait_flush());
        CHECK(probe_b->wait_flush());
        CHECK(probe_c->wait_flush());

        a.shutdown();
        std::size_t a_flushes = 0;
        std::size_t b_flushes = 0;
        {
            std::lock_guard<std::mutex> lock(probe_a->mutex);
            a_flushes = probe_a->flushes;
        }
        {
            std::lock_guard<std::mutex> lock(probe_b->mutex);
            b_flushes = probe_b->flushes;
        }

        CHECK(probe_b->wait_flush(b_flushes + 1)); // b 仍在被刷
        {
            std::lock_guard<std::mutex> lock(probe_a->mutex);
            CHECK(probe_a->flushes == a_flushes); // a 已注销，不再被刷
        }

        b.shutdown();
        c.shutdown();
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
        auto probe = std::make_shared<GatedProbe>();
        SinkConfig extra;
        extra.factory = [probe]
        {
            return std::make_unique<gated_sink>(probe, 1);
        };
        cfg.extra_sinks.push_back(extra);
        Logger logger(cfg);
        LOG_INFO(logger, "A");
        CHECK(probe->wait_entered());
        LOG_INFO(logger, "B");
        // 后台停在 A、队列被 B 占满；定时刷新不能覆盖 B 或增加丢弃统计。
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(logger.stats().overrun_count == 0);
        CHECK(logger.stats().discard_count == 0);
        probe->open_gate();
        logger.shutdown();
        CHECK(probe->taken_messages() == std::vector<std::string>({"A", "B"}));
        CHECK(probe->taken_flushes() >= 2); // 已触发的周期刷新 + 关闭刷新
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
            // 在后台线程上取一次指针：只验证“在该线程上调 shutdown 被拒绝”
            auto *snapshot = SingletonLogger::acquire();
            if (!snapshot || acted.exchange(true))
                return;
            correct = snapshot->is_on_background_thread();
            snapshot->shutdown(); // 必须拒绝自关闭
            if (!snapshot->is_initialized())
                correct = false;
            SingletonLogger::shutdown(); // 转交回收线程执行
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
        SingletonLogger::shutdown(); // 排空转交的释放
        CHECK(SingletonLogger::get() != nullptr);
        CHECK(!SingletonLogger::get()->is_initialized());
    }

    void test_new_empty_and_failure_paths()
    {
        // 未初始化的实例（非法级别 => 构造失败，没有任何 sink）：设置操作必须无副作用
        LogConfig empty_cfg;
        empty_cfg.logger_name = "uninitialized_setters";
        empty_cfg.enable_console = false;
        empty_cfg.enable_file = false;
        empty_cfg.console_level = static_cast<Level>(99);
        empty_cfg.error_handler = [](const std::string &) {};
        Logger empty(empty_cfg);
        empty.set_console_level(Level::Debug);
        empty.set_file_level(Level::Debug);
        empty.set_console_pattern("x");
        empty.set_file_pattern("x");
        empty.set_formatter(std::make_unique<prefix_formatter>(std::make_shared<int>(0)));
        empty.enable_backtrace(2);
        empty.dump_backtrace();
        empty.disable_backtrace();
        CHECK(!empty.is_initialized());
        CHECK(empty.stats().queue_size == 0);
        CHECK(!empty.should_log(Level::Info));

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

    // 重建失败后单例回到未初始化状态（对象仍在）：不崩溃、不残留旧 sink。
    void test_failed_reinit_leaves_empty()
    {
        auto probe = std::make_shared<Probe>();
        auto cfg = probe_config("periodic_snapshot", probe);
        cfg.flush_interval = std::chrono::milliseconds(20);
        CHECK(SingletonLogger::init(cfg));

        auto failed_cfg = cfg;
        failed_cfg.logger_name = "failed_reinit";
        failed_cfg.extra_sinks[0].factory = []() -> std::unique_ptr<spdlog::sinks::sink>
        { throw std::runtime_error("expected reinit failure"); };
        CHECK(!SingletonLogger::init_checked(failed_cfg));

        // 旧资源已释放：对象还在，但已经不可用
        CHECK(SingletonLogger::get() != nullptr);
        CHECK(!SingletonLogger::get()->is_initialized());
        LOGS_INFO("dropped silently");

        SingletonLogger::shutdown(); // 幂等
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

    // 两个实例都开启回溯、回调里向对方写日志，同时导出回溯。
    // 修复前：A 持有自身回溯锁等待 B 的回溯锁，B 反向等待 A，两个线程永久卡死。
    // 修复后：所有导出串行化，同一时刻只有一个线程持有回溯锁，两边都能完成。
    void test_concurrent_backtrace_dump_with_cross_writes()
    {
        auto probe_a = std::make_shared<Probe>();
        auto probe_b = std::make_shared<Probe>();
        auto cfg_a = probe_config("dump_cross_a", probe_a);
        auto cfg_b = probe_config("dump_cross_b", probe_b);
        Logger *first = nullptr;
        Logger *second = nullptr;

        // 回调里先在“补打缓存”时碰头：让两个线程都停在自己的回溯锁内，
        // 再往对方写 debug（低于总门槛，只会进对方的回溯缓存）。
        std::mutex meet_mutex;
        std::condition_variable meet_cv;
        int arrived = 0;
        auto rendezvous = [&]
        {
            std::unique_lock<std::mutex> lock(meet_mutex);
            ++arrived;
            meet_cv.notify_all();
            meet_cv.wait_for(lock, std::chrono::milliseconds(500), [&] { return arrived >= 2; });
        };

        for (auto *config : {&cfg_a, &cfg_b})
        {
            config->level = Level::Info;
            config->backtrace_capacity = 1;
            config->callback_level = Level::Trace; // 开始/结束分隔线也会通知
        }
        cfg_a.log_callback = [&](const LogEvent &event)
        {
            // Start/End 分隔线在回溯锁外发出，只有补打的缓存消息才处于锁内：
            // 在那种回调里会合，才能确定两个线程都拿着各自的回溯锁。
            if (event.message.find("Backtrace") != std::string::npos)
                return;
            rendezvous();
            LOG_DEBUG(*second, "cross-a");
        };
        cfg_b.log_callback = [&](const LogEvent &event)
        {
            if (event.message.find("Backtrace") != std::string::npos)
                return;
            rendezvous();
            LOG_DEBUG(*first, "cross-b");
        };

        Logger a(cfg_a);
        Logger b(cfg_b);
        first = &a;
        second = &b;
        CHECK(a.is_initialized() && b.is_initialized());
        LOG_DEBUG(a, "cached-a");
        LOG_DEBUG(b, "cached-b");

        std::thread dumper_a([&] { a.dump_backtrace(); });
        std::thread dumper_b([&] { b.dump_backtrace(); });
        dumper_a.join();
        dumper_b.join();

        // 每个实例导出 Start + 一条缓存 + End；卡死时根本走不到这里
        CHECK(probe_a->messages.size() == 3);
        CHECK(probe_b->messages.size() == 3);
    }

    // 多消费线程下关闭：先执行到的 flush 可能早于最后一条写入。
    // 修复前只向队列投递一个刷新请求，另一条消费线程先刷、最后一条日志随后才写。
    void test_async_close_final_flush_order()
    {
        auto probe = std::make_shared<OrderedProbe>();
        LogConfig cfg;
        cfg.logger_name = "final_flush_order";
        cfg.enable_console = false;
        cfg.enable_file = false;
        cfg.async = true;
        cfg.async_queue_size = 16;
        cfg.async_thread_count = 2; // 关键：另一条消费线程可以先把刷新请求执行完
        cfg.level = Level::Trace;
        cfg.pattern = "%v";
        SinkConfig extra;
        extra.factory = [probe]
        {
            return std::make_unique<ordered_sink>(probe);
        };
        cfg.extra_sinks.push_back(extra);

        Logger logger(cfg);
        CHECK(logger.is_initialized());

        LOG_INFO(logger, "hold");
        CHECK(probe->wait_entered()); // 消费线程 A 已取出日志，但还没写入

        std::thread closer([&logger] { logger.shutdown(); });

        // 修复前关闭会把 flush 投进队列：空闲的消费线程先刷新，随后这条写入才落下。
        // 这里只等待旧行为收敛，不影响断言方向（断言只看最终顺序）。
        probe->wait_flush(std::chrono::milliseconds(1000));
        probe->open_gate();
        closer.join();

        std::vector<std::string> events;
        {
            std::lock_guard<std::mutex> lock(probe->mutex);
            events = probe->events;
        }
        CHECK(std::count(events.begin(), events.end(), "w:hold") == 1);
        CHECK(!events.empty());
        if (events.empty())
            return;
        // 最后一步必须是刷新：证明最后一条写入之后还有一次同步刷新
        CHECK(events.back() == "f");
    }

    // 定时刷新的 error_handler 运行在调度线程上；它调用 shutdown() 不能自锁。
    void test_shutdown_from_timer_error_handler()
    {
        const fs::path dir = kBase / "timer_release";
        auto probe = std::make_shared<Probe>();
        auto cfg = probe_config("timer_release", probe);
        cfg.enable_file = true;
        cfg.rotation = LogConfig::RotationPolicy::None;
        cfg.log_dir = dir.string();
        cfg.log_file = "timer.log";
        cfg.flush_interval = std::chrono::milliseconds(20);
        probe->throw_flush = true; // 每次刷新都失败，从而在调度线程上进入 error_handler

        std::atomic<bool> handled{false};
        cfg.error_handler = [&](const std::string &)
        {
            if (handled.exchange(true))
                return;
            SingletonLogger::shutdown(); // 从调度线程发起：必须被安全处理
        };

        CHECK(SingletonLogger::init(cfg));
        CHECK(probe->wait_destroyed());
        CHECK(handled.load());

        // 资源已归还：同一文件可以立刻重新初始化（指针不变）
        probe->throw_flush = false;
        auto reopened = cfg;
        reopened.logger_name = "timer_release_again";
        reopened.flush_interval = std::chrono::milliseconds(0);
        reopened.error_handler = nullptr;
        CHECK(SingletonLogger::init(reopened));
        LOGS_INFO("reopened after timer shutdown");
        SingletonLogger::shutdown();
    }

    // 运行期被过滤的日志不能求值参数；放行时恰好求值一次（不能因新增的门禁变成两次）。
    void test_macro_does_not_evaluate_when_filtered()
    {
        auto probe = std::make_shared<Probe>();
        auto cfg = probe_config("macro_eval", probe);
        cfg.level = Level::Warn;
        Logger logger(cfg);
        CHECK(logger.is_initialized());

        int evals = 0;
        LOG_INFO(logger, "{}", ++evals);
        LOG_DEBUG(logger, "{}", ++evals);
        CHECK(evals == 0);
        LOG_WARN(logger, "{}", ++evals);
        CHECK(evals == 1);

        CHECK(SingletonLogger::init(cfg));
        evals = 0;
        LOGS_INFO("{}", ++evals);
        LOGS_TRACE("{}", ++evals);
        CHECK(evals == 0);
        LOGS_WARN("{}", ++evals);
        CHECK(evals == 1);
        SingletonLogger::shutdown();
    }

    // 显式 flush() 不能按溢出策略挤掉已排队的日志（修复前 flush 会覆盖 B）。
    void test_flush_does_not_drop_queued_log()
    {
        LogConfig cfg;
        cfg.logger_name = "flush_no_drop";
        cfg.enable_console = false;
        cfg.enable_file = false;
        cfg.async = true;
        cfg.async_queue_size = 1;
        cfg.async_thread_count = 1;
        cfg.async_overflow = OverflowPolicy::OverrunOldest;
        cfg.level = Level::Trace;
        cfg.pattern = "%v";

        auto probe = std::make_shared<GatedProbe>();
        SinkConfig extra;
        extra.factory = [probe]
        {
            return std::make_unique<gated_sink>(probe, 1);
        };
        cfg.extra_sinks.push_back(extra);

        Logger logger(cfg);
        CHECK(logger.is_initialized());

        LOG_INFO(logger, "A");
        CHECK(probe->wait_entered());
        LOG_INFO(logger, "B"); // 容量为 1，队列已满

        // flush 以 block 方式入队：需要在另一条线程上开闸，它才能拿到位置。
        // 旧实现按 OverrunOldest 直接入队，B 会被这条控制消息挤掉。
        std::thread opener([&probe]
                           {
                               std::this_thread::sleep_for(std::chrono::milliseconds(50));
                               probe->open_gate();
                           });
        logger.flush();
        opener.join();

        CHECK(logger.stats().overrun_count == 0);
        logger.shutdown();

        bool saw_b = false;
        for (const auto &text : probe->taken_messages())
            if (text == "B")
                saw_b = true;
        CHECK(saw_b);
    }

    // 在自己的后台线程上关闭：只能转交回收线程，shutdown 返回后仍需下一次
    // init/shutdown 把回收排空，随后可以立刻重开同一文件。
    void test_shutdown_from_worker_error_handler()
    {
        const fs::path dir = kBase / "bg_release";
        auto probe = std::make_shared<Probe>();
        auto cfg = probe_config("bg_release", probe);
        cfg.enable_file = true;
        cfg.log_dir = dir.string();
        cfg.log_file = "bg_release.log";
        cfg.pattern = "%v";
        cfg.async = true;
        cfg.flush_interval = std::chrono::milliseconds(20);
        probe->throw_flush = true;

        std::mutex mutex;
        std::condition_variable cv;
        bool released_on_background = false;
        cfg.error_handler = [&](const std::string &)
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (released_on_background)
                    return;
            }
            // 在消费线程上发起关闭：必须转交回收线程，不能同步 join 自己
            SingletonLogger::shutdown();
            {
                std::lock_guard<std::mutex> lock(mutex);
                released_on_background = true;
            }
            cv.notify_all();
        };

        CHECK(SingletonLogger::init(cfg));
        {
            std::unique_lock<std::mutex> lock(mutex);
            CHECK(cv.wait_for(lock, std::chrono::seconds(5), [&]
                              { return released_on_background; }));
        }

        SingletonLogger::shutdown(); // 回收可能还在进行中，这里必须等它结束
        std::size_t destroyed = 0;
        {
            std::lock_guard<std::mutex> lock(probe->mutex);
            destroyed = probe->destroyed;
        }
        CHECK(destroyed == 1);

        // 文件占用已归还：同一文件可以立刻重开
        probe->throw_flush = false;
        cfg.flush_interval = std::chrono::milliseconds(0);
        cfg.error_handler = nullptr;
        cfg.logger_name = "bg_release_again";
        CHECK(SingletonLogger::init(cfg));
        LOGS_INFO("reopened");
        SingletonLogger::shutdown();
    }

    // 同名并发初始化后，“全局指针”与“registry 里的名字”必须指向同一个实例。
    // 不使用文件目标，避免文件占用把并发折叠成“只有一个能成功”。
    void test_concurrent_same_name_init_consistency()
    {
        auto make_config = []
        {
            auto probe = std::make_shared<Probe>();
            auto cfg = probe_config("same_name", probe);
            return cfg;
        };

        for (int round = 0; round < 20; ++round)
        {
            std::atomic<bool> start{false};
            std::vector<std::thread> threads;
            for (int i = 0; i < 8; ++i)
            {
                threads.emplace_back([&, i]
                                     {
                                         while (!start.load(std::memory_order_acquire))
                                             std::this_thread::yield();
                                         SingletonLogger::init(make_config());
                                     });
            }
            start.store(true, std::memory_order_release);
            for (auto &thread : threads)
                thread.join();

            auto *snapshot = SingletonLogger::acquire();
            CHECK(snapshot != nullptr);
            CHECK(spdlog::get("same_name") != nullptr);

            // 关闭时封装会用 get(name) 与自己的内部日志器做身份比对；若两者已经分叉
            // （初始化把注册和换指针拆到两个临界区），这里就会留下一个永久的残留项。
            SingletonLogger::shutdown();
            CHECK(spdlog::get("same_name") == nullptr);
        }
    }

    // 关闭后对外就是“未初始化的空实例”，但名字不变；文件占用已归还。
    void test_state_after_shutdown()
    {
        const fs::path dir = kBase / "state";
        LogConfig cfg;
        cfg.logger_name = "state_after";
        cfg.enable_console = false;
        cfg.enable_file = true;
        cfg.log_dir = dir.string();
        cfg.log_file = "state.log";
        {
            Logger logger(cfg);
            CHECK(logger.is_initialized());
            CHECK(logger.name() == "state_after");
            logger.shutdown();

            CHECK(!logger.is_initialized());
            CHECK(logger.name() == "state_after"); // 关闭不清空名字
            CHECK(!logger.init_result().ok);
            CHECK(!logger.init_result().degraded);
            CHECK(logger.init_result().error == InitError::None);
            CHECK(logger.init_result().message.empty());
            CHECK(logger.level() == Level::Off);
            logger.shutdown(); // 重复关闭必须安全
        }
        CHECK(SingletonLogger::init(cfg));
        SingletonLogger::shutdown();
    }

    // 非法 Level（调用方 static_cast 出来的）必须被拒绝，不能静默变成 Info。
    void test_invalid_level_is_rejected()
    {
        auto probe = std::make_shared<Probe>();
        auto cfg = probe_config("invalid_level", probe);
        cfg.level = Level::Warn;
        std::atomic<int> errors{0};
        cfg.error_handler = [&](const std::string &msg)
        {
            if (msg.find("invalid level") != std::string::npos)
                ++errors;
        };
        Logger logger(cfg);
        CHECK(logger.is_initialized());
        CHECK(logger.level() == Level::Warn);

        logger.set_level(static_cast<Level>(42));
        CHECK(logger.level() == Level::Warn); // 值保持不变
        logger.set_flush_level(static_cast<Level>(-3));
        CHECK(errors == 2); // 控制台/文件目标未启用，对应 setter 直接返回

        logger.set_level(Level::Off);
        CHECK(logger.level() == Level::Off);
        logger.shutdown();
    }

    // 移动赋值：目标接管状态，源变成空壳，文件占用随旧内容释放。
    void test_move_assignment()
    {
        const fs::path dir = kBase / "move_assign";
        LogConfig cfg;
        cfg.logger_name = "move_a";
        cfg.enable_console = false;
        cfg.enable_file = true;
        cfg.log_dir = dir.string();
        cfg.log_file = "a.log";
        cfg.pattern = "%v";

        Logger first(cfg);
        CHECK(first.is_initialized());

        LogConfig other_cfg = cfg;
        other_cfg.logger_name = "move_b";
        other_cfg.log_file = "b.log";
        Logger second(other_cfg);
        CHECK(second.is_initialized());

        second = std::move(first);
        CHECK(second.is_initialized());
        CHECK(second.name() == "move_a");
        CHECK(!first.is_initialized()); // 迁走的实例变成空壳
        CHECK(first.name().empty());

        LOG_INFO(second, "after-move-assign");
        second.shutdown();
        CHECK(count_lines(dir / "a.log") == 1);

        // b.log 的占用已随被覆盖的旧内容释放
        CHECK(SingletonLogger::init(other_cfg));
        SingletonLogger::shutdown();
    }

    // flush_and_wait：异步下返回时日志已经落盘（不需要 shutdown）。
    void test_flush_and_wait()
    {
        const fs::path dir = kBase / "flush_wait";
        LogConfig cfg;
        cfg.logger_name = "flush_wait";
        cfg.enable_console = false;
        cfg.enable_file = true;
        cfg.log_dir = dir.string();
        cfg.log_file = "flush_wait.log";
        cfg.async = true;
        cfg.async_queue_size = 4096;
        cfg.pattern = "%v";

        Logger logger(cfg);
        CHECK(logger.is_initialized());

        const int kCount = 500;
        for (int i = 0; i < kCount; ++i)
            LOG_INFO(logger, "line {}", i);

        // 没有 shutdown，只等刷盘：此刻文件里就该有全部行
        CHECK(logger.flush_and_wait(std::chrono::seconds(5)));
        CHECK(count_lines(dir / "flush_wait.log") == static_cast<std::size_t>(kCount));
        logger.shutdown();

        // 消费线程被卡住时，超时必须生效，不能无限等
        auto probe = std::make_shared<GatedProbe>();
        LogConfig blocked_cfg;
        blocked_cfg.logger_name = "flush_wait_blocked";
        blocked_cfg.enable_console = false;
        blocked_cfg.enable_file = false;
        blocked_cfg.async = true;
        blocked_cfg.async_queue_size = 8;
        blocked_cfg.level = Level::Trace;
        blocked_cfg.pattern = "%v";
        SinkConfig extra;
        extra.factory = [probe]
        {
            return std::make_unique<gated_sink>(probe, 1);
        };
        blocked_cfg.extra_sinks.push_back(extra);

        Logger blocked(blocked_cfg);
        CHECK(blocked.is_initialized());
        LOG_INFO(blocked, "hold");
        CHECK(probe->wait_entered());
        LOG_INFO(blocked, "queued");

        const auto start = std::chrono::steady_clock::now();
        CHECK(!blocked.flush_and_wait(std::chrono::milliseconds(50)));
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));

        probe->open_gate();
        blocked.shutdown();
    }

    // native()：拿到底层 spdlog 日志器后，spdlog 原生 API 也生效。
    void test_native_handle()
    {
        auto probe = std::make_shared<Probe>();
        auto cfg = probe_config("native_handle", probe);
        Logger logger(cfg);
        CHECK(logger.is_initialized());

        auto sp = logger.native();
        CHECK(sp != nullptr);
        if (sp)
        {
            sp->set_level(spdlog::level::warn); // 原生 API 改级别
            CHECK(logger.level() == Level::Warn); // 封装侧读到同一个值

            sp->log(spdlog::source_loc{}, spdlog::level::warn, "via-native-{}", 1);
            logger.flush(); // 同步实例，刷完即可读
            std::lock_guard<std::mutex> lock(probe->mutex);
            CHECK(std::count(probe->messages.begin(), probe->messages.end(), "via-native-1") == 1);
        }

        logger.shutdown();
        CHECK(logger.native() == nullptr); // 关闭后不再给原生句柄
        sp.reset();
    }

    // 反复写日志后 shutdown() 仍能立刻归还文件，同一文件可以马上重开；
    // 关闭后的调用静默丢弃。
    void test_reopen_same_file_after_shutdown()
    {
        const fs::path dir = kBase / "snapshot_cache";
        LogConfig cfg;
        cfg.logger_name = "snapshot_cache";
        cfg.enable_console = false;
        cfg.enable_file = true;
        cfg.log_dir = dir.string();
        cfg.log_file = "cache.log";
        cfg.pattern = "%v";

        CHECK(SingletonLogger::init(cfg));
        for (int i = 0; i < 100; ++i)
            LOGS_INFO("cached {}", i); // 让本线程的缓存命中
        SingletonLogger::shutdown();

        // 缓存不得延长寿命：同一文件必须能立刻重开
        CHECK(SingletonLogger::init(cfg));
        LOGS_INFO("after reinit");
        SingletonLogger::shutdown();
        CHECK(count_lines(dir / "cache.log") == 101);

        // 关闭后的调用仍然静默丢弃（缓存已失效，不会写到旧实例）
        LOGS_INFO("after shutdown");
        CHECK(count_lines(dir / "cache.log") == 101);
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
    test_concurrent_backtrace_dump_with_cross_writes();
    test_async_close_final_flush_order();
    test_shutdown_from_timer_error_handler();
    test_discarded_messages_have_no_callback();
    test_periodic_flush(false);
    test_periodic_flush(true);
    test_shared_flush_scheduler();
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
    test_failed_reinit_leaves_empty();
    test_explicit_console_fallback_with_extra();
    test_uninitialized_is_silent();
    test_init_failure_callback_reentry();
    test_directory_failure();
    test_file_sink_fallback();
    test_async_drain();
    test_pointer_stable_across_shutdown();
    test_overflow_discard();
    test_reinit_and_repeat_shutdown();
    test_same_file_held_by_instance();
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
    test_reinit_under_quiet_period();
    test_daily_rotation();

    // 本轮审计新增的回归用例
    test_macro_does_not_evaluate_when_filtered();
    test_flush_does_not_drop_queued_log();
    test_shutdown_from_worker_error_handler();
    test_concurrent_same_name_init_consistency();
    test_state_after_shutdown();
    test_invalid_level_is_rejected();
    test_move_assignment();
    test_flush_and_wait();
    test_native_handle();
    test_reopen_same_file_after_shutdown();

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
