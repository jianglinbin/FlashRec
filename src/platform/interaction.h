#pragma once
// 系统交互阈值（UI_INTERACTION_PLAN.md §2/§3）。平台差异收口于此（规则 5）：
// Win 读 GetSystemMetrics / GetDoubleClickTime；Linux/macOS 常量兜底。
// 数值一律为**逻辑像素**口径（与 ViewInput/nanovg 坐标同系）：调用方传入
// 窗口 DPI 缩放系数，接口内部把系统物理像素折算回逻辑像素。
namespace fr {

struct InteractionThresholds {
  float drag_px = 4.f;        // 按下→位移超过此值 = 拖动（防抖；Win=SM_CXDRAG）
  float dbl_rect_px = 4.f;    // 双击第二击相对第一击的最大位移（Win=SM_CXDOUBLECLK）
  double dbl_time_sec = 0.5;  // 双击判定窗口，秒（Win=GetDoubleClickTime）；
                              // 用于全部「Windows 习惯」区域（顶栏空白双击最大化等）
  // 视频空白区专用双击窗口（§3 用户定值 150ms）：全 UI 唯一单击/双击动作互斥的
  // 场景（单击=播放/暂停 vs 双击=全屏）。UP1→DOWN2 口径已剔除首击按住时长，
  // 150ms ≈ 原生 DOWN1→DOWN2 口径 230~350ms 的等效余量（§3 依据）。手感调整
  // 只改这一个常量（platform 单一改动点）。
  double stage_dbl_time_sec = 0.150;
  // d78：播放/全屏态的双击窗口 + 挂起单击确认时长（d80 用户定值 200ms，首版
  // 300ms 手感偏钝）——看片双击节奏偏慢；单击在此二态挂起到窗口过，双击成立
  // 即取消（不夹带暂停）。
  double stage_dbl_pending_sec = 0.200;
};

// scale = 当前窗口 DPI 缩放（WindowGLFW::content_scale()）。取不到传 1。
InteractionThresholds interaction_thresholds(float scale);

}  // namespace fr
