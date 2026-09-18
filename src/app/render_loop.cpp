#include "app/render_loop.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/gl.h>
#include <nanovg.h>

#include "app/event_bus.h"
#include "app/log.h"
#include "platform/gl_lock.h"
#include "platform/interaction.h"
#include "platform/seh_guard.h"
#include "platform/window_glfw.h"
#include "player/player_controller.h"
#include "player/thumb_preview.h"
#include "ui/theme.h"
#include "ui/views/chrome.h"
#include "ui/views/empty_state.h"
#include "ui/views/player_view.h"
#include "ui/widgets.h"

namespace fr {

namespace {
double mono_now() {
  using namespace std::chrono;
  return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

// mpv GL 符号解析（渲染线程持有 GL 上下文，glfwGetProcAddress 本身线程安全）
void* gl_proc(void* /*user*/, const char* name) {
  return (void*)glfwGetProcAddress(name);
}

// —— d125：显卡驱动故障的 SEH 兜底 ——
// 全项目只有**一条**高危路径：mpv 的 GL 渲染链（render_frame → mpv_render_context_render）。
// 老 AMD 驱动（HD 6570 / Catalyst 15.7.1）在编译/执行 mpv 的 NV12→RGB 转换 pass 时会
// 直接抛访问违例（0xC0000005，实测写 NULL+0x10）—— 这是**驱动内部**故障，C++ 的
// try/catch 接不住（它不是 C++ 异常），进程当场退出、日志都来不及落盘：minidump 里
// 5 次崩溃的 ExceptionAddress 逐字节相同（atio6axx.dll+0xD7804C），与 flashrec.exe /
// libmpv-2.dll 都无关。这里用 __try/__except 把这一小段包住：接住 → 上报 → 下一帧
// 自动切软件渲染（d124 的 SW 后端），换成"出画"而不是"闪退"。
struct GlRenderJob {
  PlayerController* player = nullptr;
  unsigned fbo = 0;
  int w = 0;
  int h = 0;
};

// 必须是普通函数指针：__try 所在函数不能有需要析构的 C++ 对象（MSVC C2712）。
void gl_render_job(void* arg) {
  auto* j = static_cast<GlRenderJob*>(arg);
  j->player->render_frame(static_cast<int>(j->fbo), j->w, j->h);
}

// —— d43 按需渲染：变化判定 ——
// d59 起渲染侧的"是否变化"判定整体并入 frame() 的**归因表**（矩形集即判定）：
// 快照/输入的逐字段比较按"影响哪块画面"拆进各归因条目，不再有整份布尔。
// memcmp 有 struct padding 陷阱（两份拷贝 padding 字节可能不同 → 恒判脏），
// 归因条目一律显式字段比较 —— 新增会改变画面的字段必须同步加归因条目。

}  // namespace

// d47：input_changed 从匿名命名空间提升为 fr 自由函数——主循环的按需发布
// 复用同一份字段表（见 render_loop.h 声明处的注释）。
bool input_changed(const ViewInput& a, const ViewInput& b) {
  return a.mx != b.mx || a.my != b.my || a.last_input != b.last_input ||
         a.down != b.down || a.down_middle != b.down_middle ||
         a.kb_progress != b.kb_progress || a.kb_progress_value != b.kb_progress_value ||
         a.kb_preview_until != b.kb_preview_until || a.fullscreen != b.fullscreen ||
         a.maximized != b.maximized || a.win_radius != b.win_radius ||
         a.osd_kind != b.osd_kind || a.osd_value != b.osd_value || a.osd_at != b.osd_at ||
         a.osd_dir != b.osd_dir ||
         a.clip_visible != b.clip_visible || a.clip_at != b.clip_at;  // d74 提示条
}

namespace {

// d51：悬停语义脏判定 —— 鼠标坐标 → 悬停目标位掩码。
// 坐标变化不再直接当重绘理由（4K 下空白处移动 = 画面零变化却整窗 20fps 重绘），
// 升级为「悬停目标变化」：跨控件才出帧。凡是悬停会改变画面的目标各占一位：
//   bit0-2 顶栏三键（全屏不渲染顶栏 → 无热区）  bit3-5 播放/上一集/下一集
//   bit6 进度条  bit7 时间文字  bit8 静音键  bit9 音量条  bit10 画中画
//   bit11 全屏键  bit12 中央大播放键（暂停态）
// *follow 出参 = 「坐标每动一步都要出帧」的白名单：悬停/拖拽进度条时缩略图
// 浮层锚点逐帧跟坐标走。位掩码与绘制几何同源（bar_hit / center_play_hit），
// 禁止两处各算。vg 的文本测量是纯 fontstash 操作（无 GL），跳帧路径调用安全。
// （d59：位 → 矩形的映射在归因表 add_hover_rects，几何取 UiRegions。）
uint32_t hover_key_of(NVGcontext* vg, const Theme& t, float w, float h,
                      const PlaybackSnapshot& snap, const ViewInput& in, bool media,
                      bool pip, bool fullscreen, bool has_picture, bool kb_preview_now,
                      bool* follow) {
  *follow = false;
  if (pip) return 0;  // 画中画小窗没有 chrome，悬停无任何视觉差异
  const auto& L = t.layout;
  uint32_t key = 0;
  if (!fullscreen) {
    for (int i = 0; i < 3; i++) {
      const float bx = w - (float)(3 - i) * L.winBtnW;
      if (in.mx >= bx && in.mx < bx + L.winBtnW && in.my >= 0.f && in.my < L.topBarH)
        key |= 1u << i;
    }
  }
  const BarHit bh = bar_hit(vg, t, w, h, snap, in, media);
  if (bh.play) key |= 1u << 3;
  if (bh.prev) key |= 1u << 4;
  if (bh.next) key |= 1u << 5;
  if (bh.track) key |= 1u << 6;
  if (bh.time) key |= 1u << 7;
  if (bh.mute) key |= 1u << 8;
  if (bh.vol) key |= 1u << 9;
  if (bh.pip) key |= 1u << 10;
  if (bh.fs) key |= 1u << 11;
  if (snap.state == TransportState::PausedPlayback &&
      center_play_hit(t, w, h, in.mx, in.my))
    key |= 1u << 12;
  // d74：剪贴板提示条按钮悬停（几何与 regions 同源 prompt_layout_of；
  // 悬停/按压动画需要逐帧出帧 → 必须进悬停位掩码）
  if (!pip && in.clip_url[0]) {
    const PromptLayout pl =
        prompt_layout_of(vg, t, w, h, fullscreen, in.now, in.clip_at, in.clip_url,
                         in.clip_visible);
    if (pl.visible) {
      const float mx = in.mx, my = in.my;
      if (mx >= pl.btn_play_x && mx < pl.btn_play_x + pl.btn_w && my >= pl.btn_y &&
          my < pl.btn_y + pl.btn_h)
        key |= 1u << 13;
      if (mx >= pl.btn_ignore_x && mx < pl.btn_ignore_x + pl.btn_w && my >= pl.btn_y &&
          my < pl.btn_y + pl.btn_h)
        key |= 1u << 14;
    }
  }
  if (has_picture && snap.duration > 0 &&
      (bh.track || in.drag_progress || kb_preview_now))
    *follow = true;
  return key;
}

// d74：提示条动作按钮（横幅内小圆角钮；悬停/按压用 ButtonFx 缓动值着色）。
// alpha 缓动只能「乘」不能「覆盖」：忽略钮底色 badgeBg 是 9% 白的徽章水洗色，
// 首版用 0.55 地板强抬 alpha → 实心灰药丸 + 灰字直接不可读（用户截图定罪）。
void draw_prompt_btn(NVGcontext* vg, const Theme& t, float x, float y, float w, float h,
                     const char* label, const BtnState& s, bool primary) {
  NVGcolor bg = primary ? t.played : t.badgeBg;
  bg.a = std::min(1.f, bg.a * (1.f + 1.2f * s.hover));
  if (s.press > 0.01f) {
    bg.a *= 1.f - 0.3f * s.press;
  }
  rounded_rect(vg, x, y, w, h, t.layout.promptBtnRadius, bg);
  const NVGcolor tc = primary ? t.topBarBg : t.badgeText;
  text(vg, t, x + w * 0.5f, y + h * 0.5f, t.layout.promptFont, tc, label,
       NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
}

}  // namespace

RenderLoop::RenderLoop(WindowGLFW& window, EventBus& bus, PlayerController& player,
                       ViewStateChannel& channel, const Theme& theme)
    : window_(window), bus_(bus), player_(player), channel_(channel), theme_(theme) {}

RenderLoop::~RenderLoop() { stop(); }

bool RenderLoop::init_gl_resources() {
  // GL 上下文已在本线程 current（run() 开头 make_current 过）
  if (!gladLoadGL((GLADloadfunc)glfwGetProcAddress)) {
    FR_LOG_ERROR("[UI] glad 初始化失败（渲染线程）");
    return false;
  }
  if (!nvg_.init()) {
    FR_LOG_ERROR("[UI] nanovg 初始化失败（渲染线程）");
    return false;
  }
  // mpv render context 需要 current 的 GL 上下文 —— 在渲染线程内初始化
  if (player_.init_render(&gl_proc, nullptr)) {
    render_ready_.store(true, std::memory_order_release);
    FR_LOG_INFO("[UI] mpv render context 就绪（渲染线程）");
  } else {
    FR_LOG_WARN("[UI] mpv render 初始化失败（继续运行，投屏不出画面）");
  }
  return true;
}

bool RenderLoop::start() {
  if (thread_.joinable()) return false;
  thread_ = std::thread([this] { run(); });
  return true;
}

// d94：唤醒 = 条件变量通知（不做任何窗口系统调用）。
// 旧实现是 glfwPostEmptyEvent，它在 Wayland 下等于 wl_display_sync + flush ——
// 又一次从渲染线程触碰主线程正在使用的 wl_display（见 header 的 wait_wake 注释）。
void RenderLoop::wake() {
  {
    std::lock_guard<std::mutex> lk(wake_m_);
    ++wake_seq_;
  }
  wake_cv_.notify_all();
}

// d94：渲染线程的等待 —— 等唤醒或超时，全程不碰 GLFW/Wayland。
void RenderLoop::wait_wake(double seconds) {
  std::unique_lock<std::mutex> lk(wake_m_);
  const uint64_t seen = wake_seq_;
  wake_cv_.wait_for(lk, std::chrono::duration<double>(seconds), [this, seen] {
    return quit_.load(std::memory_order_acquire) || wake_seq_ != seen;
  });
}

// v0.4.0 d56：on_refresh 绑定（main.cpp）。系统要求重绘 = 表面可能已失效，
// 必须整窗补画：force_dirty_ 在归因表里对应 damage.add_all（整窗），见 frame()。
void RenderLoop::request_full_repaint() {
  force_dirty_.store(true, std::memory_order_release);
  wake();
}

void RenderLoop::stop() {
  if (!thread_.joinable()) return;
  quit_.store(true, std::memory_order_release);
  wake();  // 立即唤醒，避免等超时（d94：条件变量通知，原为 post_empty_event）
  thread_.join();
}

void RenderLoop::run() {
  // —— 接管 GL 上下文（GLFW 要求：已在主线程 make null，此处 make current）——
  window_.make_current();
  if (!init_gl_resources()) {
    quit_.store(true, std::memory_order_release);
    return;
  }
  prev_now_ = mono_now();

  while (!quit_.load(std::memory_order_acquire)) {
    // 空闲等唤醒。d47 改自适应：一切改动点（输入回调 / DMR 驱动的发布 / 意图
    // 执行 / ESC 关菜单请求）都会 wake() 即时唤醒，超时只是兜底 ——
    // 出帧后 8ms（动画/播放逐帧推进），跳帧后 60ms（缩略图就绪这类无事件变化
    // 的感知粒度）。此前固定 8ms + 主循环每 8ms post 一次 = 双线程 125Hz 空转
    //（静默 CPU ~7% 的主因）。
    // d94：等待改走自有条件变量 —— 原 glfwWaitEventsTimeout 会让渲染线程驱动
    // Wayland 默认事件队列，与主线程的 wl_display_roundtrip 互抢回包而至死锁
    //（header 的 wait_wake 注释有真机栈证据）。
    wait_wake(next_wait_);
    if (quit_.load(std::memory_order_acquire)) break;
    frame(mono_now());
  }

  // —— 收尾：GL 资源必须在本线程销毁 ——
  if (pic_img_ != 0) {
    nvg_.delete_image(pic_img_);
    pic_img_ = 0;
  }
  if (thumb_img_ != 0) {
    nvg_.delete_image(thumb_img_);
    thumb_img_ = 0;
  }
  nvg_.shutdown();
  window_.release_context();
}

namespace {
// —— v0.4.0 d62 → d146 R5：徽章内容量化与列包络（归因条目与绘制段共用，禁单改一处）——
// 量化粒度与绘制段 snprintf 格式严格对应（文本从量化值派生，杜绝"文字在闪但
// 量化没动"的漏判）。UI 帧率 ≥10 取整（×10+1 标记整数档，避免与一位小数档
// 撞值）、<10 一位小数；视频帧率一位小数；三合一徽章各字段定点/指纹见 BadgeQ。
int media_str_q(const std::string& s) {  // FNV-1a 32bit（空串 = -1 无数据）
  if (s.empty()) return -1;
  uint32_t h = 2166136261u;
  for (const char c : s) {
    h ^= (uint8_t)c;
    h *= 16777619u;
  }
  return (int)(h & 0x7fffffff);
}
BadgeQ badge_quant(bool show_fps, double fps_ema, bool show_vfps, double vfps,
                   bool show_info, const BadgeMedia& m) {
  BadgeQ q;
  if (show_info) {
    q.tio = true;
    q.lines = 1;  // 行1（fps）恒在
    if (vfps > 0) q.vfps_q = (int)std::llround(vfps * 10.0);
    if (m.w > 0) {
      q.lines++;
      q.wh = (m.w << 16) | (m.h & 0xFFFF);
      q.vcodec_q = media_str_q(m.vcodec);
      q.vbr_q = m.vbr > 0 ? (int)std::llround((double)m.vbr / 1e5) : -1;
      q.hw_q = m.hwname.empty() ? 0
                                : (m.hwname == "no"
                                       ? 1
                                       : 2 + (media_str_q(m.hwname) & 0x3fffffff));
    }
    const bool audio =
        !m.acodec.empty() || m.asr > 0 || m.ach > 0 || m.abr > 0;
    if (m.w > 0 && audio) {
      q.lines++;
      q.acodec_q = media_str_q(m.acodec);
      q.asr_q = m.asr > 0 ? (int)std::llround((double)m.asr / 100.0) : -1;
      q.ach_q = m.ach > 0 ? m.ach : -1;
      q.abr_q = m.abr > 0 ? (int)std::llround((double)m.abr / 1000.0) : -1;
    }
  } else {
    if (show_vfps) {
      q.lines++;
      q.vfps_q = vfps > 0 ? (int)std::llround(vfps * 10.0) : -1;
    }
    if (show_fps) {
      q.lines++;
      q.fps_q = fps_ema >= 10.0 ? (int)std::llround(fps_ema) * 10 + 1
                                : (int)std::llround(fps_ema * 10.0);
    }
  }
  return q;
}
// 徽章列包络矩形（起点公式与绘制段同式；行数/形态由 badge_quant 给出——同源禁两算）。
// w = 窗宽。d149 起两种模式同为「每行一条」纵向堆叠：高 = n*(font+2*padY+6) - 6。
DmgRect badge_column_rect(bool fullscreen, float top_bar_h, int win_w, int lines,
                          float badge_font, float badge_pady) {
  if (lines <= 0) return DmgRect{0, 0, 0, 0};
  const float by0 = fullscreen ? 12.f : top_bar_h + 10.f;
  const float bhh = lines * (badge_font + badge_pady * 2 + 6) - 6;
  return DmgRect{0, (int)by0, win_w, (int)bhh + 12};
}
}  // namespace

void RenderLoop::frame(double now) {
  // GL 临界区：macOS 需要与 Cocoa 主线程的 drawable 访问互斥（Win/Linux 空实现）
  FR_GL_GUARD();

  const int w = window_.width();
  const int h = window_.height();
  if (w <= 0 || h <= 0) return;

  // d63：prev_now_ 的推进移到**出帧路径**（闸门放行后）——跳帧（限帧/静默）
  // 不吞时间：跳过的间隔累进到下一出帧帧的 dt，approach 类动画（按钮缓动等）
  // 的时长不被限帧拉长、不变形。
  double dt = now - prev_now_;
  if (dt < 0 || dt > 0.1) dt = 0.1;

  // 输入快照（副本）+ 本帧动画状态
  ViewInput in = channel_.input_snapshot();
  // 渲染线程独占槽里的跨帧状态必须逐帧续接（快照里的同名字段恒为默认值）：
  //   - btns：ButtonFx 时钟状态，漏了 → press_id 每帧归零、clicked 边沿判不出，
  //     控制栏所有按钮点了没反应（d19 修）。
  //   - drag_progress / drag_volume / drag_value：滑条拖拽，漏了 → 拖拽状态每帧清零，
  //     光标一离开细条命中区拖动即中断、单击进度条不落位（d24 修）。
  {
    const ViewInput& rs = channel_.render_state();
    in.btns = rs.btns;
    in.drag_progress = rs.drag_progress;
    in.drag_volume = rs.drag_volume;
    in.drag_value = rs.drag_value;
  }
  in.now = now;
  in.dt = (float)dt;
  in.click_consumed = false;
  in.fullscreen = window_.fullscreen();
  in.maximized = window_.maximized();
  in.win_radius = (in.fullscreen || in.maximized) ? 0.f : theme_.shape.windowRadius;

  // —— d45：右键菜单状态机（必须跑在跳帧判定**之前**：按下/弹起边沿一帧都不能漏，
  //     否则"按下记位、弹起开菜单"的配对会错乱。状态切换时置 force_dirty 保证
  //     开/关的那一帧一定被画出来）——
  if (pip_mode_ && ctx_open_) ctx_open_ = false;  // 进画中画：菜单连带关闭
  if (menu_dismiss_.exchange(false, std::memory_order_acq_rel)) {  // 主线程 ESC 请求
    if (ctx_open_) {
      ctx_open_ = false;
      force_dirty_.store(true, std::memory_order_release);
    } else {
      channel_.post_intent({UiIntent::Kind::Close});  // 菜单没开 = 保持 ESC 原关窗行为
    }
  }
  if (!pip_mode_) {
    if (in.down_right && !right_prev_) {              // 右键按下边沿
      if (ctx_open_) {                                // 菜单开着再按右键 = 关闭
        ctx_open_ = false;
        ctx_right_suppress_ = true;                   // 本次弹起不得重开
        force_dirty_.store(true, std::memory_order_release);
      }
      right_x_ = in.mx;
      right_y_ = in.my;
    } else if (!in.down_right && right_prev_) {       // 右键弹起边沿
      if (!ctx_right_suppress_ && !ctx_open_) {
        const float dx = in.mx - right_x_, dy = in.my - right_y_;
        if (dx * dx + dy * dy <= 36.f) {              // 原地弹起（6px 防抖，d25 同口径）
          ctx_open_ = true;
          ctx_anchor_x_ = right_x_;
          ctx_anchor_y_ = right_y_;
          ctx_press_ = -1;
          force_dirty_.store(true, std::memory_order_release);
        }
      }
      ctx_right_suppress_ = false;
    }
    right_prev_ = in.down_right;
  }

  // d50：快照按版本号读取 —— 版本没变时复用上次内容，跳过 mutex + 3 个字符串
  // 的整份拷贝（事件洪流期每个 wake 都全量拷一遍是 CPU 空转的另一来源）。
  // EventBus 快照唯一写点 publish_player 每次必推版本号，版本不变 = 内容必同。
  PlaybackSnapshot snap;
  {
    const uint64_t sver = bus_.snapshot_version();
    if (sver == snap_ver_seen_) {
      snap = snap_cache_;
    } else {
      snap = bus_.snapshot();
      snap_cache_ = snap;
      snap_ver_seen_ = sver;
    }
  }
  const bool has_picture = snap.picture_ready && !snap.uri.empty();

  // —— d53：视频帧旗标 peek（每 wake 恰好一次，skip/限流路径也不丢）——
  // 播放中出帧率尊重视频帧率：mpv 报新帧才出帧（视频 25fps = 25 次出帧），
  // UI 动画帧（OSD/淡出/悬停）独立按 20fps 插入。取代 d48 的「播放不限 60fps」
  // ——用户实测 720p25 投屏恒 60fps 出帧 GPU 16%+，其中大半是白跑的呈现+合成。
  if (has_picture) {
    // d124：GL 渲染后端被判定失效（老显卡驱动编译不了 mpv 的 GLSL 转换 pass）
    // → 在此切到软件渲染。必须落在渲染线程：mpv_render_context_free 要求与 create
    // 同线程、同 GL 上下文。切换成功即强制重渲一帧，否则画面会一直停在切换前那帧。
    if (player_.switch_video_backend_if_needed()) mpv_frame_pending_ = true;
    mpv_frame_pending_ = mpv_frame_pending_ || player_.video_needs_render();
    // d124：顶栏"软件渲染"徽章的数据源（渲染线程自己的真值，当帧即准）
    in.sw_video = player_.software_video();
  }

  // —— 缩略图预览：取最新结果，seq 变化时经 nvgCreateImageMem 上传（渲染线程独占 GL）——
  bool thumb_new = false;  // d43：新预览帧到达 = 一次重绘理由
  int thumb_img = 0;
  if (thumb_ != nullptr && thumb_->ready()) {
    // d52：seq 廉价前查（原子读）——结果没变就不做 mutex + jpg 全量深拷贝。
    // 前查与 latest() 之间 worker 若又发了更新的代数（tf.seq > tseq），本帧跳过、
    // 下一 wake 用新代数再取 —— seq 单调递增，不会丢帧。
    if (const uint64_t tseq = thumb_->latest_seq(); tseq != 0 && tseq != thumb_seq_) {
      const ThumbFrame tf = thumb_->latest();
      if (tf.seq == tseq && !tf.bytes.empty()) {
        int img = nvg_.create_image_mem(
            const_cast<unsigned char*>(tf.bytes.data()), (int)tf.bytes.size());
        if (img > 0) {
          if (thumb_img_ != 0) nvg_.delete_image(thumb_img_);
          thumb_img_ = img;
          thumb_seq_ = tf.seq;
          thumb_new = true;
        }
      }
    }
    if (!pip_mode_) thumb_img = thumb_img_;
  }

  // —— d43 按需渲染（脏帧）：静默期完全不出帧 ——
  // 背景：主线程每 8ms publish+wake 一次；此前渲染线程醒来就整条 GL 链全跑
  // （mpv 重渲 + nanovg 全场景 + swap + DWM 合成），静默待机恒 ~120fps ——
  // 用户任务管理器实锤核显白吃 15%+ GPU。现在画面与上一帧完全一致就跳过：
  // 不 begin_frame、不 swap，已呈现画面由合成器保持。
  // "活动"判定三路：① 内容变化（输入/快照/窗口尺寸/新缩略图/强制置位）；
  // ② 计划中的动画窗口（都有可计算的静止边界，不能漏 —— 漏一个 = UI 冻结）；
  // ③ 2s 兜底心跳（表面被系统丢弃等异常的自愈重绘）。
  const auto& Lay = theme_.layout;
  // FR_DBG_DAMAGE 级别（1=脏矩形描边+坐标日志，3=额外限频原因日志）；每帧一次 getenv
  const int dbg_lvl_ = getenv("FR_DBG_DAMAGE") ? atoi(getenv("FR_DBG_DAMAGE")) : 0;
  const bool forced = force_dirty_.exchange(false, std::memory_order_acq_rel);
  // d46：视频播放中必须恒出帧——快照 position 只 1s 节流（GENA 位置类），不补这条
  // 投屏画面会变 1fps 幻灯片（诊断旁路掩盖了它：d44 时代徽章开着恒 60fps 从未触发）。
  // 暂停/待机/无画面仍走跳帧。d53 起播放中的出帧驱动由 playing_video 旗标条款
  // 升级为 mpv_frame_pending_（见上）——不只是"恒出帧"，而是"视频帧率出帧"。
  const bool playing_video = has_picture && snap.state == TransportState::Playing;
  // d51：悬停语义脏判定的前置量。d59 起出帧判定整体并入下方归因表（矩形集即判定）：
  // 旧的 scheduled/hover_dirty/input_dirty 布尔被逐条矩形归因取代 —— 布尔说"变了"
  // 而矩形算得出"变在哪里"，后者才是区域重绘要的答案。
  const bool kb_preview_now = in.kb_progress || in.now < in.kb_preview_until;
  bool hover_follow = false;
  const uint32_t hover_key =
      hover_key_of(nvg_.ctx(), theme_, (float)w, (float)h, snap, in, snap.has_session,
                   pip_mode_, in.fullscreen, has_picture, kb_preview_now, &hover_follow);

  // —— v0.4.0 d68：统一指针三态机 + 四事件三段式单/双击（UI_INTERACTION_PLAN §2/§3）——
  // 位置铁律：必须跑在跳帧闸门**之前** —— 按下/弹起/移动一帧都不能漏，否则拖动
  // 防抖与双击窗口的判定错乱（同 d45 右键菜单状态机的前置理由；跳帧路径提前
  // return，不含绘制段）。区域判定用 hover_key 位掩码 + 顶栏纯几何，不依赖
  // bar_hit（其结果只在绘制段可算，且顶栏几何禁两处各算）。
  // 阈值口径：本应用 UI 坐标 = 未缩放 framebuffer 物理像素（无 DPI 缩放渲染），
  // 与 GetSystemMetrics 同口径 → scale 恒传 1（接口保留 scale 给未来缩放渲染）。
  {
    const InteractionThresholds thr = interaction_thresholds(1.f);
    if (pip_mode_ || ctx_open_) {
      // 画中画/右键菜单接管期间：打断进行中的按压（各自语义自理，见 d25/d45）
      if (ptr_state_ != PtrState::Idle) {
        ptr_state_ = PtrState::Idle;
        in_dbl_ = false;
      }
      pend_single_ = false;  // 接管态不补发挂起单击（语义已移交 d25/d45）
      up1_at_ = 0;
    } else {
      // d78：播放/全屏态的舞台单击挂起到期检查（先于边沿处理：挂起窗一到
      // 即消费挂起并武装清零，边界帧不会再被判成 DOWN2）。
      if (pend_single_ && up1_at_ > 0 && now - up1_at_ >= thr.stage_dbl_pending_sec) {
        pend_single_ = false;
        up1_at_ = 0;  // 消费即清：同刻落下的第二击按新单击记账
        channel_.post_intent({UiIntent::Kind::PlayPause});
      }
      const bool down_edge = in.down && !down_prev_;
      const bool up_edge = !in.down && down_prev_;
      if (down_edge) {
        // 按下归类（弹起沿用）：顶栏空白（纯几何）→ Widget（任一悬停位）→ Stage。
        // 顶栏三键落在 hover bit0-2 → Widget；全屏态无顶栏（chrome 不渲染）。
        const bool top_bar = !in.fullscreen && in.my < Lay.topBarH &&
                             in.mx < (float)w - 3.f * Lay.winBtnW;
        ptr_region_ = top_bar ? PtrRegion::TopBar
                              : (hover_key != 0 ? PtrRegion::Widget : PtrRegion::Stage);
        ptr_state_ = PtrState::Pressed;
        in_dbl_ = false;
        press_x_ = in.mx;
        press_y_ = in.my;
        // 三段式②：本按下若落在双击窗口内（UP1→DOWN2 ≤ 窗口 且 ≤ dblRect）→
        // 双击在 DOWN2 立即成立（Windows 口径 WM_LBUTTONDBLCLK 在第二击按下发出）。
        // 双击时长分区域：视频空白 150ms 用户定值，其余读系统 GetDoubleClickTime；
        // 播放/全屏态放宽（d80 用户定值 200ms，首版 300ms 手感偏钝——看片双击
        // 节奏偏慢，150ms 容易
        // 被拆成两次单击 = 连续 PlayPause「干扰」）。
        if (ptr_region_ != PtrRegion::Widget && up1_at_ > 0) {
          const double win_sec =
              ptr_region_ == PtrRegion::Stage
                  ? (in.fullscreen || snap.state == TransportState::Playing
                         ? thr.stage_dbl_pending_sec
                         : thr.stage_dbl_time_sec)
                  : thr.dbl_time_sec;
          if (now - up1_at_ <= win_sec &&
              std::fabs(in.mx - up1_x_) <= thr.dbl_rect_px &&
              std::fabs(in.my - up1_y_) <= thr.dbl_rect_px) {
            in_dbl_ = true;
            up1_at_ = 0;  // 三击不连判：双击成立后下一轮从头计（测试矩阵 §9）
            pend_single_ = false;  // 双击成立 = 取消挂起单击（否则全屏夹带暂停）
            if (ptr_region_ == PtrRegion::Stage)
              channel_.post_intent({UiIntent::Kind::ToggleFullscreen});
            else
              channel_.post_intent({UiIntent::Kind::ToggleMaximize});
          }
        }
      } else if (ptr_state_ != PtrState::Idle) {
        // PRESSED 推进：|Δ|>kDragPx 一次性转 DRAGGED（防抖，§2）。
        // 拖动指令恒产生、消费与否由主线程按区域决定（需求 #3「指令必须被识别」）。
        if (ptr_state_ == PtrState::Pressed &&
            (std::fabs(in.mx - press_x_) > thr.drag_px ||
             std::fabs(in.my - press_y_) > thr.drag_px)) {
          ptr_state_ = PtrState::Dragged;
          const UiIntent::DragRegion rg = ptr_region_ == PtrRegion::TopBar
                                              ? UiIntent::DragRegion::TopBar
                                              : ptr_region_ == PtrRegion::Widget
                                                    ? UiIntent::DragRegion::Widget
                                                    : UiIntent::DragRegion::Stage;
          channel_.post_intent({UiIntent::Kind::DragMove, 0, false, rg});
          if (ptr_region_ == PtrRegion::Widget) {
            // 按钮拖出 = 取消按压归属：按压视觉淡出、抬起不再可能误触
            //（关闭键按下→反悔拖出→拖回→弹起 = 不再误关，需求 #3）。
            // 直接写渲染线程独占槽：本帧即使被跳帧也不丢（跳帧路径不存回铁律）。
            channel_.render_state().btns.press_id = -1;
            in.btns.press_id = -1;
          }
        }
        if (up_edge) {
          if (ptr_state_ == PtrState::Pressed && !in_dbl_) {
            if (ptr_region_ == PtrRegion::Stage) {
              // 三段式①：单击触发点 = UP1。窗口态立即触发（d69：废除全局
              // 延迟确认）；播放/全屏态挂起待消歧（d78，窗宽 d80 定 200ms）——立即发会
              // 在双击成立时夹带一次 PlayPause（全屏切换必然伴随暂停=干扰）。
              if (in.fullscreen || snap.state == TransportState::Playing) {
                pend_single_ = true;  // up1_at_ 下方记账即挂起截止基准
              } else {
                channel_.post_intent({UiIntent::Kind::PlayPause});
              }
            }
            // 三段式②记账（Widget 区不记：按钮无双击语义，§4 Windows 惯例）
            up1_at_ = now;
            up1_x_ = in.mx;
            up1_y_ = in.my;
          }
          ptr_state_ = PtrState::Idle;
          in_dbl_ = false;
        }
      }
    }
    down_prev_ = in.down;
  }

  // —— v0.4.0 d59：完整归因表（P1+P2，区域重绘真正生效）——
  // 两类条目（REGION_REDRAW_PLAN.md §6）：
  //   持续动画：窗口活动期间每帧 invalidate 对应区域（fade 窗口 / OSD 生命周期 /
  //     缩略图浮层 / 按钮缓动槽位 / 诊断徽章列）；
  //   瞬时事件：旧∪新矩形（悬停差集 / state 切换 / OSD 与浮层出现·消失 / 按下边沿）。
  // 几何唯一来源 = UiRegions：本帧 R 与上帧 last_regions_ 逐字段差集归因。
  // 不可区域化的条目保持整窗：视频整窗底层（mpv 帧）/ 窗口形态 / 媒体信息类快照 /
  // on_refresh 与 set_pip（forced）/ 首帧 / 2s 心跳自愈（勿动）。
  // 安全阀：并集 bbox > 60% 窗口面积升整窗（免碎片矩形，绘制成本近似整窗）。
  // ⚠️ 新增会改变画面的状态/字段必须在此表加归因条目（延续 d43/d51 铁律）；
  //    漏归因 = 残影，由 2s 心跳自愈 + FR_DBG_DAMAGE=2 差分校验器兜底。
  const float fade_now =
      pip_mode_ ? 1.f : chrome_fade(nvg_.ctx(), theme_, (float)w, (float)h, snap, in,
                                    snap.has_session);
  UiRegions R = compute_regions(nvg_.ctx(), theme_, (float)w, (float)h, snap, in,
                                snap.has_session, pip_mode_, fade_now, thumb_ != nullptr);
  uint32_t dmg_reasons = 0;
  Damage dmg;
  // 脏区加入：外扩 2px（反锯齿/圆角溢出，§11）+ 窗口内钳制（Damage.add 不裁越界）
  auto dmg_add = [&](const DmgRect& r) {
    if (r.w <= 0 || r.h <= 0) return;
    const int x0 = r.x - 2 < 0 ? 0 : r.x - 2;
    const int y0 = r.y - 2 < 0 ? 0 : r.y - 2;
    const int x1 = r.x + r.w + 2 > w ? w : r.x + r.w + 2;
    const int y1 = r.y + r.h + 2 > h ? h : r.y + r.h + 2;
    if (x1 > x0 && y1 > y0) dmg.add(x0, y0, x1 - x0, y1 - y0);
  };
  auto dmg_all = [&](uint32_t bit) {
    dmg.add_all(w, h);
    dmg_reasons |= bit;
  };
  // 悬停位 → 控件矩形（与 hover_key_of 位定义一一对应，几何取自 UiRegions）
  auto add_hover_rects = [&](uint32_t key, const UiRegions& Rg, bool center_vis) {
    for (int b = 0; b <= 14; b++) {
      if (!(key & (1u << b))) continue;
      switch (b) {
        case 0: case 1: case 2: dmg_add(Rg.win_btn[b]); break;
        case 3: dmg_add(Rg.bar_btn[kBtnPlay]); break;
        case 4: dmg_add(Rg.bar_btn[kBtnPrev]); break;
        case 5: dmg_add(Rg.bar_btn[kBtnNext]); break;
        case 6: dmg_add(Rg.track); break;
        case 7: dmg_add(Rg.time_cur); dmg_add(Rg.time_left); break;
        case 8: dmg_add(Rg.bar_btn[kBtnMute]); break;
        case 9: dmg_add(Rg.volume); break;
        case 10: dmg_add(Rg.bar_btn[kBtnPip]); break;
        case 11: dmg_add(Rg.bar_btn[kBtnFullscreen]); break;
        case 12: if (center_vis) dmg_add(Rg.center_play); break;
        case 13: case 14: dmg_add(Rg.prompt_bar); break;  // d74 提示条两按钮
        default: break;
      }
    }
  };

  // —— 整窗条目 ——
  if (!ever_drawn_) dmg_all(1u << 0);                  // 首帧
  if (thumb_new && R.thumb_visible) {                  // 新缩略图帧（浮层内容更新）
    dmg_add(R.thumb_card);
    dmg_reasons |= 1u << 1;
  }
  if (forced) dmg_all(1u << 2);                        // set_pip/名字/on_refresh/菜单开关
  if (w != last_w_ || h != last_h_) dmg_all(1u << 3);  // 尺寸
  if (mpv_frame_pending_) dmg_all(1u << 7);            // 视频新帧（整窗底层，不可区域化）
  if (now - last_keepalive_ >= 2.0) dmg_all(1u << 9);  // 2s 心跳（自愈通道，勿动）

  // —— 瞬时：窗口形态（全屏/最大化/圆角）——
  if (in.fullscreen != last_in_key_.fullscreen || in.maximized != last_in_key_.maximized ||
      in.win_radius != last_in_key_.win_radius)
    dmg_all(1u << 4);
  // —— 瞬时：媒体信息类快照（模式切换级，低频、保守整窗）——
  if (snap.uri != last_snap_.uri || snap.metadata != last_snap_.metadata ||
      snap.title != last_snap_.title || snap.next_uri != last_snap_.next_uri ||
      snap.next_metadata != last_snap_.next_metadata ||
      snap.has_session != last_snap_.has_session ||
      snap.picture_ready != last_snap_.picture_ready ||
      snap.stopped_at != last_snap_.stopped_at ||
      snap.is_live != last_snap_.is_live || snap.seekable != last_snap_.seekable)
    dmg_all(1u << 5);

  // —— 瞬时：悬停差集（旧∪新悬停目标的控件矩形）——
  if (hover_key != last_hover_key_) {
    add_hover_rects(last_hover_key_, last_regions_, last_regions_.center_visible);
    add_hover_rects(hover_key, R, R.center_visible);
    dmg_reasons |= 1u << 4;
  }
  // —— 瞬时：按下/抬起边沿（按压视觉：底色加深/图标缩小/音量滑块放大）——
  if (in.down != last_in_key_.down) {
    add_hover_rects(hover_key, R, R.center_visible);
    dmg_reasons |= 1u << 4;
  }
  // —— 瞬时：拖拽开始/结束边沿（knob 与条样式切换、seek 提交后条回真实位置）——
  if (in.drag_progress != last_in_key_.drag_progress) {
    dmg_add(R.track); dmg_add(R.time_cur); dmg_add(R.time_left); dmg_add(R.mini_line);
    dmg_reasons |= 1u << 4;
  }
  if (in.drag_volume != last_in_key_.drag_volume) {
    dmg_add(R.volume);
    dmg_reasons |= 1u << 4;
  }
  // —— 持续：拖拽/键盘预览进行中（每帧 invalidate 跟随区域）——
  if (in.drag_progress || in.drag_volume || kb_preview_now) {
    if (in.drag_progress || kb_preview_now) {
      dmg_add(R.track); dmg_add(R.time_cur); dmg_add(R.time_left); dmg_add(R.mini_line);
    }
    if (in.drag_volume) { dmg_add(R.volume); dmg_add(R.bar_btn[kBtnMute]); }
    dmg_reasons |= 1u << 4;
  }
  // —— 瞬时：快照过程字段（播放进度/音量/播放态——高频路径的精确化主力）——
  if (snap.state != last_snap_.state) {
    dmg_add(last_regions_.center_play);  // 中央键出现/消失：旧∪新
    dmg_add(R.center_play);
    dmg_add(R.bar_btn[kBtnPlay]);        // 播放键图标 播放↔暂停
    dmg_reasons |= 1u << 5;
  }
  if (snap.position != last_snap_.position || snap.duration != last_snap_.duration) {
    dmg_add(R.track); dmg_add(R.time_cur); dmg_add(R.time_left); dmg_add(R.mini_line);
    dmg_reasons |= 1u << 5;
  }
  if (snap.volume != last_snap_.volume || snap.muted != last_snap_.muted) {
    dmg_add(R.volume); dmg_add(R.bar_btn[kBtnMute]);
    dmg_reasons |= 1u << 5;
  }
  // —— 持续：面板淡入淡出窗口（底栏带 + 迷你线带；顶栏常显/中央键常显不参与）——
  if (!pip_mode_ && has_picture &&
      now - in.last_input < Lay.idleHideSec + Lay.fadeSec + 0.15) {
    dmg_add(R.bottom_bar);
    dmg_add(R.mini_line);
    dmg_reasons |= 1u << 6;
  }
  // —— 持续：OSD 生命周期（出现→保持→淡出，纯时间推算）——
  if (R.osd_visible && now - in.osd_at < Lay.osdFadeIn + Lay.osdHold + Lay.osdFadeOut + 0.05) {
    dmg_add(R.osd);
    dmg_reasons |= 1u << 6;
  }
  // —— 瞬时：OSD 出现/消失（kind 归零帧生命周期条目已失效，靠可见性差集兜住）——
  if (R.osd_visible != last_regions_.osd_visible) {
    dmg_add(last_regions_.osd);
    dmg_add(R.osd);
    dmg_reasons |= 1u << 6;
  }
  // —— d74 提示条：出现/消失（旧∪新横幅矩形）+ 淡入淡出窗口（静止边界可算，
  //     与 OSD 同理；停留中段只有按钮缓动 → bit8，无整条持续开销）——
  if (R.prompt_visible != last_regions_.prompt_visible) {
    dmg_add(last_regions_.prompt_bar);
    dmg_add(R.prompt_bar);
    dmg_reasons |= 1u << 11;
  }
  if (R.prompt_visible) {
    const double age = now - in.clip_at;
    if (age < Lay.promptFadeSec || age >= Lay.promptHoldSec) {
      dmg_add(R.prompt_bar);
      dmg_reasons |= 1u << 11;
    }
  }
  // —— 持续：键盘预览保留期（迷你线 kb 值回真实位置的窗口）——
  if (!pip_mode_ && now < in.kb_preview_until + 0.05) {
    dmg_add(R.mini_line);
    dmg_reasons |= 1u << 6;
  }
  // —— 缩略图浮层：可见性/位置差集（跟随移动 = y 每帧不同 → 自动持续 invalidate）——
  if (R.thumb_visible != last_regions_.thumb_visible ||
      R.thumb_card.y != last_regions_.thumb_card.y) {
    dmg_add(last_regions_.thumb_card);
    dmg_add(R.thumb_card);
    dmg_reasons |= 1u << 1;
  }
  // —— 持续：按钮缓动（busy 槽 → 控件矩形；pip 不跑 button_hit，已门控）——
  // d54 语义保持：hover/press 处于中间态就出帧，动画驱动到收敛才静默。
  const uint32_t busy_mask = pip_mode_ ? 0u : buttons_busy_mask(in.btns);
  if (busy_mask) {
    for (int b = 0; b < ButtonFx::kMax; b++) {
      if (!(busy_mask & (1u << b))) continue;
      if (b <= kBtnFullscreen) dmg_add(R.bar_btn[b]);
      else if (b <= kBtnWinClose) dmg_add(R.win_btn[b - kBtnWinMin]);
      else if (b == kBtnCenterPlay) dmg_add(R.center_play);
      else dmg_add(R.prompt_bar);  // d74：kBtnClipPlay/kBtnClipIgnore（横幅内两按钮）
    }
    dmg_reasons |= 1u << 8;
  }
  // —— 持续：右键菜单开着（hover 高亮/按下反馈都在面板内；几何固定，用上帧存档）——
  if (ctx_open_) {
    dmg_add(last_regions_.ctx_menu);
    dmg_reasons |= 1u << 4;
  }
  // —— 瞬时：诊断徽章内容变化（d62 重构）——
  // 原「持续」条目只要徽章开着就每帧 invalidate 徽章列 → 开帧率显示 = 恒
  // 60fps 自举：徽章自己制造自己显示的帧率，静默读数永不回落（用户实测抓到，
  // 也是 d58 决议"最后限帧"前必须先除掉的验证污染源）。改为内容变化驱动：
  // 量化值对「已绘制基准」比较，变了才 invalidate；基准只在出帧路径推进，
  // 且要求徽章列实际被画到（bbox 覆盖）——否则基准动了屏幕没画 = 停更。
  if (!pip_mode_ && (show_info_ || show_fps_ || show_video_fps_)) {
    const BadgeQ q =
        badge_quant(show_fps_, fps_ema_, show_video_fps_, media_.vfps, show_info_, media_);
    if (!q.eq(badge_q_drawn_)) {
      // 新旧包络并集：行数变化（音频行出现/消失、徽章开关）时旧胶囊区域也失效
      dmg_add(badge_column_rect(in.fullscreen, Lay.topBarH, w, badge_q_drawn_.lines,
                                Lay.badgeFont, Lay.badgePadY));
      dmg_add(badge_column_rect(in.fullscreen, Lay.topBarH, w, q.lines,
                                Lay.badgeFont, Lay.badgePadY));
      dmg_reasons |= 1u << 10;
    }
  }
  // —— 安全阀：并集 bbox > 60% 窗口面积 → 升整窗 ——
  if (!dmg.empty() && !dmg.full()) {
    const DmgRect bb = damage_bbox(dmg);
    if ((long long)bb.w * bb.h * 5 >= (long long)w * h * 3) {
      dmg.clear();
      dmg_all(1u << 11);
    }
  }

  if (dmg.empty()) {
    // 跳帧：render_state 槽未推进，无需存回（帧末存回只在出帧路径）。
    // d47：跳帧后放宽下一轮等待（无事件变化靠轮询感知：缩略图就绪等）；
    // 有事件的变化都会 post 即时唤醒，不走这个超时。
    // d53：播放中保持 8ms 短轮询 —— 旗标 peek 的感知粒度就是视频帧的到帧延迟
    //（25fps 帧距 40ms，若放宽到 60ms 下一帧最多晚 20ms，会抖）。
    next_wait_ = playing_video ? 0.008 : 0.06;
    return;
  }
  // —— d63 动画限帧：纯动画归因帧限到 30fps，事件帧永不限流 ——
  // 依据（d58 决议的收尾步）：按钮悬停期 GPU ~10% 的根源不是绘制面积（0.7%
  // 窗口），而是「每帧固定税（blit + swap + DWM 合成）× 帧率」。动画类帧的
  // 内容由时间连续推进，60Hz 采样是浪费；事件类（悬停/按下/拖拽边沿、快照
  // 字段、视频帧、缩略图、尺寸、强制、心跳、首帧、徽章内容变化）响应零延迟
  // 永不限流；混合归因（动画+事件同帧）按事件帧放行。
  // 跳帧不存回任何基准：动画源连续，下轮归因会重新触发同条目，跳过的间隔由
  // 下一出帧帧的大 dt 一次补齐（prev_now_ 只在本闸门放行后推进）——动画时长
  // 不变形。d62 的徽章内容变化（bit10）按事件对待：EMA 衰减的读数刷新不被限流。
  static constexpr uint32_t kAnimBits = (1u << 6) | (1u << 8);  // sched + btns
  if ((dmg_reasons & ~kAnimBits) == 0) {
    if (now - last_anim_draw_ < 1.0 / 30.0) {
      next_wait_ = 1.0 / 30.0 - (now - last_anim_draw_);
      if (next_wait_ < 0.008) next_wait_ = 0.008;
      return;
    }
    last_anim_draw_ = now;
  }
  // —— 出帧闸门放行：从这里起本帧一定画，时间基准在此推进 ——
  prev_now_ = now;
  // d54：放开 20fps 上限（用户裁决：d48 时代 60fps 的顾虑是「触发点多、占用高
  // 且持续」，根因已被 d51 悬停语义 + d53 旗标驱动消解 —— 出帧已全部按需，
  // 动画期占用高点只是事件响应）。min_gap/rate_ok 门控整体移除：动画帧由
  // vsync 自然限到 60Hz，静态期（面板可见但画面不变的 scheduled 段）出相同帧
  // 的代价用户已知情接受。
  // 出帧路径：播放中 8ms 短轮询（等下一个视频帧旗标，帧距感知 ≤8ms）；
  // 非播放 16ms —— scheduled/OSD/按钮缓动窗口内的动画采样密度（vsync 兜底）。
  next_wait_ = playing_video ? 0.008 : 0.016;

  // FR_DBG_DAMAGE=3：出帧原因（位标记）+ 出帧间隔，限频 1s（§10 度量项）。
  // 用途：定位"谁在触发渲染"——forced 高频=refresh 洪流、input=鼠标/悬停、
  // snap=DMR 事件洪流、sched=动画窗口。正常静默期此日志应几乎不出现。
  if (dbg_lvl_ >= 3 && now - last_dmg_log_ >= 1.0) {
    static constexpr const char* kNames[12] = {"first",  "thumb", "forced",   "size",
                                               "input",  "snap",  "sched",    "mpv",
                                               "btns",   "keepalive", "badges", "safety"};
    std::string rs;
    for (int i = 0; i < 12; i++)
      if (dmg_reasons & (1u << i)) (rs += kNames[i]) += ' ';
    FR_LOG_INFO("[DMG3] reasons=[{}] dt={:.3f}s", rs, now - last_draw_at_);
    if (dmg_reasons & (1u << 8)) {
      // 按钮级细查：打出每个非收敛槽的 id 与值（d57 修复定位）
      static constexpr const char* kBtn[ButtonFx::kMax] = {
          "play", "prev", "next", "mute", "pip",  "fs",
          "winMin", "winMax", "winClose", "center", "b10", "b11", "b12", "b13", "b14", "b15"};
      for (int i = 0; i < ButtonFx::kMax; i++) {
        const float h = in.btns.hover[i];
        const float p = in.btns.press[i];
        if (h != 0.f && h != 0.5f && h != 1.f)
          FR_LOG_INFO("[DMG3] busy btn={} hover={:.4f}", kBtn[i], h);
        if (p != 0.f && p != 1.f)
          FR_LOG_INFO("[DMG3] busy btn={} press={:.4f}", kBtn[i], p);
      }
    }
    last_dmg_log_ = now;
  }

  // 是否处于"用户拖拽调整大小"中：期间冻结 FBO 重建与 mpv 重渲，
  // 否则放大时每帧 glTexImage2D + 跑一遍视频管线，是拖拽卡顿的主因。
  const bool live_resize = window_.interactive_resize();
  // 拖动中不重建 FBO（用现有内容拉伸顶上），拖完再对齐精确尺寸
  const bool allow_fbo_resize = !live_resize;
  const bool size_changed = w != fbo_.width() || h != fbo_.height();

  // —— ① 视频路径：mpv → FBO（仅画面就绪时）——
  bool have_src = false;
  if (has_picture) {
    // 只在真正重渲时推进 FBO 内容 —— d50 起按 mpv 更新旗标门控（视频层的
    // "区域重绘"）：mpv 报告有新帧（播放中的新画面、seek/暂停切换后的首帧）
    // 或 FBO 刚重建（内容为空）才跑完整视频渲染链；否则 FBO 里仍是上一帧的
    // 有效画面，UI 重绘直接复用该纹理 —— 暂停时操作 UI / 播放中的纯 UI 帧
    // 不再白跑 mpv 渲染（此前每一帧 UI 重绘都无条件重渲一遍视频）。
    // d53：旗标在 frame() 开头 peek 一次累积进 mpv_frame_pending_（skip/限流
    // 也不丢），这里只读成员、不再调 video_needs_render()（二次调用会消费
    // 下一帧的旗标）。
    // 拖动中始终沿用上一帧内容（拉伸显示）。
    bool fbo_fresh = false;
    if (allow_fbo_resize && size_changed) {
      if (!fbo_.ensure(w, h)) return;
      fbo_fresh = true;
    } else if (fbo_.fbo() == 0) {
      // 拖动中首次也要保证有可用 FBO（否则没内容可拉伸）
      if (!fbo_.ensure(w, h)) return;
      fbo_fresh = true;
    }
    const bool rerender = !live_resize && (fbo_fresh || mpv_frame_pending_);
    if (rerender) {
      if (player_.software_video()) {
        // d124 软件兜底路径：mpv 在 CPU 侧出 RGBA 缓冲（不碰任何着色器），
        // 由这里直接上传到视频 FBO 的纹理 —— FBO 的内容即其颜色附件，
        // 后续 nvgImagePattern 贴图 / 合成链与 GL 路径完全一致，无需改动。
        const int vw = fbo_.alloc_width(), vh = fbo_.alloc_height();
        fbo_.bind();
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);  // 上传失败/尺寸不匹配时留透明，避免残留脏内容
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (player_.render_sw_frame(vw, vh)) {
          const unsigned char* px = player_.sw_pixels();
          if (px != nullptr) {
            // 逐行 = w*4 字节，天然 4 字节对齐，无需动 UNPACK_ALIGNMENT
            glBindTexture(GL_TEXTURE_2D, fbo_.texture());
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, vw, vh, GL_RGBA, GL_UNSIGNED_BYTE, px);
          }
        }
      } else {
        fbo_.bind();
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        // mpv 的 fbo.w/h 语义是「FBO **容量**尺寸」而非内容尺寸
        // （render_gl.h: "This must refer to the size of the framebuffer. This must always be set."）。
        // 容量恒等于内容（framebuffer.h：尺寸变化即精确重建，无棘轮），传容量即传内容。
        // 注意 mpv 只按 fbo.w/h 定宽高比、不负责居中摆放，居中是"把整张纹理映射到整窗"实现的。
        // d125：整条调用包在 SEH guard 里 —— 这是驱动故障的唯一高危入口，接不住就闪退。
        GlRenderJob job;
        job.player = &player_;
        job.fbo = fbo_.fbo();
        job.w = fbo_.alloc_width();
        job.h = fbo_.alloc_height();
        platform::SehFault fault;
        if (!platform::sehCall(&gl_render_job, &job, &fault)) {
          glBindFramebuffer(GL_FRAMEBUFFER, 0);
          // 上报 → 下一帧帧首的 switch_video_backend_if_needed() 会切到 SW 后端
          player_.report_gl_crash();
          mpv_frame_pending_ = true;  // 逼下一帧重走渲染链（这次走 SW 分支）
          next_wait_ = 0.008;         // 且尽快回来，不要等跳帧的 60ms 兜底间隔
          FR_LOG_ERROR("[MPV] GL 渲染链抛驱动异常已被 SEH 接住 code=0x{:X} addr={} → 降级软件渲染",
                       static_cast<unsigned long>(fault.code), fault.address);
          // 本帧就此作罢：驱动内部状态未知，不再往下走 nvg / blit / swap，
          // 让窗口停留在上一帧画面（这几处早期 return 是本函数既有模式）。
          return;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
      }
      mpv_frame_pending_ = false;  // d53：真正渲完才清（skip/限流路径不清，视频帧不丢）

      // 尺寸观测（opt-in）：设 FR_DBG_SIZE=1 时，在"三套尺寸任一变化"的那一帧打印。
      // 比"只打前 N 帧"有用：能直接看到窗口缩放时 win / fbo 是否跟着走。
      // 排查"画面不随窗口缩放 / 全屏尺寸不对 / 穿窗 / 镜像"时的第一手证据 —— 正常状态静默。
      {
        static int last_w = -1, last_h = -1, last_cw = -1, last_ch = -1, last_aw = -1, last_ah = -1;
        if (getenv("FR_DBG_SIZE")) {
          const int cw = fbo_.width(), ch = fbo_.height();
          const int aw = fbo_.alloc_width(), ah = fbo_.alloc_height();
          if (w != last_w || h != last_h || cw != last_cw || ch != last_ch || aw != last_aw ||
              ah != last_ah) {
            last_w = w; last_h = h; last_cw = cw; last_ch = ch; last_aw = aw; last_ah = ah;
            FR_LOG_INFO("[SIZE] win={}x{} fbo_content={}x{} fbo_alloc={}x{}", w, h, cw, ch, aw, ah);
          }
        }
      }

      // 注册为 nanovg 图像：nvgImagePattern 收 nanovg 句柄，不是裸 GL 纹理名
      // （传错会采样空纹理 → 画面区域全透明），w/h 必须是**纹理容量**。
      // flags 传 0：**不加任何翻转标志**。mpv 渲进 FBO 的朝向已经与 nanovg 的
      // 采样约定一致，加 NVG_IMAGE_FLIPY 反而会造成上下镜像（d17/d18 实测）。
      if (pic_img_ == 0 || fbo_.texture() != pic_tex_ || fbo_.alloc_width() != pic_w_ ||
          fbo_.alloc_height() != pic_h_) {
        if (pic_img_ != 0) nvg_.delete_image(pic_img_);
        pic_img_ = nvg_.create_image_from_handle(fbo_.texture(), fbo_.alloc_width(),
                                                fbo_.alloc_height(), 0);
        pic_tex_ = fbo_.texture();
        pic_w_ = fbo_.alloc_width();
        pic_h_ = fbo_.alloc_height();
      }
    }
    have_src = pic_img_ != 0;
  }

