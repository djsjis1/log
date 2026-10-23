// Logger.h
// 对 spdlog 的轻量封装：业务代码仅通过本文件提供的宏/类使用日志，
// 将来若要替换底层日志库，只需修改 Logger.h / Logger.cpp 即可。
//
// 线程安全约定（重要）：
//   * 同一个 Logger 实例可以被多个线程同时写日志；
//   * 但 shutdown() / 析构 不能与写日志并发：调用者需先停止写日志再关闭；
//   * 访问全局单例时，优先使用 SingletonLogger::acquire() 取得快照，
//     它会延长实例生命周期，即使其他线程同时执行 shutdown() 也不会悬空；
//   * SingletonLogger::shutdown() 只解除全局持有，真正的资源回收发生在
//     最后一个快照释放时，因此不会与正在写日志的线程互踩。
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "spdlog/common.h"
#include "spdlog/spdlog.h"

namespace mylog
{

    // 对外暴露的日志级别（与 spdlog 对齐，业务代码不直接依赖其枚举值）
    enum class Level : int
    {
        Trace = 0,
        Debug = 1,
        Info = 2,
        Warn = 3,
        Error = 4,
        Critical = 5,
        Off = 6
    };

    // 异步队列满时的处理策略
    enum class OverflowPolicy : int
    {
        Block = 0,       // 阻塞生产者直到队列有空位（不丢日志，默认）
        OverrunOldest = 1, // 覆盖队列中最旧的消息
        DiscardNew = 2     // 丢弃新产生的消息
    };

    // 初始化失败分类，便于调用方决定重试还是降级
    enum class InitError : int
    {
        None = 0,
        InvalidConfig, // 配置本身不合法（空名字、非法轮转参数等）
        Directory,     // 日志目录创建失败
        FileSink,      // 文件 sink 创建失败
        ThreadPool,    // 异步线程池创建失败
        Unknown        // 其他异常
    };

    // 初始化结果：既能当 bool 用，也能拿到失败原因
    struct InitResult
    {
        bool ok = false;       // 日志器是否可用
        bool degraded = false; // 可用但能力被降级（如文件输出回退到控制台）
        InitError error = InitError::None;
        std::string message; // 失败或降级说明

        explicit operator bool() const noexcept { return ok; }
    };

    // 异步队列运行期统计
    struct AsyncStats
    {
        std::size_t queue_size = 0;    // 当前积压的消息条数
        std::size_t overrun_count = 0; // 被覆盖的最旧消息数（OverrunOldest）
        std::size_t discard_count = 0; // 被丢弃的新消息数（DiscardNew）
    };

    namespace detail
    {
        // 内部工具：mylog::Level <-> spdlog::level::level_enum
        spdlog::level::level_enum to_spdlog_level(Level lvl);
        Level from_spdlog_level(spdlog::level::level_enum lvl);
    }

    // 日志配置项，均有默认值，可按需覆盖
    struct LogConfig
    {
        std::string logger_name = "app"; // logger 名称（多实例时需唯一）
        std::string log_dir = "logs";    // 日志目录（相对或绝对路径）
        std::string log_file = "app.log"; // 日志文件名
        bool enable_console = true;       // 是否输出到控制台（带颜色）
        bool enable_file = true;          // 是否输出到文件（滚动）
        bool async = false;               // 是否使用异步日志
        Level level = Level::Info;        // 日志级别
        Level flush_level = Level::Warn;  // 触发自动 flush 的级别
        std::string pattern = "[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] [%t] [%s:%#] %v"; // 日志格式

