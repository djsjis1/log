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
//
// 文件占用约定（重要）：
//   * 同一个日志文件（规范化后相同的基础路径）在进程内只允许一个存活的文件 sink，
//     因为轮转状态保存在 sink 内部，两个 sink 指向同一文件会各自计数。
//     冲突时初始化失败（InitError::FileSink），不会降级到控制台。
//   * 旧快照还活着时，它占用的文件仍被视为占用中；要重新打开同一文件，
//     必须先把全局与所有快照的持有全部释放（重新写日志前完成）。
//   * 只想改级别、格式时请用已有的 set_level / set_pattern，不需要重建日志器。
#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "spdlog/sinks/sink.h"

#include "spdlog/common.h"
#include "spdlog/spdlog.h"

namespace mylog
{

    // 日志级别：数字越大越严重。设定某个级别后，比它"轻"的日志会被直接丢掉，
    // 例如设为 Info，Trace 和 Debug 就不会输出。
    // 这里单独定义一套枚举，是为了让业务代码不直接依赖 spdlog 的类型。
    enum class Level : int
    {
        Trace = 0,    // 最细的追踪信息，平时关闭
        Debug = 1,    // 调试细节，开发阶段打开
        Info = 2,     // 正常业务节点（默认级别）
        Warn = 3,     // 不致命但需要关注：重试、接近上限等
        Error = 4,    // 一次操作失败，但程序还能继续跑
        Critical = 5, // 严重故障，通常意味着已经不可用
        Off = 6       // 彻底关闭日志输出
    };

    // 异步日志的队列像一条固定长度的传送带：满了就必须做选择，这里定义三种选择。
    enum class OverflowPolicy : int
    {
        Block = 0,         // 写日志的线程停下来等，直到传送带空出位置（不丢日志，默认）
        OverrunOldest = 1, // 扔掉最旧的一条，给新消息腾位置（保住最新现场）
        DiscardNew = 2     // 直接丢掉刚来的新消息（保住已排队消息的完整顺序）
    };

    // 初始化失败的原因分类，调用方据此决定是重试、换目录，还是降级运行。
    enum class InitError : int
    {
        None = 0,      // 没有错误（成功，或尚未尝试）
        InvalidConfig, // 配置本身不合法（空名字、非法轮转参数等）
        Directory,     // 日志目录创建失败
        FileSink,      // 文件 sink 创建失败（含文件被其他日志器占用）
        ThreadPool,    // 异步线程池创建失败
        Unknown        // 其他异常
    };

    // 初始化结果：既能当 bool 用，也能拿到具体失败原因。
    // 用法：auto r = SingletonLogger::init_checked(cfg);  if (r) { 成功 }  else { 看 r.error }
    struct InitResult
    {
        bool ok = false;       // 日志器是否可用
        bool degraded = false; // 可用但能力被降级（如文件输出回退到控制台）
        InitError error = InitError::None; // 失败原因
        std::string message;               // 失败或降级的具体说明

        // 支持 if (result) 这种写法，等价于 if (result.ok)
        explicit operator bool() const noexcept { return ok; }
    };

    // 异步队列的运行期统计，可用来观察"日志是不是快积压了"。
    // 同步模式下列表项全部为 0；通过 Logger::stats() 获取。
    struct AsyncStats
    {
        std::size_t queue_size = 0;    // 当前积压的消息条数
        std::size_t overrun_count = 0; // 被覆盖的最旧消息数（OverrunOldest）
        std::size_t discard_count = 0; // 被丢弃的新消息数（DiscardNew）
    };

    namespace detail
    {
        // 内部转换工具：把本库的 Level 翻译成 spdlog 的级别，以及反向翻译。
        // 放在 detail 命名空间里表示"只给库内部用"，业务代码不需要直接调用。
        spdlog::level::level_enum to_spdlog_level(Level lvl);
        Level from_spdlog_level(spdlog::level::level_enum lvl);
    }

    // 回调事件自带字符串；需要在回调结束后使用时，请复制整个事件。
    struct LogEvent
    {
        Level level;
        std::string logger_name;
        std::string message;
    };

