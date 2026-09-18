#include "ui/widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include <glad/gl.h>
#include <nanovg.h>

#include "ui/theme.h"

namespace fr {

// —— 区域重绘：脏区裁剪通道（v0.4.0 d56）——
bool dmg_hit(const DmgRect* clip, float x, float y, float w, float h) {
  if (clip == nullptr) return true;  // 全量绘制
  return x < (float)(clip->x + clip->w) && x + w > (float)clip->x &&
         y < (float)(clip->y + clip->h) && y + h > (float)clip->y;
}

void dmg_scissor_begin(NVGcontext* vg, int win_h, const DmgRect& r) {
  glEnable(GL_SCISSOR_TEST);
  // 本项目窗口尺寸 == framebuffer 尺寸（render_loop glViewport 用同一 w/h），
  // 逻辑像素即物理像素；y 翻转换算唯一入口 gl_scissor_y（damage.h）。
  glScissor(r.x, gl_scissor_y(win_h, r), r.w, r.h);
  // nanovg 侧同步裁剪（逻辑坐标）：把 nanovg 自身状态也压到脏区内，
  // 与 GL 硬边界互为冗余；内部既有 win_clip_rect 的求交语义不冲突（只会更严）。
  nvgScissor(vg, (float)r.x, (float)r.y, (float)r.w, (float)r.h);
}

void dmg_scissor_end(NVGcontext* vg) {
  nvgResetScissor(vg);
  glDisable(GL_SCISSOR_TEST);
}

void begin_frame(NVGcontext* vg, int w, int h) {
  nvgBeginFrame(vg, (float)w, (float)h, 1.0f);
}

void end_frame(NVGcontext* vg) { nvgEndFrame(vg); }

void rounded_rect(NVGcontext* vg, float x, float y, float w, float h, float r, NVGcolor fill) {
  nvgBeginPath(vg);
  nvgRoundedRect(vg, x, y, w, h, r);
  nvgFillColor(vg, fill);
  nvgFill(vg);
}

void panel_bg(NVGcontext* vg, float x, float y, float w, float h, float rad, NVGcolor c) {
  nvgBeginPath(vg);
  if (rad > 0.5f)
    nvgRoundedRect(vg, x, y, w, h, rad);
  else
    nvgRect(vg, x, y, w, h);
  nvgFillColor(vg, c);
  nvgFill(vg);
}

void win_clip_rect(NVGcontext* vg, float x, float y, float w, float h) {
  // 与窗口矩形求交的裁剪。参数就是普通的矩形，调用方自己算好范围
  // （底栏按钮区 = (0, botY, win_w, win_h - botY)）。
  // 之所以不提供"内缩 pad"那种接口：pad 是四边同缩的语义，用它表达
  // "只裁底栏"会算出负高度的裁剪区，把整条底栏连按钮一起裁没（d17 踩过）。
  // nanovg 的 scissor 只有矩形语义，圆角镂空表达不了 —— 窗口四角由调用方
  // 保证不绘制元素（顶栏三键的悬停底色已按窗口圆角逐角收边）。
  if (w <= 0.f || h <= 0.f) return;  // 空矩形：不设裁剪（保持现状，别把它变成"全裁掉"）
  nvgSave(vg);
  nvgIntersectScissor(vg, x, y, w, h);
}

void win_clip_reset(NVGcontext* vg) { nvgRestore(vg); }

void bar(NVGcontext* vg, const Theme& t, float x, float y, float w, float h, bool top, float rad,
         const NVGcolor* bg) {
  // 贴边栏：顶栏只有"上左 / 上右"两角圆，底栏只有"下左 / 下右"两角圆。
  // 用逐角圆角半径构造路径，避免圆角外落地像素（透明帧缓冲下会露出不透明块）。
  const float r_tl = top ? rad : 0.f;
  const float r_tr = top ? rad : 0.f;
  const float r_br = top ? 0.f : rad;
  const float r_bl = top ? 0.f : rad;
  nvgBeginPath(vg);
  if (rad > 0.5f)
    nvgRoundedRectVarying(vg, x, y, w, h, r_tl, r_tr, r_br, r_bl);
  else
    nvgRect(vg, x, y, w, h);
  nvgFillColor(vg, bg ? *bg : t.barBg);
  nvgFill(vg);
  // 1px 分隔描边（顶栏画底边，底栏画顶边）——只画内侧横线，不触碰圆角
  nvgBeginPath(vg);
  nvgMoveTo(vg, x, top ? y + h : y);
  nvgLineTo(vg, x + w, top ? y + h : y);
  nvgStrokeWidth(vg, 1.f);
  nvgStrokeColor(vg, top ? t.barBorderBottomBar : t.barBorderTopBar);
  nvgStroke(vg);
}

void text(NVGcontext* vg, const Theme& t, float x, float y, float size, NVGcolor color,
          const char* str, int align) {
  (void)t;
  nvgFontSize(vg, size);
  nvgFontFace(vg, "cjk");
  nvgFillColor(vg, color);
  nvgTextAlign(vg, align);
  nvgText(vg, x, y, str, nullptr);
}

float text_width(NVGcontext* vg, float size, const char* str) {
  nvgFontSize(vg, size);
  nvgFontFace(vg, "cjk");
  float b[4];
  nvgTextBounds(vg, 0, 0, str, nullptr, b);
  return b[2] - b[0];
}

void text_truncated(NVGcontext* vg, const Theme& t, float x, float y, float size, NVGcolor color,
                    const char* str, float max_w) {
  if (text_width(vg, size, str) <= max_w) {
    text(vg, t, x, y, size, color, str);
    return;
  }
  std::string s(str ? str : "");
  while (!s.empty() && text_width(vg, size, (s + "…").c_str()) > max_w) {
    // 按 UTF-8 码点回退（中文场景足够；跳过 continuation byte）
    size_t n = s.size();
    do {
      n--;
    } while (n > 0 && (s[n] & 0xC0) == 0x80);
    s.resize(n);
  }
  text(vg, t, x, y, size, color, (s + "…").c_str());
}

void circle_btn(NVGcontext* vg, const Theme& t, float cx, float cy, float d, NVGcolor bg,
                NVGcolor border, NVGcolor icon) {
  (void)t;
  nvgBeginPath(vg);
  nvgCircle(vg, cx, cy, d * 0.5f);
  nvgFillColor(vg, bg);
  nvgFill(vg);
  if (border.a > 0.001f) {
    nvgStrokeWidth(vg, 1.f);
    nvgStrokeColor(vg, border);
    nvgStroke(vg);
  }
  (void)icon;  // 图标由调用方随后用 icon_* 绘制
}

// —— 按钮交互：悬停淡入 + 按压反馈 + 归属锁定式点击 ——
namespace {

// 指数趋近：cur → target，时长 dur_sec；到位后吸附，避免无限小的残差
float approach(float cur, float target, float dt, float dur_sec) {
  if (dur_sec <= 0.0001f) return target;
  float k = dt / dur_sec;
  k = k < 0.f ? 0.f : (k > 1.f ? 1.f : k);
  cur += (target - cur) * k;
  if (std::fabs(target - cur) < 0.003f) cur = target;
  return cur;
}

}  // namespace

BtnState button_hit(ButtonFx& fx, int id, bool inside, bool enabled, bool mouse_down, float dt,
                    const Theme& t) {
  BtnState s;
  s.enabled = enabled;
  if (id < 0 || id >= ButtonFx::kMax) return s;

  const bool down_edge = mouse_down && !fx.was_down;
  const bool up_edge = !mouse_down && fx.was_down;

  // 按下归属：只认"按在按钮热区里"的那一次按下。按在别处（如进度条）不会半路认领 ——
  // 这正是"拖进度条滑过右下角导致误触画中画/全屏"的修复点。
  if (down_edge && enabled && inside && fx.press_id < 0) fx.press_id = id;

  // 禁用态给弱化反馈（上限 0.5），可用态 0→1
  s.hover = approach(fx.hover[id], inside ? (enabled ? 1.f : 0.5f) : 0.f, dt,
                     t.layout.hoverFadeSec);
  fx.hover[id] = s.hover;

  const bool held = inside && enabled && fx.press_id == id;
  s.press = approach(fx.press[id], held ? 1.f : 0.f, dt, t.layout.pressFadeSec);
  fx.press[id] = s.press;

  s.clicked = up_edge && inside && enabled && fx.press_id == id;
  return s;
}

void button_fx_end_frame(ButtonFx& fx, bool mouse_down) {
  fx.was_down = mouse_down;
  if (!mouse_down) fx.press_id = -1;  // 松开即解锁归属
}

bool buttons_busy(const ButtonFx& fx) { return buttons_busy_mask(fx) != 0; }

uint32_t buttons_busy_mask(const ButtonFx& fx) {
  uint32_t m = 0;
  for (int i = 0; i < ButtonFx::kMax; ++i) {
    const float h = fx.hover[i];
    if (h != 0.f && h != 0.5f && h != 1.f) {
      m |= 1u << i;
      continue;
    }
    const float p = fx.press[i];
    if (p != 0.f && p != 1.f) m |= 1u << i;
  }
  return m;
}

float btn_bg(NVGcontext* vg, const Theme& t, float cx, float cy, float d, float box,
             const BtnState& st) {
  const float k = 1.f - (1.f - t.layout.btnPressScale) * st.press;
  // 底色尺寸随按压同步缩小；圆角按比例收，避免小尺寸时圆角过大变"药丸"
  const float bw = box * k;
  const float r = t.layout.btnRadius * k;
  if (st.hover > 0.004f) {
    NVGcolor c = t.btnHoverBg;
    c.a *= st.hover;
    rounded_rect(vg, cx - bw * 0.5f, cy - bw * 0.5f, bw, bw, r, c);
  }
  if (st.press > 0.004f) {
    NVGcolor c = t.btnPressBg;
    c.a *= st.press;
    rounded_rect(vg, cx - bw * 0.5f, cy - bw * 0.5f, bw, bw, r, c);
  }
  (void)d;
  return k;
}

float round_btn_bg(NVGcontext* vg, const Theme& t, float cx, float cy, float d,
                   const BtnState& st) {
  return btn_bg(vg, t, cx, cy, d, t.layout.btnBoxW, st);
}

NVGcolor btn_icon_color(const Theme& t, const BtnState& st) {
  if (!st.enabled) return t.iconDisabled;
  return nvgLerpRGBA(t.iconDim, t.icon, st.hover);
}

// —— 进度条：全部子元素垂直恒为 cy（严格居中，不手算偏移）——
void progress_track(NVGcontext* vg, const Theme& t, float x, float cy, float w, float percent,
                    float buffer_percent, bool hovered_or_drag) {
  const float th = t.layout.trackH;
  // 轨道
  nvgBeginPath(vg);
  nvgRoundedRect(vg, x, cy - th * 0.5f, w, th, t.shape.progressRadius);
  nvgFillColor(vg, t.track);
  nvgFill(vg);
  // 缓冲
  if (buffer_percent > 0) {
    float bw = w * buffer_percent;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, cy - th * 0.5f, bw, th, t.shape.progressRadius);
    nvgFillColor(vg, t.buffer);
    nvgFill(vg);
  }
  // 已播放
  float pw = w * percent;
  if (pw > 0.5f) {
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, cy - th * 0.5f, pw, th, t.shape.progressRadius);
    nvgFillColor(vg, t.played);
    nvgFill(vg);
  }
  // 端点 knob：悬停放大；拖拽出光环；尺寸可被皮肤覆盖
  float kd = t.shape.knobSizeOverride > 0 ? t.shape.knobSizeOverride
                                          : (hovered_or_drag ? t.layout.knobSizeHover : t.layout.knobSize);
  float px = x + w * percent;
  if (hovered_or_drag) {
    nvgBeginPath(vg);
    nvgCircle(vg, px, cy, t.layout.knobHaloDrag * 0.5f);
    nvgFillColor(vg, t.knobHaloDrag);
    nvgFill(vg);
  }
  nvgBeginPath(vg);
  if (t.shape.knobCircle)
    nvgCircle(vg, px, cy, kd * 0.5f);
  else
    nvgRect(vg, px - kd * 0.5f, cy - kd * 0.5f, kd, kd);
  nvgFillColor(vg, t.knob);
  nvgFill(vg);
}

