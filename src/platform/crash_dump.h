#pragma once
// 崩溃自捕获（平台差异，仅 _WIN32 有实体；其余平台空实现）。
// 进程级未处理异常 → 写 minidump 到日志目录，事后定位崩溃模块/偏移，
// 不再靠"时隐时现的段错误 + 无现场"排查。
#include <string>

namespace fr::crash_dump {

// 须在 paths::init() 之后调用（需要日志目录）。
void install(const std::string& log_dir);

}  // namespace fr::crash_dump
