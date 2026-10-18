#include "platform/keep_awake.h"

#ifdef _WIN32
// CMake 已全局 /D 这两个宏；加卫语句避免 C4005 宏重定义警告（tray_win32 同款）。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "app/log.h"

namespace fr::keep_awake {

void set(bool on) {
  EXECUTION_STATE flags = ES_CONTINUOUS;
  if (on) flags |= ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED;
  SetThreadExecutionState(flags);
}

}  // namespace fr::keep_awake

#else
#include "app/log.h"

namespace fr::keep_awake {

void set(bool on) {
  // Linux：接 org.freedesktop.ScreenSaver dbus 抑制（与托盘同批做）
  static bool last = false;
  if (on != last) {
    last = on;
    FR_LOG_DEBUG("[APP] keep_awake({})：Linux dbus 抑制待 M5 Linux 专项", on);
  }
}

}  // namespace fr::keep_awake
#endif
