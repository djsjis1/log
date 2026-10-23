// Logger.cpp
#include "Logger.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "spdlog/async.h"
#include "spdlog/sinks/daily_file_sink.h"
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

    // 实例的全部可变状态都放在堆上：地址稳定，因此移动 Logger、
    // 以及在错误回调里保存指针都不会悬空。
    struct Logger::State
    {
        // 声明顺序即析构逆序：logger 必须先于 pool 释放
        std::shared_ptr<spdlog::details::thread_pool> pool;
        std::shared_ptr<spdlog::logger> logger;
        std::string name;
        InitResult init;
    };

    namespace
    {
        // 记录当前线程属于哪个 Logger 实例的异步线程池
        thread_local const void *t_worker_state = nullptr;

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

        // 释放单例的全局引用。真正的销毁发生在最后一个快照释放时；
        // 若当前就在该实例的后台线程上，必须把释放动作交给独立线程，
        // 否则线程池析构时会 join 自己。
        void release_singleton_ref(std::shared_ptr<Logger> logger)
        {
            if (!logger)
                return;

            if (!logger->is_on_worker_thread())
                return; // 交给形参析构即可

            try
            {
                std::thread([ptr = std::move(logger)]() mutable { ptr.reset(); }).detach();
            }
            catch (...)
            {
                // 无法创建回收线程时保持原状：资源随进程退出，
                // Logger 析构中的 worker 保护逻辑会避免自 join。
            }
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

        try
        {
            if (cfg.enable_console)
                sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

            if (cfg.enable_file)
            {
                spdlog::sink_ptr file_sink;
                if (cfg.rotation == LogConfig::RotationPolicy::Daily)
                {
                    file_sink = std::make_shared<spdlog::sinks::daily_file_sink_mt>(
                        path, cfg.daily_rotation_hour, cfg.daily_rotation_minute,
                        false, static_cast<std::uint16_t>(cfg.daily_max_files));
                }
                else
                {
                    file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                        path, cfg.max_file_size, cfg.max_files);
                }
                sinks.push_back(file_sink);
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
            // 降级：保留控制台输出，让程序先跑起来
            st.init.degraded = true;
            st.init.message = msg;
            report_error(handler, msg + " (falling back to console)");
        }

        // 两个输出开关都关闭（或文件降级后没有 sink）时回退控制台：
        // 明确标记为降级，避免“以为在写文件其实没有输出”。
        if (sinks.empty())
        {
            try
            {
                sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
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
            st.logger->set_pattern(cfg.pattern);

            if (handler)
            {
                auto guarded = handler;
                st.logger->set_error_handler([guarded](const std::string &msg)
                {
                    // 运行期错误回调可能在异步后台线程上执行，
                    // 这里只隔离异常，不替用户做关闭动作。
                    try
                    {
                        guarded(msg);
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
                });
            }

            // 同名旧实例先从 registry 摘掉；旧实例对象本身仍由持有者管理，
            // 其 shutdown 会用 get() 比对，不会误删新注册的同名 logger。
            spdlog::drop(cfg.logger_name);
            spdlog::register_logger(st.logger);

            st.name = cfg.logger_name;
        }
        catch (const std::exception &ex)
        {
            st.logger.reset();
            st.pool.reset();
            fail(InitError::Unknown, std::string("[mylog] init failed: ") + ex.what());
            return;
        }

        st.init.ok = true;
        st.init.error = InitError::None;
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
            release();
            state_ = std::move(other.state_);
        }
        return *this;
    }

    Logger::~Logger()
    {
        if (is_on_worker_thread())
        {
            // 在自身后台线程上析构时不能等待线程池退出（会 join 自身），
            // 只能把状态泄漏出去，优先保证进程不崩溃。
            std::fprintf(stderr, "[mylog] Logger destroyed on its own async worker thread; "
                                 "resources are leaked to avoid joining itself\n");
            (void)state_.release();
            return;
        }
        release();
    }

    void Logger::shutdown()
    {
        if (is_on_worker_thread())
        {
            std::fprintf(stderr, "[mylog] shutdown() ignored on the logger's own async worker thread; "
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

        if (st->logger)
        {
            try
            {
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
        st->init.ok = false;
    }

    bool Logger::is_on_worker_thread() const noexcept
    {
        return state_ && t_worker_state == state_.get();
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
        if (auto *target = impl())
            target->set_level(detail::to_spdlog_level(lvl));
    }

    void Logger::set_flush_level(Level lvl)
    {
        if (auto *target = impl())
            target->flush_on(detail::to_spdlog_level(lvl));
    }

    void Logger::set_pattern(const std::string &pattern)
    {
        if (auto *target = impl())
            target->set_pattern(pattern);
    }

    void Logger::flush()
    {
        if (auto *target = impl())
            target->flush();
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
        release_singleton_ref(std::move(previous));
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

        release_singleton_ref(std::move(previous));
    }

} // namespace mylog
