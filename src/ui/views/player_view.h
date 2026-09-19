#pragma once
// 播放态视图：IINA 排版（上下贴边半透明双栏 + idle 隐去 + 悬停进度缩略图）+ Windows 右上三键。
// 底栏与三键由 ui/views/chrome 提供（与待机态**共用一份实现**）；本文件只管播放态特有部分：
// 顶栏标题 / 来源徽章、悬停缩略图浮层、中央大播放键。
// 立即模式：draw 内完成 hit-test 与回调；控件动画状态由 ViewInput 持有。
#include <functional>

#include <nanovg.h>

#include "ui/widgets.h"  // ButtonFx

namespace fr {

struct Theme;
struct PlaybackSnapshot;

struct ViewInput {
  float mx = 0, my = 0;          // 鼠标（客户区坐标，原点左上）
  // d222：全局屏幕坐标（物理像素）。拖拽/双击判定专用 —— 双击时全局位置不变、拖动会变，
  // 且**不受窗口几何跳变影响**（最大化/还原会让客户区坐标整体位移，但全局坐标不动）。
  // 主线程在鼠标移动/按键时刷新；Wayland 无全局坐标时回落为客户区物理坐标。
  float gx = 0, gy = 0;
  double now = 0;                // 当前时刻（秒，主循环 steady_clock）
  double last_input = 0;         // 最近输入时刻（idle 隐去基准）
  float dt = 0;                  // 帧间隔（秒）——按钮 hover/press 动画步长
  bool down = false;             // 左键按住
  bool down_middle = false;      // 中键按住（音量区 = 静音）
  bool down_right = false;       // 右键按住（d45：菜单边沿检测在渲染线程，见 render_loop）
  // 本帧是否已有控件消费掉这次点击（空白处单击/双击判定据此让位；每帧开始由 main 清零）
  bool click_consumed = false;
  // 拖拽状态（调用方持有）
  bool drag_progress = false;
  bool drag_volume = false;
  float drag_value = 0;          // 拖拽中的 percent
  // 键盘拖动进度（d29：左右键按住 = 进度条动 + 预览显示，**不触发 seek**；
  // 抬起才提交 seek。主线程写，渲染线程只读 —— 不入渲染线程跨帧槽）
  bool kb_progress = false;
  float kb_progress_value = 0;   // 键盘拖动当前值（0..1）
  double kb_preview_until = 0;   // 抬起后预览保留到该时刻（mono 秒；防单发闪烁）
  bool fullscreen = false;       // 全屏态：顶栏任何情况不渲染（仅窗口模式渲染）
  bool maximized = false;        // 最大化中：Max 按钮显示"还原"图标

  // —— d74 剪贴板链接提示条（主线程写、渲染线程只读显示）——
  // 定长缓冲是平凡复制约束下的形态（ViewInput 整块 memcpy 跨线程）；URL 的
  // 业务真值在主线程（播放动作按主线程持有的 URL 执行），这里仅承载显示。
  char clip_url[512] = {};
  bool clip_visible = false;
  double clip_at = 0;            // 出现时刻（与 now 同源 mono 秒；停留/淡出窗口计算）
  // 窗口模式的圆角半径（全屏/最大化时由调用方置 0 → 直角；窗口模式取 skin.shape）
  float win_radius = 0.f;
  // 按钮 hover/press 动画状态（两态共用同一实例；id 见 ui/views/chrome.h BtnId）
  ButtonFx btns;
  // OSD 反馈（d28；主线程写，随 publish 广播；渲染线程零跨帧状态 ——
  // 淡出曲线由 now - osd_at 推算，不依赖任何渲染线程续接）
  int osd_kind = 0;      // 0=无 1=音量 2=进度(seek 目标时间) 3=动作反馈(播放/暂停/全屏/画中画)
  double osd_value = 0;  // 音量 0..100；seek = 目标绝对秒；kind3 = 动作 id（1 播放 2 暂停
                         // 3 进全屏 4 退全屏 5 进画中画 6 退画中画）
  double osd_at = 0;     // 触发时刻（与 now 同源，主循环 mono 秒）
  int osd_dir = 1;       // kind2：方向图标 1=前进 0=后退（d32：触发时定格，不再按实时
                         // 位置判定——seek 完成后 position==target 会误判成后退）
  // d124：视频正走软件渲染兜底（GL 后端被判定失效时由渲染线程自动切换）。
  // 纯渲染线程每帧从 player_ 自己的真值刷新，不是跨线程输入 ⇒ 不入 input_changed 同步。
  bool sw_video = false;
};

struct ViewCallbacks {
  std::function<void()> on_close, on_minimize, on_maximize;
  std::function<void()> on_play_pause, on_prev, on_next, on_stop;  // d158：停止键（无上/下一集数据时）
  std::function<void()> on_pip, on_fullscreen;
  std::function<void()> on_speed;          // d169：倍速按钮点击（渲染线程切换档位弹层）
  std::function<void()> on_skin;           // d182：顶栏皮肤按钮点击（渲染线程切换皮肤弹层）
  std::function<void(double)> on_seek;     // 绝对秒（释放/点击时提交）
  std::function<void(float)> on_volume;    // 0..1
  std::function<void(bool)> on_mute;       // true = 静音
};

void draw_player_view(NVGcontext* vg, const Theme& t, float w, float h,
                      const PlaybackSnapshot& snap, ViewInput& in, ViewCallbacks& cb,
                      int thumb_img = 0, const DmgRect* clip = nullptr);

}  // namespace fr