        // ---- 文件滚动策略 ----
        enum class RotationPolicy : int
        {
            BySize, // 按大小滚动（默认）
            Daily   // 按天滚动（在每天指定时刻切割）
        };
        RotationPolicy rotation = RotationPolicy::BySize;
        std::size_t max_file_size = 10 * 1024 * 1024; // 单文件最大字节数 (10MB)，仅 BySize 生效
        std::size_t max_files = 5;                    // 保留的滚动文件数量，仅 BySize 生效
        std::size_t daily_max_files = 0;              // 保留的日志文件数量，仅 Daily 生效（0=不删除）
        int daily_rotation_hour = 0;                  // 切割时刻（时），仅 Daily 生效
        int daily_rotation_minute = 0;                // 切割时刻（分），仅 Daily 生效

        // ---- 异步选项 ----
        std::size_t async_queue_size = 8192;  // 异步队列容量（消息条数，不是字节数）
        std::size_t async_thread_count = 1;   // 异步后台线程数（1 可保证写入顺序）
        OverflowPolicy async_overflow = OverflowPolicy::Block; // 队列满时的处理策略

        // 文件 sink 创建失败时是否降级为控制台输出。
        // false（默认）= 视为初始化失败；true = 仍可用，但 InitResult::degraded 为 true。
        bool fallback_to_console_on_file_error = false;

        // ---- 错误回调 ----
        // 初始化失败、降级或运行时输出错误时调用（可选）。
        // 注意：可能在任何线程（包括异步后台线程）被调用；回调内不要同步关闭
        // 同一个 Logger，也不要向外抛异常（库内部会兜底捕获）。
        std::function<void(const std::string &msg)> error_handler;
    };

    // 普通类：由用户自己持有（构造函数接收配置并立即初始化）。
    // 不可默认构造，不可拷贝，可移动（内部状态地址稳定，移动不会破坏回调）。
    class Logger
    {
    public:
        explicit Logger(const LogConfig &cfg);
        ~Logger();

        Logger(const Logger &) = delete;
        Logger &operator=(const Logger &) = delete;
        Logger(Logger &&) noexcept;
        Logger &operator=(Logger &&) noexcept;

        // 立即释放资源（析构时也会调用）。调用前必须确保没有其他线程正在写日志。
        // 若在自身的异步后台线程上调用，会打印警告并直接返回（避免线程 join 自身）。
        void shutdown();

        // 当前线程是否是该实例的异步后台线程
        bool is_on_worker_thread() const noexcept;

        // 运行期设置
        void set_level(Level lvl);
        void set_flush_level(Level lvl);
        void set_pattern(const std::string &pattern);
        void flush();

        bool is_initialized() const noexcept;
        const std::string &name() const noexcept;
        const InitResult &init_result() const noexcept;

        // 级别判断：可用于避免提前计算昂贵的日志参数
        bool should_log(Level lvl) const;
        Level level() const;

        // 异步队列统计（同步模式全为 0）
        AsyncStats stats() const;

        // 通用 log 接口：由宏调用，带源码位置
        template <typename... Args>
        void log(spdlog::source_loc loc,
                 Level lvl,
                 spdlog::format_string_t<Args...> fmt,
                 Args &&...args)
        {
            auto *target = impl();
            if (!target)
                return;
            target->log(loc, detail::to_spdlog_level(lvl), fmt, std::forward<Args>(args)...);
        }

        // 不带格式化的原始字符串版本
        void log(spdlog::source_loc loc, Level lvl, std::string_view msg);

    private:
        struct State;
        struct NullTag
        {
        };

        // 空状态实例（不创建 sink、不注册名称），仅用于单例未初始化时兜底
        explicit Logger(NullTag);
        Logger() = delete; // 必须通过 LogConfig 构造

        spdlog::logger *impl() const noexcept;
        void release();

        std::unique_ptr<State> state_;
        friend class SingletonLogger;
    };

    // 全局单例包装：无需手动持有 Logger，直接 LOGS_XXX(...) 即可。
    // 使用前必须先调用 SingletonLogger::init(cfg)。
    class SingletonLogger
    {
    public:
        // 兼容接口：返回借用引用。仅适用于“初始化后、关闭前”的串行使用，
        // 不能抵御其他线程并发调用 shutdown()。
        static Logger &instance();