  // —— ② UI 路径：合成到 app_fbo_（d60：窗口内容唯一真值）——
  // v0.4.0 d56：full = 整窗路径（clip=nullptr，行为同 v0.3.1）；
  // partial = 区域路径：脏区并集 bbox（P0/P1 单矩形，见 damage.h damage_bbox）
  // → GL scissor（物理像素）+ nvgScissor（逻辑像素）双裁剪 → **只清脏区**（半透明
  // 栏不幂等，必须先清到透明再重画整栈，规划 §4.2）→ 全图层带 clip 重画。
  // nanovg BeginFrame 的 viewport 与 GL 状态不冲突（其实现从不调 glScissor，已核）。
  // d60：绘制目标从默认帧缓冲换成 app_fbo_ —— 区域帧的非脏区内容自持（FBO 是
  // 我们自己的纹理，内容永远保留），不再依赖 back buffer 残留（GL 规范未定义、
  // 驱动 flip 轮转下残留可能是旧帧 → 实测偶发闪烁）。帧末整体 blit 到默认帧
  // 缓冲再 swap（见帧末提交段）。ensure 无条件调用：live_resize 中也随窗口精确
  // 重建（该帧必为 full，全画覆盖新容量；分配走驱动内存池，代价可忽略）。
  if (!app_fbo_.ensure(w, h)) return;
  glBindFramebuffer(GL_FRAMEBUFFER, app_fbo_.fbo());
  glViewport(0, 0, w, h);
  begin_frame(nvg_.ctx(), w, h);
  DmgRect clip_r{};
  const DmgRect* clip = nullptr;
  if (dmg.full()) {
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
  } else {
    clip_r = damage_bbox(dmg);
    clip = &clip_r;
    dmg_scissor_begin(nvg_.ctx(), h, clip_r);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);  // GL scissor 内 = 只清脏区
  }
  const float frame_rad = in.win_radius;

