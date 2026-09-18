#include "platform/single_instance.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "app/log.h"

namespace fr::single_instance {

namespace {
HANDLE g_mutex = nullptr;
const char* kMutexName = "Local\\FlashRec.SingleInstance.v2";
}  // namespace

bool acquire() {
  g_mutex = CreateMutexA(nullptr, FALSE, kMutexName);
  if (g_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
    FR_LOG_INFO("[APP] 已有 FlashRec 实例在运行");
    return false;
  }
  return true;
}

void raise_existing() {
  // d81：托盘模式下旧实例窗口可能处于隐藏态 —— 先 ShowWindow 再置前台。
  // 标题 = friendly_name（"FlashRec 投屏接收"），EnumWindows 前缀匹配兼容
  // 旧标题 "FlashRec"（FindWindowW 只能整名匹配，标题改过即失效）。
  struct Ctx {
    HWND hwnd = nullptr;
  } ctx;
  EnumWindows(
      [](HWND hwnd, LPARAM lp) -> BOOL {
        auto* c = (Ctx*)lp;
        wchar_t title[128] = {};
        GetWindowTextW(hwnd, title, 128);
        if (wcsncmp(title, L"FlashRec", 8) == 0) {
          DWORD pid = 0;
          GetWindowThreadProcessId(hwnd, &pid);
          if (pid != GetCurrentProcessId()) {
            c->hwnd = hwnd;
            return FALSE;
          }
        }
        return TRUE;
      },
      (LPARAM)&ctx);
  if (!ctx.hwnd) return;
  if (IsIconic(ctx.hwnd)) ShowWindow(ctx.hwnd, SW_RESTORE);
  else if (!IsWindowVisible(ctx.hwnd)) ShowWindow(ctx.hwnd, SW_SHOW);
  SetForegroundWindow(ctx.hwnd);
}

}  // namespace fr::single_instance

#else  // 非 Windows：M5 只保 Win/Linux 一等，Linux 走文件锁
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cstdio>
#include <string>

#include "app/log.h"

namespace fr::single_instance {

static int g_lock = -1;

bool acquire() {
  std::string path = getenv("XDG_RUNTIME_DIR") ? std::string(getenv("XDG_RUNTIME_DIR"))
                                               : std::string("/tmp");
  path += "/flashrec.lock";
  g_lock = open(path.c_str(), O_CREAT | O_RDWR, 0666);
  if (g_lock < 0) return true;  // 锁失败不拦运行
  if (flock(g_lock, LOCK_EX | LOCK_NB) != 0) {
    FR_LOG_INFO("[APP] 已有 FlashRec 实例在运行");
    close(g_lock);
    g_lock = -1;
    return false;
  }
  return true;
}

void raise_existing() {
  // Wayland/X11 唤起留待 M5 Linux 专项（托盘同批），当前仅日志
  FR_LOG_DEBUG("[APP] raise_existing: Linux 暂未实现窗口唤起");
}

}  // namespace fr::single_instance
#endif
