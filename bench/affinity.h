// 基准程序共用的进程绑核：Windows 上真正生效；其他平台是 no-op——
// 绝对数值不可比，只看比值 / 扩展性结论（见 README「测量口径与可信度」）。
#pragma once

#include <cstdint>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace bench
{
    inline bool pin_process(std::uint64_t cpu_mask)
    {
        return SetProcessAffinityMask(GetCurrentProcess(),
                                      static_cast<DWORD_PTR>(cpu_mask)) != 0;
    }
} // namespace bench
#else
namespace bench
{
    inline bool pin_process(std::uint64_t /*cpu_mask*/) { return false; }
} // namespace bench
#endif