    // 工厂每次创建一个独立、线程安全的 sink，不得返回已被其他对象持有的裸指针。
    // 文件型 sink 必须声明全部基础路径；其目录创建和具体打开方式由工厂负责。
    // 路径不可在运行期改变；封装只能保护已声明的路径，无法检查工厂隐含的文件操作。
    struct SinkConfig
    {
        std::function<std::unique_ptr<spdlog::sinks::sink>()> factory;
        Level level = Level::Trace;
        std::optional<std::string> pattern;
        std::vector<std::string> file_paths;
    };

    // 日志配置项：所有字段都有默认值，使用时只改需要变的那几项即可。
    // 把这些零散的设置集中到一个结构体里，是为了不用每次重复写 spdlog 的初始化代码。
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

        // ---- 文件滚动策略：日志文件不能无限变大，这里选一种"换新文件"的规则 ----
        enum class RotationPolicy : int
        {
            BySize, // 按大小滚动：写满 max_file_size 就换一个新文件（默认）
            Daily,  // 按天滚动：到达每天的指定时刻就换新文件
            Hourly, // 跨小时后的第一条日志触发换文件
            None    // 不滚动，始终追加到同一文件
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
        // 初始化失败回调可以访问现有单例；运行期回调中重入同一实例的操作会被忽略。
        std::function<void(const std::string &msg)> error_handler;

        // 总门槛 level 先过滤，之后各目标再独立过滤；未指定格式时继承 pattern。
        Level console_level = Level::Trace;
        Level file_level = Level::Trace;
        std::optional<std::string> console_pattern;
        std::optional<std::string> file_pattern;
        std::size_t hourly_max_files = 0; // 仅 Hourly 生效，0=不删除，最大 65535
        std::vector<SinkConfig> extra_sinks;

        // 正常日志的通知，与“日志系统自身出错”的 error_handler 不同。
        // 同步时在调用线程、异步时在消费线程执行，回调必须自行保护共享数据。
        // 回调内对同一实例的日志、修改、刷新、回溯和直接关闭会被忽略。
        // 回溯导出的日志也会触发通知；通知不表示其他目标已成功写出。
        std::function<void(const LogEvent &)> log_callback;
        Level callback_level = Level::Error;

        std::size_t backtrace_capacity = 0; // 0=关闭；导出仍受各目标门槛限制
        std::chrono::milliseconds flush_interval{0}; // 0=关闭；启用时每实例多一个定时线程
    };

    // 普通日志器：由用户自己创建和持有，每个实例管一套输出目标。
    // 不可默认构造（必须给配置），不可拷贝（会变成两个日志器抢同一个文件），但可以移动。
    class Logger
    {
    public:
        // 构造函数：拿着配置立即完成初始化（建目录、建输出目标、建线程池、登记名字）。
        // 成功与否用 is_initialized() 判断，失败细节看 init_result()。
        explicit Logger(const LogConfig &cfg);

        // 析构函数：自动关闭并释放资源（内部会先把缓冲区刷到磁盘）。
        ~Logger();

        // 禁止拷贝：内部持有文件句柄和后台线程，复制会导致两个实例写同一个文件。
        Logger(const Logger &) = delete;
        Logger &operator=(const Logger &) = delete;

        // 允许移动：把内部状态整体交给新对象，原对象变成"未初始化"的空壳。
        Logger(Logger &&) noexcept;
        Logger &operator=(Logger &&) noexcept;

        // 立即释放资源（析构时也会调用）。调用前必须确保没有其他线程正在写日志。
        // 若在自身的异步线程、定时线程或用户回调中调用，会打印警告并直接返回。
        void shutdown();

        // 当前线程是否属于本实例的异步后台线程（用于防止自己关自己）
        bool is_on_worker_thread() const noexcept;

        // ---- 运行期修改：不用重建日志器，直接改设置 ----
        void set_level(Level lvl);                    // 修改过滤级别
        void set_flush_level(Level lvl);              // 达到该级别时自动刷盘
        void set_pattern(const std::string &pattern); // 修改输出格式
        void flush();                                 // 同步刷新；异步仅提交请求，不等待完成

        // 对应目标未启用时不操作；后调用的格式设置覆盖之前的设置。
        void set_console_level(Level lvl);
        void set_file_level(Level lvl);
        void set_console_pattern(const std::string &pattern);
        void set_file_pattern(const std::string &pattern);
        void set_formatter(std::unique_ptr<spdlog::formatter> formatter);

