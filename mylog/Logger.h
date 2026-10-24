// Logger.h
// 对 spdlog 的轻量封装：业务代码只通过本文件的宏/类使用日志。
//
// 线程与生命周期契约（与实现一致，改实现时必须同步这里）：
//   * 同一个 Logger 实例可以被多个线程同时写日志；
//   * 但 shutdown()/析构不能与该实例上的写日志并发：release() 会销毁内部
//     spdlog::logger，热路径刚取到的裸指针会立刻失效；
//   * 全局单例请用 SingletonLogger::acquire() 取快照。快照延长的是"对象生命周期"，
//     它能抵御别的线程调用 SingletonLogger::shutdown()，但这并不表示可以并发
//     调用某个 Logger 上的 shutdown()；
//   * 最后一个快照若在后台线程（异步消费、定时刷新）或用户回调里释放，析构会
//     交给进程内的回收线程执行：既不 join 自己，也不泄漏资源；
//     SingletonLogger::shutdown() 返回前会等待这些回收完成。
//
// 文件占用：同一日志文件（规范化后相同）在进程内只允许一个存活的文件 sink，
// 因为轮转状态保存在 sink 内部。冲突时初始化失败（InitError::FileSink），
// 不会降级到控制台；旧快照存活期间该文件仍被视为占用。
#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "spdlog/common.h"
#include "spdlog/logger.h"

namespace mylog
{

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

    enum class OverflowPolicy : int
    {
        Block = 0,         // 队列满时写日志的线程等待，不丢日志（默认）
        OverrunOldest = 1, // 挤掉最旧的一条，保住最新现场
        DiscardNew = 2     // 丢掉刚来的新消息，保住已排队消息的顺序
    };

    enum class InitError : int
    {
        None = 0,      // 没有错误（成功，或尚未尝试）
        InvalidConfig, // 配置本身不合法（空名字、非法轮转参数等）
        Directory,     // 日志目录创建失败
        FileSink,      // 文件 sink 创建失败（含文件被其他日志器占用）
        ThreadPool,    // 异步线程池创建失败
        Unknown        // 其他异常
    };

    struct InitResult
    {
        bool ok = false;
        bool degraded = false;             // 可用但能力被降级（如文件输出回退到控制台）
        InitError error = InitError::None; // 失败原因
        std::string message;               // 失败或降级的具体说明

        explicit operator bool() const noexcept { return ok; }
    };

    struct AsyncStats
    {
        std::size_t queue_size = 0;
        std::size_t overrun_count = 0;
        std::size_t discard_count = 0;
    };

    namespace detail
    {
        // 库内部使用：把本库的 Level 翻译成 spdlog 的级别。
        // 非法值一律返回 off，而不是悄悄变成 info。
        spdlog::level::level_enum to_spdlog_level(Level lvl);
    }

    struct LogEvent
    {
        Level level;
        std::string logger_name;
        std::string message;
    };

    // 工厂每次必须返回一个新建的、线程安全的 sink，不得返回已被他人持有的裸指针。
    // 文件型 sink 必须声明全部基础路径；路径不可在运行期改变。
    struct SinkConfig
    {
        std::function<std::unique_ptr<spdlog::sinks::sink>()> factory;
        Level level = Level::Trace;
        std::optional<std::string> pattern;
        std::vector<std::string> file_paths;
    };

    struct LogConfig
    {
        std::string logger_name = "app";
        std::string log_dir = "logs";
        std::string log_file = "app.log";
        bool enable_console = true;
        bool enable_file = true;
        bool async = false;
        Level level = Level::Info;
        Level flush_level = Level::Warn;
        std::string pattern = "[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] [%t] [%s:%#] %v";

        enum class RotationPolicy : int
        {
            BySize,
            Daily,
            Hourly,
            None
        };
        RotationPolicy rotation = RotationPolicy::BySize;
        std::size_t max_file_size = 10 * 1024 * 1024; // 仅 BySize 生效
        std::size_t max_files = 5;                    // 仅 BySize 生效
        std::size_t daily_max_files = 7;              // 仅 Daily 生效，0=不删除
        int daily_rotation_hour = 0;                  // 仅 Daily 生效
        int daily_rotation_minute = 0;                // 仅 Daily 生效