float progress_percent_at(float px, float track_x, float track_w) {
  if (track_w <= 0) return 0;
  float p = (px - track_x) / track_w;
  if (p < 0) p = 0;
  if (p > 1) p = 1;
  return p;
}

void chapter_ticks(NVGcontext* vg, const Theme& t, float x, float cy, float w,
                   const float* percents, int n) {
  for (int i = 0; i < n; i++) {
    if (percents[i] <= 0 || percents[i] >= 1) continue;
    float tx = x + w * percents[i];
    nvgBeginPath(vg);
    nvgRoundedRect(vg, tx - t.layout.chapterTickW * 0.5f, cy - t.layout.chapterTickH * 0.5f,
                   t.layout.chapterTickW, t.layout.chapterTickH, 1);
    nvgFillColor(vg, t.chapterTick);
    nvgFill(vg);
  }
}

void volume_bar(NVGcontext* vg, const Theme& t, float cx, float cy, float w, float percent,
                float knob_r, bool muted) {
  float x = cx - w * 0.5f;
  const float h = t.layout.volH;
  const float r = h * 0.5f;
  // 静音态：轨道与已播放段整体弱化，和"音量真的调到 0"在视觉上区分开
  NVGcolor c_track = muted ? t.muteTrack : t.track;
  NVGcolor c_played = muted ? t.muteKnob : t.played;
  nvgBeginPath(vg);
  nvgRoundedRect(vg, x, cy - r, w, h, r);
  nvgFillColor(vg, c_track);
  nvgFill(vg);
  float pw = w * percent;
  if (pw > 0.5f) {
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, cy - r, pw, h, r);
    nvgFillColor(vg, c_played);
    nvgFill(vg);
  }
  // 滑块：条端点即滑块锚点（top:50% + translate(-50%,-50%)，与进度条同一铁律）
  if (knob_r > 0.1f) {
    const float px = x + w * percent;
    nvgBeginPath(vg);
    nvgCircle(vg, px, cy, knob_r);
    nvgFillColor(vg, muted ? t.muteKnob : t.knob);
    nvgFill(vg);
  }
}