        // 安全接口：返回持有实例所有权的快照，可跨线程安全使用。
        static std::shared_ptr<Logger> acquire();

        // 初始化；失败时保留原有实例不变
        static bool init(const LogConfig &cfg = LogConfig{});
        static InitResult init_checked(const LogConfig &cfg = LogConfig{});

        // 解除全局持有；若仍有 acquire() 快照存活，资源会在最后一个快照释放时回收。
        static void shutdown();

    private:
        SingletonLogger() = delete;
    };

    namespace detail
    {
        // 单例日志入口：持有一份快照后再写，shutdown 并发时也不会悬空
        template <typename... Args>
        void log_singleton(spdlog::source_loc loc,
                           Level lvl,
                           spdlog::format_string_t<Args...> fmt,
                           Args &&...args)
        {
            if (auto logger = SingletonLogger::acquire())
                logger->log(loc, lvl, fmt, std::forward<Args>(args)...);
        }

        inline void log_singleton(spdlog::source_loc loc, Level lvl, std::string_view msg)
        {
            if (auto logger = SingletonLogger::acquire())
                logger->log(loc, lvl, msg);
        }
    } // namespace detail

} // namespace mylog

// ----------------------------- 对外宏 -----------------------------
// 两套日志宏：
//   1) LOG_XXX(logger, ...) ：传 mylog::Logger 实例（左值），适合多实例场景。
//   2) LOGS_XXX(...)        ：使用全局单例，无需传 logger，更简洁。
//      使用前需先调用 mylog::SingletonLogger::init(cfg)。
#define MYLOG_SRC_LOC \
    ::spdlog::source_loc { __FILE__, __LINE__, SPDLOG_FUNCTION }

// ---- 带实例参数的宏 ----
#define LOG_TRACE(logger, ...) \
    (logger).log(MYLOG_SRC_LOC, ::mylog::Level::Trace, __VA_ARGS__)
#define LOG_DEBUG(logger, ...) \
    (logger).log(MYLOG_SRC_LOC, ::mylog::Level::Debug, __VA_ARGS__)
#define LOG_INFO(logger, ...) \
    (logger).log(MYLOG_SRC_LOC, ::mylog::Level::Info, __VA_ARGS__)
#define LOG_WARN(logger, ...) \
    (logger).log(MYLOG_SRC_LOC, ::mylog::Level::Warn, __VA_ARGS__)
#define LOG_ERROR(logger, ...) \
    (logger).log(MYLOG_SRC_LOC, ::mylog::Level::Error, __VA_ARGS__)
#define LOG_CRITICAL(logger, ...) \
    (logger).log(MYLOG_SRC_LOC, ::mylog::Level::Critical, __VA_ARGS__)

// ---- 使用全局单例的宏（每次调用持有一份快照，可与其他线程的 shutdown 并发） ----
#define LOGS_TRACE(...) \
    ::mylog::detail::log_singleton(MYLOG_SRC_LOC, ::mylog::Level::Trace, __VA_ARGS__)
#define LOGS_DEBUG(...) \
    ::mylog::detail::log_singleton(MYLOG_SRC_LOC, ::mylog::Level::Debug, __VA_ARGS__)
#define LOGS_INFO(...) \
    ::mylog::detail::log_singleton(MYLOG_SRC_LOC, ::mylog::Level::Info, __VA_ARGS__)
#define LOGS_WARN(...) \
    ::mylog::detail::log_singleton(MYLOG_SRC_LOC, ::mylog::Level::Warn, __VA_ARGS__)
#define LOGS_ERROR(...) \
    ::mylog::detail::log_singleton(MYLOG_SRC_LOC, ::mylog::Level::Error, __VA_ARGS__)
#define LOGS_CRITICAL(...) \
    ::mylog::detail::log_singleton(MYLOG_SRC_LOC, ::mylog::Level::Critical, __VA_ARGS__)
