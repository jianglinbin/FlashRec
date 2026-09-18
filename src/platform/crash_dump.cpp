#include "platform/crash_dump.h"

#ifdef _WIN32

#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <ctime>

#include "app/log.h"

#pragma comment(lib, "dbghelp.lib")

namespace fr::crash_dump {

namespace {

std::string g_dir;

std::string dump_path() {
  char name[128];
  std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _MSC_VER
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  snprintf(name, sizeof(name), "crash-%04d%02d%02d-%02d%02d%02d-%lu.dmp", tm.tm_year + 1900,
           tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
           (unsigned long)GetCurrentProcessId());
  return g_dir + "/" + name;
}

LONG WINAPI on_crash(EXCEPTION_POINTERS* ep) {
  const std::string path = dump_path();
  HANDLE f = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    HANDLE proc = GetCurrentProcess();
    DWORD pid = GetCurrentProcessId();
    HANDLE snap = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    MiniDumpWriteDump(snap ? snap : proc, pid, f, MiniDumpNormal, ep ? &mei : nullptr, nullptr,
                      nullptr);
    if (snap) CloseHandle(snap);
    CloseHandle(f);
    FR_LOG_ERROR("[CRASH] 未处理异常 0x{:08X}，minidump: {}", (unsigned)ep->ExceptionRecord
                                                              ? ep->ExceptionRecord->ExceptionCode
                                                              : 0,
                 path);
  } else {
    FR_LOG_ERROR("[CRASH] 未处理异常（minidump 写入失败）");
  }
  return EXCEPTION_EXECUTE_HANDLER;  // 不再交给 WER（进程随即退出，日志已留证）
}

}  // namespace

void install(const std::string& log_dir) {
  g_dir = log_dir;
  SetUnhandledExceptionFilter(on_crash);
}

}  // namespace fr::crash_dump

#else  // !_WIN32

namespace fr::crash_dump {
void install(const std::string&) {}
}  // namespace fr::crash_dump

#endif
