// JsonFormatter.h
// 可选组件：把每条日志格式化成单行 JSON（JSON Lines）。
//
// 这是一个"纯头文件"组件：实现全部 inline，谁 include 谁才编译，
// 不用 JSON 的工程不会为它付出任何编译时间或二进制体积。
// 库本体（Logger.cpp、LogConfig）没有为它改动一行。
//
// 用法：
//     #include "JsonFormatter.h"
//     logger.set_formatter(std::make_unique<mylog::JsonFormatter>());
//
// 输出形如：
//     {"ts":"2026-10-24T10:00:00.123","level":"info","logger":"app",
//      "tid":12345,"msg":"hello 42","src":"main.cpp:42"}
//
// 与 pattern 的关系：set_formatter 与 pattern 是二选一，设置格式器之后
// 该日志器上的 LogConfig::pattern 不再生效。想让"控制台普通格式、文件 JSON"，
// 需要按 sink 分别设置（set_formatter 会作用于全部 sink）：
//     auto native = logger.native();
//     if (native && native->sinks().size() > 1)
//         native->sinks()[1]->set_formatter(std::make_unique<mylog::JsonFormatter>());
//
// 注意：
//   * payload 里的引号、反斜杠、换行、制表符以及 0x00-0x1F 控制字符都会被转义，
//     所以"一条日志 = 一行"，可以直接交给采集端按行解析；
//   * 中文按 UTF-8 原样输出（JSON 允许），但前提是日志内容本身是 UTF-8。
//     MSVC 下源文件若为 GBK 且未加 /utf-8，中文字面量会产出非法 UTF-8，本格式器不负责转码；
//   * ts 是本地时间、不含时区偏移；要 UTC 把 append_time() 里的
//     details::os::localtime 换成 details::os::gmtime 即可。
#pragma once

#include <chrono>
#include <cstddef>
#include <ctime>
#include <iterator>
#include <memory>
#include <string_view>

#include "spdlog/common.h"
#include "spdlog/details/log_msg.h"
#include "spdlog/details/os.h"
#include "spdlog/fmt/fmt.h"
#include "spdlog/formatter.h"

namespace mylog
{

    class JsonFormatter final : public spdlog::formatter
    {
    public:
        // include_source = false 时省略 src 字段（不想在日志里暴露源文件路径时用）。
        explicit JsonFormatter(bool include_source = true) noexcept
            : include_source_(include_source)
        {
        }

        void format(const spdlog::details::log_msg &msg, spdlog::memory_buf_t &dest) override
        {
            append_text(dest, "{\"ts\":\"");
            append_time(msg.time, dest);

            append_text(dest, "\",\"level\":\"");
            // spdlog::string_view_t 在 bundled fmt 下是 fmt::string_view，
            // 与 std::string_view 类型不同，必须显式转一次。
            const auto level_name = spdlog::level::to_string_view(msg.level);
            append_text(dest, std::string_view(level_name.data(), level_name.size()));

            append_text(dest, "\",\"logger\":\"");
            append_escaped(dest, std::string_view(msg.logger_name.data(), msg.logger_name.size()));

            fmt::format_to(std::back_inserter(dest), "\",\"tid\":{},\"msg\":\"", msg.thread_id);
            append_escaped(dest, std::string_view(msg.payload.data(), msg.payload.size()));
            append_text(dest, "\"");

            if (include_source_ && msg.source.filename != nullptr)
            {
                append_text(dest, ",\"src\":\"");
                append_escaped(dest, std::string_view(msg.source.filename));
                fmt::format_to(std::back_inserter(dest), ":{}", msg.source.line);
                append_text(dest, "\"");
            }

            // pattern_formatter 也是自己写行尾的（见 pattern_formatter-inl.h），
            // 自定义格式器必须同样补上，否则所有日志会连成一行。
            append_text(dest, "}\n");
        }

        std::unique_ptr<spdlog::formatter> clone() const override
        {
            return std::make_unique<JsonFormatter>(*this);
        }

    private:
        // 本类只有这个 bool 成员，默认拷贝构造即可用于 clone()。

        static void append_text(spdlog::memory_buf_t &dest, std::string_view text)
        {
            dest.append(text.data(), text.data() + text.size());
        }

        // 本地时间 + 毫秒：2026-10-24T10:00:00.123
        static void append_time(const spdlog::log_clock::time_point &tp, spdlog::memory_buf_t &dest)
        {
            const std::time_t seconds = spdlog::log_clock::to_time_t(tp);
            const std::tm tm_time = spdlog::details::os::localtime(seconds);

            long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               tp.time_since_epoch())
                               .count() %
                           1000;
            if (ms < 0)
                ms += 1000; // 1970 年之前的时间戳：取余为负，补齐到 [0, 999]

            fmt::format_to(std::back_inserter(dest), "{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}.{:03d}",
                           tm_time.tm_year + 1900, tm_time.tm_mon + 1, tm_time.tm_mday,
                           tm_time.tm_hour, tm_time.tm_min, tm_time.tm_sec,
                           static_cast<int>(ms));
        }

        // JSON 字符串转义。UTF-8 字节（>= 0x80）原样保留，只有必须转义的才处理。
        static void append_escaped(spdlog::memory_buf_t &dest, std::string_view text)
        {
            static constexpr char kHex[] = "0123456789abcdef";
            for (const char ch : text)
            {
                switch (ch)
                {
                case '"':
                    append_text(dest, "\\\"");
                    break;
                case '\\':
                    append_text(dest, "\\\\");
                    break;
                case '\b':
                    append_text(dest, "\\b");
                    break;
                case '\f':
                    append_text(dest, "\\f");
                    break;
                case '\n':
                    append_text(dest, "\\n");
                    break;
                case '\r':
                    append_text(dest, "\\r");
                    break;
                case '\t':
                    append_text(dest, "\\t");
                    break;
                default:
                {
                    const auto byte = static_cast<unsigned char>(ch);
                    if (byte < 0x20)
                    {
                        // 其余控制字符走 \u00XX，避免生成非法 JSON
                        dest.push_back('\\');
                        dest.push_back('u');
                        dest.push_back('0');
                        dest.push_back('0');
                        dest.push_back(kHex[(byte >> 4) & 0x0F]);
                        dest.push_back(kHex[byte & 0x0F]);
                    }
                    else
                    {
                        dest.push_back(ch);
                    }
                    break;
                }
                }
            }
        }

        bool include_source_;
    };

} // namespace mylog
