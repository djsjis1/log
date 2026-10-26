// Logger.cpp
#include "Logger.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <exception>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <vector>

#include "spdlog/spdlog.h"

#include "spdlog/async.h"
#include "spdlog/details/periodic_worker.h"
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
            // 非法枚举值（调用方 static_cast 出来时）一律关闭，绝不悄悄变成 info。
            return spdlog::level::off;
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
        spdlog::sink_ptr console_sink;
        spdlog::sink_ptr file_sink;
        std::string name;
        InitResult init;
        // 关闭时做最后刷新失败时仍要通知用户，所以留着这份副本。
        std::function<void(const std::string &)> error_handler;
        // 名字是否仍在 spdlog registry 里：关闭时用它做幂等注销，
        // 因此不必把 name 清空（name() 在关闭后仍应保持稳定）。
        bool registered = false;
        // 定时刷新登记令牌：最后声明，回收时先注销它，保证调度线程不再触碰本实例。
        std::shared_ptr<void> flush_token;
    };

    namespace
    {
        // 记录当前线程属于哪个 Logger 实例的异步线程池
        thread_local const void *t_worker_state = nullptr;
        thread_local const void *t_timer_state = nullptr;

        // 回溯导出串行化锁：spdlog 的 dump 会在持有本实例回溯锁的情况下
        // 调用所有目标（含用户回调），两个实例同时导出、回调又互相写日志时，
        // 会各自等待对方的回溯锁。串行化后同一时刻只有一个导出在执行。
        // 用递归锁，允许回调里再导出另一个实例（同一线程持有，不会互等）。
        std::recursive_mutex g_backtrace_dump_mutex;

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

        // 回收线程（定义在本文件后部）：这里只用来把"不能在本线程做的收尾"转交出去。
        void defer_destroy(std::shared_ptr<spdlog::details::periodic_worker> worker);

        // ---- 定时刷新调度 ----
        // 全进程共用一个 spdlog::details::periodic_worker 线程：实例只在启动时登记
        // 一个回调、关闭时注销。没人配置 flush_interval 时线程根本不存在。
        // 相同间隔共用一个桶，不同间隔各一个桶（实际几乎只有一种间隔）。
        class FlushScheduler
        {
        public:
            // 登记定时刷新；返回的令牌析构即注销。interval<=0 或 work 为空时返回空令牌。
            std::shared_ptr<void> add(std::chrono::milliseconds interval, std::function<void()> work)
            {
                if (interval.count() <= 0 || !work)
                    return {};

                std::lock_guard<std::mutex> lock(mutex_);
                Slot &slot = slots_[interval];
                if (!slot.bucket)
                {
                    slot.bucket = std::make_shared<Bucket>();
                    auto bucket = slot.bucket;
                    // periodic_worker 构造即起线程；同类实例共享它。
                    slot.worker = std::make_unique<spdlog::details::periodic_worker>(
                        [bucket] { tick(bucket); }, interval);
                }

                const std::size_t id = next_id_++;
                {
                    std::lock_guard<std::mutex> bucket_lock(slot.bucket->mutex);
                    // 写时复制：tick 只需拷一个指针，锁内不做任何分配/深拷。
                    auto next = std::make_shared<std::vector<Entry>>();
                    if (slot.bucket->entries)
                        *next = *slot.bucket->entries;
                    next->push_back(Entry{id, std::move(work)});
                    slot.bucket->entries = std::move(next);
                }

                // 令牌的析构函数就是注销动作；调度器本身故意不析构，this 永远有效。
                FlushScheduler *self = this;
                return std::shared_ptr<void>(nullptr,
                                             [self, interval, id](void *) { self->remove(interval, id); });
            }

            static FlushScheduler &instance()
            {
                // 同 Reaper：故意不析构，避免与静态析构顺序纠缠。
                static FlushScheduler *scheduler = new FlushScheduler();
                return *scheduler;
            }

        private:
            struct Entry
            {
                std::size_t id;
                std::function<void()> work;
            };

            struct Bucket
            {
                std::mutex mutex;
                std::condition_variable cv;
                // 名单不可变，改一份就整体换：tick 锁内只拷指针，锁外慢慢跑。
                std::shared_ptr<const std::vector<Entry>> entries;
                int running = 0; // 正在执行的 tick 数（0 或 1），注销时要等它归零
            };

            struct Slot
            {
                std::shared_ptr<Bucket> bucket;
                std::unique_ptr<spdlog::details::periodic_worker> worker;
            };

            // 回调在锁外执行：work 里可能重入注销/用户回调，持锁调用会死锁。
            static void tick(const std::shared_ptr<Bucket> &bucket)
            {
                std::shared_ptr<const std::vector<Entry>> snapshot;
                {
                    std::lock_guard<std::mutex> lock(bucket->mutex);
                    if (!bucket->entries || bucket->entries->empty())
                        return;
                    ++bucket->running;
                    snapshot = bucket->entries; // 只拷指针，零深拷贝
                }

                for (const auto &entry : *snapshot)
                    entry.work();

                {
                    std::lock_guard<std::mutex> lock(bucket->mutex);
                    --bucket->running;
                }
                bucket->cv.notify_all();
            }

            void remove(std::chrono::milliseconds interval, std::size_t id)
            {
                std::unique_ptr<spdlog::details::periodic_worker> dying;
                std::shared_ptr<Bucket> bucket;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto it = slots_.find(interval);
                    if (it == slots_.end())
                        return;

                    bucket = it->second.bucket;
                    bool empty = false;
                    {
                        std::lock_guard<std::mutex> bucket_lock(bucket->mutex);
                        if (!bucket->entries)
                            return;
                        auto next = std::make_shared<std::vector<Entry>>();
                        next->reserve(bucket->entries->size());
                        for (const auto &entry : *bucket->entries)
                            if (entry.id != id)
                                next->push_back(entry);
                        empty = next->empty();
                        bucket->entries = std::move(next);
                    }
                    if (!empty)
                        return;

                    // 最后一个登记项走了：把这个桶的 worker 摘下来，
                    // 等本轮 tick 结束再在锁外销毁（join）。
                    dying = std::move(it->second.worker);
                    slots_.erase(it);
                }

                {
                    std::unique_lock<std::mutex> bucket_lock(bucket->mutex);
                    bucket->cv.wait(bucket_lock, [&bucket] { return bucket->running == 0; });
                }

                if (!dying)
                    return;

                // 在调度线程自己身上 join 会终止进程：交给回收线程收尾。
                if (t_timer_state != nullptr)
                {
                    defer_destroy(std::shared_ptr<spdlog::details::periodic_worker>(std::move(dying)));
                    return;
                }
                dying.reset();
            }

            std::mutex mutex_;
            std::map<std::chrono::milliseconds, Slot> slots_;
            std::size_t next_id_ = 0;
        };

        FlushScheduler &flush_scheduler()
        {
            return FlushScheduler::instance();
        }

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
                    LogEvent event{from_spdlog_level(msg.level),
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
            // Windows 文件系统通常大小写不敏感，统一小写以免大小写别名绕过占用检查。
            // 已知局限：这里只折叠 ASCII；非 ASCII 字符（如西里尔字母）不做 Unicode
            // 大小写折叠，理论上可用这类别名绕过占用检查（常见日志文件名不受影响）。
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

        // ---- 日志器回收 ----
        // 最后一个快照可能在后台线程（异步消费、定时刷新）或用户回调里释放：
        // 在那里直接 delete 会 join 自己，detach 一个线程又会让进程退出期与
        // spdlog 的静态对象析构相互穿插，而且 shutdown() 返回时资源还没真正归还。
        // 因此改为常驻一个回收线程，shutdown()/init() 末了等队列排空。
        thread_local bool t_on_reaper = false;

        class Reaper
        {
        public:
            Reaper()
                : worker_([this]
                          {
                              t_on_reaper = true;
                              for (;;)
                              {
                                  Logger *logger = nullptr;
                                  std::function<void()> task;
                                  {
                                      std::unique_lock<std::mutex> lock(mutex_);
                                      cv_.wait(lock, [this] { return !pending_.empty() || !tasks_.empty(); });
                                      busy_ = true;
                                      if (!tasks_.empty())
                                      {
                                          task = std::move(tasks_.front());
                                          tasks_.pop_front();
                                      }
                                      else
                                      {
                                          logger = pending_.front();
                                          pending_.pop_front();
                                      }
                                  }
                                  if (task)
                                      task();
                                  else
                                      delete logger;
                                  {
                                      std::lock_guard<std::mutex> lock(mutex_);
                                      busy_ = false;
                                  }
                                  idle_cv_.notify_all();
                              }
                          })
            {
            }

            void enqueue(Logger *logger)
            {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    pending_.push_back(logger);
                }
                cv_.notify_one();
            }

            // 通用任务队列：用于“当前线程不能做”的收尾（如在调度线程上 join 自己）。
            void enqueue_task(std::function<void()> task)
            {
                if (!task)
                    return;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    tasks_.push_back(std::move(task));
                }
                cv_.notify_one();
            }

            // 等待已转交的实例与任务全部完成。回收线程自身或任何一个日志器的后台线程
            // 上调用时直接返回：等下去等于等自己。
            void drain()
            {
                if (t_on_reaper || t_worker_state || t_timer_state)
                    return;
                std::unique_lock<std::mutex> lock(mutex_);
                idle_cv_.wait(lock, [this] { return pending_.empty() && tasks_.empty() && !busy_; });
            }

        private:
            std::mutex mutex_;
            std::condition_variable cv_;
            std::condition_variable idle_cv_;
            std::deque<Logger *> pending_;
            std::deque<std::function<void()>> tasks_;
            bool busy_ = false;
            std::thread worker_;
        };

        std::atomic<Reaper *> g_reaper{nullptr};

        Reaper &reaper_instance()
        {
            // 故意不析构：回收线程在进程退出阶段仍可能持有待销毁对象，
            // 让它随进程结束由系统回收，避免与静态析构顺序纠缠。
            static Reaper *instance = []
            {
                auto *created = new Reaper();
                g_reaper.store(created, std::memory_order_release);
                return created;
            }();
            return *instance;
        }

        // 定义在 Reaper 之后：把当前线程不能做的 join 转交给回收线程。
        void defer_destroy(std::shared_ptr<spdlog::details::periodic_worker> worker)
        {
            if (!worker)
                return;
            reaper_instance().enqueue_task([worker]() mutable { worker.reset(); });
        }

        // 退出前清空转交队列；声明位置早于 g_singleton，因此析构晚于它。
        struct ReaperGuard
        {
            ~ReaperGuard()
            {
                // 只在回收线程确实启动过时才等待，避免在静态析构阶段新建线程。
                if (auto *reaper = g_reaper.load(std::memory_order_acquire))
                    reaper->drain();
            }
        };

    } // namespace

    // 把内部日志器登记到 spdlog registry。同名旧项先摘掉：单例替换旧实例时
    // 这是预期行为，普通实例在构造时已确认过没有同名冲突。
    // 单例场景下必须与“全局指针交换”处于同一临界区（g_singleton_mutex）。
    void Logger::attach_registry()
    {
        State &st = *state_;
        if (!st.logger || st.name.empty())
            return;
        spdlog::drop(st.name);
        spdlog::register_logger(st.logger);
        st.registered = true;
    }

    // 普通实例的构造：先按 DeferRegister 完成全部资源初始化，再检查名字冲突并注册。
    // 同名已被占用时明确失败，不再静默把别人的注册顶掉（旧实现直接 spdlog::drop）。
    Logger::Logger(const LogConfig &cfg)
        : Logger(cfg, DeferRegister{})
    {
        State &st = *state_;
        if (!st.init.ok)
            return; // 构造阶段已经失败并上报过

        std::string failure;
        try
        {
            if (spdlog::get(st.name) != nullptr)
                failure = "[mylog] logger name '" + st.name + "' is already registered";
            else
                attach_registry();
        }
        catch (const std::exception &ex)
        {
            failure = std::string("[mylog] cannot register logger name '") + st.name + "': " + ex.what();
        }

        if (failure.empty())
            return;

        // 回滚：先释放日志器（它持有 sink 的强引用），再放掉 sink 自己的那份，
        // 线程池也要停，否则这个“不可用”的实例会白白养着一批消费线程。
        st.flush_token.reset();
        st.logger.reset();
        st.pool.reset();
        st.console_sink.reset();
        st.file_sink.reset();
        st.init.ok = false;
        st.init.degraded = false;
        st.init.error = InitError::InvalidConfig;
        st.init.message = failure;
        report_error(st.error_handler, failure);
    }

    Logger::Logger(const LogConfig &cfg, DeferRegister)
        : state_(std::make_unique<State>())
    {
        build_into(*state_, cfg);
    }

    // 把配置装配进指定的 State：构造与"原地重建"共用同一段逻辑。
    // 用 lambda 包住原来的提前返回，末尾统一用 st.init.ok 判定成败。
    bool Logger::build_into(State &st, const LogConfig &cfg)
    {
        const auto &handler = cfg.error_handler;

        auto assemble = [&]()
        {

        auto fail = [&](InitError code, const std::string &msg)
        {
            st.init.ok = false;
            st.init.error = code;
            st.init.message = msg;
            report_error(handler, msg);
        };

        st.error_handler = cfg.error_handler;

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

            // 名字只做记录：真正登记到 spdlog registry 由 attach_registry()
            // 在调用方指定的临界区里完成（单例必须与全局指针交换原子）。
            st.name = cfg.logger_name;
            st.console_sink = std::move(console_sink);
            st.file_sink = std::move(primary_sink);
        }
        catch (const std::exception &ex)
        {
            st.flush_token.reset();
            st.logger.reset();
            st.pool.reset();
            fail(InitError::Unknown, std::string("[mylog] init failed: ") + ex.what());
            return;
        }

        st.init.ok = true;
        st.init.error = InitError::None;

        // 定时刷新：登记到进程内共享调度器（全进程一个 periodic_worker 线程）。
        // 注册放在“初始化完全成功”之后，保证第一次 tick 不会碰到半成品。
        if (cfg.flush_interval.count() > 0)
        {
            State *state_ptr = &st;
            st.flush_token = flush_scheduler().add(cfg.flush_interval,
                [state_ptr, guarded = st.error_handler]
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
                        {
                            state_ptr->logger->flush();
                        }
                    }
                    catch (...)
                    {
                        CallbackScope scope(state_ptr);
                        report_error(guarded, "[mylog] periodic flush failed");
                    }
                    t_timer_state = nullptr;
                });
        }
        };
        assemble();
        return st.init.ok;
    }

    bool Logger::rebuild(const LogConfig &cfg)
    {
        // 先把旧资源还回去：同一个日志文件再次打开必须先释放租约，
        // 否则会被判 "still in use"。所以重建失败时单例会回到未初始化状态。
        release();

        if (!state_)
            state_ = std::make_unique<State>();

        if (!build_into(*state_, cfg))
            return false;

        // 名字已在 release() 里注销，这里重新登记。
        attach_registry();
        return true;
    }

    Logger::Logger(Logger &&other) noexcept
        : state_(std::move(other.state_))
    {
    }

    Logger &Logger::operator=(Logger &&other) noexcept
    {
        if (this == &other)
            return *this;

        // 在目标自己的回调或后台线程上销毁它，会直接截断正在执行的调用栈，
        // 所以这种场合只能放弃这次移交；Debug 下立即暴露出来。
        if (!can_operate() || !other.can_operate() ||
            is_on_background_thread() || other.is_on_background_thread())
        {
            assert(false && "[mylog] move assignment ignored inside a callback or background thread");
            return *this;
        }

        release();
        state_ = std::move(other.state_);
        return *this;
    }

    Logger::~Logger()
    {
        if (is_on_background_thread() || !can_operate())
        {
            // 在自身后台线程上析构不能等线程池退出（会 join 自身），在回调里析构
            // 会拆掉正在使用的目标；两种情况都只能泄漏状态，优先保证进程不崩。
            std::fprintf(stderr, "[mylog] Logger destroyed on its own background thread or inside its own callback; "
                                 "resources are leaked to avoid destroying it while in use\n");
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

        // 先注销定时刷新：调度线程不会再触碰本实例。
        st->flush_token.reset();

        // 目标可能在销毁 logger 时一起释放，先留一份持有，供最后刷新使用。
        std::vector<spdlog::sink_ptr> sinks;
        if (st->logger)
            sinks = st->logger->sinks();

        if (st->registered)
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
            // 名字保留：name() 在关闭后仍应返回构造时的名字。
            st->registered = false;
        }

        // 先销毁线程池：它会先给每个消费线程发终止消息再 join，队列先进先出，
        // 返回时已入队的日志都写进了目标；这段时间 logger 仍然有效，
        // 消费线程在收尾消息里回调用户代码也不会撞到被重置的 state。
        st->pool.reset();
        // 再释放 logger：此后任何并发写日志都是生命周期契约禁止的用法。
        st->logger.reset();

        // 多消费线程时，先执行到的 flush 可能早于最后一条写入（消费线程已从队列
        // 取出日志但还没写进目标）。写入全部结束后由当前线程再同步刷新一次，
        // 保证最后一个目标也真的被刷出，而不是只剩一个排队中的刷新请求。
        for (auto &sink : sinks)
        {
            try
            {
                sink->flush();
            }
            catch (const std::exception &ex)
            {
                report_error(st->error_handler, std::string("[mylog] final flush failed: ") + ex.what());
            }
            catch (...)
            {
                report_error(st->error_handler, "[mylog] final flush failed: unknown exception");
            }
        }

        st->console_sink.reset();
        st->file_sink.reset();
        // 关闭后对外就是"未初始化的空实例"：不保留上一次初始化/降级的说明，
        // 也不再把用户回调及其捕获的资源拖着不放。
        st->error_handler = nullptr;
        st->init.ok = false;
        st->init.degraded = false;
        st->init.error = InitError::None;
        st->init.message.clear();
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
        if (!valid_level(lvl))
        {
            if (state_)
                report_error(state_->error_handler, "[mylog] set_level: invalid level");
            return;
        }
        if (auto *target = impl())
            target->set_level(detail::to_spdlog_level(lvl));
    }

    void Logger::set_flush_level(Level lvl)
    {
        if (!can_operate())
            return;
        if (!valid_level(lvl))
        {
            if (state_)
                report_error(state_->error_handler, "[mylog] set_flush_level: invalid level");
            return;
        }
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
        if (!can_operate() || !state_)
            return;

        // 异步实例用 block 策略投递刷新请求：spdlog 默认按本实例的溢出策略入队，
        // OverrunOldest / DiscardNew 下这条控制消息会把真正排队的日志挤掉。
        if (auto async = std::dynamic_pointer_cast<spdlog::async_logger>(state_->logger))
        {
            if (!state_->pool)
                return;
            try
            {
                state_->pool->post_flush(std::move(async), spdlog::async_overflow_policy::block);
            }
            catch (const std::exception &ex)
            {
                report_error(state_->error_handler, std::string("[mylog] flush failed: ") + ex.what());
            }
            return;
        }

        if (auto *target = impl())
            target->flush();
    }

    bool Logger::flush_and_wait(std::chrono::milliseconds timeout)
    {
        if (!can_operate() || !state_ || !state_->logger)
            return false;

        const auto deadline = std::chrono::steady_clock::now() + timeout;
        auto wait_drained = [&deadline](spdlog::details::thread_pool *pool)
        {
            while (pool->queue_size() != 0)
            {
                if (std::chrono::steady_clock::now() >= deadline)
                    return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return true;
        };

        if (auto *pool = state_->pool.get())
        {
            if (!wait_drained(pool))
                return false;

            // 队列已空时再投一次刷新：不会因队列满而阻塞，也不会挤掉任何日志。
            try
            {
                auto async = std::static_pointer_cast<spdlog::async_logger>(state_->logger);
                pool->post_flush(std::move(async), spdlog::async_overflow_policy::block);
            }
            catch (...)
            {
                return false;
            }
            if (!wait_drained(pool))
                return false;
        }

        // 最后由调用线程把每个目标同步刷一遍：这一步是把文件缓冲真正推到磁盘上。
        auto sinks = state_->logger->sinks();
        bool ok = true;
        for (auto &sink : sinks)
        {
            try
            {
                sink->flush();
            }
            catch (...)
            {
                ok = false;
            }
        }
        return ok;
    }

    void Logger::set_console_level(Level lvl)
    {
        if (!can_operate() || !state_ || !state_->console_sink)
            return;
        if (!valid_level(lvl))
        {
            report_error(state_->error_handler, "[mylog] set_console_level: invalid level");
            return;
        }
        state_->console_sink->set_level(detail::to_spdlog_level(lvl));
    }

    void Logger::set_file_level(Level lvl)
    {
        if (!can_operate() || !state_ || !state_->file_sink)
            return;
        if (!valid_level(lvl))
        {
            report_error(state_->error_handler, "[mylog] set_file_level: invalid level");
            return;
        }
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
        if (!can_operate())
            return;
        auto *target = impl();
        if (!target)
            return;
        // 导出期间 spdlog 持有本实例的回溯锁；串行化导出避免与其他实例的导出互等。
        std::lock_guard<std::recursive_mutex> lock(g_backtrace_dump_mutex);
        target->dump_backtrace();
    }

    bool Logger::should_log(Level lvl) const
    {
        // 回调重入时不碰 state：此时别的线程可能正在 release()。
        if (!can_operate())
            return false;
        auto *target = impl();
        return target && target->should_log(detail::to_spdlog_level(lvl));
    }

    bool Logger::should_process(Level lvl) const
    {
        // 和 log() 一样先过重入保护：宏先调这里，若在此读 state_，
        // 用户回调里写日志时就会与另一个线程的 release() 撞上。
        if (!can_operate())
            return false;
        auto *target = impl();
        if (!target)
            return false;
        // 回溯缓存开启时，低于门槛的日志也要走到 logger::log() 才会被存进环形缓冲，
        // 否则 dump_backtrace() 永远只能看到高于门槛的那几条。
        return target->should_log(detail::to_spdlog_level(lvl)) || target->should_backtrace();
    }

    Level Logger::level() const
    {
        auto *target = impl();
        return target ? from_spdlog_level(target->level()) : Level::Off;
    }

    std::shared_ptr<spdlog::logger> Logger::native() const
    {
        return state_ ? state_->logger : nullptr;
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
        // 声明在 g_singleton 之前：析构更晚，保证退出时先排空转交队列。
        ReaperGuard g_reaper_guard;

        // 单例对象只创建一次、永不更换、永不销毁：
        // 所以热路径可以只持有裸指针，不需要快照。
        // init()/shutdown() 只重建/释放它的内部资源，对象地址不变。
        std::shared_ptr<Logger> g_singleton;
        std::mutex g_singleton_mutex; // 串行化 init/shutdown 与首次创建

        // 热路径只读这一个原子指针；写入只在首次创建时发生一次。
        std::atomic<Logger *> g_singleton_raw{nullptr};

        // init() 期间用户回调再调 init/shutdown 会重复获取 g_singleton_mutex，
        // 直接判为非法调用（返回失败），而不是挂住。
        thread_local bool t_in_singleton_control = false;

        struct ControlScope
        {
            ControlScope() noexcept : previous_(t_in_singleton_control) { t_in_singleton_control = true; }
            ~ControlScope() { t_in_singleton_control = previous_; }
            ControlScope(const ControlScope &) = delete;
            ControlScope &operator=(const ControlScope &) = delete;

        private:
            bool previous_;
        };
    } // namespace

    // 直接销毁还是转交回收线程，取决于"当前线程能不能等"：
    // 后台线程 join 自己会死锁，回调里销毁正在使用的目标也是 UB。
    void Logger::DeferredDeleter::operator()(Logger *logger) const noexcept
    {
        if (!logger)
            return;
        if (!logger->is_on_background_thread() && logger->can_operate())
        {
            delete logger;
            return;
        }
        try
        {
            reaper_instance().enqueue(logger);
        }
        catch (...)
        {
            // 连入队都做不到时，只能泄漏以免自等待；
            // Logger::~Logger 的同名保护分支也覆盖直接析构的情形。
            std::fprintf(stderr, "[mylog] cannot hand off logger destruction; "
                                 "resources are leaked to avoid joining itself\n");
        }
    }

    Logger *SingletonLogger::get() noexcept
    {
        return g_singleton_raw.load(std::memory_order_acquire);
    }

    Logger *SingletonLogger::acquire() noexcept
    {
        return get();
    }

    InitResult SingletonLogger::init_checked(const LogConfig &cfg)
    {
        InitResult refused;
        if (t_in_singleton_control)
        {
            refused.error = InitError::Unknown;
            refused.message = "[mylog] init() called from a logger callback; ignored";
            return refused;
        }

        ControlScope scope; // 覆盖构造 + 登记，回调里再调 init/shutdown 被拒绝

        std::lock_guard<std::mutex> lock(g_singleton_mutex);

        // 排空之前转交给回收线程的销毁/释放。必须持锁后做：后台线程发起的
        // shutdown 把 release() 排进了回收队列，若不先等它跑完，
        // 下面的原地重建/释放就会与回收线程并发触碰同一份 State（TSan 实测）。
        reaper_instance().drain();

        if (!g_singleton)
        {
            // 首次：构造 + 注册都成功后才发布指针，失败什么都不留下。
            std::shared_ptr<Logger> created(new Logger(cfg, Logger::DeferRegister{}),
                                            Logger::DeferredDeleter{});
            if (!created->is_initialized())
                return created->init_result();

            try
            {
                created->attach_registry();
            }
            catch (const std::exception &ex)
            {
                InitResult result = created->init_result();
                result.ok = false;
                result.degraded = false;
                result.error = InitError::InvalidConfig;
                result.message = std::string("[mylog] cannot register logger name: ") + ex.what();
                report_error(created->state_->error_handler, result.message);
                return result;
            }

            g_singleton = std::move(created);
            // 对象地址从此固定不变，裸指针永远有效。
            g_singleton_raw.store(g_singleton.get(), std::memory_order_release);
            return g_singleton->init_result();
        }

        // 已有对象：在它身上原地重建（地址不变，get() 拿到的指针继续有效）。
        if (g_singleton->is_on_background_thread())
        {
            InitResult guarded;
            guarded.error = InitError::Unknown;
            guarded.message = "[mylog] init() called from the logger's background thread; ignored";
            return guarded;
        }
        g_singleton->rebuild(cfg);
        return g_singleton->init_result();
    }

    bool SingletonLogger::init(const LogConfig &cfg)
    {
        return init_checked(cfg).ok;
    }

    void SingletonLogger::shutdown()
    {
        if (t_in_singleton_control)
            return; // 从回调里调过来的，先让外层 init 跑完

        std::lock_guard<std::mutex> lock(g_singleton_mutex);
        if (!g_singleton)
            return;

        // 在它自己的后台线程 / 用户回调里不能同步释放（会 join 自己）：
        // 把释放转交回收线程，shutdown() 立刻返回。
        if (g_singleton->is_on_background_thread() || !g_singleton->can_operate())
        {
            std::shared_ptr<Logger> target = g_singleton;
            try
            {
                reaper_instance().enqueue_task([target]() mutable { target->release(); });
            }
            catch (...)
            {
                std::fprintf(stderr, "[mylog] cannot defer singleton shutdown; skipped\n");
            }
            return;
        }

        // 先等掉之前转交给回收线程的释放（后台线程发起的 shutdown 走的就是这条路），
        // 否则它会与本线程下面的同步 release() 并发跑在同一份 State 上。
        // 期间不可能再进新的转交：后台线程的 shutdown 也要拿这把锁。
        reaper_instance().drain();

        // 只释放内部资源（flush、关文件、停线程、注销名字），对象与裸指针保留：
        // shutdown 之后写日志会被静默丢弃，但不会悬空。随后可以重新 init 同一文件。
        g_singleton->release();
        reaper_instance().drain();
    }

} // namespace mylog