// —— 图标（path 手绘；stroke 用 icon 色，fill 用同色）——
void icon_play(NVGcontext* vg, float cx, float cy, float h, NVGcolor c) {
  float w = h * 0.8f;
  nvgBeginPath(vg);
  nvgMoveTo(vg, cx - w * 0.5f, cy - h * 0.5f);
  nvgLineTo(vg, cx + w * 0.5f, cy);
  nvgLineTo(vg, cx - w * 0.5f, cy + h * 0.5f);
  nvgClosePath(vg);
  nvgFillColor(vg, c);
  nvgFill(vg);
}

void icon_pause(NVGcontext* vg, float cx, float cy, float h, NVGcolor c) {
  float w = h * 0.62f;
  nvgFillColor(vg, c);
  nvgBeginPath(vg);
  nvgRect(vg, cx - w * 0.5f, cy - h * 0.5f, w * 0.34f, h);
  nvgFill(vg);
  nvgBeginPath(vg);
  nvgRect(vg, cx + w * 0.5f - w * 0.34f, cy - h * 0.5f, w * 0.34f, h);
  nvgFill(vg);
}

void icon_prev(NVGcontext* vg, float cx, float cy, float w, NVGcolor c) {
  nvgFillColor(vg, c);
  nvgBeginPath(vg);  // 左竖条
  nvgRect(vg, cx - w * 0.5f, cy - w * 0.42f, w * 0.14f, w * 0.84f);
  nvgFill(vg);
  nvgBeginPath(vg);  // 左向三角
  nvgMoveTo(vg, cx + w * 0.5f, cy - w * 0.42f);
  nvgLineTo(vg, cx - w * 0.32f, cy);
  nvgLineTo(vg, cx + w * 0.5f, cy + w * 0.42f);
  nvgClosePath(vg);
  nvgFill(vg);
}

