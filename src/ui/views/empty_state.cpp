#include "ui/views/empty_state.h"

#include <algorithm>

#include <nanovg.h>

#include "app/event_bus.h"
#include "ui/theme.h"
#include "ui/views/chrome.h"
#include "ui/widgets.h"

namespace fr {

void draw_empty_state(NVGcontext* vg, const Theme& t, float w, float h,
                      const char* friendly_name, const PlaybackSnapshot& snap, ViewInput& in,
                      ViewCallbacks& cb, const DmgRect* clip) {
  const auto& L = t.layout;
  const float rad = in.win_radius;  // 全屏 / 最大化 = 0（直角）

  // —— 舞台底色（窗口模式圆角化：四角外不落地，透明帧缓冲透出桌面）——
  // 全窗层：区域帧也要画（GL scissor 硬裁到脏区即可，几何不裁）
  panel_bg(vg, 0, 0, w, h, rad, t.stageBg);

  // —— 顶栏：产品字标 + Windows 三键（窗口模式常显、不透明、不自动隐藏；
  //     全屏时整个不渲染 —— 与播放态 player_view 同一规则，d25 修：待机态漏了判断）——
  // 字标文字是纯绘制段可裁；三键交互在 draw_win_buttons 内部每帧全跑
  if (!in.fullscreen) {
    if (dmg_hit(clip, 0.f, 0.f, w, L.topBarH)) {
      bar(vg, t, 0, 0, w, L.topBarH, true, rad, &t.topBarBg);
      text(vg, t, L.topBarPadL, L.topBarH * 0.5f, L.titleFont, t.titleText, "FlashRec");
      text(vg, t, L.topBarPadL + 58, L.topBarH * 0.5f, L.badgeFont, t.mutedText, "投屏接收端");
    }
    draw_win_buttons(vg, t, w, in, cb, clip);
    draw_skin_button(vg, t, w, in, cb, clip);  // d182：顶栏皮肤按钮
  }

  // —— 中央：设备名 + 引导文案 ——
  // d220：舞台文字用皮肤可配 token `stageText`/`stageSubText`（未设则回落 titleText/subText），
  // 不写死颜色 —— 舞台底色每个皮肤不同，文字色必须由皮肤配置决定。
  const NVGcolor stage_t = t.stageText.a > 0.003f ? t.stageText : t.titleText;
  const NVGcolor stage_s = t.stageSubText.a > 0.003f ? t.stageSubText : t.subText;
  text(vg, t, w * 0.5f, h * 0.5f - 14, 20, stage_t,
       friendly_name && *friendly_name ? friendly_name : "FlashRec",
       NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
  text(vg, t, w * 0.5f, h * 0.5f + 12, 12, stage_s, "等待投屏 · 在播放器的投屏设备中选择本机",
       NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

  // —— 底栏：与播放态同一份实现；媒体类控件随会话可用性，窗口类控件恒可用 ——
  // d202：待机态**也**按 idle 淡出（此前写死 fade=1 恒显，导致"操作栏不隐藏"）。
  const float fade = chrome_fade(vg, t, w, h, snap, in, snap.has_session);
  draw_bottom_bar(vg, t, w, h, snap, in, cb, snap.has_session, fade, false, rad, 0, clip);

  // —— OSD 反馈浮层（待机态音量键/滚轮同样有反馈）——
  draw_osd(vg, t, w, h, snap, in, clip);

  // 待机态：底栏之外全是舞台，点击语义交给 main（空白处单击/双击）；
  // 这里只需保证 chrome 已把自己的命中结果记进 in.click_consumed。
}

void draw_standby_mini(NVGcontext* vg, const Theme& t, float w, float h,
                       const char* friendly_name, const DmgRect* clip) {
  (void)clip;  // 画中画小窗全窗层（P3 如需细化再拆）
  panel_bg(vg, 0, 0, w, h, t.shape.windowRadius, t.stageBg);

  const char* name = friendly_name && *friendly_name ? friendly_name : "FlashRec";
  const float cx = w * 0.5f, cy = h * 0.5f;
  const NVGcolor stage_t = t.stageText.a > 0.003f ? t.stageText : t.titleText;
  const NVGcolor stage_s = t.stageSubText.a > 0.003f ? t.stageSubText : t.subText;
  if (text_width(vg, 15, name) <= w - 24)
    text(vg, t, cx, cy - 11, 15, stage_t, name, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
  else
    text_truncated(vg, t, 12, cy - 11, 15, stage_t, name, w - 24);
  text(vg, t, cx, cy + 11, 11, stage_s, "等待投屏", NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
}

}  // namespace fr