  // —— 空白处点击语义（单击 = 播放/暂停，双击 = 全屏）+ 中键静音 ——
  // 消歧：单击后等 dblClickSec（token，默认 300ms）；窗口内出现第二击 → 判双击，只切全屏。
  // "空白处"由 chrome 的几何判定（bar_hit / 顶栏三键），此处不重复实现第二份几何。
  const bool on_winbtn =
      in.my < theme_.layout.topBarH && in.mx >= w - 3 * theme_.layout.winBtnW;
  // d45：菜单开着 → 底层 UI 冻结交互。给视图层传 down=false 的副本（按钮悬停仍亮、
  // 按压/点击不落——菜单面板叠在哪块按钮上，点菜单就不能误触底下的按钮）。
  // 拖拽进行中的除外：传 false 会让松手瞬间误判"拖完提交"，故保持原值。
  ViewInput vin = in;
  if (ctx_open_ && !in.drag_progress && !in.drag_volume) vin.down = false;

  // —— v0.4.0 d57 修：隐藏按钮的冻结值清理（buttons_busy 恒真 → 60fps 死循环）——
  // d54 的 busy 收敛前提是"每个按钮每帧都被 button_hit 推进"；但**本帧不绘制的
  // 按钮没人推进**：播放/停止态的中央大播放键（只在暂停态绘制）、全屏态的顶栏
  // 三键（顶栏整段不渲染）、画中画的全部按钮。值冻在中间态（如悬停半程 0.37）
  // → buttons_busy 恒真 → 60fps 恒出帧（实测 [btns] dt=0.016 连续不收敛）。
  // 隐藏 = 视觉瞬间消失、无淡出动画可停 → 每帧幂等清零（代价 0）。只清 vin：
  // 帧末存回槽后下一帧 in.btns 即干净，busy 判定一帧自愈。
  if (pip_mode_) {
    vin.btns = ButtonFx{};
  } else {
    if (in.fullscreen) {
      vin.btns.hover[kBtnWinMin] = vin.btns.hover[kBtnWinMax] = vin.btns.hover[kBtnWinClose] = 0.f;
      vin.btns.press[kBtnWinMin] = vin.btns.press[kBtnWinMax] = vin.btns.press[kBtnWinClose] = 0.f;
    }
    if (snap.state != TransportState::PausedPlayback) {
      vin.btns.hover[kBtnCenterPlay] = 0.f;
      vin.btns.press[kBtnCenterPlay] = 0.f;
    }
    if (!in.clip_visible) {
      // d74：提示条隐藏 → 两按钮幂等清零（与隐藏按钮同理由：无人推进 = 值冻结）
      vin.btns.hover[kBtnClipPlay] = vin.btns.hover[kBtnClipIgnore] = 0.f;
      vin.btns.press[kBtnClipPlay] = vin.btns.press[kBtnClipIgnore] = 0.f;
    }
  }
  const BarHit bh = bar_hit(nvg_.ctx(), theme_, (float)w, (float)h, snap, vin, snap.has_session);
  // on_center 必须跟随中央键的真实可见性：中央大播放键只在**暂停态**绘制
  // （player_view.cpp: if (paused)），播放态中央是空白画面，点击应落入下面的
  // 单/双击消歧（单击=播放/暂停、双击=全屏），不能被纯几何命中挡掉。
  const bool on_center = snap.state == TransportState::PausedPlayback &&
                         center_play_hit(theme_, (float)w, (float)h, vin.mx, vin.my);
  // d45：菜单开着时压住空白处单双击语义（点菜单外的空白=只关菜单，不触发播放/暂停）
  const bool in_stage = !ctx_open_ && !on_winbtn && !on_center && !bh.consumed;

