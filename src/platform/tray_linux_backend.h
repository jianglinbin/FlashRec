#pragma once
// Linux 托盘后端内部接口（d84 双协议拆分；仅 platform 目录内使用）。
//   SNI    = org.kde.StatusNotifierItem（D-Bus；GNOME/KDE/新式面板，主路径）
//   XEmbed = X11 老托盘标准（XFCE/MATE/Cinnamon 等面板，兜底）
// 两者由 tray_linux.cpp 运行时探测择一装载，对外仍是中性 tray::create/poll/remove。
#include "platform/tray.h"

struct GLFWwindow;

namespace fr {
namespace tray {

class Backend {
 public:
  virtual ~Backend() = default;
  virtual bool create(GLFWwindow* win, const char* tooltip, const char* icon_png,
                      const TrayCallbacks& cb) = 0;
  virtual void poll() = 0;
  virtual void remove() = 0;
};

// SNI 宿主探测：会话总线上 org.kde.StatusNotifierWatcher 有属主 = 可用。
bool sni_watcher_available();

Backend* sni_backend();     // tray_sni_linux.cpp
Backend* xembed_backend();  // tray_xembed_linux.cpp

}  // namespace tray
}  // namespace fr
