// 日志封装库完整演示：单线程 + 多线程压力测试
// 用法：mylog::Logger 只接受 LogConfig 构造（无默认构造）
#include "Logger.h"

#include <thread>
#include <vector>
#include <chrono>
#include <cstdio>
#include <atomic>
#include <sstream>
#include <spdlog/sinks/ostream_sink.h>

// ============================================================
// 一、在任意函数中使用单例日志（无需传 logger）
// ============================================================
static void do_business_with_singleton(int user_id)
{
    LOGS_DEBUG("进入业务函数, user_id={}", user_id);
    if (user_id < 0)
    {
        LOGS_ERROR("非法 user_id: {}", user_id);
        return;
    }
    LOGS_INFO("业务处理完成, user_id={}", user_id);
}

// ============================================================
// 二、多线程 — 实例方式
// ============================================================
static void mt_worker_instance(mylog::Logger &log, int tid, int count)
{
    for (int i = 0; i < count; ++i)
    {
        LOG_INFO(log, "[thread-{}] msg #{}/{}", tid, i, count);
        if (i % 100 == 99)
        {
            LOG_DEBUG(log, "[thread-{}] 进度: {}/{}", tid, i + 1, count);
        }
    }
    LOG_WARN(log, "[thread-{}] 完成, 共 {} 条", tid, count);
}

void test_mt_instance()
{
    mylog::LogConfig cfg;
    cfg.logger_name = "mt_inst";
    cfg.log_dir = "logs";
    cfg.log_file = "mt_inst.log";
    cfg.enable_console = false;
    cfg.enable_file = true;
    cfg.async = false;
    cfg.level = mylog::Level::Info;
    cfg.max_file_size = 20 * 1024 * 1024;
    cfg.max_files = 3;

    mylog::Logger logger(cfg); // 构造即初始化
    if (!logger.is_initialized())
    {
        std::fprintf(stderr, "mt_inst logger init failed\n");
        return;
    }

    const int kThreads = 8;
    const int kPerThd = 500;

    LOG_INFO(logger, "======= 实例-同步 多线程: {} 线程 x {} 条 =======", kThreads, kPerThd);

    auto t0 = std::chrono::steady_clock::now();
    {
        std::vector<std::thread> thds;
        for (int i = 0; i < kThreads; ++i)
            thds.emplace_back(mt_worker_instance, std::ref(logger), i, kPerThd);
        for (auto &t : thds)
            t.join();
    }
    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    LOG_INFO(logger, "======= 实例-同步 完成, 总计 {} 条, 耗时 {} ms =======",
             kThreads * kPerThd, ms);
}

// ============================================================
// 三、多线程 — 单例-同步
// ============================================================
static void mt_worker_singleton(int tid, int count)
{
    for (int i = 0; i < count; ++i)
        LOGS_INFO("[s-thread-{}] msg #{}/{}", tid, i, count);
    LOGS_WARN("[s-thread-{}] 完成", tid);
}

void test_mt_singleton()
{
    mylog::LogConfig cfg;
    cfg.logger_name = "mt_single";
    cfg.log_dir = "logs";
    cfg.log_file = "mt_single.log";
    cfg.enable_console = false;
    cfg.enable_file = true;
    cfg.async = false;
    cfg.level = mylog::Level::Info;
    cfg.max_file_size = 20 * 1024 * 1024;
    cfg.max_files = 3;

    if (!mylog::SingletonLogger::init(cfg))
    {
        std::fprintf(stderr, "mt_single init failed\n");
        return;
    }

    const int kThreads = 6;
    const int kPerThd = 400;

    LOGS_INFO("======= 单例-同步 多线程: {} 线程 x {} 条 =======", kThreads, kPerThd);

    auto t0 = std::chrono::steady_clock::now();
    {
        std::vector<std::thread> thds;
        for (int i = 0; i < kThreads; ++i)
            thds.emplace_back(mt_worker_singleton, i, kPerThd);
        for (auto &t : thds)
            t.join();
    }
    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    LOGS_INFO("======= 单例-同步 完成, 总计 {} 条, 耗时 {} ms =======",
              kThreads * kPerThd, ms);

    mylog::SingletonLogger::shutdown();
}