  // —— d45：右键菜单条目表 + 布局（每帧重算；命中与绘制共用同一份几何）——
  CtxMenuItem ctx_items[16];
  int ctx_n = 0;
  CtxMenuLayout ctx_lay;
  int ctx_hover = -1;
  if (ctx_open_) {
    const bool media = snap.has_session;
    auto add = [&](const char* label, CtxAction a, bool en, bool chk = false,
                   bool checked = false) { ctx_items[ctx_n++] = {label, a, en, chk, checked}; };
    add("播放 / 暂停", CtxAction::PlayPause, media);
    add("上一个", CtxAction::Prev, media);
    add("下一个", CtxAction::Next, media);
    add(nullptr, CtxAction::None, false);
    add("全屏", CtxAction::ToggleFullscreen, true);
    add("画中画", CtxAction::Pip, true);
    add(nullptr, CtxAction::None, false);
    add("媒体信息（完整）", CtxAction::ShowMediaInfo, true, true, show_info_);
    add("投屏自动全屏", CtxAction::CastAutoFullscreen, true, true, cast_auto_fs_);
    add(nullptr, CtxAction::None, false);
    add("退出", CtxAction::Close, true);
    ctx_lay = ctx_menu_layout(nvg_.ctx(), theme_, (float)w, (float)h, ctx_items, ctx_n,
                              ctx_anchor_x_, ctx_anchor_y_);
    ctx_hover = ctx_lay.item_at(in.mx, in.my);
  }

