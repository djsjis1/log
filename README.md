# mylog

基于 [spdlog 1.17](https://github.com/gabime/spdlog) 的轻量 C++17 日志封装。只依赖标准库与随仓库一同携带的 spdlog，
可以直接 `add_subdirectory` 嵌入其他工程。

## 最小用法

```cpp
#include "Logger.h"

int main()
{
    // 多实例：自己持有，各自写各自的文件
    mylog::LogConfig cfg;
    cfg.logger_name = "app";
    cfg.log_file   = "app.log";
    mylog::Logger logger(cfg);   // 构造即初始化，失败看 init_result()
    LOG_INFO(logger, "hello {}", 42);

    // 全局单例
    mylog::SingletonLogger::init(cfg);
    LOGS_INFO("全局日志");
    mylog::SingletonLogger::shutdown();
}
```

两个宏系列：`LOG_XXX(logger, ...)` 传实例，`LOGS_XXX(...)` 用全局单例。
两者都会先查级别再求值参数，运行期被过滤的日志不会构造昂贵参数；
编译期裁剪另由 `MYLOG_ACTIVE_LEVEL` 控制。

## 生命周期契约（必须遵守）

- **同一个实例可以被多个线程同时写日志**，但 `shutdown()`/析构不能与该实例上的写日志并发。
- 单例请用 `SingletonLogger::acquire()` 取快照：快照延长的是**对象生命周期**，
  它能抵御另一个线程调用 `SingletonLogger::shutdown()`，但不代表可以并发调用某个 `Logger` 上的 `shutdown()`。
- `SingletonLogger::shutdown()` 返回时资源已经回收，可以立刻用同一个日志文件重新 `init()`。
- `flush()` 只投递刷新请求：异步下以阻塞方式入队（不会挤掉已排队的日志），但不等刷新真正完成；
  要等异步日志写完只能 `shutdown()`。
- 要"确认此刻已经落盘"又不想关掉日志器，用 `flush_and_wait(超时)`：等队列排空后同步刷盘，
  成功返回 `true`、超时返回 `false`。
- 想直接用 spdlog 原生 API（`set_pattern`、`sinks()` 等），用 `logger.native()` 取底层
  `shared_ptr<spdlog::logger>`。不要把它存到 `shutdown()` 之后，否则文件句柄会被它拖住。
- 在日志器自己的异步线程、定时线程或用户回调里调用 `shutdown()` 会被拒绝并打印警告；
  析构同理，只能泄漏资源保命。
- `flush_interval` 默认 `0`（不建任何线程）。启用后，所有实例共用进程内**一个**调度线程
  （相同间隔共用一个桶），不再为每个日志器各开一个线程。
- 在 `log_callback` 里向**另一个**实例写日志是允许的；若此时又并发导出回溯，
  spdlog 会形成 A/B 回溯锁交叉，本库用进程级锁把导出串行化，因此不会死锁
  （TSan 会报 lock-order-inversion，属已知假阳性，见 `.github/tsan.supp`）。
- 滚动保留数量默认：按大小 `5` 个文件、按天 `7` 个、按小时 `24` 个（`*_max_files = 0` 表示不删）。

## 文件独占

同一个日志文件（绝对化 + `weakly_canonical` + Windows 大小写折叠后相同）在进程内只允许一个存活的文件 sink：

```cpp
mylog::LogConfig cfg;
cfg.log_file = "app.log";
// 旧实例或旧 shared_ptr 快照还活着时，再次 init 会失败：
//   InitError::FileSink, message 含 "still in use"
```

自定义 sink 若要写文件，必须在 `SinkConfig::file_paths` 里申报路径，否则这个保护看不到它。

## 性能

### 调优先看配置，再看用法，最后才是库本身

实测（Windows / MSVC 19.44 / Ryzen 7 9700X，文件 sink，单线程，10 万条/轮）：

| 做法 | ns/条 | 相对 |
|---|---|---|
| 默认 pattern（含时间戳、`%t` 线程号、`%s:%#` 源码位置） | 781 | — |
| 去掉时间戳 `%Y-%m-%d %H:%M:%S.%e` | 688 | -12% |
| 只留 `%v` | 285 | **-64%** |

```cpp
// 推荐：高吞吐场景把 pattern 压到最小（或给文件用精简格式）
cfg.pattern      = "[%H:%M:%S.%e] [%l] %v";
cfg.file_pattern = "[%H:%M:%S.%e] [%l] [%t] %v";   // 控制台丰富、文件精简
```

**默认 `flush_level = Warn` 时，每打一条 Warn 都会强制刷盘**，实测 697ns → 10497ns（**15 倍**）。
高频 Warn 的模块请改成定时刷：

```cpp
cfg.flush_level    = mylog::Level::Off;
cfg.flush_interval = std::chrono::seconds(1);   // 共用一个调度线程，崩溃最多丢 1 秒
```

### 和原生 spdlog 比

同配置（`pattern = "%v"`、不自动刷盘）下：同步/异步主路径**持平**（8 线程同步 +2%，
异步提交 0.96~1.0）；被过滤的日志（`level=Warn` 时打 Info）**快 56%**，因为宏先过
`should_process()`，参数根本不会被求值。

### 单例的固定开销：约 26 ns

`LOGS_*` 比 `LOG_*`（已有实例）每条多约 26ns，这钱花在**确认"当前全局实例是谁" + 借一份
调用期间有效的所有权**上：

1. 全局日志器随时可能被 `init()`/`shutdown()` 换掉，裸指针会悬空；
2. 所以 `acquire()` 返回 `shared_ptr`（借条：这条写完前不许销毁，引用计数 +1/−1）；
3. 线程本地只能缓存 `weak_ptr`（强引用会让 `shutdown()` 后文件仍被占着，同文件重开报 `still in use`），
   因此每条都必然有一次 `lock()`。

**多线程下这不是竞争瓶颈**：实测快照额外开销 1 线程 +28.9ns、8 线程 +27.5ns（不随线程数放大）。

热路径要抹掉这 26ns，不用改库，改用法即可：

```cpp
// 循环外借一次，循环内复用（这张借条只在第一条付成本）
auto lg = mylog::SingletonLogger::acquire();
if (lg)
    for (int i = 0; i < 100000; ++i)
        LOG_INFO(*lg, "tick {}", i);
// 代价：这段时间旧日志器不会被回收（文件仍被占用），且换配置对手里的快照不生效
```

### 测量口径与可信度

- 进程绑核（单线程档固定一个核，多线程档固定 4 个核）、每档跑 5 轮取最快、跨轮再取最小。
- Windows 上抖动很大：同一份代码单轮可相差 30%~300%，**绝对值只适合判断量级，横向结论看"比值"**。
- "文件 sink + 异步 + 1 线程"这一档在负载机上无法比较（耗时由队列反压与磁盘决定），不要引用。
- 基准程序未入库（在 `build/bench/`，已被 gitignore）。自行编译要点：
  `cl /std:c++17 /EHsc /MD /O2 /DSPDLOG_COMPILED_LIB /I mylog /I mylog/spdlog-1.17.0/include ...`，
  链接 `mylog.lib` 与 `spdlog.lib`；**必须定义 `SPDLOG_COMPILED_LIB`**，否则 spdlog 头会以
  header-only 形式在 TU 内生成定义，与 `spdlog.lib` 撞重复符号。

## 已评估、暂不采用的优化

记录结论，避免重复调研。

| 方案 | 结论 | 原因 |
|---|---|---|
| 开 LTO（`/GL + /LTCG`） | **无收益，不启用** | 实测 8 线程档差 0.3~3ns（噪声内），1 线程档甚至略慢；热路径已基本内联在调用方 TU，跨模块可省的极少；构建与调试代价却不小 |
| `SingletonLogger::raw()` 返回裸指针 | **不加** | 能省那 26ns，但会失去"调用期间对象不被销毁"；与 spdlog `default_logger_raw()` 同风险 |
| epoch / hazard pointer 无锁快照 | **不做** | 能把 26ns 压到几 ns，但需上百行高并发代码，出错即崩溃，收益不等价 |
| "有人持快照就拒绝切换配置" | **不做** | 每条 `LOGS_*` 本身都会短暂持快照，热循环更可能贯穿进程生命周期，会导致几乎永远切不了配置 |
| 重写生命周期管理 | **不做** | 现有关闭顺序、文件占用、后台线程回收相互约束，收益不抵风险 |

## 待办（尚未实现，待决策）

| 项 | 价值 |
|---|---|
| `Logger::reconfigure(cfg)` | 实例版运行中换文件/滚动/异步（本质是安全重启，有毫秒级空窗期） |
| `SingletonLogger::generation()` + `Logger::is_current()` | 让热循环里拿着快照的代码能自检"我是否已被换掉"，检查成本近零 |
| `install(EXPORT)` / `mylogConfig.cmake` | 目前只能 `add_subdirectory` 使用 |
| `flush_and_wait` 精确边界 | 现实现排队列排空 + 同步刷盘；"正被写入的最后一条"仍可能落在刷新之后 |

## 构建与测试

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build -C Release --output-on-failure
```

也可以直接用预设：`cmake --preset release`、`cmake --build --preset release`、`ctest --preset release`
（`asan` 预设用于 ASan/UBSan）。

选项：`MYLOG_BUILD_EXAMPLE`、`MYLOG_BUILD_TESTS`（默认都开）。

## 目录

| 路径 | 说明 |
|---|---|
| `mylog/Logger.h`、`mylog/Logger.cpp` | 封装层，唯一需要阅读的业务代码 |
| `mylog/spdlog-1.17.0/` | 第三方源码（vendored，不修改） |
| `tests/test_mylog.cpp` | 回归测试，CTest 驱动 |
| `example.cpp` | 用法演示与多线程压测 |

## 许可

MIT，见 [LICENSE](LICENSE)。`mylog/spdlog-1.17.0/` 保留其原始 MIT 许可。
