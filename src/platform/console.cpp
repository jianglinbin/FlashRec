#include "platform/console.h"

#include <cstdio>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fr {

bool console_available() {
#if defined(_WIN32)
  return ::GetConsoleWindow() != nullptr;
#else
  return true;  // POSIX：终端即控制台，无需申请
#endif
}

void console_set_utf8() {
#if defined(_WIN32)
  // UTF-8 代码页：spdlog 的 wincolor sink 用 WriteConsoleA 输出 UTF-8 字节，
  // 控制台不设 65001 就会按默认 GBK(936) 解码 → 中文乱码。
  ::SetConsoleOutputCP(CP_UTF8);
  ::SetConsoleCP(CP_UTF8);
#endif
}

bool console_open() {
#if defined(_WIN32)
  if (::GetConsoleWindow() == nullptr) {
    // 优先复用启动它的终端；双击启动（无父控制台）则新建一个控制台窗口。
    if (!::AttachConsole(ATTACH_PARENT_PROCESS)) {
      if (!::AllocConsole()) return false;
    }
  }
  // GUI 子系统即使附着/新建成功，标准 FILE 与标准句柄也可能无效：
  // - freopen 让 stdout/stderr/stdin 指向控制台设备；
  // - SetStdHandle 让 GetStdHandle(STD_OUTPUT_HANDLE) 有效
  //   （spdlog wincolor sink 构造时取它，无效则拿不到控制台）。
  FILE* f = nullptr;
  f = std::freopen("CONOUT$", "w", stdout);
  (void)f;
  f = std::freopen("CONOUT$", "w", stderr);
  (void)f;
  f = std::freopen("CONIN$", "r", stdin);
  (void)f;

  HANDLE hout = ::CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                              nullptr);
  if (hout != INVALID_HANDLE_VALUE) {
    ::SetStdHandle(STD_OUTPUT_HANDLE, hout);
    ::SetStdHandle(STD_ERROR_HANDLE, hout);  // 句柄随进程退出回收，不关闭
  }
  console_set_utf8();
  return true;
#else
  return true;
#endif
}

}  // namespace fr