        std::size_t async_queue_size = 8192;      // 消息条数，不是字节数
        std::size_t async_thread_count = 1;       // 1 可保证写入顺序
        OverflowPolicy async_overflow = OverflowPolicy::Block;

        // 文件 sink 创建失败时是否降级为控制台。false=初始化失败；
        // true=仍可用，但 InitResult::degraded 为 true。
        bool fallback_to_console_on_file_error = false;

        // 初始化失败、降级或运行期输出错误时调用（可选），可能在任何线程上被调用。
        // 回调内不要同步关闭同一个 Logger，也不要抛异常（库内部会兜底捕获）。
        // 初始化失败的回调可以继续用 LOGS_* 写旧实例；回调内调用 SingletonLogger
        // 的 init/shutdown 会被拒绝（返回失败）而不是死锁。
        std::function<void(const std::string &msg)> error_handler;

        Level console_level = Level::Trace; // 总门槛 level 先过滤，之后各目标再独立过滤
        Level file_level = Level::Trace;
        std::optional<std::string> console_pattern;
        std::optional<std::string> file_pattern;
        std::size_t hourly_max_files = 24; // 仅 Hourly 生效，0=不删除，最大 65535
        std::vector<SinkConfig> extra_sinks;

        // 正常日志的通知，与"日志系统自身出错"的 error_handler 不同。
        // 同步时在调用线程、异步时在消费线程执行，回调必须自行保护共享数据。
        // 回调内对同一实例的日志、修改、刷新、回溯和直接关闭会被忽略；
        // 写另一个 Logger 是允许的（所有实例的回溯导出已串行化）。
        // 回溯导出的日志也会触发通知；通知不表示其他目标已成功写出。
        std::function<void(const LogEvent &)> log_callback;
        Level callback_level = Level::Error;

        // 0=关闭。dump_backtrace() 直接走 sink 层，绕过 logger 总门槛，
        // 只受各目标自己的 level 限制（这是 spdlog 的既有行为）。
        std::size_t backtrace_capacity = 0;
        std::chrono::milliseconds flush_interval{0}; // 0=关闭；启用时登记到全进程共享的调度线程
    };

    // 普通日志器：由用户自己创建和持有，每个实例管一套输出目标。
    // 不可默认构造，不可拷贝，可以移动。
    class Logger
    {
    public:
        explicit Logger(const LogConfig &cfg);
        ~Logger();

        Logger(const Logger &) = delete;
        Logger &operator=(const Logger &) = delete;

        Logger(Logger &&) noexcept;
        // 在自身回调或后台线程上不生效（Debug 断言），其余情况等价于先 release 再接管。
        Logger &operator=(Logger &&) noexcept;

        // 立即释放资源（析构时也会调用）。调用前必须确保没有其他线程正在写日志。
        // 若在自身的异步线程、定时线程或用户回调中调用，会打印警告并直接返回。
        void shutdown();

        // 当前线程是否属于本实例的后台线程（异步消费线程或定时刷新线程）
        bool is_on_background_thread() const noexcept;

        // ---- 运行期修改：不用重建日志器 ----
        void set_level(Level lvl);                    // 非法值被拒绝并上报 error_handler
        void set_flush_level(Level lvl);              // 达到该级别时自动刷盘
        void set_pattern(const std::string &pattern); // 修改输出格式

        // 异步时以 blocking 方式投递刷新请求：队列满会等，不会挤掉已排队的日志；
        // 两种模式都不等待刷新真正完成（要等写完只能用 shutdown）。
        void flush();

        // 等写入并刷盘：同步实例直接刷；异步实例先等队列排空再投一次刷新，
        // 最后由调用线程把每个目标刷一遍。成功 true；超时或实例不可用 false。
        // 残留边界：若某条日志此刻正在消费线程上写入，它可能落在本次刷新之后；
        // 要绝对保证全部写完，仍然只有 shutdown()。
        bool flush_and_wait(std::chrono::milliseconds timeout);

