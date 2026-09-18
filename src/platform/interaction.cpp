#include "platform/interaction.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fr {

InteractionThresholds interaction_thresholds(float scale) {
  InteractionThresholds t;
  if (scale <= 0.f) scale = 1.f;
#if defined(_WIN32)
  // SM_CXDRAG / SM_CXDOUBLECLK 为物理像素；hit-test 坐标是逻辑像素 → 除以缩放。
  // 用户在系统"指针选项/双击速度"里的自定义由此天然生效。
  const UINT drag = GetSystemMetrics(SM_CXDRAG);
  const UINT rect = GetSystemMetrics(SM_CXDOUBLECLK);
  if (drag > 0) t.drag_px = (float)drag / scale;
  if (rect > 0) t.dbl_rect_px = (float)rect / scale;
  const UINT ms = GetDoubleClickTime();
  if (ms > 0) t.dbl_time_sec = (double)ms / 1000.0;
#endif
  // Linux：X11 无统一直读口（Gtk 设置需引 gdk 依赖），V1 常量兜底，后续可经
  // XGetDefault / "gtk-double-click-time" 扩展；macOS 最低优先级同兜底。
  if (t.drag_px < 2.f) t.drag_px = 2.f;              // 高 DPI 折算下限（防误判）
  if (t.dbl_rect_px < 2.f) t.dbl_rect_px = 2.f;
  if (t.dbl_time_sec <= 0.1 || t.dbl_time_sec > 2.0) t.dbl_time_sec = 0.5;
  return t;
}

}  // namespace fr