void icon_next(NVGcontext* vg, float cx, float cy, float w, NVGcolor c) {
  nvgFillColor(vg, c);
  nvgBeginPath(vg);  // 右竖条
  nvgRect(vg, cx + w * 0.36f, cy - w * 0.42f, w * 0.14f, w * 0.84f);
  nvgFill(vg);
  nvgBeginPath(vg);  // 右向三角
  nvgMoveTo(vg, cx - w * 0.5f, cy - w * 0.42f);
  nvgLineTo(vg, cx + w * 0.32f, cy);
  nvgLineTo(vg, cx - w * 0.5f, cy + w * 0.42f);
  nvgClosePath(vg);
  nvgFill(vg);
}

void icon_volume(NVGcontext* vg, float cx, float cy, float w, NVGcolor c) {
  float h = w * 0.85f;
  nvgFillColor(vg, c);
  nvgBeginPath(vg);  // 喇叭主体
  nvgMoveTo(vg, cx - w * 0.5f, cy - h * 0.22f);
  nvgLineTo(vg, cx - w * 0.14f, cy - h * 0.22f);
  nvgLineTo(vg, cx + w * 0.08f, cy - h * 0.5f);
  nvgLineTo(vg, cx + w * 0.08f, cy + h * 0.5f);
  nvgLineTo(vg, cx - w * 0.14f, cy + h * 0.22f);
  nvgLineTo(vg, cx - w * 0.5f, cy + h * 0.22f);
  nvgClosePath(vg);
  nvgFill(vg);
  // 声波弧
  nvgStrokeWidth(vg, 1.3f);
  nvgStrokeColor(vg, c);
  nvgBeginPath(vg);
  nvgArc(vg, cx + w * 0.08f, cy, h * 0.38f, -1.0f, 1.0f, NVG_CW);
  nvgStroke(vg);
  nvgBeginPath(vg);
  nvgArc(vg, cx + w * 0.08f, cy, h * 0.62f, -1.0f, 1.0f, NVG_CW);
  nvgStroke(vg);
}

