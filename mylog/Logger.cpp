// Logger.cpp
#include "Logger.h"

#include <atomic>
#include <cctype>
#include <cstdint>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <vector>

#include "spdlog/async.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/daily_file_sink.h"
#include "spdlog/sinks/hourly_file_sink.h"
#include "spdlog/sinks/rotating_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"

namespace mylog
{

    // ---------------------- detail ----------------------

    namespace detail
    {

        spdlog::level::level_enum to_spdlog_level(Level lvl)
        {
            switch (lvl)
            {
            case Level::Trace:
                return spdlog::level::trace;
            case Level::Debug:
                return spdlog::level::debug;
            case Level::Info:
                return spdlog::level::info;
            case Level::Warn:
                return spdlog::level::warn;
            case Level::Error:
                return spdlog::level::err;
            case Level::Critical:
                return spdlog::level::critical;
            case Level::Off:
                return spdlog::level::off;
            }
            return spdlog::level::info;
        }

        Level from_spdlog_level(spdlog::level::level_enum lvl)
        {
            switch (lvl)
            {
            case spdlog::level::trace:
                return Level::Trace;
            case spdlog::level::debug:
                return Level::Debug;
            case spdlog::level::info:
                return Level::Info;
            case spdlog::level::warn:
                return Level::Warn;
            case spdlog::level::err:
                return Level::Error;
            case spdlog::level::critical:
                return Level::Critical;
            case spdlog::level::off:
                return Level::Off;
            default:
                break;
            }
            return Level::Off;
        }

    } // namespace detail

    // ---------------------- Logger ----------------------

    namespace
    {
        class PeriodicFlusher;
    }

    // 实例的全部可变状态都放在堆上：地址稳定，因此移动 Logger、
    // 以及在错误回调里保存指针都不会悬空。
    struct Logger::State
    {
        // 声明顺序即析构逆序：logger 必须先于 pool 释放
        std::shared_ptr<spdlog::details::thread_pool> pool;
        std::shared_ptr<spdlog::logger> logger;
        spdlog::sink_ptr console_sink;
        spdlog::sink_ptr file_sink;
        std::string name;
        InitResult init;
        // 最后声明，异常回收时也先停止定时器，再释放它访问的资源。
        std::unique_ptr<PeriodicFlusher> flusher;
    };

    namespace
    {
        // 记录当前线程属于哪个 Logger 实例的异步线程池
        thread_local const void *t_worker_state = nullptr;
        thread_local const void *t_timer_state = nullptr;

        // 用栈上链表记录整条嵌套回调链，A -> B -> A 也能识别，无额外分配。
        class CallbackScope;
        thread_local const CallbackScope *t_callback_scope = nullptr;
        class CallbackScope
        {
        public:
            explicit CallbackScope(const void *state) noexcept
                : state_(state), previous_(t_callback_scope) { t_callback_scope = this; }
            ~CallbackScope() { t_callback_scope = previous_; }
            static bool contains(const void *state) noexcept
            {
                for (auto scope = t_callback_scope; scope; scope = scope->previous_)
                    if (scope->state_ == state)
                        return true;
                return false;
            }
        private:
            const void *state_;
            const CallbackScope *previous_;
        };

