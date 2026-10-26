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

- **同一个实例可以被多个线程同时写日志**，但 `shutdown()`/`init()` 不能与该实例上的写日志并发。
- 单例对象**进程内常驻、地址不变**：`SingletonLogger::get()` 返回的裸指针永远有效（`LOGS_*`
  内部就是它，每条只多一次普通指针读）。
- `init()` 在已有单例上等价于“原地重建”：先释放旧资源再开新文件（换文件 / 轮转 / 异步开关都行），
  指针不变；同一文件重复 `init()` 会先归还租约再重开，不再报 `still in use`。
- `SingletonLogger::shutdown()` 只释放内部资源（flush、关文件、停线程、注销名字），对象保留：
  之后写日志会被静默丢弃，随时可以再次 `init()`。
- 重建失败时单例会回到未初始化状态（旧资源已释放），原因看 `init_checked()` 的返回值。
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
// 另一个仍然存活的 Logger 实例占着同一路径时，再次 init 会失败：
//   InitError::FileSink, message 含 "still in use"
```

自定义 sink 若要写文件，必须在 `SinkConfig::file_paths` 里申报路径，否则这个保护看不到它。

## JSON 输出（可选组件）

`mylog/JsonFormatter.h` 把每条日志格式化成单行 JSON（JSON Lines）。纯头文件组件，
谁 include 谁才编译，不用它的工程不为它付出任何编译成本：

```cpp
#include "JsonFormatter.h"
logger.set_formatter(std::make_unique<mylog::JsonFormatter>());
```

- 一条日志一行：引号、反斜杠、换行、制表符及控制字符都会被转义，可直接按行交给采集端解析；
- `JsonFormatter(false)` 省略 `src` 字段（不想在日志里暴露源码路径时用）；
- `ts` 为本地时间毫秒精度，中文按 UTF-8 原样输出（前提是日志内容本身是 UTF-8）；
- `set_formatter` 作用于该日志器的**全部**目标。想要"控制台普通格式、文件 JSON"，按 sink 单独设置：

```cpp
auto native = logger.native();
if (native && native->sinks().size() > 1)
    native->sinks()[1]->set_formatter(std::make_unique<mylog::JsonFormatter>());
```

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

### 单例开销：已归零

`LOGS_*` 现在是“取一次稳定裸指针 + 走实例路径”：单例对象首次 `init()` 后地址不再变化，
所以不需要每条日志都取快照。实测（null sink、100 万条/轮、钉核、9 轮交替取比值）：

```
median ratio (LOGS_* / LOG_INFO) = 0.949     ← 噪声内，等同
```

作为对照，改造前（每条 `weak_ptr::lock()` 取快照）这个比值稳定在 **1.16**，差值恒定 **+26ns**。

代价是契约收紧：`init()`/`shutdown()` **不能与写日志并发**（和 spdlog 换默认日志器同款要求）。

### 测量口径与可信度

- 进程绑核（单线程档固定一个核，多线程档固定 4 个核）、每档跑 5 轮取最快、跨轮再取最小。
- 基准程序已入库（`bench/` 目录，5 个程序与上文各项数据一一对应，默认不构建）：
  `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMYLOG_BUILD_BENCH=ON`，
  然后 `cmake --build build --config Release`。绑核仅 Windows 生效，其他平台只看比值。
- Windows 上抖动很大：同一份代码单轮可相差 30%~300%，**绝对值只适合判断量级，横向结论看"比值"**。
- "文件 sink + 异步 + 1 线程"这一档在负载机上无法比较（耗时由队列反压与磁盘决定），不要引用。

## 已评估、暂不采用的优化

记录结论，避免重复调研。

| 方案 | 结论 | 原因 |
|---|---|---|
| 开 LTO（`/GL + /LTCG`） | **无收益，不启用** | 实测 8 线程档差 0.3~3ns（噪声内），1 线程档甚至略慢；热路径已基本内联在调用方 TU |
| 保留快照（每条取 `shared_ptr`） | **已移除** | 改为常驻单例 + 裸指针后，单例与实例路径的比值 0.949（噪声内），26ns 开销消失 |
| 每线程槽位 + 进出计数 | **不需要了** | 该方案原本是为“既要快又要并发安全”设计的；现在契约已改为 init/shutdown 不可与写日志并发 |

## 待办（尚未实现，待决策）

| 项 | 价值 |
|---|---|
| `install(EXPORT)` / `mylogConfig.cmake` | 目前只能 `add_subdirectory` 使用 |
| `flush_and_wait` 精确边界 | 现实现排队列排空 + 同步刷盘；"正被写入的最后一条"仍可能落在刷新之后 |
| 运行中调 `init()` 的并发保护 | 目前靠契约（调用方保证无并发写）；若要支持并发重建，需要引用计数或 epoch 机制 |

## 构建与测试

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build -C Release --output-on-failure
```

也可以直接用预设：`cmake --preset release`、`cmake --build --preset release`、`ctest --preset release`
（`asan` 预设用于 ASan/UBSan；`tsan` 预设复现 CI 的 ThreadSanitizer 跑法，仅 Linux）。

选项：`MYLOG_BUILD_EXAMPLE`、`MYLOG_BUILD_TESTS`（默认都开）、
`MYLOG_BUILD_BENCH`（基准程序，默认关）、`MYLOG_WERROR`（告警当错误，默认关）。

## 目录

| 路径 | 说明 |
|---|---|
| `mylog/Logger.h`、`mylog/Logger.cpp` | 封装层，唯一需要阅读的业务代码 |
| `mylog/JsonFormatter.h` | 可选的 JSON Lines 格式器（纯头文件） |
| `mylog/spdlog-1.17.0/` | 第三方源码（vendored，不修改） |
| `tests/test_mylog.cpp` | 回归测试，CTest 驱动 |
| `example.cpp` | 用法演示与多线程压测 |
| `bench/` | 基准程序（`MYLOG_BUILD_BENCH=ON` 时构建） |

## 许可

MIT，见 [LICENSE](LICENSE)。`mylog/spdlog-1.17.0/` 保留其原始 MIT 许可。