// ============================================================
// 四、多线程 — 单例-异步（压力最大）
// ============================================================
static void mt_worker_async(int tid, int count)
{
    for (int i = 0; i < count; ++i)
        LOGS_INFO("[async-thread-{}] msg #{}/{}", tid, i, count);
    LOGS_WARN("[async-thread-{}] 完成", tid);
}

void test_mt_async()
{
    mylog::LogConfig cfg;
    cfg.logger_name = "mt_async";
    cfg.log_dir = "logs";
    cfg.log_file = "mt_async.log";
    cfg.enable_console = false;
    cfg.enable_file = true;
    cfg.async = true;
    cfg.level = mylog::Level::Info;
    cfg.async_queue_size = 32768;
    cfg.async_thread_count = 2;
    cfg.max_file_size = 50 * 1024 * 1024;
    cfg.max_files = 5;

    if (!mylog::SingletonLogger::init(cfg))
    {
        std::fprintf(stderr, "mt_async init failed\n");
        return;
    }

    const int kThreads = 16;
    const int kPerThd = 1000;

    LOGS_INFO("======= 单例-异步 多线程: {} 线程 x {} 条 =======", kThreads, kPerThd);

    auto t0 = std::chrono::steady_clock::now();
    {
        std::vector<std::thread> thds;
        for (int i = 0; i < kThreads; ++i)
            thds.emplace_back(mt_worker_async, i, kPerThd);
        for (auto &t : thds)
            t.join();
    }
    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    LOGS_INFO("======= 单例-异步 完成, 总计 {} 条, 投递耗时 {} ms =======",
              kThreads * kPerThd, ms);

    // 异步必须 shutdown 等后台写完；若关闭前已停止产生日志，
    // 关闭自己的 flush 不会抢掉队列里已排队的日志。
    mylog::SingletonLogger::shutdown();
}

// ============================================================
// 五、扩展能力：独立输出、回溯、定时刷新、自定义目标/格式与通知
// 编译期裁剪可由使用方设置：
// target_compile_definitions(your_target PRIVATE MYLOG_ACTIVE_LEVEL=MYLOG_LEVEL_INFO)
// 默认保留全部调用；裁剪后的 Debug/Trace 不会进入回溯缓存。
// ============================================================
class DemoFormatter final : public spdlog::formatter
{
public:
    void format(const spdlog::details::log_msg &msg, spdlog::memory_buf_t &dest) override
    {
        const std::string text = "[custom] " + std::string(msg.payload.data(), msg.payload.size()) + "\n";
        dest.append(text.data(), text.data() + text.size());
    }
    std::unique_ptr<spdlog::formatter> clone() const override
    {
        return std::make_unique<DemoFormatter>();
    }
};

static bool demo_extended_features()
{
    // 外部流必须比 logger 活得久；回调计数用原子量，因为这里启用了异步。
    std::ostringstream captured;
    std::atomic<int> notifications{0};
    mylog::LogConfig cfg;
    cfg.logger_name = "extended";
    cfg.log_file = "extended.log";
    cfg.rotation = mylog::LogConfig::RotationPolicy::None; // 也可选择 Hourly
    cfg.level = mylog::Level::Info;
    cfg.console_level = mylog::Level::Warn;
    cfg.file_level = mylog::Level::Trace;
    cfg.console_pattern = "[console] %v";
    cfg.file_pattern = "[%Y-%m-%d %H:%M:%S] [%l] %v";
    cfg.async = true;
    cfg.backtrace_capacity = 2;
    cfg.flush_interval = std::chrono::milliseconds(100);
    cfg.log_callback = [&notifications](const mylog::LogEvent &event)
    {
        ++notifications;
        std::fprintf(stderr, "[callback] %s: %s\n", event.logger_name.c_str(), event.message.c_str());
        // 只做通知；不要在这里重入或直接关闭同一 logger。
    };
    mylog::SinkConfig memory;
    memory.factory = [&captured]
    {
        return std::make_unique<spdlog::sinks::ostream_sink_mt>(captured);
    };
    cfg.extra_sinks.push_back(memory); // 非文件目标无需申报 file_paths

    mylog::Logger logger(cfg);
    if (!logger.is_initialized())
    {
        std::fprintf(stderr, "extended init failed: %s\n", logger.init_result().message.c_str());
        return false;
    }
    logger.set_formatter(std::make_unique<DemoFormatter>());
    // 最后一次设置生效：只恢复控制台和文件格式，内存目标继续使用自定义 formatter。
    logger.set_console_pattern("[console] %v");
    logger.set_file_pattern("[%Y-%m-%d %H:%M:%S] [%l] %v");
    LOG_DEBUG(logger, "cached diagnostic: {}", 42); // 总门槛过滤，但仍进入回溯
    LOG_INFO(logger, "visible in file and memory, not console");
    logger.dump_backtrace(); // 控制台仍只接收 Warn 以上，文件可看到缓存的 Debug
    LOG_ERROR(logger, "notification example");
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    logger.shutdown(); // 停止定时器、等异步消费结束后，才能读取外部流
    std::printf("\nCaptured logs:\n%sNotifications: %d\n", captured.str().c_str(), notifications.load());
    return notifications.load() == 1;
}