        // 对应目标未启用时不操作；后调用的格式设置覆盖之前的设置。
        void set_console_level(Level lvl);
        void set_file_level(Level lvl);
        void set_console_pattern(const std::string &pattern);
        void set_file_pattern(const std::string &pattern);
        void set_formatter(std::unique_ptr<spdlog::formatter> formatter);

        // 缓存运行期被过滤的日志，按需手动导出；编译期删除的调用不会进入缓存。
        // 导出期间 spdlog 会持有本实例的回溯锁并直接调用全部目标（含用户回调），
        // 所以本库把"所有实例的导出"串行化：回调里写别的 Logger 也不会形成
        // "A 等 B 的回溯锁、B 等 A 的回溯锁"这种死锁。导出是低频诊断操作。
        void enable_backtrace(std::size_t capacity);
        void disable_backtrace();
        void dump_backtrace();

        // ---- 状态查询 ----
        bool is_initialized() const noexcept; // 初始化是否成功且尚未关闭
        const std::string &name() const noexcept; // 构造后不变，关闭也不会清空
        // 初始化结果；Logger 关闭后返回"未初始化且无错误说明"的空结果。
        const InitResult &init_result() const noexcept;

        // 先判断这个级别会不会输出，再决定要不要构造昂贵的参数。
        bool should_log(Level lvl) const;

        // 宏内部使用：这条日志是否还有意义——通过级别门槛，或者正被回溯缓存收集
        //（后者 dump 时才输出，但现在必须把参数求值并存进环形缓冲）。
        bool should_process(Level lvl) const;

        Level level() const; // 当前生效的级别（不可用时返回 Off）

        // 取底层的 spdlog 日志器，方便直接用 spdlog 原生 API（set_pattern、sinks() 等）。
        // 返回的 shared_ptr 会延长 sink（含文件句柄）的生命周期：不要跟到 shutdown() 之后，
        // 否则同一文件会一直被视为占用；实例不可用时返回 nullptr。
        std::shared_ptr<spdlog::logger> native() const;

        AsyncStats stats() const; // 同步模式下全为 0

        // 通用写日志入口：所有 LOG_XXX 宏最终都落到这里。
        // 日志器不可用时直接返回，不崩溃也不输出任何内容。
        template <typename... Args>
        void log(spdlog::source_loc loc,
                 Level lvl,
                 spdlog::format_string_t<Args...> fmt,
                 Args &&...args)
        {
            if (!can_operate())
                return;
            auto *target = impl();
            if (!target)
                return;
            target->log(loc, detail::to_spdlog_level(lvl), fmt, std::forward<Args>(args)...);
        }

        // 同上，但只接受一段完整字符串，不做 {} 占位符替换
        void log(spdlog::source_loc loc, Level lvl, std::string_view msg);

    private:
        struct State;

        // 单例初始化时"先构造、后注册"：注册必须与全局指针交换在同一临界区。
        struct DeferRegister
        {
        };

        // 共享指针的销毁策略：最后一个快照可能是在后台线程或用户回调里释放的，
        // 那里不能 join 自己，因此转交给进程内的回收线程。
        struct DeferredDeleter
        {
            void operator()(Logger *logger) const noexcept;
        };

        explicit Logger(const LogConfig &cfg, DeferRegister);
        Logger() = delete;

        spdlog::logger *impl() const noexcept;
        bool can_operate() const noexcept;

        // 把内部 spdlog 日志器登记到 spdlog registry（同名项先摘掉）。
        // 调用方必须持有 g_singleton_mutex，保证与全局指针交换保持原子。
        void attach_registry();

        void release();

        std::unique_ptr<State> state_;
        friend class SingletonLogger;
    };