void icon_volume_muted(NVGcontext* vg, float cx, float cy, float w, NVGcolor c,
                       NVGcolor slash) {
  icon_volume(vg, cx, cy, w, c);
  // 斜线从喇叭左下穿到右上（覆盖声波弧），长度略超图标对角，视觉上"划掉"
  const float h = w * 0.85f;
  const float x0 = cx - w * 0.56f, y0 = cy + h * 0.52f;
  const float x1 = cx + w * 0.6f, y1 = cy - h * 0.52f;
  nvgBeginPath(vg);
  nvgMoveTo(vg, x0, y0);
  nvgLineTo(vg, x1, y1);
  nvgStrokeWidth(vg, 1.6f);
  nvgStrokeColor(vg, slash);
  nvgStroke(vg);
}

void icon_pip(NVGcontext* vg, float cx, float cy, float w, NVGcolor c) {
  float h = w * 0.9f;
  nvgStrokeWidth(vg, 1.3f);
  nvgStrokeColor(vg, c);
  nvgBeginPath(vg);  // 外框
  nvgRoundedRect(vg, cx - w * 0.5f, cy - h * 0.5f, w, h, 1.5f);
  nvgStroke(vg);
  nvgFillColor(vg, c);
  nvgBeginPath(vg);  // 内窗
  nvgRect(vg, cx - w * 0.02f, cy + h * 0.02f, w * 0.36f, h * 0.36f);
  nvgFill(vg);
}

void icon_fullscreen(NVGcontext* vg, float cx, float cy, float w, NVGcolor c) {
  float r = w * 0.5f;
  nvgStrokeWidth(vg, 1.3f);
  nvgStrokeColor(vg, c);
  auto corner = [&](float sx, float sy, float dx, float dy) {
    nvgBeginPath(vg);
    nvgMoveTo(vg, cx + sx * r, cy + sy * (r - w * 0.26f));
    nvgLineTo(vg, cx + sx * r, cy + sy * r);
    nvgLineTo(vg, cx + sx * (r - w * 0.26f), cy + sy * r);
    nvgStroke(vg);
  };
  corner(1, 1, 0, 0);
  corner(1, -1, 0, 0);
  corner(-1, 1, 0, 0);
  corner(-1, -1, 0, 0);
}

