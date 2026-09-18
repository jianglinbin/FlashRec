// 托盘 Linux 入口（d84 双协议落地）：SNI 为主 + XEmbed 兜底，运行时探测。
// 探测顺序（最大兼容）：
//   1. 会话总线有 org.kde.StatusNotifierWatcher → SNI（GNOME/KDE/新式面板）
//   2. 否则探测 _NET_SYSTEM_TRAY_S<screen> 持有者 → XEmbed（XFCE/MATE 等面板）
//   3. 都没有 → 无托盘（调用方关闭语义退化为「关闭即退出」）
#include "platform/tray.h"

#include "app/log.h"
#include "platform/tray_linux_backend.h"

#ifdef FLASHREC_HAVE_SNI
#define FR_SNI_PROBE() fr::tray::sni_watcher_available()
#else
// 未链 libsystemd（CMake 自动裁剪）：SNI 路径整体旁路，保持最大可移植性
#define FR_SNI_PROBE() (false)
#endif

namespace fr {
namespace tray {

namespace {
Backend* g_backend = nullptr;
}

bool create(GLFWwindow* win, const char* tooltip, const char* icon_png,
            const TrayCallbacks& cb) {
  if (g_backend) return true;  // 已创建
  if (!tooltip || !icon_png || !cb.on_toggle_window) return false;

#ifdef FLASHREC_HAVE_SNI
  if (sni_watcher_available()) {
    Backend* b = sni_backend();
    if (b->create(win, tooltip, icon_png, cb)) {
      g_backend = b;
      return true;
    }
    FR_LOG_WARN("[TRAY] SNI 装载失败，回落 XEmbed");
  } else {
    FR_LOG_INFO("[TRAY] 无 SNI 宿主（Watcher 未运行），尝试 XEmbed");
  }
#endif

  Backend* b = xembed_backend();
  if (b->create(win, tooltip, icon_png, cb)) {
    g_backend = b;
    return true;
  }
  FR_LOG_WARN("[TRAY] 无可用托盘宿主（SNI/XEmbed 均未命中）");
  return false;
}

void poll() {
  if (g_backend) g_backend->poll();
}

void remove() {
  if (g_backend) {
    g_backend->remove();
    g_backend = nullptr;
  }
}

}  // namespace tray
}  // namespace fr