    // 全局单例包装：不用自己持有 Logger，直接 LOGS_XXX(...) 就能写日志。
    // 正式使用前先调用 SingletonLogger::init(cfg)；忘记 init 也不会崩，日志被静默丢弃。
    class SingletonLogger
    {
    public:
        // 推荐接口：返回一份持有所有权的快照，即使别的线程调用了 shutdown() 也不会悬空。
        // 代价：每条 LOGS_* 要取一次快照（约 20~26ns，实测不随线程数放大）；
        // 热循环可循环外取一次、循环内用 LOG_INFO(*snapshot, ...)，
        // 但期间旧实例不回收，换配置对手里的快照也不生效。
        static std::shared_ptr<Logger> acquire();

        // 初始化；失败时保留原有实例不变（失败原因可通过 init_checked 获取）。
        // 同一日志文件仍被旧实例或旧快照占用时返回失败，需先 shutdown() 并释放全部快照。
        static bool init(const LogConfig &cfg = LogConfig{});

        // 同上，但返回 InitResult，能拿到失败分类和说明。
        static InitResult init_checked(const LogConfig &cfg = LogConfig{});

        // 解除全局持有；若仍有 acquire() 快照存活，资源在最后一个快照释放时回收。
        // 返回前会等待"转交给回收线程"的实例真正销毁，因此随后可以立刻重开同一文件。
        static void shutdown();

    private:
        SingletonLogger() = delete;
    };

} // namespace mylog

// ----------------------------- 对外宏 -----------------------------
// 两套日志宏：
//   1) LOG_XXX(logger, ...) ：传 mylog::Logger 实例，适合多实例场景。
//   2) LOGS_XXX(...)        ：使用全局单例，使用前需先 SingletonLogger::init(cfg)。
// 两者都先查 should_log：运行期被过滤时参数表达式一个都不会求值。
// 每个宏都是单条语句（do{...}while(0)），可以安全地放在不带花括号的 if/else 里。

#define MYLOG_SRC_LOC \
    ::spdlog::source_loc { __FILE__, __LINE__, SPDLOG_FUNCTION }

// 编译期裁剪独立于 spdlog。默认保留全部调用；使用方可在目标编译定义中设置
// MYLOG_ACTIVE_LEVEL=MYLOG_LEVEL_INFO。被裁剪的调用连编译都不需要成立。
#define MYLOG_LEVEL_TRACE 0
#define MYLOG_LEVEL_DEBUG 1
#define MYLOG_LEVEL_INFO 2
#define MYLOG_LEVEL_WARN 3
#define MYLOG_LEVEL_ERROR 4
#define MYLOG_LEVEL_CRITICAL 5
#define MYLOG_LEVEL_OFF 6
#ifndef MYLOG_ACTIVE_LEVEL
#define MYLOG_ACTIVE_LEVEL MYLOG_LEVEL_TRACE
#endif
#if MYLOG_ACTIVE_LEVEL < MYLOG_LEVEL_TRACE || MYLOG_ACTIVE_LEVEL > MYLOG_LEVEL_OFF
#error "MYLOG_ACTIVE_LEVEL must be between 0 and 6"
#endif
static_assert(MYLOG_ACTIVE_LEVEL >= MYLOG_LEVEL_TRACE && MYLOG_ACTIVE_LEVEL <= MYLOG_LEVEL_OFF,
              "MYLOG_ACTIVE_LEVEL must be a valid numeric level");

// ---- 宏内部实现（不是公开 API）----
// 先取一次实例引用 → 判断这条日志还有没有意义 → 写。参数只在这一处出现，
// 保证最多求值一次；回溯缓存开启时低于门槛的日志会被放行，以便缓存后导出。
#define MYLOG_LOG_INSTANCE(instance, LVL, ...)                                      \
    do                                                                              \
    {                                                                               \
        auto &&_mylog_lg_ = (instance);                                              \
        if (_mylog_lg_.should_process(::mylog::Level::LVL))                          \
            _mylog_lg_.log(MYLOG_SRC_LOC, ::mylog::Level::LVL, __VA_ARGS__);          \
    } while (0)