  // 中键：音量区 = 静音切换（与点音量图标同义）
  if (in.down_middle && !mid_prev_) {
    if (bh.vol) channel_.post_intent({UiIntent::Kind::Mute, 0, !snap.muted});
  }
  mid_prev_ = in.down_middle;

  // （d68）空白处单/双击与拖动判定已上移到 frame() 前段的统一指针三态机
  // （跳帧闸门之前，边沿零丢失）；按钮点击仍由 button_hit 归属锁定式触发。

  // —— d45：菜单内左键交互（标准菜单语义：同条目按下+弹起才激活；
  //     点菜单外/划出条目再松手 = 只关菜单不动作）——
  if (ctx_open_) {
    auto ctx_activate = [&](int idx) {
      // 禁用条目（无媒体会话的播放/暂停/上下集）：不动作，只收菜单（标准菜单语义）
      if (!ctx_items[idx].enabled) return;
      switch (ctx_items[idx].action) {
        case CtxAction::PlayPause: channel_.post_intent({UiIntent::Kind::PlayPause}); break;
        case CtxAction::Prev: channel_.post_intent({UiIntent::Kind::Prev}); break;
        case CtxAction::Next: channel_.post_intent({UiIntent::Kind::Next}); break;
        case CtxAction::ToggleFullscreen:
          channel_.post_intent({UiIntent::Kind::ToggleFullscreen});
          break;
        case CtxAction::Pip: channel_.post_intent({UiIntent::Kind::Pip}); break;
        // d146：勾选在渲染线程即时生效（本帧反馈）；同时回投主线程落盘（R1）。
        // force_dirty_：徽章整列出现/消失 + 包络并集失效兜底，全窗刷新最稳（低频事件）。
        case CtxAction::ShowMediaInfo:
          show_info_ = !show_info_;
          force_dirty_.store(true, std::memory_order_release);
          channel_.post_intent({UiIntent::Kind::UiPrefChanged, 0.0, show_info_,
                                UiIntent::DragRegion::None,
                                (int)UiIntent::UiPref::ShowInfo});
          break;
        case CtxAction::CastAutoFullscreen:
          cast_auto_fs_ = !cast_auto_fs_;
          channel_.post_intent({UiIntent::Kind::UiPrefChanged, 0.0, cast_auto_fs_,
                                UiIntent::DragRegion::None,
                                (int)UiIntent::UiPref::CastAutoFullscreen});
          break;
        case CtxAction::Close: channel_.post_intent({UiIntent::Kind::Close}); break;
        case CtxAction::None: break;
      }
    };
    if (in.down && !ctx_lprev_) {
      ctx_press_ = ctx_hover;                       // 按下：锁定条目（菜单外 -1）
    } else if (!in.down && ctx_lprev_) {
      if (ctx_press_ >= 0 && ctx_hover == ctx_press_) ctx_activate(ctx_press_);
      ctx_open_ = false;                            // 任何左键弹起都收菜单
      force_dirty_.store(true, std::memory_order_release);
    }
    ctx_lprev_ = in.down;
  }