        // 缓存运行期被过滤的日志，按需手动导出；编译期删除的调用不会进入缓存。
        void enable_backtrace(std::size_t capacity);
        void disable_backtrace();
        void dump_backtrace();

        // ---- 状态查询 ----
        bool is_initialized() const noexcept;           // 初始化是否成功
        const std::string &name() const noexcept;       // 日志器名字
        const InitResult &init_result() const noexcept; // 失败原因 / 降级说明

        // 先判断这个级别会不会输出，再决定要不要构造昂贵的参数。
        // 例如：if (log.should_log(Level::Debug)) { 拼一段很贵的字符串; LOG_DEBUG(...); }
        bool should_log(Level lvl) const;

        // 当前生效的级别（不可用时返回 Off）
        Level level() const;

        // 异步队列的运行状态（同步模式下全为 0）
        AsyncStats stats() const;

        // 通用写日志入口：所有 LOG_XXX 宏最终都落到这里。
        // loc 是"哪一行代码打的日志"；lvl 是级别；fmt 是带 {} 占位符的格式串。
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
        // 内部状态（输出目标、日志器、线程池等）定义在 .cpp 里，头文件不暴露这些细节
        struct State;

        // 私有标签：用来构造一个"什么都不做"的空实例
        struct NullTag
        {
        };

        // 空状态实例（不创建 sink、不注册名称），仅用于单例未初始化时兜底
        explicit Logger(NullTag);
        Logger() = delete; // 必须通过 LogConfig 构造

        // 取出底层 spdlog 日志器；未初始化时返回 nullptr
        spdlog::logger *impl() const noexcept;
        bool can_operate() const noexcept;
        bool is_on_background_thread() const noexcept;

        // 真正执行资源释放：刷盘、注销名字、销毁日志器和线程池
        void release();

        std::unique_ptr<State> state_;
        friend class SingletonLogger;
    };

    // 全局单例包装：不用自己持有 Logger，直接 LOGS_XXX(...) 就能写日志。
    // 正式使用前要先调用 SingletonLogger::init(cfg)；忘记 init 也不会崩，日志会被静默丢弃。
    class SingletonLogger
    {
    public:
        // 旧接口：返回"借来的引用"，不会延长实例寿命。
        // 只适合"初始化之后、关闭之前"的单线程串联使用；多线程场景请用 acquire()。
        static Logger &instance();

        // 推荐接口：返回一份持有所有权的快照。
        // 只要这份快照还在，即使别的线程调用了 shutdown()，日志器也不会被销毁。
        static std::shared_ptr<Logger> acquire();

        // 初始化；失败时保留原有实例不变（失败原因可通过 init_checked 获取）。
        // 注意：同一日志文件仍被旧实例或旧快照占用时，这里会返回失败，
        // 需先 shutdown() 并释放全部快照，或改用不同的日志文件。
        static bool init(const LogConfig &cfg = LogConfig{});

        // 同上，但会返回 InitResult，能拿到失败分类和说明（排查问题用）
        static InitResult init_checked(const LogConfig &cfg = LogConfig{});

        // 解除全局持有；若仍有 acquire() 快照存活，资源会在最后一个快照释放时回收。
        static void shutdown();

    private:
        static void release_ref(std::shared_ptr<Logger> logger);
        SingletonLogger() = delete; // 纯静态工具类，不允许创建对象
    };