// 单例版：整个宏里只取一次快照，回调或并发 shutdown 都不会让它悬空。
#define MYLOG_LOG_SINGLETON(LVL, ...)                                               \
    do                                                                              \
    {                                                                               \
        auto _mylog_snapshot_ = ::mylog::SingletonLogger::acquire();                 \
        if (_mylog_snapshot_)                                                        \
            MYLOG_LOG_INSTANCE(*_mylog_snapshot_, LVL, __VA_ARGS__);                  \
    } while (0)

// ---- 带实例参数的宏 ----
#if MYLOG_ACTIVE_LEVEL <= MYLOG_LEVEL_TRACE
#define LOG_TRACE(logger, ...) MYLOG_LOG_INSTANCE(logger, Trace, __VA_ARGS__)
#else
#define LOG_TRACE(logger, ...) (void)0
#endif
#if MYLOG_ACTIVE_LEVEL <= MYLOG_LEVEL_DEBUG
#define LOG_DEBUG(logger, ...) MYLOG_LOG_INSTANCE(logger, Debug, __VA_ARGS__)
#else
#define LOG_DEBUG(logger, ...) (void)0
#endif
#if MYLOG_ACTIVE_LEVEL <= MYLOG_LEVEL_INFO
#define LOG_INFO(logger, ...) MYLOG_LOG_INSTANCE(logger, Info, __VA_ARGS__)
#else
#define LOG_INFO(logger, ...) (void)0
#endif
#if MYLOG_ACTIVE_LEVEL <= MYLOG_LEVEL_WARN
#define LOG_WARN(logger, ...) MYLOG_LOG_INSTANCE(logger, Warn, __VA_ARGS__)
#else
#define LOG_WARN(logger, ...) (void)0
#endif
#if MYLOG_ACTIVE_LEVEL <= MYLOG_LEVEL_ERROR
#define LOG_ERROR(logger, ...) MYLOG_LOG_INSTANCE(logger, Error, __VA_ARGS__)
#else
#define LOG_ERROR(logger, ...) (void)0
#endif
#if MYLOG_ACTIVE_LEVEL <= MYLOG_LEVEL_CRITICAL
#define LOG_CRITICAL(logger, ...) MYLOG_LOG_INSTANCE(logger, Critical, __VA_ARGS__)
#else
#define LOG_CRITICAL(logger, ...) (void)0
#endif

// ---- 使用全局单例的宏 ----
// 没有初始化（或已 shutdown）时静默丢弃，不会报错也不会崩溃。
#if MYLOG_ACTIVE_LEVEL <= MYLOG_LEVEL_TRACE
#define LOGS_TRACE(...) MYLOG_LOG_SINGLETON(Trace, __VA_ARGS__)
#else
#define LOGS_TRACE(...) (void)0
#endif
#if MYLOG_ACTIVE_LEVEL <= MYLOG_LEVEL_DEBUG
#define LOGS_DEBUG(...) MYLOG_LOG_SINGLETON(Debug, __VA_ARGS__)
#else
#define LOGS_DEBUG(...) (void)0
#endif
#if MYLOG_ACTIVE_LEVEL <= MYLOG_LEVEL_INFO
#define LOGS_INFO(...) MYLOG_LOG_SINGLETON(Info, __VA_ARGS__)
#else
#define LOGS_INFO(...) (void)0
#endif
#if MYLOG_ACTIVE_LEVEL <= MYLOG_LEVEL_WARN
#define LOGS_WARN(...) MYLOG_LOG_SINGLETON(Warn, __VA_ARGS__)
#else
#define LOGS_WARN(...) (void)0
#endif
#if MYLOG_ACTIVE_LEVEL <= MYLOG_LEVEL_ERROR
#define LOGS_ERROR(...) MYLOG_LOG_SINGLETON(Error, __VA_ARGS__)
#else
#define LOGS_ERROR(...) (void)0
#endif
#if MYLOG_ACTIVE_LEVEL <= MYLOG_LEVEL_CRITICAL
#define LOGS_CRITICAL(...) MYLOG_LOG_SINGLETON(Critical, __VA_ARGS__)
#else
#define LOGS_CRITICAL(...) (void)0
#endif