  // —— 画面贴图（圆角内落地，四角保持透明）——
  // nvgImagePattern 的 w/h = "采样纹理的哪个区域"，这里给**纹理容量**（pic_w_/pic_h_ =
  // alloc 尺寸）：纹理容量 == FBO 容量（同一张纹理），采样区域给满才不会被
  // GL_LINEAR 把边界外/未渲染的像素混进来。
  //
  // ⚠️ 这里**不翻**，也不需要翻（d18 用四象限图实测确认）。
  // 历史教训：这段代码先后试过三种翻转写法，全是错的 ——
  //   a. NVG_IMAGE_FLIPY：枢轴取自 frag->extent[1] * 0.5f（nanovg_gl.h），
  //      隐含要求"pattern 的 h == 注册图像高"，条件一变就静默失效（d17 的镜像）；
  //   b. nvgImagePattern(cy=+pic_h, h=-pic_h)：nanovg 内部对 paint->xform 取了**逆**
  //      （nanovg_gl.h: nvgTransformInverse → xformToMat3x4），由 pt=(inv(xform)·fpos)/extent
  //      得 pt.y = (y - cy)/h = 1 - y/h → 实际是**上下翻**，于是画面镜像；
  //   c. nvgImagePattern(cy=-pic_h, h=-pic_h)：pt.y = -y/h - 1 ∈ [-2,-1]，
  //      全落在纹理外 → 只显示纹理的上半（下半屏糊成上半屏复制）。
  //
  // 结论：mpv 渲染进 FBO 的内容**已经是 nanovg 采样所需的朝向**
  // （根本原因：FBO 纹理原点在左下，而 nanovg 的 v 轴约定与之相容），
  // 直接用默认 pattern（cx=cy=0、extent 取正数）即可，任何额外翻转都是错的。
  // 本式有离线护栏：tools/flip_math_check.py（该脚本已按"禁止翻转"重写判定）。
  if (have_src && pic_w_ > 0 && pic_h_ > 0) {
    // d128：先铺一层**不透明舞台底色**，再叠视频纹理。
    // 为什么必须有这层：纹理是"整窗铺开一张图"，而 mpv 只填**视频矩形** ——
    // letterbox 的竖边/黑边按定义不画 ⇒ 那些像素 alpha=0；本应用是半透明自绘窗，
    // 不铺底就直接透出桌面（真机实测：窗口左右两条露出壁纸）。
    // 待机态本来就有这层（empty_state.cpp 用 stageBg 铺满整窗），有画面时漏了。
    // 用 stageBg 而非纯黑：舞台底色就是"此处本应是画面"的那块面，与空态保持一致。
    // 也顺带兜住 SW 上传失败/尺寸不匹配：留透明会透桌面，留底色至少是块干净的面。
    nvgBeginPath(nvg_.ctx());
    if (frame_rad > 0.5f)
      nvgRoundedRect(nvg_.ctx(), 0, 0, (float)w, (float)h, frame_rad);
    else
      nvgRect(nvg_.ctx(), 0, 0, (float)w, (float)h);
    nvgFillColor(nvg_.ctx(), theme_.stageBg);
    nvgFill(nvg_.ctx());

    nvgBeginPath(nvg_.ctx());  // 画面本体：source-over 叠在底色上，未覆盖处仍是 alpha=0 → 透出底色
    if (frame_rad > 0.5f)
      nvgRoundedRect(nvg_.ctx(), 0, 0, (float)w, (float)h, frame_rad);
    else
      nvgRect(nvg_.ctx(), 0, 0, (float)w, (float)h);
    nvgFillPaint(nvg_.ctx(), nvgImagePattern(nvg_.ctx(), 0, 0.0f, (float)pic_w_,
                                             (float)pic_h_, 0, pic_img_, 1.0f));
    nvgFill(nvg_.ctx());
  }