// ============================================================
// main
// ============================================================
int main()
{
    std::printf("\n==================== 单线程基本演示 ====================\n");

    // ---------- 实例方式 ----------
    {
        mylog::LogConfig cfg;
        cfg.logger_name = "demo";
        cfg.log_dir = "logs";
        cfg.log_file = "demo.log";
        cfg.enable_console = true;
        cfg.enable_file = true;
        cfg.level = mylog::Level::Trace;
        cfg.max_file_size = 5 * 1024 * 1024;
        cfg.max_files = 3;

        mylog::Logger logger(cfg); // 构造即初始化
        LOG_TRACE(logger, "[实例] trace 日志");
        LOG_DEBUG(logger, "[实例] debug, value={}", 42);
        LOG_INFO(logger, "[实例] 程序启动 v{}.{}.{}", 1, 0, 0);
        LOG_WARN(logger, "[实例] 阈值 {:.2f} 接近上限", 0.95);
        LOG_ERROR(logger, "[实例] 读取文件失败: {}", "/tmp/missing.txt");
        LOG_CRITICAL(logger, "[实例] 致命错误, code={}", -1);

        // 第二个独立实例
        mylog::LogConfig cfg2 = cfg;
        cfg2.logger_name = "audit";
        cfg2.log_file = "audit.log";
        cfg2.enable_console = false;
        mylog::Logger audit(cfg2);
        LOG_INFO(audit, "[审计] 仅写入 audit.log 的日志");
    }

    // ---------- 单例方式 ----------
    {
        mylog::LogConfig cfg;
        cfg.logger_name = "global";
        cfg.log_dir = "logs";
        cfg.log_file = "global.log";
        cfg.enable_console = true;
        cfg.enable_file = true;
        cfg.level = mylog::Level::Trace;

        // 不关心失败原因时可以直接用 init()；需要区分原因用 init_checked()
        mylog::SingletonLogger::init(cfg);

        LOGS_TRACE("[单例] trace 日志");
        LOGS_DEBUG("[单例] debug, value={}", 100);
        LOGS_INFO("[单例] 全局单例工作正常");
        LOGS_WARN("[单例] 警告: {}", "queue 80% 满");
        LOGS_ERROR("[单例] 出错: code={}", 500);
        LOGS_CRITICAL("[单例] 致命: {}", "out of memory");

        do_business_with_singleton(7);
        do_business_with_singleton(-1);

        // 单例对象进程内常驻：shutdown() 只释放内部资源（flush、关文件、停线程），
        // 对象和 SingletonLogger::get() 拿到的指针都一直有效；
        // shutdown 之后写日志会被静默丢弃，随时可以再用同一文件 init()。
        mylog::SingletonLogger::shutdown();
    }

    // ========== 多线程压力测试 ==========
    std::printf("\n==================== 多线程压力测试 ====================\n");

    test_mt_instance();
    test_mt_singleton();
    test_mt_async();
    if (!demo_extended_features())
        return 1;

    std::printf("\n==================== 全部测试完成 ====================\n");
    std::printf("日志文件位于 ./logs/ 目录下\n");
    return 0;
}