void win_button(NVGcontext* vg, const Theme& t, float x, float y, float w, float h, WinBtn kind,
                const BtnState& st, bool active, float rad, float win_w) {
  const bool close = kind == WinBtn::Close;
  // 底色：hover 一档、press 再深一档，都按淡入量乘 alpha。
  // 用逐角圆角矩形：只有"贴着窗口顶部、且自身就是窗口最外侧那个角"的一角带圆角。
  // 直角矩形压在窗口圆角上会溢出圆角边界，透明帧缓冲下于角上露出一块方角。
  const bool touches_tl = x <= 0.5f;
  const bool touches_tr = win_w > 0.f && x + w >= win_w - 0.5f;
  const float r_tl = (rad > 0.5f && touches_tl) ? rad : 0.f;
  const float r_tr = (rad > 0.5f && touches_tr) ? rad : 0.f;
  if (st.hover > 0.004f) {
    NVGcolor c = close ? t.winCloseHoverBg : t.winHoverBg;
    c.a *= st.hover;
    nvgBeginPath(vg);
    nvgRoundedRectVarying(vg, x, y, w, h, r_tl, r_tr, 0.f, 0.f);
    nvgFillColor(vg, c);
    nvgFill(vg);
  }
  if (st.press > 0.004f) {
    NVGcolor c = close ? t.winClosePressBg : t.winPressBg;
    c.a *= st.press;
    nvgBeginPath(vg);
    nvgRoundedRectVarying(vg, x, y, w, h, r_tl, r_tr, 0.f, 0.f);
    nvgFillColor(vg, c);
    nvgFill(vg);
  }
  float cx = x + w * 0.5f, cy = y + h * 0.5f;
  float s = t.layout.winIconSize * (1.f - (1.f - t.layout.btnPressScale) * st.press);
  const float amt = st.hover > st.press ? st.hover : st.press;
  NVGcolor c = close ? nvgLerpRGBA(t.winIcon, t.winCloseHoverIcon, amt) : t.winIcon;
  if (!st.enabled) c = t.iconDisabled;
  nvgStrokeWidth(vg, t.layout.winIconStroke);
  nvgStrokeColor(vg, c);
  switch (kind) {
    case WinBtn::Min:
      nvgBeginPath(vg);
      nvgMoveTo(vg, cx - s * 0.5f, cy);
      nvgLineTo(vg, cx + s * 0.5f, cy);
      nvgStroke(vg);
      break;
    case WinBtn::Max:
      if (active) {
        // 还原样式：前小方 + 后小方（后方的左下边被前方遮挡，画 L 形）
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx - s * 0.15f, cy - s * 0.15f);
        nvgLineTo(vg, cx - s * 0.15f, cy - s * 0.45f);
        nvgLineTo(vg, cx + s * 0.45f, cy - s * 0.45f);
        nvgLineTo(vg, cx + s * 0.45f, cy + s * 0.15f);
        nvgLineTo(vg, cx + s * 0.15f, cy + s * 0.15f);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgRect(vg, cx - s * 0.45f, cy - s * 0.15f, s * 0.6f, s * 0.6f);
        nvgStroke(vg);
      } else {
        nvgBeginPath(vg);
        nvgRect(vg, cx - s * 0.45f, cy - s * 0.45f, s * 0.9f, s * 0.9f);
        nvgStroke(vg);
      }
      break;
    case WinBtn::Close:
      nvgBeginPath(vg);
      nvgMoveTo(vg, cx - s * 0.5f, cy - s * 0.5f);
      nvgLineTo(vg, cx + s * 0.5f, cy + s * 0.5f);
      nvgMoveTo(vg, cx + s * 0.5f, cy - s * 0.5f);
      nvgLineTo(vg, cx - s * 0.5f, cy + s * 0.5f);
      nvgStroke(vg);
      break;
  }
}

}  // namespace fr