  // —— UI 叠加 ——
  ViewCallbacks cb = make_intent_callbacks(channel_);
  if (pip_mode_) {
    // 画中画：有画面时只有画面；待机态给小窗专用缩小版内容（底色 + 设备名 + 等待投屏）
    if (!have_src)
      draw_standby_mini(nvg_.ctx(), theme_, (float)w, (float)h, friendly_name_.c_str(), clip);
  } else if (has_picture) {
    draw_player_view(nvg_.ctx(), theme_, (float)w, (float)h, snap, vin, cb, thumb_img, clip);
  } else {
    draw_empty_state(nvg_.ctx(), theme_, (float)w, (float)h, friendly_name_.c_str(), snap, vin,
                     cb, clip);
  }

  // —— d44/d45 → d146 R5 常驻徽章（叠在视频/舞台左上角，画中画小窗不画）——
  // 三合一（show_info_）：每行一条独立胶囊纵向堆叠（d149 用户改稿），fps 条恒在
  // 最前（video|ui 英文标签）、视频条（分辨率/编码/码率/硬解软解）、音频条
  //（无音频流整条隐藏）。
  // 旧诊断路径（show_fps_/show_video_fps_，settings.json 直开、无菜单项）保留。
  if (!pip_mode_ && (show_info_ || show_fps_ || show_video_fps_)) {
    const auto& L = theme_.layout;
    // UI 帧率：相邻两次**出帧**的间隔 EMA（d46 起跳帧与徽章共存，此值即真实出帧率）。
    // d47：平滑权重按**时间常数**而非帧数 —— 原固定 α=0.1 在按需渲染下失真：
    // 静默期出帧只剩 2s 心跳，每帧只挪 10%，60fps 读数要 80 秒才降到真实值
    //（用户实测「为什么UI帧率下降的这么慢」）。改为 w=1-e^(-span/τ)：
    // 高频帧（span 16ms）≈0.05 与原手感一致，低频帧（span 2s）≈0.999 一步到位，
    // 收敛速度恒定 ~τ 秒，与出帧率无关。
    // d62：徽章自身触发的帧（唯一归因 = 徽章内容变化）不喂 EMA、不推进分母
    // —— 展示成本不得污染被展示的指标。否则徽章帧自己制造自己的样本：
    // 每次读数变化 → 出帧 → 出帧又制造新样本 → 读数被钉死在展示帧率上。
    if (dmg_reasons != (1u << 10)) {
      if (fps_last_draw_ > 0) {
        const double span = now - fps_last_draw_;
        if (span > 0.0005) {
          const double fps = 1.0 / span;
          constexpr double kTau = 0.3;  // 平滑时间常数（秒）：读数稳定 vs 跟随速度的折中
          const double wgt = 1.0 - std::exp(-span / kTau);
          fps_ema_ = fps_ema_ > 0 ? fps_ema_ * (1.0 - wgt) + fps * wgt : fps;
        }
      }
      fps_last_draw_ = now;
    }
    // mpv 信息整包：2Hz 一次（三合一与旧视频帧率徽章共用同一次查询，d45）
    if ((show_info_ || show_video_fps_) && now - media_at_ > 0.5) {
      media_at_ = now;
      const VideoStats st = player_.video_stats();
      media_.vfps = st.fps;
      media_.w = st.w;
      media_.h = st.h;
      media_.vbr = st.video_br > 0 ? st.video_br : st.bitrate;  // 视频码率优先退总码率
      media_.abr = st.audio_br;
      media_.asr = st.audio_sr;
      media_.ach = st.audio_ch;
      media_.vcodec = st.video_codec;
      media_.acodec = st.audio_codec;
      media_.hwname = st.hwdec_name;
    }
    const float px = 12.f;
    const float py = in.fullscreen ? 12.f : L.topBarH + 10.f;
    if (show_info_) {
      // 行文本（取不到的字段显示 "-" 不猜；音频行任一字段有值即画）
      char r1[64], r2[160], r3[160];
      const char* rows[3] = {r1, r2, r3};
      int rows_n = 0;
      char v[24] = "--";
      if (media_.vfps > 0) snprintf(v, sizeof(v), "%.1f", media_.vfps);
      char u[24] = "--";
      if (fps_ema_ >= 10.f)
        snprintf(u, sizeof(u), "%.0f", fps_ema_);
      else if (fps_ema_ > 0)
        snprintf(u, sizeof(u), "%.1f", fps_ema_);
      snprintf(r1, sizeof(r1), "video %s|ui %s", v, u);
      rows_n = 1;
      auto f1 = [](double x) {
        char t[32];
        snprintf(t, sizeof(t), "%.1f", x);
        return std::string(t);
      };
      if (media_.w > 0) {
        std::string s = "分辨率 " + std::to_string(media_.w) + "×" +
                        std::to_string(media_.h);
        s += " · 编码 ";
        s += media_.vcodec.empty() ? "-" : media_.vcodec;
        if (media_.vbr > 0)
          s += " · 码率 " + f1((double)media_.vbr / 1e6) + "Mbps";
        else
          s += " · 码率 -";
        if (!media_.hwname.empty()) {
          if (media_.hwname == "no")
            s += " · 软解";
          else
            s += " · 硬解 " + media_.hwname;
        }
        snprintf(r2, sizeof(r2), "%s", s.c_str());
        rows_n = 2;
        if (!media_.acodec.empty() || media_.asr > 0 || media_.ach > 0 ||
            media_.abr > 0) {
          std::string a = "音频编码 ";
          a += media_.acodec.empty() ? "-" : media_.acodec;
          a += " · 采样率 ";
          if (media_.asr > 0) {
            char t[32];
            snprintf(t, sizeof(t), "%gkHz", media_.asr / 1000.0);
            a += t;
          } else {
            a += "-";
          }
          a += " · 声道数 ";
          a += media_.ach > 0 ? std::to_string(media_.ach) : std::string("-");
          a += " · 码率 ";
          if (media_.abr > 0) {  // 文本从量化值派生（kbps 整数；mpv audio-bitrate 会抖）
            char t[32];
            snprintf(t, sizeof(t), "%lldkbps",
                     std::llround((double)media_.abr / 1000.0));
            a += t;
          } else {
            a += "-";
          }
          snprintf(r3, sizeof(r3), "%s", a.c_str());
          rows_n = 3;
        }
      }
      // d149 用户改稿：每行一条 —— 独立胶囊纵向堆叠（节奏与旧逐条徽章一致：
      // 条高 font+2*padY、间距 6px），文本与显隐规则不变
      float yy = py;
      auto badge = [&](const char* s) {
        const float bw = text_width(nvg_.ctx(), L.badgeFont, s) + L.badgePadX * 2;
        const float bh = L.badgeFont + L.badgePadY * 2;
        rounded_rect(nvg_.ctx(), px, yy, bw, bh, L.badgeRadius, theme_.badgeBg);
        text(nvg_.ctx(), theme_, px + L.badgePadX, yy + bh * 0.5f, L.badgeFont,
             theme_.badgeText, s);
        yy += bh + 6;
      };
      for (int i = 0; i < rows_n; ++i) badge(rows[i]);
    } else {
      // 旧诊断路径：逐条胶囊（视频帧率 / UI 帧率），格式与 v0.4.0 一致
      char buf[48];
      float yy = py;
      auto badge = [&](const char* s) {
        const float bw = text_width(nvg_.ctx(), L.badgeFont, s) + L.badgePadX * 2;
        const float bh = L.badgeFont + L.badgePadY * 2;
        rounded_rect(nvg_.ctx(), px, yy, bw, bh, L.badgeRadius, theme_.badgeBg);
        text(nvg_.ctx(), theme_, px + L.badgePadX, yy + bh * 0.5f, L.badgeFont,
             theme_.badgeText, s);
        yy += bh + 6;
      };
      if (show_video_fps_) {
        if (media_.vfps > 0)
          snprintf(buf, sizeof(buf), "视频 %.1f fps", media_.vfps);
        else
          snprintf(buf, sizeof(buf), "视频 --");
        badge(buf);
      }
      if (show_fps_) {
        // d47：按需渲染下静默期真实出帧 ~0.5fps（2s 心跳），整数格式会显示成
        // "0 fps" 像坏了 —— 读数 <10 时用一位小数，如实呈现。
        if (fps_ema_ >= 10.f)
          snprintf(buf, sizeof(buf), "UI %.0f fps", fps_ema_);
        else
          snprintf(buf, sizeof(buf), "UI %.1f fps", fps_ema_ > 0 ? fps_ema_ : 0.0);
        badge(buf);
      }
    }
    // d62：徽章「已绘制内容」基准推进 —— 仅当徽章列实际被本帧画到（整窗或
    // bbox 覆盖包络；绘制侧按 bbox 单矩形裁剪，bbox 相交 = 必然画到）。没画到
    // 就不推进：归因条目下轮继续触发，直到真画上为止（基准先动 = 变化被吞）。
    // 跳帧路径根本进不到绘制段，天然满足「跳帧不存回」铁律。
    {
      const BadgeQ bq = badge_quant(show_fps_, fps_ema_, show_video_fps_,
                                    media_.vfps, show_info_, media_);
      const DmgRect env = badge_column_rect(in.fullscreen, L.topBarH, w, bq.lines,
                                            L.badgeFont, L.badgePadY);
      bool painted = dmg.full();
      if (!painted && env.w > 0) painted = rects_overlap(damage_bbox(dmg), env);
      if (painted) badge_q_drawn_ = bq;
    }
  }