        // 构造后先休眠，初始化完全成功才启动；等待与执行回调不共用持锁区间。
        class PeriodicFlusher
        {
        public:
            PeriodicFlusher(std::chrono::milliseconds interval, std::function<void()> work)
                : thread_([this, interval, work = std::move(work)]
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cv_.wait(lock, [this] { return started_ || stopped_; });
                    while (!stopped_)
                    {
                        if (cv_.wait_for(lock, interval, [this] { return stopped_; }))
                            break;
                        lock.unlock();
                        work();
                        lock.lock();
                    }
                }) {}
            ~PeriodicFlusher()
            {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    stopped_ = true;
                }
                cv_.notify_all();
                if (thread_.joinable())
                    thread_.join();
            }
            void start()
            {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    started_ = true;
                }
                cv_.notify_all();
            }
        private:
            std::mutex mutex_;
            std::condition_variable cv_;
            bool started_ = false;
            bool stopped_ = false;
            std::thread thread_;
        };

        void report_error(const std::function<void(const std::string &)> &handler,
                          const std::string &msg)
        {
            if (!handler)
            {
                std::fprintf(stderr, "%s\n", msg.c_str());
                return;
            }
            try
            {
                handler(msg);
            }
            catch (const std::exception &ex)
            {
                std::fprintf(stderr, "[mylog] error_handler threw: %s (original: %s)\n",
                             ex.what(), msg.c_str());
            }
            catch (...)
            {
                std::fprintf(stderr, "[mylog] error_handler threw unknown exception (original: %s)\n",
                             msg.c_str());
            }
        }

        // 用户回调没有 sink 锁；多线程可同时通知，用户负责其共享数据同步。
        class EventSink final : public spdlog::sinks::sink
        {
        public:
            EventSink(const void *state, std::function<void(const LogEvent &)> callback,
                      std::function<void(const std::string &)> handler)
                : state_(state), callback_(std::move(callback)), handler_(std::move(handler)) {}
            void log(const spdlog::details::log_msg &msg) override
            {
                if (CallbackScope::contains(state_))
                    return;
                CallbackScope scope(state_);
                try
                {
                    LogEvent event{detail::from_spdlog_level(msg.level),
                                   std::string(msg.logger_name.data(), msg.logger_name.size()),
                                   std::string(msg.payload.data(), msg.payload.size())};
                    callback_(event);
                }
                catch (const std::exception &ex)
                {
                    report_error(handler_, std::string("[mylog] log_callback threw: ") + ex.what());
                }
                catch (...)
                {
                    report_error(handler_, "[mylog] log_callback threw unknown exception");
                }
            }
            void flush() override {}
            void set_pattern(const std::string &) override {}
            void set_formatter(std::unique_ptr<spdlog::formatter>) override {}
        private:
            const void *state_;
            const std::function<void(const LogEvent &)> callback_;
            const std::function<void(const std::string &)> handler_;
        };

        spdlog::async_overflow_policy to_overflow_policy(OverflowPolicy policy)
        {
            switch (policy)
            {
            case OverflowPolicy::OverrunOldest:
                return spdlog::async_overflow_policy::overrun_oldest;
            case OverflowPolicy::DiscardNew:
                return spdlog::async_overflow_policy::discard_new;
            case OverflowPolicy::Block:
            default:
                return spdlog::async_overflow_policy::block;
            }
        }

        // ---- 文件占用登记 ----
        // 同一个日志文件只允许一个存活的文件 sink：spdlog 的轮转状态（文件大小、
        // 轮转锁）保存在 sink 内部，两个 sink 指向同一文件会各自计数、互相干扰。
        // 登记表由令牌共同持有，避免进程退出时静态析构顺序造成访问失效。
        struct FileLeaseTable
        {
            std::mutex mutex;
            std::unordered_set<std::string> paths;
        };

        // 占用令牌：析构时归还路径。必须与实际文件 sink 同生命周期，
        // 这样句柄关闭之后才允许其他实例重新占用同一文件。
        class FileLease
        {
        public:
            FileLease(std::shared_ptr<FileLeaseTable> table, std::string key)
                : table_(std::move(table)), key_(std::move(key))
            {
            }
            FileLease(const FileLease &) = delete;
            FileLease &operator=(const FileLease &) = delete;
            void activate(std::shared_ptr<FileLeaseTable> table) noexcept { table_ = std::move(table); }

            ~FileLease()
            {
                if (!table_)
                    return;
                std::lock_guard<std::mutex> lock(table_->mutex);
                table_->paths.erase(key_);
            }

        private:
            std::shared_ptr<FileLeaseTable> table_;
            std::string key_;
        };

        std::shared_ptr<FileLeaseTable> file_lease_table()
        {
            static std::shared_ptr<FileLeaseTable> table = std::make_shared<FileLeaseTable>();
            return table;
        }

        // 规范化为绝对路径，用于判断“是不是同一个文件”。
        // 失败时通过 error 返回原因、由初始化流程上报错误，不做静默回退：
        // 拿不准路径就无法保证“同一文件只有一个轮转器”。
        bool normalize_file_key(const std::string &path, std::string &key, std::string &error)
        {
            std::error_code ec;
            std::filesystem::path base = std::filesystem::absolute(std::filesystem::path(path), ec);
            if (ec)
            {
                error = "[mylog] cannot resolve absolute path for log file '" + path +
                        "': " + ec.message();
                return false;
            }

            std::filesystem::path canonical = std::filesystem::weakly_canonical(base, ec);
            if (ec)
            {
                error = "[mylog] cannot normalize log file path '" + path + "': " + ec.message();
                return false;
            }

            key = canonical.string();
#ifdef _WIN32
            // Windows 文件系统通常大小写不敏感，统一小写以免大小写别名绕过占用检查
            for (char &c : key)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
            return true;
        }

        struct FileLeaseRequest
        {
            std::shared_ptr<FileLease> lease;
            InitError error = InitError::None;
            std::string message;
        };

        // 尝试占用路径：只做短临界区的检查与登记，打开文件、调用用户回调都在锁外。
        FileLeaseRequest acquire_file_lease(const std::string &path)
        {
            FileLeaseRequest result;
            std::string key;
            if (!normalize_file_key(path, key, result.message))
            {
                result.error = InitError::InvalidConfig;
                return result;
            }

            auto table = file_lease_table();
            // 先分配未激活的令牌，避免登记成功后分配失败而永久占用路径。
            auto lease = std::make_shared<FileLease>(nullptr, key);
            {
                std::lock_guard<std::mutex> lock(table->mutex);
                if (table->paths.count(key) != 0)
                {
                    result.error = InitError::FileSink;
                    result.message =
                        "[mylog] log file is still in use by a live logger: '" + path + "'";
                    return result;
                }
                table->paths.insert(key);
                lease->activate(table);
            }
            result.lease = std::move(lease);
            return result;
        }

        // 文件 sink 的所有者：先声明 lease、后声明 sink，
        // 保证销毁顺序是先关闭文件句柄、再归还路径占用。
        struct FileSinkOwner
        {
            std::vector<std::shared_ptr<FileLease>> leases;
            std::unique_ptr<spdlog::sinks::sink> sink;
        };

        bool valid_level(Level level)
        {
            return level >= Level::Trace && level <= Level::Off;
        }

    } // namespace

    Logger::Logger(const LogConfig &cfg)
        : state_(std::make_unique<State>())
    {
        State &st = *state_;
        const auto &handler = cfg.error_handler;

        auto fail = [&](InitError code, const std::string &msg)
        {
            st.init.ok = false;
            st.init.error = code;
            st.init.message = msg;
            report_error(handler, msg);
        };

        // ---------- 1. 配置校验（先于任何资源申请） ----------
        if (cfg.logger_name.empty())
        {
            fail(InitError::InvalidConfig, "[mylog] logger_name must not be empty");
            return;
        }
        if (cfg.enable_file && cfg.log_file.empty())
        {
            fail(InitError::InvalidConfig, "[mylog] log_file must not be empty when file output is enabled");
            return;
        }
        if (cfg.async)
        {
            if (cfg.async_queue_size == 0)
            {
                fail(InitError::InvalidConfig, "[mylog] async_queue_size must be greater than 0");
                return;
            }
            if (cfg.async_thread_count == 0 || cfg.async_thread_count > 1000)
            {
                fail(InitError::InvalidConfig, "[mylog] async_thread_count must be in range [1, 1000]");
                return;
            }
        }
        if (cfg.enable_file && cfg.rotation == LogConfig::RotationPolicy::BySize)
        {
            if (cfg.max_file_size == 0)
            {
                fail(InitError::InvalidConfig, "[mylog] max_file_size must be greater than 0");
                return;
            }
            if (cfg.max_files > spdlog::sinks::rotating_file_sink<std::mutex>::MaxFiles)
            {
                fail(InitError::InvalidConfig, "[mylog] max_files exceeds spdlog limit (200000)");
                return;
            }
        }
        if (cfg.enable_file && cfg.rotation == LogConfig::RotationPolicy::Daily)
        {
            if (cfg.daily_rotation_hour < 0 || cfg.daily_rotation_hour > 23 ||
                cfg.daily_rotation_minute < 0 || cfg.daily_rotation_minute > 59)
            {
                fail(InitError::InvalidConfig, "[mylog] invalid daily rotation time");
                return;
            }
            if (cfg.daily_max_files > 65535)
            {
                fail(InitError::InvalidConfig, "[mylog] daily_max_files exceeds 65535");
                return;
            }
        }

        if (!valid_level(cfg.level) || !valid_level(cfg.flush_level) ||
            !valid_level(cfg.console_level) || !valid_level(cfg.file_level) ||
            !valid_level(cfg.callback_level) || cfg.flush_interval.count() < 0)
        {
            fail(InitError::InvalidConfig, "[mylog] invalid level or flush interval");
            return;
        }
        if (cfg.enable_file &&
            (cfg.rotation < LogConfig::RotationPolicy::BySize ||
             cfg.rotation > LogConfig::RotationPolicy::None ||
             (cfg.rotation == LogConfig::RotationPolicy::Hourly && cfg.hourly_max_files > 65535)))
        {
            fail(InitError::InvalidConfig, "[mylog] invalid rotation policy or hourly max files");
            return;
        }
        for (const auto &extra : cfg.extra_sinks)
        {
            if (!extra.factory || !valid_level(extra.level))
            {
                fail(InitError::InvalidConfig, "[mylog] invalid extra sink factory or level");
                return;
            }
            for (const auto &file : extra.file_paths)
                if (file.empty())
                {
                    fail(InitError::InvalidConfig, "[mylog] extra sink file path must not be empty");
                    return;
                }
        }

        // ---------- 2. 日志目录 ----------
        std::string path;
        if (cfg.enable_file)
        {
            path = cfg.log_dir.empty() ? cfg.log_file : (cfg.log_dir + "/" + cfg.log_file);
            if (!cfg.log_dir.empty())
            {
                std::error_code ec;
                std::filesystem::create_directories(cfg.log_dir, ec);
                if (ec)
                {
                    fail(InitError::Directory, "[mylog] cannot create log dir '" + cfg.log_dir +
                                                   "': " + ec.message());
                    return;
                }
            }
        }

        // ---------- 3. sinks ----------
        std::vector<spdlog::sink_ptr> sinks;
        spdlog::sink_ptr console_sink;
        spdlog::sink_ptr primary_sink;
        std::shared_ptr<FileLease> primary_lease;
        std::vector<std::vector<std::shared_ptr<FileLease>>> extra_leases;

        // 所有路径先登记，再执行可能打开文件的工厂，重复路径不会先被打开一次。
        try
        {
            extra_leases.resize(cfg.extra_sinks.size());
            if (cfg.enable_file)
            {
                auto request = acquire_file_lease(path);
                if (!request.lease)
                {
                    fail(request.error, request.message);
                    return;
                }
                primary_lease = std::move(request.lease);
            }
            for (std::size_t i = 0; i < cfg.extra_sinks.size(); ++i)
                for (const auto &file : cfg.extra_sinks[i].file_paths)
                {
                    auto request = acquire_file_lease(file);
                    if (!request.lease)
                    {
                        fail(request.error, request.message);
                        return;
                    }
                    extra_leases[i].push_back(std::move(request.lease));
                }
        }
        catch (const std::exception &ex)
        {
            fail(InitError::Unknown, std::string("[mylog] file lease init failed: ") + ex.what());
            return;
        }

        auto add_console = [&]
        {
            auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            sink->set_level(detail::to_spdlog_level(cfg.console_level));
            sink->set_pattern(cfg.console_pattern.value_or(cfg.pattern));
            sinks.push_back(sink);
            console_sink = std::move(sink);
        };
        try
        {
            if (cfg.enable_console)
                add_console();

            if (cfg.enable_file)
            {
                // 同一文件不允许两个存活的文件 sink（否则轮转计数互相干扰，
                // Windows 上还可能因旧句柄无法重命名而截断日志）。
                std::unique_ptr<spdlog::sinks::sink> file_sink;
                switch (cfg.rotation)
                {
                case LogConfig::RotationPolicy::Daily:
                    file_sink = std::make_unique<spdlog::sinks::daily_file_sink_mt>(
                        path, cfg.daily_rotation_hour, cfg.daily_rotation_minute,
                        false, static_cast<std::uint16_t>(cfg.daily_max_files));
                    break;
                case LogConfig::RotationPolicy::Hourly:
                    file_sink = std::make_unique<spdlog::sinks::hourly_file_sink_mt>(
                        path, false, static_cast<std::uint16_t>(cfg.hourly_max_files));
                    break;
                case LogConfig::RotationPolicy::None:
                    file_sink = std::make_unique<spdlog::sinks::basic_file_sink_mt>(path, false);
                    break;
                default:
                    file_sink = std::make_unique<spdlog::sinks::rotating_file_sink_mt>(
                        path, cfg.max_file_size, cfg.max_files);
                    break;
                }
                file_sink->set_level(detail::to_spdlog_level(cfg.file_level));
                file_sink->set_pattern(cfg.file_pattern.value_or(cfg.pattern));

                // 别名 shared_ptr：占用令牌随实际 sink 一起释放，
                // 因此“旧快照还活着”期间该文件会一直被占用。
                auto owner = std::make_shared<FileSinkOwner>();
                owner->leases.push_back(std::move(primary_lease));
                owner->sink = std::move(file_sink);
                primary_sink = spdlog::sink_ptr(owner, owner->sink.get());
                sinks.push_back(primary_sink);
            }
        }
        catch (const std::exception &ex)
        {
            const std::string msg = std::string("[mylog] file sink init failed: ") + ex.what();
            if (!cfg.fallback_to_console_on_file_error)
            {
                fail(InitError::FileSink, msg);
                return;
            }
            primary_lease.reset();
            primary_sink.reset();
            // 降级：保留控制台输出，让程序先跑起来
            st.init.degraded = true;
            st.init.message = msg;
            report_error(handler, msg + " (falling back to console)");
            try
            {
                // 显式要求的文件失败降级，即使有额外目标，也必须真的建立控制台。
                if (!console_sink)
                    add_console();
            }
            catch (const std::exception &console_error)
            {
                fail(InitError::Unknown, std::string("[mylog] console fallback failed: ") + console_error.what());
                return;
            }
        }

        // 额外目标和事件通知也是有效输出；它们存在时不自动添加控制台。
        try
        {
            for (std::size_t i = 0; i < cfg.extra_sinks.size(); ++i)
            {
                const auto &extra = cfg.extra_sinks[i];
                auto owner = std::make_shared<FileSinkOwner>();
                owner->leases = std::move(extra_leases[i]);
                owner->sink = extra.factory();
                if (!owner->sink)
                {
                    fail(InitError::InvalidConfig, "[mylog] extra sink factory returned null");
                    return;
                }
                owner->sink->set_level(detail::to_spdlog_level(extra.level));
                owner->sink->set_pattern(extra.pattern.value_or(cfg.pattern));
                sinks.emplace_back(owner, owner->sink.get());
            }
            if (cfg.log_callback)
            {
                auto callback = std::make_shared<EventSink>(&st, cfg.log_callback, handler);
                callback->set_level(detail::to_spdlog_level(cfg.callback_level));
                sinks.push_back(std::move(callback));
            }
        }
        catch (const std::exception &ex)
        {
            fail(InitError::Unknown, std::string("[mylog] extra sink init failed: ") + ex.what());
            return;
        }
        catch (...)
        {
            fail(InitError::Unknown, "[mylog] extra sink init failed: unknown exception");
            return;
        }

        // 两个输出开关都关闭（或文件降级后没有 sink）时回退控制台：
        // 明确标记为降级，避免“以为在写文件其实没有输出”。
        if (sinks.empty())
        {
            try
            {
                add_console();
                st.init.degraded = true;
                if (st.init.message.empty())
                    st.init.message = "[mylog] no output target configured; console fallback enabled";
            }
            catch (const std::exception &ex)
            {
                fail(InitError::Unknown, std::string("[mylog] console sink init failed: ") + ex.what());
                return;
            }
        }

        // ---------- 4. logger（同步 / 异步） ----------
        if (cfg.async)
        {
            try
            {
                // 每个 Logger 拥有独立线程池，避免多个实例互相抢全局池。
                // on_thread_start 记录该线程归属，供 shutdown 自 join 保护使用。
                const void *state_ptr = &st;
                st.pool = std::make_shared<spdlog::details::thread_pool>(
                    cfg.async_queue_size, cfg.async_thread_count,
                    [state_ptr] { t_worker_state = state_ptr; },
                    [] {});

                st.logger = std::make_shared<spdlog::async_logger>(
                    cfg.logger_name, sinks.begin(), sinks.end(),
                    st.pool, to_overflow_policy(cfg.async_overflow));
            }
            catch (const std::exception &ex)
            {
                st.pool.reset();
                fail(InitError::ThreadPool, std::string("[mylog] async init failed: ") + ex.what());
                return;
            }
        }
        else
        {
            try
            {
                st.logger = std::make_shared<spdlog::logger>(
                    cfg.logger_name, sinks.begin(), sinks.end());
            }
            catch (const std::exception &ex)
            {
                fail(InitError::Unknown, std::string("[mylog] logger init failed: ") + ex.what());
                return;
            }
        }

        // ---------- 5. 格式 / 回调 / 注册 ----------
        try
        {
            st.logger->set_level(detail::to_spdlog_level(cfg.level));
            st.logger->flush_on(detail::to_spdlog_level(cfg.flush_level));
            if (cfg.backtrace_capacity > 0)
                st.logger->enable_backtrace(cfg.backtrace_capacity);

            if (handler)
            {
                const void *state_ptr = &st;
                st.logger->set_error_handler([guarded = handler, state_ptr](const std::string &msg)
                {
                    // 初始化错误仍可重入单例；运行期错误回调禁止重入同一实例。
                    CallbackScope scope(state_ptr);
                    report_error(guarded, msg);
                });
            }

            if (cfg.flush_interval.count() > 0)
            {
                State *state_ptr = &st;
                st.flusher = std::make_unique<PeriodicFlusher>(cfg.flush_interval,
                    [state_ptr, guarded = handler]
                    {
                        t_timer_state = state_ptr;
                        try
                        {
                            if (state_ptr->pool)
                            {
                                auto async = std::static_pointer_cast<spdlog::async_logger>(state_ptr->logger);
                                state_ptr->pool->post_flush(std::move(async), spdlog::async_overflow_policy::block);
                            }
                            else
                                state_ptr->logger->flush();
                        }
                        catch (...)
                        {
                            CallbackScope scope(state_ptr);
                            report_error(guarded, "[mylog] periodic flush failed");
                        }
                        t_timer_state = nullptr;
                    });
            }

            // 同名旧实例先从 registry 摘掉；旧实例对象本身仍由持有者管理，
            // 其 shutdown 会用 get() 比对，不会误删新注册的同名 logger。
            st.name = cfg.logger_name;
            spdlog::drop(cfg.logger_name);
            spdlog::register_logger(st.logger);
            st.console_sink = std::move(console_sink);
            st.file_sink = std::move(primary_sink);
        }
        catch (const std::exception &ex)
        {
            st.flusher.reset();
            st.logger.reset();
            st.pool.reset();
            fail(InitError::Unknown, std::string("[mylog] init failed: ") + ex.what());
            return;
        }

        st.init.ok = true;
        st.init.error = InitError::None;
        if (st.flusher)
            st.flusher->start();
    }

    // 空状态：不创建 sink、不注册名称、不创建线程池，
    // 所有写日志与设置操作都会因为 impl() 为空而自然变成无副作用操作。
    Logger::Logger(NullTag)
        : state_(std::make_unique<State>())
    {
        state_->init.ok = false;
        state_->init.error = InitError::None;
    }

    Logger::Logger(Logger &&other) noexcept
        : state_(std::move(other.state_))
    {
    }

    Logger &Logger::operator=(Logger &&other) noexcept
    {
        if (this != &other)
        {
            if (!can_operate() || !other.can_operate() || is_on_background_thread() || other.is_on_background_thread())
                return *this;
            release();
            state_ = std::move(other.state_);
        }
        return *this;
    }

    Logger::~Logger()
    {
        if (is_on_background_thread() || !can_operate())
        {
            // 在自身后台线程上析构时不能等待线程池退出（会 join 自身），
            // 只能把状态泄漏出去，优先保证进程不崩溃。
            std::fprintf(stderr, "[mylog] Logger destroyed on its own background thread or callback; "
                                 "resources are leaked to avoid joining itself\n");
            (void)state_.release();
            return;
        }
        release();
    }

    void Logger::shutdown()
    {
        if (is_on_background_thread() || !can_operate())
        {
            std::fprintf(stderr, "[mylog] shutdown() ignored in the logger's background thread or callback; "
                                 "close it from another thread\n");
            return;
        }
        release();
    }

    void Logger::release()
    {
        State *st = state_.get();
        if (!st)
            return;

        st->flusher.reset();
        if (st->logger)
        {
            try
            {
                // 异步实例的 flush 本身就是一条队列消息，会沿用该实例的溢出策略。
                // 关闭是控制动作，必须阻塞入队，否则 OverrunOldest 下会挤掉
                // 队列里尚未写出的用户日志（生产者此时已停止，属于额外丢失）。
                auto async = st->pool ? std::dynamic_pointer_cast<spdlog::async_logger>(st->logger)
                                      : nullptr;
                if (async)
                    st->pool->post_flush(std::move(async), spdlog::async_overflow_policy::block);
                else
                    st->logger->flush();
            }
            catch (...)
            {
            }
        }

        if (!st->name.empty())
        {
            try
            {
                // 只在 registry 里的同名项仍然是本实例时才 drop，
                // 避免旧实例关闭时误删刚接管同名的新实例。
                auto registered = spdlog::get(st->name);
                if (registered && registered.get() == st->logger.get())
                    spdlog::drop(st->name);
            }
            catch (...)
            {
            }
            st->name.clear();
        }

        // 先释放 logger（可能还在向 pool 投递），再释放线程池
        st->logger.reset();
        st->pool.reset();
        st->console_sink.reset();
        st->file_sink.reset();
        st->init.ok = false;
    }

    bool Logger::is_on_worker_thread() const noexcept
    {
        return state_ && t_worker_state == state_.get();
    }

    bool Logger::is_on_background_thread() const noexcept
    {
        return state_ && (t_worker_state == state_.get() || t_timer_state == state_.get());
    }

    bool Logger::can_operate() const noexcept
    {
        return !CallbackScope::contains(state_.get());
    }

    spdlog::logger *Logger::impl() const noexcept
    {
        return (state_ && state_->logger) ? state_->logger.get() : nullptr;
    }

    bool Logger::is_initialized() const noexcept
    {
        return impl() != nullptr;
    }

    const std::string &Logger::name() const noexcept
    {
        static const std::string empty_name;
        return state_ ? state_->name : empty_name;
    }

    const InitResult &Logger::init_result() const noexcept
    {
        static const InitResult empty_result;
        return state_ ? state_->init : empty_result;
    }

    void Logger::set_level(Level lvl)
    {
        if (!can_operate())
            return;
        if (auto *target = impl())
            target->set_level(detail::to_spdlog_level(lvl));
    }

    void Logger::set_flush_level(Level lvl)
    {
        if (!can_operate())
            return;
        if (auto *target = impl())
            target->flush_on(detail::to_spdlog_level(lvl));
    }

    void Logger::set_pattern(const std::string &pattern)
    {
        if (!can_operate())
            return;
        if (auto *target = impl())
            target->set_pattern(pattern);
    }

    void Logger::flush()
    {
        if (!can_operate())
            return;
        if (auto *target = impl())
            target->flush();
    }

    void Logger::set_console_level(Level lvl)
    {
        if (can_operate() && state_ && state_->console_sink)
            state_->console_sink->set_level(detail::to_spdlog_level(lvl));
    }

    void Logger::set_file_level(Level lvl)
    {
        if (can_operate() && state_ && state_->file_sink)
            state_->file_sink->set_level(detail::to_spdlog_level(lvl));
    }

    void Logger::set_console_pattern(const std::string &pattern)
    {
        if (can_operate() && state_ && state_->console_sink)
            state_->console_sink->set_pattern(pattern);
    }

    void Logger::set_file_pattern(const std::string &pattern)
    {
        if (can_operate() && state_ && state_->file_sink)
            state_->file_sink->set_pattern(pattern);
    }

    void Logger::set_formatter(std::unique_ptr<spdlog::formatter> formatter)
    {
        if (formatter && can_operate())
            if (auto *target = impl())
                target->set_formatter(std::move(formatter));
    }

    void Logger::enable_backtrace(std::size_t capacity)
    {
        if (!can_operate())
            return;
        if (auto *target = impl())
        {
            if (capacity > 0)
                target->enable_backtrace(capacity);
            else
                target->disable_backtrace();
        }
    }

    void Logger::disable_backtrace()
    {
        enable_backtrace(0);
    }

    void Logger::dump_backtrace()
    {
        if (can_operate())
            if (auto *target = impl())
                target->dump_backtrace();
    }

    bool Logger::should_log(Level lvl) const
    {
        auto *target = impl();
        return target && target->should_log(detail::to_spdlog_level(lvl));
    }

    Level Logger::level() const
    {
        auto *target = impl();
        return target ? detail::from_spdlog_level(target->level()) : Level::Off;
    }

    AsyncStats Logger::stats() const
    {
        AsyncStats result;
        if (!state_ || !state_->pool)
            return result;

        result.queue_size = state_->pool->queue_size();
        result.overrun_count = state_->pool->overrun_counter();
        result.discard_count = state_->pool->discard_counter();
        return result;
    }

    void Logger::log(spdlog::source_loc loc, Level lvl, std::string_view msg)
    {
        if (!can_operate())
            return;
        if (auto *target = impl())
            target->log(loc, detail::to_spdlog_level(lvl), msg);
    }

    // ---------------------- SingletonLogger ----------------------

    namespace
    {
        // 用原子读写代替全局锁：日志热路径不再争抢同一把互斥量，
        // 同时保证 acquire() 拿到的 shared_ptr 快照是完整的。
        std::shared_ptr<Logger> g_singleton;
        std::mutex g_singleton_mutex; // 只用于串行化 init()/shutdown() 的交换动作
    } // namespace

    std::shared_ptr<Logger> SingletonLogger::acquire()
    {
        return std::atomic_load(&g_singleton);
    }

    Logger &SingletonLogger::instance()
    {
        auto current = std::atomic_load(&g_singleton);
        if (current)
            return *current;

        static Logger empty_logger{Logger::NullTag{}};
        return empty_logger;
    }

    void SingletonLogger::release_ref(std::shared_ptr<Logger> logger)
    {
        if (!logger || (!logger->is_on_background_thread() && logger->can_operate()))
            return;
        // 后台线程不能回收自己的线程池/定时器；同步回调也不能销毁正在使用的 sink。
        try
        {
            std::thread([ptr = std::move(logger)]() mutable { ptr.reset(); }).detach();
        }
        catch (...)
        {
            // 无法启动回收线程时，析构中的保护分支会避免自等待。
        }
    }

    InitResult SingletonLogger::init_checked(const LogConfig &cfg)
    {
        // 关键：构造在锁外完成。初始化失败时错误回调可以安全地再次
        // 访问单例（例如 LOGS_ERROR），不会自锁。
        auto candidate = std::make_shared<Logger>(cfg);
        if (!candidate->is_initialized())
        {
            InitResult result = candidate->init_result();
            return result; // 失败时保留原有实例，不影响已在运行的服务
        }

        std::shared_ptr<Logger> previous;
        {
            std::lock_guard<std::mutex> lock(g_singleton_mutex);
            previous = std::atomic_load(&g_singleton);
            std::atomic_store(&g_singleton, candidate);
        }

        // 锁外释放旧实例：若仍有 acquire() 快照存活，
        // 资源会延迟到最后一个快照释放时回收。
        release_ref(std::move(previous));
        return candidate->init_result();
    }

    bool SingletonLogger::init(const LogConfig &cfg)
    {
        return init_checked(cfg).ok;
    }

    void SingletonLogger::shutdown()
    {
        std::shared_ptr<Logger> previous;
        {
            std::lock_guard<std::mutex> lock(g_singleton_mutex);
            previous = std::atomic_load(&g_singleton);
            std::atomic_store(&g_singleton, std::shared_ptr<Logger>{});
        }

        release_ref(std::move(previous));
    }

} // namespace mylog