    namespace detail
    {
        // 单例日志入口：持有一份快照后再写，shutdown 并发时也不会悬空。
        // 快照为空（没初始化）时什么也不做。
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

// 源码位置：记录"这条日志是从哪个文件、哪一行、哪个函数打出来的"，
// 展开后是一份源码位置信息，最终会填进格式串里的 %s / %#。
#define MYLOG_SRC_LOC \
    ::spdlog::source_loc { __FILE__, __LINE__, SPDLOG_FUNCTION }

// 编译期裁剪独立于 spdlog。默认保留全部调用；使用方可在目标编译定义中设置
// MYLOG_ACTIVE_LEVEL=MYLOG_LEVEL_INFO。被裁剪的调用连参数也不会求值。
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

// ---- 带实例参数的宏：适合一个程序里有多个日志器，各自写各自的文件 ----
// 展开后就是：logger.log(源码位置, 级别, 你的参数...)
#if MYLOG_ACTIVE_LEVEL <= MYLOG_LEVEL_TRACE
#define LOG_TRACE(logger, ...) \
    (logger).log(MYLOG_SRC_LOC, ::mylog::Level::Trace, __VA_ARGS__)
#else
#define LOG_TRACE(logger, ...) (void)0
#endif
#if MYLOG_ACTIVE_LEVEL <= MYLOG_LEVEL_DEBUG
#define LOG_DEBUG(logger, ...) \
    (logger).log(MYLOG_SRC_LOC, ::mylog::Level::Debug, __VA_ARGS__)
#else
#define LOG_DEBUG(logger, ...) (void)0
#endif
#if MYLOG_ACTIVE_LEVEL <= MYLOG_LEVEL_INFO
#define LOG_INFO(logger, ...) \
    (logger).log(MYLOG_SRC_LOC, ::mylog::Level::Info, __VA_ARGS__)
#else
#define LOG_INFO(logger, ...) (void)0
#endif
#if MYLOG_ACTIVE_LEVEL <= MYLOG_LEVEL_WARN
#define LOG_WARN(logger, ...) \
    (logger).log(MYLOG_SRC_LOC, ::mylog::Level::Warn, __VA_ARGS__)
#else
#define LOG_WARN(logger, ...) (void)0
#endif
#if MYLOG_ACTIVE_LEVEL <= MYLOG_LEVEL_ERROR
#define LOG_ERROR(logger, ...) \
    (logger).log(MYLOG_SRC_LOC, ::mylog::Level::Error, __VA_ARGS__)
#else
#define LOG_ERROR(logger, ...) (void)0
#endif
#if MYLOG_ACTIVE_LEVEL <= MYLOG_LEVEL_CRITICAL
#define LOG_CRITICAL(logger, ...) \
    (logger).log(MYLOG_SRC_LOC, ::mylog::Level::Critical, __VA_ARGS__)
#else
#define LOG_CRITICAL(logger, ...) (void)0
#endif

// ---- 使用全局单例的宏（每次调用持有一份快照，可与其他线程的 shutdown 并发） ----
// 不需要传 logger；没有初始化时静默丢弃，不会报错也不会崩溃。
#if MYLOG_ACTIVE_LEVEL <= MYLOG_LEVEL_TRACE
#define LOGS_TRACE(...) \
    ::mylog::detail::log_singleton(MYLOG_SRC_LOC, ::mylog::Level::Trace, __VA_ARGS__)
#else
#define LOGS_TRACE(...) (void)0
#endif
#if MYLOG_ACTIVE_LEVEL <= MYLOG_LEVEL_DEBUG
#define LOGS_DEBUG(...) \
    ::mylog::detail::log_singleton(MYLOG_SRC_LOC, ::mylog::Level::Debug, __VA_ARGS__)
#else
#define LOGS_DEBUG(...) (void)0
#endif
#if MYLOG_ACTIVE_LEVEL <= MYLOG_LEVEL_INFO
#define LOGS_INFO(...) \
    ::mylog::detail::log_singleton(MYLOG_SRC_LOC, ::mylog::Level::Info, __VA_ARGS__)
#else
#define LOGS_INFO(...) (void)0
#endif
#if MYLOG_ACTIVE_LEVEL <= MYLOG_LEVEL_WARN
#define LOGS_WARN(...) \
    ::mylog::detail::log_singleton(MYLOG_SRC_LOC, ::mylog::Level::Warn, __VA_ARGS__)
#else
#define LOGS_WARN(...) (void)0
#endif
#if MYLOG_ACTIVE_LEVEL <= MYLOG_LEVEL_ERROR
#define LOGS_ERROR(...) \
    ::mylog::detail::log_singleton(MYLOG_SRC_LOC, ::mylog::Level::Error, __VA_ARGS__)
#else
#define LOGS_ERROR(...) (void)0
#endif
#if MYLOG_ACTIVE_LEVEL <= MYLOG_LEVEL_CRITICAL
#define LOGS_CRITICAL(...) \
    ::mylog::detail::log_singleton(MYLOG_SRC_LOC, ::mylog::Level::Critical, __VA_ARGS__)
#else
#define LOGS_CRITICAL(...) (void)0
#endif