  // —— d74 剪贴板链接提示条（层位：徽章之上、右键菜单之下）——
  // 生命周期/几何唯一真值在 prompt_layout_of；淡入淡出 alpha 由 clip_at 推算
  //（归因侧同式，禁两处各算）。按钮走 button_hit 归属锁定 → 拖出取消、
  // 悬停缓动全部复用统一三态机（无需特判）。
  if (!pip_mode_ && R.prompt_visible) {
    const auto& L = theme_.layout;
    const PromptLayout pl =
        prompt_layout_of(nvg_.ctx(), theme_, (float)w, (float)h, in.fullscreen, now,
                         in.clip_at, in.clip_url, in.clip_visible);
    const double age = now - in.clip_at;
    float alpha = 1.f;
    if (age < L.promptFadeSec)
      alpha = (float)(age / L.promptFadeSec);
    else if (age >= L.promptHoldSec)
      alpha = (float)std::max(0.0, (L.promptHoldSec + L.promptFadeSec - age) / L.promptFadeSec);
    nvgGlobalAlpha(nvg_.ctx(), alpha);

    // 横幅底（不透明，与顶栏同色系）+ 1px 细边
    rounded_rect(nvg_.ctx(), pl.x, pl.y, pl.w, pl.h, L.promptBarRadius, theme_.topBarBg);
    nvgBeginPath(nvg_.ctx());
    nvgRoundedRect(nvg_.ctx(), pl.x + 0.5f, pl.y + 0.5f, pl.w - 1.f, pl.h - 1.f,
                   L.promptBarRadius);
    nvgStrokeWidth(nvg_.ctx(), 1.f);
    nvgStrokeColor(nvg_.ctx(), theme_.barBorderTopBar);
    nvgStroke(nvg_.ctx());

    // URL 文本（垂直居中；超宽打省略号）
    text_truncated(nvg_.ctx(), theme_, pl.url_x, pl.y + pl.h * 0.5f, L.promptFont,
                   theme_.subText, in.clip_url, pl.url_max_w);

    // 两按钮（播放 / 忽略）
    const bool inside_play = in.mx >= pl.btn_play_x && in.mx < pl.btn_play_x + pl.btn_w &&
                             in.my >= pl.btn_y && in.my < pl.btn_y + pl.btn_h;
    const bool inside_ign = in.mx >= pl.btn_ignore_x && in.mx < pl.btn_ignore_x + pl.btn_w &&
                            in.my >= pl.btn_y && in.my < pl.btn_y + pl.btn_h;
    const BtnState sp =
        button_hit(vin.btns, kBtnClipPlay, inside_play, true, vin.down, vin.dt, theme_);
    const BtnState si =
        button_hit(vin.btns, kBtnClipIgnore, inside_ign, true, vin.down, vin.dt, theme_);
    draw_prompt_btn(nvg_.ctx(), theme_, pl.btn_play_x, pl.btn_y, pl.btn_w, pl.btn_h, "播放", sp,
                    true);
    draw_prompt_btn(nvg_.ctx(), theme_, pl.btn_ignore_x, pl.btn_y, pl.btn_w, pl.btn_h, "忽略",
                    si, false);
    nvgGlobalAlpha(nvg_.ctx(), 1.f);

    if (sp.clicked) {
      channel_.post_intent({UiIntent::Kind::ClipboardPlay});
    } else if (si.clicked) {
      channel_.post_intent({UiIntent::Kind::ClipboardDismiss});
    }
  }

  // —— d45：右键菜单（全栈最顶层：盖过视图与徽章）——
  if (!pip_mode_ && ctx_open_)
    draw_ctx_menu(nvg_.ctx(), theme_, ctx_lay, ctx_items, ctx_n, ctx_hover, clip);

  // 按钮交互收尾（必须在每帧末尾推进，画中画等无按钮帧也不例外，
  // 否则"松开"边沿会漏掉、按压归属会一直挂着）
  button_fx_end_frame(vin.btns, vin.down);

  // —— 缩略图预览请求（渲染线程判定悬停 → 主线程驱动 thumb worker）——
  // 节流：百分比变化超 0.4% 才投递（拖拽/滑动不至于每帧一请求；worker 侧另有
  // latest-wins + 近重复去重兜底）。键盘拖动进度同样出预览（d29）。
  const bool kb_preview = in.kb_progress || in.now < in.kb_preview_until;
  const bool preview_on = !pip_mode_ && has_picture && snap.duration > 0 &&
                          (bh.track || in.drag_progress || kb_preview);
  if (preview_on) {
    const float p = in.drag_progress  ? in.drag_value
                    : kb_preview      ? in.kb_progress_value
                                      : progress_percent_at(in.mx, bh.track_x, bh.track_w);
    if (preview_posted_ < 0 || std::fabs(p - preview_posted_) > 0.004f) {
      preview_posted_ = p;
      channel_.post_intent({UiIntent::Kind::Preview, (double)p, false});
    }
  } else if (preview_posted_ >= 0) {
    preview_posted_ = -1.f;  // 离开条身：复位，下次进入立即投递
  }

  // 把跨帧状态写回渲染线程独占槽（下一帧帧首再装回来，形成跨帧续接）：
  // ButtonFx 动画 + 滑条拖拽（d24：后者此前漏存 → 拖动条全废）。
  // d45：一律取 vin —— 视图层（draw_player_view/draw_bottom_bar）收到的是 vin 副本，
  // 本帧推进发生在 vin.btns / vin.drag_* 上；取 in 会丢掉推进结果（按钮点击失灵）。
  {
    ViewInput& rs = channel_.render_state();
    rs.btns = vin.btns;
    rs.drag_progress = vin.drag_progress;
    rs.drag_volume = vin.drag_volume;
    rs.drag_value = vin.drag_value;
  }

  // —— d43：记录本帧基准（下一帧脏判定用；in 整份存，比较时才清零易变字段）——
  last_in_key_ = in;
  last_snap_ = snap;
  last_hover_key_ = hover_key;  // d51：悬停目标基准（坐标变化的裁决依据）
  // d59：区域存档（下一帧差集归因基准）。ctx_menu 面板矩形在此补填 —— ctx_lay
  // 在出帧路径算出，菜单开着期间几何固定（锚点定死），下一帧归因直接消费。
  if (ctx_open_)
    R.ctx_menu = DmgRect{(int)ctx_lay.x, (int)ctx_lay.y, (int)ctx_lay.w, (int)ctx_lay.h};
  last_regions_ = R;
  last_w_ = w;
  last_h_ = h;
  last_keepalive_ = now;
  last_draw_at_ = now;  // d56：DMG3 的出帧间隔基准
  ever_drawn_ = true;

  // —— v0.4.0 d56：区域路径收尾 + FR_DBG_DAMAGE 诊断 ——
  // =1：脏矩形描 1px 品红边 + 每次出帧打逻辑/GL 坐标（暴露 y 翻转换算是否写错）。
  // =3：额外限频打出帧原因与间隔（见归因段 DMG3）。
  // d61：描边从合成缓冲挪到**提交之后**（见帧末）—— 描边若画进 app_fbo_ 会
  // 永久残留（合成缓冲内容自持，没人清它），表现为历史框线越积越多。
  const bool dbg_dmg = dbg_lvl_ >= 1;
  if (clip != nullptr) dmg_scissor_end(nvg_.ctx());
  if (dbg_dmg) {
    if (dmg.full())
      FR_LOG_INFO("[DMG] FULL {}x{}", w, h);
    else
      FR_LOG_INFO("[DMG] rect l=({},{},{},{}) gl_y={}", clip_r.x, clip_r.y, clip_r.w, clip_r.h,
                  gl_scissor_y(h, clip_r));
  }

  end_frame(nvg_.ctx());

  // —— d60 提交：app_fbo_ 全窗 blit 到默认帧缓冲 → swap ——
  // 绘制（区域）与提交（全窗）在此解耦：blit 是纯像素拷贝（无光栅化/混合），
  // back buffer 里是什么内容无所谓 —— 每次呈现的都是 app_fbo_ 的完整一致画面，
  // 根治区域重绘依赖 back 残留导致的偶发闪烁（按钮切换/拖窗实测）。
  // NEAREST：同尺寸 blit 无缩放采样问题；格式 GL_RGBA8 与默认帧缓冲一致。
  glBindFramebuffer(GL_READ_FRAMEBUFFER, app_fbo_.fbo());
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glBlitFramebuffer(0, 0, app_fbo_.width(), app_fbo_.height(), 0, 0, w, h,
                    GL_COLOR_BUFFER_BIT, GL_NEAREST);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  // —— d61 诊断描边：画在默认帧缓冲上（blit 之后、swap 之前）——
  // 只随本帧上屏、不写进合成缓冲 → 零残留。仅诊断模式多一次 begin/end_frame。
  if (dbg_dmg && clip != nullptr) {
    glViewport(0, 0, w, h);
    begin_frame(nvg_.ctx(), w, h);
    nvgBeginPath(nvg_.ctx());
    nvgRect(nvg_.ctx(), (float)clip_r.x + 0.5f, (float)clip_r.y + 0.5f,
            (float)clip_r.w - 1.f, (float)clip_r.h - 1.f);
    nvgStrokeColor(nvg_.ctx(), nvgRGBf(1.f, 0.f, 1.f));
    nvgStrokeWidth(nvg_.ctx(), 1.f);
    nvgStroke(nvg_.ctx());
    end_frame(nvg_.ctx());
  }
  window_.swap_buffers();
  // mpv 官方要求：每次 render 后成对调用，否则内部同步退化
  player_.report_swap();
}

// 把视图回调包装成"投递意图"（渲染线程 → 主线程）。
// 视图层只表达"用户想做什么"，具体执行（改状态机/调窗口 API）在主线程。
ViewCallbacks make_intent_callbacks(ViewStateChannel& ch) {
  ViewCallbacks cb;
  cb.on_close = [&ch] { ch.post_intent({UiIntent::Kind::Close}); };
  cb.on_minimize = [&ch] { ch.post_intent({UiIntent::Kind::Minimize}); };
  cb.on_maximize = [&ch] { ch.post_intent({UiIntent::Kind::ToggleMaximize}); };
  cb.on_fullscreen = [&ch] { ch.post_intent({UiIntent::Kind::ToggleFullscreen}); };
  cb.on_pip = [&ch] { ch.post_intent({UiIntent::Kind::Pip}); };
  cb.on_play_pause = [&ch] { ch.post_intent({UiIntent::Kind::PlayPause}); };
  cb.on_prev = [&ch] { ch.post_intent({UiIntent::Kind::Prev}); };
  cb.on_next = [&ch] { ch.post_intent({UiIntent::Kind::Next}); };
  cb.on_seek = [&ch](double sec) { ch.post_intent({UiIntent::Kind::Seek, sec, false}); };
  cb.on_volume = [&ch](float v) { ch.post_intent({UiIntent::Kind::Volume, v, false}); };
  cb.on_mute = [&ch](bool m) { ch.post_intent({UiIntent::Kind::Mute, 0, m}); };
  return cb;
}

}  // namespace fr
