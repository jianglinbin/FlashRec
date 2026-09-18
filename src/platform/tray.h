#pragma once
// 托盘（M5，d46 双协议定稿的落地接口）。平台差异收口于 platform（规则 5）：
//   Win   = Shell_NotifyIcon + 原生 TrackPopupMenu（tray_win32.cpp）
//   Linux = SNI(D-Bus) 为主 + XEmbed(X11) 兜底，运行时探测（tray_linux.cpp）
// 托盘回调一律在主线程触发（Win：GLFW 主循环 poll_events 泵本线程消息窗口；
// Linux：X11 事件与 GLFW 同队列）——回调里可安全触碰窗口与事件总线。
//
// 关闭语义约定（main 接线）：托盘可用时「关闭窗口」= 隐到托盘（DMR 持续
// 在线，手机可继续投屏），「退出」只走托盘菜单项；托盘不可用 = 关闭即退出。
#include <functional>
#include <string>
#include <vector>

struct GLFWwindow;

namespace fr {

struct TrayMenuItem {
  int id = 0;          // 应用自定义；separator() 时忽略
  std::string label;   // UTF-8
  bool enabled = true;
  bool checked = false;
  bool sep = false;    // 分隔线（label 忽略）
  static TrayMenuItem Sep() {
    TrayMenuItem it;
    it.sep = true;  // d82：默认构造 sep=false，漏置会把分隔线画成空白可点行
    return it;
  }
};

struct TrayCallbacks {
  // 打开托盘菜单前取当前条目（每次右键实时构建，标签可随播放态变化）
  std::function<std::vector<TrayMenuItem>()> menu_provider;
  // 菜单项被选中（id 与 menu_provider 里的条目对应；约定 -1 = 退出）
  std::function<void(int)> on_menu_action;
  // 托盘图标左键单击（约定：显示/隐藏主窗口）
  std::function<void()> on_toggle_window;
};

namespace tray {

// 装托盘。icon_png 为 32x32 级别的 PNG（资产随包）；失败 = 无托盘（调用方
// 关闭语义自动退化为「关闭即退出」）。tooltip/menu_provider 不可为空。
bool create(GLFWwindow* win, const char* tooltip, const char* icon_png,
            const TrayCallbacks& cb);

// 主循环每轮调用（Linux 处理图标窗口 X 事件；Win 为空操作——GLFW poll
// 已泵本线程消息）。高频调用安全（XPending/PeekMessage 非阻塞）。
void poll();

// 退出路径调用一次（删图标 + 销毁消息窗口）。未创建时为空操作。
void remove();

}  // namespace tray

}  // namespace fr
