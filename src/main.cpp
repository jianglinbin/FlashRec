// FlashRec 入口（app 层：唯一允许编排 dmr/player/ui/platform 的地方，规则 1/2）。
//
// 线程模型（ledger d15）：
//   主线程（消息/状态）—— glfw 事件、输入、DMR 与播放状态机、窗口形态操作。
//   渲染线程（RenderLoop）—— 独占 GL 上下文，跑 mpv→FBO→nanovg→swap 全链。
// 拆开的原因：Windows 拖拽窗口边缘时进入系统模态循环，glfwWaitEvents 不返回
// （GLFW 官方 FAQ 3.5 承认此为 Windows 设计使然），单线程下渲染整体停摆，
// 屏幕由 DWM 兜底拉伸 → "缩小正常、放大跟不上"。渲染搬线程后模态循环只能
// 阻塞消息线程，碰不到渲染线程。详见 src/app/render_loop.h 顶部注释。
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "app/config.h"

// 版本号由 CMake 从 project(VERSION) 下发（见 CMakeLists.txt，d99）。
// 留兜底只为让"绕过 CMake 手编"这种非常规操作仍能编过，不至于卡住排查。
#ifndef FLASHREC_VERSION
#define FLASHREC_VERSION "0.0.0-unknown"
#endif
#include "app/event_bus.h"
#include "app/log.h"
#include "app/render_loop.h"
#include "app/view_state.h"
#include "dmr/dmr_device.h"
#include "platform/net_if.h"   // 多网卡（d110）：启动打印候选接口
#include "platform/clipboard.h"
#include "platform/console.h"  // 运行时控制台（--console）+ UTF-8（d163）
#include "platform/keep_awake.h"
#include "platform/paths.h"
#include "platform/crash_dump.h"
#include "platform/single_instance.h"
#include "platform/tray.h"
#include "platform/window_glfw.h"
#include "player/player_controller.h"
#include "player/thumb_preview.h"
#include "player/url_probe.h"
#include "ui/theme.h"

using namespace fr;

namespace {

double mono_now() {
  using namespace std::chrono;
  return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

// d74：从剪贴板文本提取首个 http(s) URL（形式校验只到 scheme 级——「可播」
// 的裁决交给 mpv 探测，这里不做媒体特征猜测，用户拍板「任意 URL+探测确认」）。
std::string extract_clipboard_url(const std::string& text) {
  if (text.empty() || text.size() > 64 * 1024) return "";
  size_t pos = std::string::npos;
  for (const char* s : {"https://", "http://"}) {
    const size_t p = text.find(s);
    if (p != std::string::npos && (pos == std::string::npos || p < pos)) pos = p;
  }
  if (pos == std::string::npos) return "";
  const size_t scheme_len = text[pos] == 'h' && text.compare(pos, 8, "https://") == 0 ? 8 : 7;
  size_t end = pos + scheme_len;
  for (; end < text.size(); ++end) {
    const unsigned char c = (unsigned char)text[end];
    if (c <= 0x20 || c == '"' || c == '\'' || c == '<' || c == '>' || c == '`' || c == '\\')
      break;
  }
  std::string url = text.substr(pos, end - pos);
  // 句尾标点剥离（"看这个 https://x/a.mp4。"——中文标点非 ASCII 已天然截断）
  while (!url.empty() && std::strchr(".,;:!?)]}>\"'", url.back())) url.pop_back();
  if (url.size() < 11 || url.size() > 2048) return "";  // 短于最短合法 host 串 / 超长
  return url;
}

// —— d146 R4：多屏工具（启动恢复 / cast 管线 / 托盘投屏窗口共用；全主线程）——

// 可用屏收集：monitor_info 内部已做三步可用性判定（在列表 + 模式可取 + 工作区），
// false = 休眠/拔线/禁用，直接跳过。返回收到的条数（≤cap）。
int collect_monitors(MonitorInfo* out, int cap) {
  int n = 0;
  const int total = WindowGLFW::monitor_count();
  for (int i = 0; i < total && n < cap; ++i)
    if (WindowGLFW::monitor_info(i, &out[n])) ++n;
  return n;
}

// 记忆条目按指纹查找（命中 = 这块屏的摆位记忆）
const MonitorMemory* mem_for(const Config& cfg, const char* fp) {
  for (const auto& m : cfg.ui_window_monitors)
    if (m.fp == fp) return &m;
  return nullptr;
}

// P2 摆位：尺寸先压进目标屏工作区、位置再夹进工作区（跨分辨率恢复先缩再夹）；
// 无记忆 = 默认 0.6×工作区居中。
void apply_placement(WindowGLFW& w, const MonitorInfo& target,
                     const MonitorMemory* mem) {
  if (!mem) {
    const int cw = target.aw * 6 / 10, ch = target.ah * 6 / 10;
    w.set_pos(target.ax + (target.aw - cw) / 2, target.ay + (target.ah - ch) / 2);
    w.set_window_size(cw, ch);
    return;
  }
  int x = mem->x, y = mem->y, cw = mem->w, ch = mem->h;
  if (cw > target.aw) cw = target.aw;
  if (ch > target.ah) ch = target.ah;
  if (x < target.ax) x = target.ax;
  if (y < target.ay) y = target.ay;
  if (x + cw > target.ax + target.aw) x = target.ax + target.aw - cw;
  if (y + ch > target.ay + target.ah) y = target.ay + target.ah - ch;
  w.set_pos(x, y);
  w.set_window_size(cw, ch);
}

// P3 显窗（R3 显窗是无条件的；面板不弹由"不碰 last_input"保证，d36 语义）
void ensure_window_shown(WindowGLFW& w) {
  if (glfwGetWindowAttrib(w.raw(), GLFW_ICONIFIED)) glfwRestoreWindow(w.raw());
  if (!glfwGetWindowAttrib(w.raw(), GLFW_VISIBLE)) w.show_window();
}

// P1 目标屏选择（§4.3 指纹降级链）。cur_idx ≥ 0 且可用 = 就地不动（不打扰）；
// prefer=cursor 走鼠标屏；否则走 last 链：①指纹完全命中 ②分辨率唯一命中
// ③位置包含 ④主屏兜底 → 首块可用屏。
const MonitorInfo* pick_target(const Config& cfg,
                                           const MonitorInfo* mons,
                                           int n, int cur_idx) {
  if (cur_idx >= 0)
    for (int i = 0; i < n; ++i)
      if (mons[i].idx == cur_idx) return &mons[i];
  const MonitorMemory* newest = nullptr;
  for (const auto& m : cfg.ui_window_monitors)
    if (!newest || m.last_used > newest->last_used) newest = &m;
  if (cfg.ui_window_prefer == "cursor") {
    int gx = 0, gy = 0;
    if (WindowGLFW::global_cursor(&gx, &gy))
      for (int i = 0; i < n; ++i)
        if (gx >= mons[i].x && gx < mons[i].x + mons[i].w && gy >= mons[i].y &&
            gy < mons[i].y + mons[i].h)
          return &mons[i];
  } else if (newest) {
    for (int i = 0; i < n; ++i)
      if (mons[i].fp == newest->fp) return &mons[i];  // ①
    if (newest->w > 0) {                              // ②
      const MonitorInfo* hit = nullptr;
      int hits = 0;
      for (int i = 0; i < n; ++i)
        if (mons[i].w == newest->w && mons[i].h == newest->h) {
          hit = &mons[i];
          ++hits;
        }
      if (hits == 1) return hit;
    }
    for (int i = 0; i < n; ++i)                       // ③
      if (newest->x >= mons[i].x && newest->x < mons[i].x + mons[i].w &&
          newest->y >= mons[i].y && newest->y < mons[i].y + mons[i].h)
        return &mons[i];
  }
  for (int i = 0; i < n; ++i)                         // ④
    if (mons[i].primary) return &mons[i];
  return n > 0 ? &mons[0] : nullptr;
}

// P5 记忆写回：窗口中心所在屏的条目更新（无则插入，>4 条淘汰 last_used 最旧，
// §13）→ patch_monitors 整体落盘（键值无变化时内部不写盘）。
void save_window_geometry(Config& cfg, WindowGLFW& w, const std::string& path) {
  MonitorInfo mons[8];
  const int n = collect_monitors(mons, 8);
  const int cur = w.monitor_index_of_window();
  if (cur < 0) return;
  const MonitorInfo* cm = nullptr;
  for (int i = 0; i < n; ++i)
    if (mons[i].idx == cur) cm = &mons[i];
  if (!cm) return;
  int x, y, ww, wh;
  w.pos_size(&x, &y, &ww, &wh);
  const long long t = (long long)mono_now();
  bool updated = false;
  for (auto& m : cfg.ui_window_monitors)
    if (m.fp == cm->fp) {
      m.x = x;
      m.y = y;
      m.w = ww;
      m.h = wh;
      m.maximized = w.maximized();
      m.fullscreen = w.fullscreen();
      m.last_used = t;
      updated = true;
    }
  if (!updated) {
    MonitorMemory m;
    m.fp = cm->fp;
    m.x = x;
    m.y = y;
    m.w = ww;
    m.h = wh;
    m.maximized = w.maximized();
    m.fullscreen = w.fullscreen();
    m.last_used = t;
    if (cfg.ui_window_monitors.size() >= 4) {
      auto it = std::min_element(cfg.ui_window_monitors.begin(),
                                 cfg.ui_window_monitors.end(),
                                 [](const MonitorMemory& a, const MonitorMemory& b) {
                                   return a.last_used < b.last_used;
                                 });
      cfg.ui_window_monitors.erase(it);
    }
    cfg.ui_window_monitors.push_back(std::move(m));
  }
  Config::patch_monitors(path, cfg.ui_window_monitors);
}

}  // namespace

int main(int argc, char** argv) {
  // 控制台策略（d163）：默认无控制台（GUI 子系统）；带 `--console` 才申请一个。
  // 必须在 init_logger 之前，控制台 sink 才能在构造时拿到有效句柄。
  bool want_console = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--console") == 0 || std::strcmp(argv[i], "-c") == 0) {
      want_console = true;
    }
  }
  bool have_console = want_console ? console_open() : console_available();
  if (have_console && !want_console) console_set_utf8();

  paths::init();
  crash_dump::install(paths::log_dir());  // 未处理异常 → minidump（事后定位崩溃模块）
  Config cfg = Config::load(paths::config_file());
  init_logger(paths::log_dir(), cfg.log_level, have_console);
  FR_LOG_INFO("[APP] FlashRec 启动 v{}（控制台={}）", FLASHREC_VERSION,
              have_console ? "on" : "off");

  // d22：pupnp 全量请求入口日志（miniserver.c 的 fr_http_trace 补丁读此变量）。
  // 默认关闭（settings.json 加 "log.http_entry": true 才开）——每个请求一次
  // fopen/fclose 且无轮转，常开会在大流量轮询下拖慢协议层并撑爆磁盘。
  // 必须在 dmr.start()（UpnpInit）之前设置；关掉时必须清空变量（防止外部环境残留）。
  if (cfg.log_http_entry) {
    const std::string http_trace = paths::log_dir() + "/http_trace.log";
    paths::set_env("FR_HTTP_TRACE_LOG", http_trace);
    FR_LOG_INFO("[APP] HTTP 入口跟踪开启: {}", http_trace);
  } else {
    paths::set_env("FR_HTTP_TRACE_LOG", "");
    FR_LOG_INFO("[APP] HTTP 入口跟踪关闭（settings.json: log.http_entry）");
  }

  // d144：请求原文「收件箱」—— pupnp 收件箱补丁（miniserver.c 的 fr_soap_inbox）读此变量。
  // 与 http_entry 的区别：那条是每请求一行摘要（体积小、可长开），这条是**整段请求原文**
  // （含报文体全文、逐字节、单文件不轮转）。挂点在 dispatch_request 的第一条语句 ⇒
  // 内容早于我方任何内部逻辑落盘，下游"静默 return"吞不掉它。同样必须在 UpnpInit 之前设好。
  // ⚠️ 体积：1Hz 轮询下约 4–5 MB/小时，取证完就关（settings.json: log.soap_inbox = false）。
  if (cfg.log_soap_inbox) {
    const std::string soap_inbox = paths::log_dir() + "/soap_inbox.log";
    paths::set_env("FR_SOAP_INBOX_LOG", soap_inbox);
    FR_LOG_INFO("[APP] SOAP 收件箱开启: {}", soap_inbox);
  } else {
    paths::set_env("FR_SOAP_INBOX_LOG", "");
    FR_LOG_INFO("[APP] SOAP 收件箱关闭（settings.json: log.soap_inbox）");
  }

  if (!single_instance::acquire()) {
    single_instance::raise_existing();
    FR_LOG_WARN("[APP] 已有实例运行，退出");
    shutdown_logger();
    return 0;
  }

  // —— 窗口（主线程；GL 上下文稍后交给渲染线程）——
  WindowGLFW window;
  const Theme& theme = Theme::get(cfg.skin);
  if (!window.create(theme.layout.winW, theme.layout.winH, cfg.friendly_name.c_str())) {
    FR_LOG_ERROR("[APP] 窗口初始化失败，退出");
    shutdown_logger();
    return 1;
  }
  // d83：托盘模式下启动即隐窗。先隐再跑 mpv/DMR 初始化（全程不闪现）；
  // 若托盘装载失败，后面恢复显示。渲染循环对隐藏窗口照常工作。
  // d91：全部显隐一律走 window.hide_window()/show_window() 包装 —— 它们负责
  // 与渲染线程的交换闸门握手，直接调 glfwHideWindow 会踩 Wayland 竞态（见 ledger d91）。
  window.hide_window();

  // d146 R4：启动恢复窗口几何 —— 最新记忆条目的指纹命中当前某块可用屏才恢复
  //（尺寸变了走夹取；没命中保持 create() 的默认居中）。在隐窗后做，不改变
  // "启动即隐窗"的托盘语义；无记忆 = 什么都不动。
  {
    MonitorInfo ms[8];
    const int n = collect_monitors(ms, 8);
    const MonitorMemory* newest = nullptr;
    for (const auto& m : cfg.ui_window_monitors)
      if (!newest || m.last_used > newest->last_used) newest = &m;
    if (newest)
      for (int i = 0; i < n; ++i)
        if (ms[i].fp == newest->fp) {
          apply_placement(window, ms[i], newest);
          break;
        }
  }

  // —— 播放 + DMR（主线程持有状态机）——
  EventBus bus;
  PlayerController player(bus, cfg);
  if (!player.init()) {
    FR_LOG_ERROR("[APP] mpv 初始化失败（继续运行，投屏不可用）");
  }
  // d163：恢复持久化的音量/静音 —— 上次调好的音量跨重启保留。bilibili 投屏开始
  // 在 Play 后 ~1.5s 才推 SetVolume，恢复缺位的窗口期就是"默认 100 最响很炸"的那段。
  if (cfg.player_volume != 100 || cfg.player_muted) {
    DmrCommand vc;
    vc.type = DmrCommand::Type::SetVolume;
    vc.volume = cfg.player_volume;
    player.on_command(vc);  // req#0：本地恢复（与 UI 操作同口径）
    if (cfg.player_muted) {
      DmrCommand mc;
      mc.type = DmrCommand::Type::SetMute;
      mc.mute = true;
      player.on_command(mc);
    }
    FR_LOG_INFO("[APP] 音量恢复 {}{}（settings.json: player.*）", cfg.player_volume,
                cfg.player_muted ? " 静音" : "");
  }
  // d169：恢复持久化的倍速（player.speed）。mpv speed 是全局属性，启动设一次即
  // 对后续所有 loadfile 生效（含后端降级重载），无需在文件加载后再补。
  if (cfg.player_speed != 1.0) {
    DmrCommand sc;
    sc.type = DmrCommand::Type::SetSpeed;
    sc.speed = cfg.player_speed;
    player.on_command(sc);
    FR_LOG_INFO("[APP] 倍速恢复 {}x（settings.json: player.speed）", cfg.player_speed);
  }
  DmrDevice dmr(bus, cfg);
  // —— 多网卡（v0.5.0 / d110）：把参与 SSDP/HTTP 的候选接口全列出来 ——
  // 排查「控制点搜不到设备」的第一现场就是「选错了网卡」，此前只能靠猜（本机真被
  // Hyper-V 虚拟交换机抢过）。顺序即选路优先级：物理网卡在前，虚拟/隧道在后；
  // 列表首项是 ALIVE 广播与跨网段兜底用的主接口。必须在 dmr.start() 之前打印，
  // 这样它与紧接着的「协议栈就绪 ip=…」对照着看，一眼能看出 libupnp 原本选的是哪块。
  {
    const int n = fr_net_if_scan();
    FR_LOG_INFO("[APP] 候选网络接口 {} 个（顺序=优先级，主接口={}）", n,
                n > 0 ? fr_net_if_ip(0) : "无");
    for (int i = 0; i < n; ++i) {
      FR_LOG_INFO("[APP]   接口[{}] ip={} mask={} ifindex={} name={}", i,
                  fr_net_if_ip(i) ? fr_net_if_ip(i) : "?",
                  fr_net_if_mask(i) ? fr_net_if_mask(i) : "?",
                  fr_net_if_index(i),
                  fr_net_if_name(i) ? fr_net_if_name(i) : "?");
    }
    if (n == 0) {
      FR_LOG_WARN("[APP] 未枚举到可用 IPv4 接口 —— SSDP/HTTP 退回单接口行为");
    }
  }
  if (!dmr.start()) {
    FR_LOG_WARN("[APP] DMR 启动失败（检查端口占用/防火墙），UI 继续运行");
  }
  keep_awake::set(true);

  // —— 跨线程通道 + 渲染线程 ——
  ViewStateChannel channel;
  RenderLoop render(window, bus, player, channel, theme);
  render.set_friendly_name(cfg.friendly_name.c_str());
  // d44→d146：show_fps/show_video_fps 为无菜单项的诊断键（决策点 7，settings.json
  // 直开）；show_info = 三合一媒体信息徽章总开关（唯一菜单勾选项）；
  // cast_auto_fullscreen 初值同步给右键菜单勾选态。
  render.set_show_fps(cfg.ui_show_fps);
  render.set_show_video_fps(cfg.ui_show_video_fps);
  render.set_show_info(cfg.ui_show_info);
  render.set_cast_auto_fullscreen(cfg.ui_cast_auto_fullscreen);
  if (cfg.ui_show_fps || cfg.ui_show_video_fps)
    FR_LOG_INFO("[APP] 帧率诊断开启：ui={} video={}", cfg.ui_show_fps, cfg.ui_show_video_fps);
  // 缩略图预览（d27）：worker 出目标帧 jpg → 渲染线程 nvgCreateImageMem 上传。
  // 失败不致命：预览退化为占位框。worker 线程不碰 GL，先于渲染线程启动无碍。
  ThumbPreview thumb;
  // FR_NO_THUMB=1：临时二分开关（排查渲染崩溃用）
  if (!getenv("FR_NO_THUMB") && thumb.init(paths::app_data_dir() + "/cache/thumb")) {
    render.set_thumb(&thumb);
  } else {
    FR_LOG_WARN("[APP] 缩略图预览初始化失败（悬停进度条无预览图）");
  }
  window.release_context();   // 交出 GL 上下文（GLFW 要求先在旧线程 make null）
  render.start();             // 渲染线程接管：make current → 初始化 GL 资源 → 循环

  // —— d74 剪贴板链接 → mpv 探测 → 提示条（settings.json: clipboard.play）——
  // Win 事件模式（系统级监听，免前台）；Linux/macOS 返回 false → 主循环 1s 轮询。
  UrlProbe url_probe;
  bool clip_event_mode = false;
  bool tray_active = false;  // d81：托盘可用 = 关闭窗口改隐到托盘（DMR 持续在线）
  std::string clip_last_url;          // 最近提交探测的 URL（2s 短窗：同值连发事件吞掉；
                                      //   长期按文本去重会封死「忽略后重贴同一链接」，d74b 改）
  double clip_last_url_at = -1e9;
  std::string clip_last_ok;           // 提示条当前展示的 URL（播放动作取值真值）
  std::string clip_played_url;        // 最近点「播放」的 URL（10 分钟免打扰只对它生效）
  double clip_played_at = -1e9;
  double clip_poll_at = 0;
  auto clip_on_text = [&](const std::string& text) {
    if (text.empty()) return;
    const std::string url = extract_clipboard_url(text);
    if (url.empty()) return;
    const double now = mono_now();
    if (url == clip_last_url && now - clip_last_url_at < 2.0) return;  // 单次复制连发
    clip_last_url = url;
    clip_last_url_at = now;
    if (url == clip_played_url && now - clip_played_at < 600) return;  // 已播过的免打扰
    FR_LOG_INFO("[CLIP] 提交探测 {}", url);
    url_probe.submit(url);
  };
  if (cfg.clipboard_play) {
    clip_event_mode = clipboard::start_listener(window.raw());
    // 事件由 window_glfw 的 wndproc 收 WM_CLIPBOARDUPDATE 后触发本 lambda
    window.on_clipboard_update = [&] { clip_on_text(clipboard::get_text(window.raw())); };
    // d79：启动即检一次剪贴板——复制好的链接不必再复制一遍才出提示
    clip_on_text(clipboard::get_text(window.raw()));
    FR_LOG_INFO("[CLIP] 剪贴板监听已启动（{}）",
                clip_event_mode ? "Win 事件模式" : "1s 轮询模式");
  }

  // —— 输入采集（主线程）——
  ViewInput& in = channel.writable();
  double prev_now = mono_now();
  // 起始时刻即视为"刚有过输入"：ViewInput::last_input 默认 0，而 now 取的是单调时钟
  // （数值很大），若不初始化，idle_sec 一上来就是天文数字 → 播放态 chrome 直接处于
  // 淡出终态，用户投屏后第一眼看到的就是"按钮全无"（d19 修）。
  in.last_input = prev_now;
  in.now = prev_now;

  // 画中画态（无 chrome，点击画面退出）
  bool pip_mode = false;
  bool pip_had_picture = false;                     // 小窗内出现过画面（消失则自动退回大窗）
  int pip_x = 0, pip_y = 0, pip_w = 0, pip_h = 0;   // 进入前窗口矩形
  constexpr int kPipW = 420, kPipH = 236;           // 16:9 小窗
  // 画中画的"单击恢复 vs 按住拖动"判定（d25，用户定的两条规则）：
  //   1. 弹起位置与按下位置相距 > kPipDragPx → 拖动意图（进入 OS 拖拽）；
  //   2. 按下超过 200ms 仍未弹起 → 拖动意图。
  //   都不满足（快速原地按下弹起）→ 恢复窗口。
  bool pip_pressed = false;                          // 左键已按下且尚未判定
  bool pip_drag = false;                             // 已进入 OS 拖拽（本次按下不再恢复）
  double pip_press_t = 0;                            // 按下时刻
  float pip_press_x = 0, pip_press_y = 0;            // 按下位置
  constexpr float kPipDragPx = 6.f;                  // 防抖范围（物理像素，与边缘缩放带同量级）

  // 最大化前的"还原矩形"（d68 §7：最大化态拖标题栏 = 还原并跟手）。
  // 进入最大化那一刻捕获；拖还原跟手时用它定位还原后的窗口。
  int norm_x = 0, norm_y = 0, norm_w = 0, norm_h = 0;

  // —— 统一窗口拖拽状态机（d39：全平台一致，纯 GLFW API）——
  // 移动 = 顶栏空白区/画中画小窗按下拖动；缩放 = 边缘带按下拖动（几何与原
  // WM_NCHITTEST 版一致）。渲染线程降级沿用 resizing_ 标记（缩放中冻结 FBO 重建）。
  bool win_move_drag = false;                        // 移动中（顶栏/画中画小窗）
  // d90：本次移动已交给合成器（Wayland 原生 xdg_toplevel_move）。为 true 时
  // 本地跟手必须整体让位 —— 该后端下 glfwSetWindowPos 是空操作，且与合成器的
  // 抓取抢同一件事。拖拽结束（弹起）时清零。
  bool win_move_system = false;
  bool win_resize_drag = false;                      // 边缘缩放中
  WindowGLFW::WinEdge resize_edge = WindowGLFW::WinEdge::None;
  bool drag_global = false;                          // 锚定方式：true=全局光标（首选）
  int drag_gx0 = 0, drag_gy0 = 0;                    // 按下时全局光标屏幕坐标（锚点）
  float drag_px0 = 0, drag_py0 = 0;                  // 按下时光标的物理屏幕坐标（回退锚点）
  int drag_wx0 = 0, drag_wy0 = 0;                    // 按下时窗口屏幕位置
  int drag_ww0 = 0, drag_wh0 = 0;                    // 按下时窗口尺寸
  constexpr int kWinMinW = 320, kWinMinH = 180;      // 缩放下限
  // 回退锚定：GLFW 光标坐标是相对窗口的——窗口一动基准就变。换算成物理屏幕
  // 坐标（客户区坐标 + 当前窗口位置）。仅当取不到全局光标时用（X11 上窗口
  // 位置异步生效，拖动期间读到旧值会漂移——X11 必须走全局光标路径）。
  auto cursor_phys = [&](double x, double y, int* ox, int* oy) {
    int wx, wy, ww, wh;
    window.pos_size(&wx, &wy, &ww, &wh);
    *ox = int(x) + wx;
    *oy = int(y) + wy;
  };
  auto win_drag_start_move = [&]() {
    if (win_move_system) return;                     // 已交给合成器，勿重复抓取
    // d90：Wayland 下本地跟手无效（协议不允许客户端定位窗口）→ 优先把移动整体
    // 交给合成器。成功即返回，不设锚点不记状态：后续 on_mouse_move 只需让位。
    if (window.begin_system_move(false)) {
      win_move_drag = true;
      win_move_system = true;
      return;
    }
    win_move_drag = true;
    int wx, wy, ww, wh;
    window.pos_size(&wx, &wy, &ww, &wh);
    // 锚点二选一：全局光标（X11 首选，与窗口位置无关）失败则回退物理换算。
    // 全局成功时把 px0/py0 直接覆写为全局值 —— 两条路径共用同一套 dx/dy 数学。
    if ((drag_global = WindowGLFW::global_cursor(&drag_gx0, &drag_gy0))) {
      drag_px0 = float(drag_gx0); drag_py0 = float(drag_gy0);
    } else {
      drag_px0 = in.mx + wx; drag_py0 = in.my + wy;
    }
    drag_wx0 = wx; drag_wy0 = wy; drag_ww0 = ww; drag_wh0 = wh;
  };
  auto win_drag_start_resize = [&](WindowGLFW::WinEdge e) {
    win_resize_drag = true;
    resize_edge = e;
    int wx, wy, ww, wh;
    window.pos_size(&wx, &wy, &ww, &wh);
    if ((drag_global = WindowGLFW::global_cursor(&drag_gx0, &drag_gy0))) {
      drag_px0 = float(drag_gx0); drag_py0 = float(drag_gy0);
    } else {
      drag_px0 = in.mx + wx; drag_py0 = in.my + wy;
    }
    drag_wx0 = wx; drag_wy0 = wy; drag_ww0 = ww; drag_wh0 = wh;
    window.set_resizing(true);                       // 渲染线程：缩放中冻结 FBO 重建
  };

  // —— 面板醒睡 + 光标显隐（d36）——
  // 面板唤醒**唯一**来源 = 鼠标移动（带防抖：位移超过 kMouseWakePx 才算有效）。
  // 键盘 / 点击 / 滚轮 / 手机端 DMR 命令（含投屏进入）一律不唤醒面板 ——
  // 动作反馈走 OSD，沉浸画面不被打扰（用户定值）。光标停在底栏热区上时
  // ui 层 hit.any() 仍会强制显示面板（悬停即亮，不受防抖影响）。
  bool chrome_awake = true;              // 面板醒（主线程镜像；ui 层按 last_input 算 fade）
  float wake_ax = 0, wake_ay = 0;        // 转睡时的光标位置（防抖基准点）
  constexpr float kMouseWakePx = 6.f;    // 唤醒防抖阈值（物理像素，与画中画 kPipDragPx 同口径）
  bool mouse_inside = true;              // 光标是否在窗口客户区内（on_mouse_enter 维护）
  bool cursor_hidden = false;            // 当前是否已隐藏光标

  // 任何输入都唤醒渲染线程（保证即时跟手）。
  // ⚠️ 必须 publish()：in 是 channel.writable() 返回的**主线程自有缓冲**，
  // 渲染线程只有经 publish() 才能看到（不 publish 等于输入石沉大海 ——
  // 曾导致"鼠标一动不动的悬停态永不出现、两栏永远停在 idle 淡出态"）。
  auto touch = [&]() {
    in.now = mono_now();
    in.last_input = in.now;
    chrome_awake = true;
    channel.publish();
    render.wake();
  };
  // 仅发布、不当作"用户输入"（键盘/点击/滚轮/DMR 命令不唤醒面板：d36 用户定值）
  auto publish_only = [&]() {
    in.now = mono_now();
    channel.publish();
    render.wake();
  };
  // d47：发布 + 唤醒（渲染线程已改按需睡眠，不再被主循环每 8ms 踢醒 ——
  // 一切改动点必须自行 publish+wake，漏一个 = 渲染线程睡着看不见这个变化）。
  // 所有原先裸调 channel.publish() 的地位一律换成它。
  auto publish_wake = [&]() {
    channel.publish();
    render.wake();
  };

  window.on_mouse_move = [&](double x, double y) {
    in.mx = (float)x;
    in.my = (float)y;
    // —— 统一窗口拖拽（d39）：全局光标锚定跟随（X11 免疫异步 move），纯 GLFW API ——
    // px/py 取当前光标的锚定坐标系坐标（全局成功=屏幕坐标，否则物理换算），数学同构。
    if (win_move_drag) {
      // d90：合成器接管中 —— 本地跟手整体让位（Wayland 上 set_pos 是空操作，
      // 在这里算出来的坐标只会被丢弃；X11/Win32 不会走到这个分支）。
      if (win_move_system) {
        publish_wake();
        return;
      }
      int px, py;
      if (drag_global) {
        if (!WindowGLFW::global_cursor(&px, &py)) { publish_wake(); return; }
      } else {
        cursor_phys(x, y, &px, &py);
      }
      window.set_pos(drag_wx0 + px - int(drag_px0), drag_wy0 + py - int(drag_py0));
      publish_wake();
      return;
    }
    if (win_resize_drag) {
      int px, py;
      if (drag_global) {
        if (!WindowGLFW::global_cursor(&px, &py)) { publish_wake(); return; }
      } else {
        cursor_phys(x, y, &px, &py);
      }
      const int dx = px - int(drag_px0), dy = py - int(drag_py0);
      int nx = drag_wx0, ny = drag_wy0, nw = drag_ww0, nh = drag_wh0;
      if (resize_edge == WindowGLFW::WinEdge::W ||
          resize_edge == WindowGLFW::WinEdge::NW || resize_edge == WindowGLFW::WinEdge::SW) {
        nw = std::max(kWinMinW, drag_ww0 - dx);
        nx = drag_wx0 + (drag_ww0 - nw);           // 左缘追光标
      } else if (resize_edge == WindowGLFW::WinEdge::E ||
                 resize_edge == WindowGLFW::WinEdge::NE || resize_edge == WindowGLFW::WinEdge::SE) {
        nw = std::max(kWinMinW, drag_ww0 + dx);
      }
      if (resize_edge == WindowGLFW::WinEdge::N ||
          resize_edge == WindowGLFW::WinEdge::NW || resize_edge == WindowGLFW::WinEdge::NE) {
        nh = std::max(kWinMinH, drag_wh0 - dy);
        ny = drag_wy0 + (drag_wh0 - nh);
      } else if (resize_edge == WindowGLFW::WinEdge::S ||
                 resize_edge == WindowGLFW::WinEdge::SW || resize_edge == WindowGLFW::WinEdge::SE) {
        nh = std::max(kWinMinH, drag_wh0 + dy);
      }
      window.set_pos(nx, ny);
      window.set_window_size(nw, nh);
      publish_wake();
      return;
    }
    // 画中画：按住且移动超出防抖范围 → 进入统一拖拽（移动小窗）
    if (pip_mode && pip_pressed && !pip_drag && in.down) {
      const float dx = (float)x - pip_press_x, dy = (float)y - pip_press_y;
      if (dx * dx + dy * dy > kPipDragPx * kPipDragPx) {
        pip_pressed = false;
        pip_drag = true;                            // 弹起时不再判定"恢复"
        win_drag_start_move();                      // d39：不再走 OS 模态拖拽
        return;
      }
    }
    // 边缘带光标暗示（非拖拽、非 pip、窗口态）：贴边/贴角即换 resize 光标
    if (!pip_mode && !window.fullscreen() && !window.maximized())
      window.update_cursor_for_edge(
          WindowGLFW::hit_window_edge(window.width(), window.height(), in.mx, in.my));
    // 面板唤醒防抖（d36）：面板隐藏后光标位移超过阈值才算"有效移动"；
    // 微动只更新光标位置（悬停判定仍工作），面板保持隐藏。
    if (!chrome_awake) {
      const float dx = (float)x - wake_ax, dy = (float)y - wake_ay;
      if (dx * dx + dy * dy > kMouseWakePx * kMouseWakePx)
        touch();              // 有效移动 → 唤醒面板 + 恢复光标（主循环统一管理显隐）
      else
        publish_only();
      return;
    }
    touch();
  };
  window.on_mouse_btn = [&](int button, int action) {
    publish_only();           // d36：点击不唤醒面板（动作反馈走 OSD；热区悬停本身已亮面板）
    if (button == GLFW_MOUSE_BUTTON_MIDDLE) {
      in.down_middle = action == GLFW_PRESS;
      publish_wake();
      return;
    }
    if (button == GLFW_MOUSE_BUTTON_RIGHT) {
      // d45：右键状态透传给渲染线程（按下/弹起边沿检测、防抖与开菜单都在那边 ——
      // 菜单几何/绘制/hit-test 与 chrome 同源，归渲染线程）
      in.down_right = action == GLFW_PRESS;
      publish_wake();
      return;
    }
    if (button != GLFW_MOUSE_BUTTON_LEFT) {
      // d68 §7（采纳）：鼠标侧键 XBUTTON1/2 = 上一集/下一集（GLFW 按钮 4/5，
      // 按下沿触发；无会话时 Prev/Next 意图本身无动作，见 exec_intent）。
      if (button == GLFW_MOUSE_BUTTON_4 || button == GLFW_MOUSE_BUTTON_5) {
        if (action == GLFW_PRESS) {
          channel.post_intent(UiIntent{button == GLFW_MOUSE_BUTTON_4
                                           ? UiIntent::Kind::Prev
                                           : UiIntent::Kind::Next});
          publish_wake();
        }
      }
      return;
    }
    in.down = action == GLFW_PRESS;
    if (pip_mode) {
      if (action == GLFW_PRESS) {
        // 画中画：记录按下，等待"移动/超时 → 拖动"或"原地弹起 → 恢复"
        pip_pressed = true;
        pip_drag = false;
        pip_press_t = mono_now();
        pip_press_x = in.mx;
        pip_press_y = in.my;
      } else if (win_move_drag) {
        // 统一拖拽结束（d39）：拖动后弹起，不再判定"恢复"
        win_move_drag = false;
        win_move_system = false;                     // d90：合成器抓取在弹起时自然结束
        pip_pressed = false;
        pip_drag = false;
      } else if (pip_pressed) {
        // 原地快速弹起（未超时、未越出防抖范围）→ 恢复窗口（经意图队列，
        // exec_intent 定义在本回调之后，此处不可直接调用）
        pip_pressed = false;
        channel.post_intent(UiIntent{UiIntent::Kind::Pip});
        publish_wake();
        return;
      }
      publish_wake();
      return;
    }
    // 统一拖拽结束（d39）：弹起即收
    if (action == GLFW_RELEASE) {
      if (win_move_drag) {
        win_move_drag = false;
        win_move_system = false;                     // d90：合成器抓取在弹起时自然结束
        publish_wake();
        return;
      }
      if (win_resize_drag) {
        win_resize_drag = false;
        resize_edge = WindowGLFW::WinEdge::None;
        window.set_resizing(false);                 // 渲染线程恢复常态
        publish_wake();
        return;
      }
    }
    // —— 统一窗口拖拽（d39）：边缘缩放优先于顶栏移动（角落带盖过顶栏），与
    // 原 WM_NCHITTEST 优先序一致；全屏/最大化不进入 ——
    if (in.down && !window.fullscreen() && !window.maximized()) {
      const WindowGLFW::WinEdge e =
          WindowGLFW::hit_window_edge(window.width(), window.height(), in.mx, in.my);
      if (e != WindowGLFW::WinEdge::None) {
        win_drag_start_resize(e);
        publish_wake();
        return;
      }
    }
    // （d68）顶栏「按下即拖」已废除：窗口移动改由渲染线程三态机的 DragMove
    // 意图（超 kDragPx 防抖阈值）触发，见 exec_intent —— 按下→原地弹起不再
    // 进入拖拽态，顶栏单击/双击/拖动三者由此互斥（UI_INTERACTION_PLAN §1#1）。
    publish_wake();
  };
  // 画中画"按住 200ms 未弹起 = 拖动意图"（用户规则 2）：主循环巡检兜底
  auto pip_hold_check = [&]() {
    if (pip_mode && pip_pressed && !pip_drag && in.down &&
        mono_now() - pip_press_t > 0.2) {
      pip_pressed = false;
      pip_drag = true;
      win_drag_start_move();                            // d39：不再走 OS 模态拖拽
      publish_wake();
    }
  };
  // 通用小步进动作（键 + 滚轮共用；滚轮回调在 on_key 之后也需要它们）
  // OSD 触发统一入口（d28 问题 3）：同类型同值且旧面板还在显示期 → 不重新淡入
  //（直接置为"已完全出现"，静默续期），消除"画面还在又闪一次"的重触发闪烁。
  // d32：dir 参数 = 进度方向（触发时定格，防 seek 完成后按实时位置误判）；
  // kind3 = 动作反馈，value 直接编码动作 id（1 播放 2 暂停 3 进全屏 4 退全屏 5/6 画中画）。
  // d32 修复（音量 OSD 闪烁回归）：d32 曾把续期收紧为"同 kind+同 value+同 dir"，
  // 但 kind1/2 连发时 value/dir 每次都变（音量滚轮每格 +1、方向键 ±5s）→ renew 永不
  // 成立 → 每次触发重置 osd_at 从 0 淡入 → 闪烁。改为：kind1/2 属持续调节型面板，
  // 显示期内一律续期（内容实时更新，不重置淡入）；kind3 才是离散动作事件，
  // 同 value 重复触发续期、value 变化 = 新事件重新淡入（面板宽度随标签变化）。
  auto show_osd = [&](int kind, double value, int dir = 1) {
    const double now = mono_now();
    const bool panel = kind == 1 || kind == 2;
    const bool renew = in.osd_kind == kind && (panel || in.osd_value == value) &&
                       now - in.osd_at < theme.layout.osdFadeIn + theme.layout.osdHold;
    if (renew) {
      in.osd_at = now - theme.layout.osdFadeIn;
    } else {
      in.osd_at = now;
    }
    in.osd_kind = kind;
    in.osd_value = value;
    in.osd_dir = dir;
    // d47：OSD 状态入 master_，必须自行发布+唤醒（原先靠主循环每 8ms 的
    // 无条件 publish 兜底出画；调用点在 publish_only 之后改动的路径同样被覆盖）。
    publish_wake();
  };
  auto play_pause = [&]() {
    PlaybackSnapshot s = bus.snapshot();
    DmrCommand c;
    c.type = s.state == TransportState::Playing ? DmrCommand::Type::Pause
                                                : DmrCommand::Type::Play;
    player.on_command(c);
    // d32：动作反馈 OSD（kind3：1=播放 2=暂停）
    show_osd(3, s.state == TransportState::Playing ? 2 : 1);
  };
  auto nudge_volume = [&](int delta) {
    PlaybackSnapshot s = bus.snapshot();
    int v = s.volume + delta;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    if (v == s.volume) return;
    DmrCommand c;
    c.type = DmrCommand::Type::SetVolume;
    c.volume = v;
    player.on_command(c);
    show_osd(1, v);           // d28：音量 OSD（键/滚轮/拖拽释放共用）
  };
  // 键盘拖动进度状态（d29）：左右键按住 = 进度条动 + 预览，**不 seek**；抬起才提交
  bool kb_progress = false;
  float kb_progress_value = 0.f;
  constexpr double kKbPreviewHoldSec = 0.6;  // 抬起后预览保留时长（防单发闪烁）
  window.on_mouse_enter = [&](bool entered) {
    mouse_inside = entered;   // d36：光标显隐前提之一；离开窗口立即恢复光标显示
    publish_only();
  };
  window.on_key = [&](int key, int action, int mods) {
    // —— 左右键抬起：键盘拖动结束，此刻才真正 seek（d29 问题 1）——
    if (action == GLFW_RELEASE) {
      if (kb_progress && (key == GLFW_KEY_LEFT || key == GLFW_KEY_RIGHT)) {
        kb_progress = false;
        PlaybackSnapshot s = bus.snapshot();
        if (s.duration > 0) {
          const double target = kb_progress_value * s.duration;
          DmrCommand c;
          c.type = DmrCommand::Type::Seek;
          c.seek_to = target;
          player.on_command(c);
          show_osd(2, target, target > s.position + 0.25 ? 1 : 0);  // 方向触发时定格（d32）
        }
        in.kb_progress = false;
        in.kb_preview_until = mono_now() + kKbPreviewHoldSec;  // 预览延迟消失
      }
      publish_only();
      return;
    }
    if (action != GLFW_PRESS && action != GLFW_REPEAT) return;
    // 连发（REPEAT）只对音量/进度步进键有意义：连按空格=连切播放/暂停、Ctrl+Enter
    // 连切全屏都不可接受
    const bool step_key = key == GLFW_KEY_LEFT || key == GLFW_KEY_RIGHT;
    if (action == GLFW_REPEAT && !step_key && key != GLFW_KEY_UP && key != GLFW_KEY_DOWN) return;
    // 进度步进（拖动语义）：d36 键盘彻底不唤醒面板 —— publish_only 发布预览
    //（chrome.cpp 侧进度条/时间/缩略图用 track_alpha 独立于面板 fade 绘制）
    if (step_key && bus.snapshot().duration > 0) {
      publish_only();
      if (!kb_progress) {
        kb_progress = true;
        kb_progress_value = (float)(bus.snapshot().position / bus.snapshot().duration);
      }
      const float step = (key == GLFW_KEY_RIGHT ? 5.f : -5.f) /
                         (float)bus.snapshot().duration;
      kb_progress_value = std::clamp(kb_progress_value + step, 0.f, 1.f);
      in.kb_progress = true;
      in.kb_progress_value = kb_progress_value;
      in.kb_preview_until = 0;  // 按住期间预览常显（由 kb_progress 驱动）
      return;
    }
    publish_only();
    switch (key) {
      case GLFW_KEY_SPACE:   play_pause(); break;
      case GLFW_KEY_UP:      nudge_volume(+5); break;
      case GLFW_KEY_DOWN:    nudge_volume(-5); break;
      case GLFW_KEY_ENTER:
      case GLFW_KEY_KP_ENTER:
        if (mods & GLFW_MOD_CONTROL) {
          window.toggle_fullscreen();
          show_osd(3, window.fullscreen() ? 3 : 4);   // d32：全屏动作反馈
        }
        break;
      case GLFW_KEY_ESCAPE:
        // 全屏 / 画中画态：ESC = 回窗口模式（d29 用户定值）；普通窗口态仍为关闭
        if (pip_mode) {
          show_osd(3, 6);                             // d32：退出画中画反馈
          channel.post_intent(UiIntent{UiIntent::Kind::Pip});
          publish_wake();
        } else if (window.fullscreen()) {
          window.toggle_fullscreen();
          show_osd(3, 4);                             // d32：退出全屏反馈
        } else {
          // d45：窗口态 ESC = 先关右键菜单；菜单没开时渲染线程回投 Close（原关窗行为）
          render.request_menu_dismiss();
        }
        break;
      default: break;
    }
  };
  // 滚轮 → 音量（d25：一格 ±5，与方向键同步长，任意位置可用）；d36 不唤醒面板
  window.on_scroll = [&](double, double y) {
    publish_only();
    if (y == 0) return;
    nudge_volume(y > 0 ? +5 : -5);
  };
  // 系统请求重绘（模态循环内唯一出帧通道）：v0.4.0 d56 起升级为全窗重绘请求
  //（表面可能已失效，必须整窗补画 —— render_loop 归因表里 forced = add_all）
  window.on_refresh = [&] { render.request_full_repaint(); };
  // d92：显窗后必须补一帧整窗重绘 —— Wayland 的 glfwShowWindow 不提交缓冲，
  // 按需渲染下静默帧又不出图，不补这一帧窗口会空等 2s 心跳才露面
  //（详见 window_glfw.cpp 的 show_window 注释）。挂在 on_shown 上，显窗点无一可漏。
  window.on_shown = [&] { render.request_full_repaint(); };

  // —— 意图执行（渲染线程 → 主线程）——
  auto exec_intent = [&](const UiIntent& it) {
    switch (it.kind) {
      case UiIntent::Kind::Close:
        // d68 §5：先隐窗再收尾 —— 视觉秒退（DWM 立即停止合成，GPU/CPU 即时归零），
        // BYEBYE 等网络清理由主循环退出后的收尾段原样执行（顺序铁律不动）。
        // 三入口（三键/右键菜单/ESC 回投）同路。
        // d81：托盘可用 = 关闭窗口改「隐到托盘」（DMR 持续在线可继续投屏），
        // 真正退出只走托盘菜单「退出」；无托盘维持关闭即退出。
        window.hide_window();
        if (!tray_active) glfwSetWindowShouldClose(window.raw(), GLFW_TRUE);
        break;
      case UiIntent::Kind::Minimize:
        glfwIconifyWindow(window.raw());
        break;
      case UiIntent::Kind::ToggleMaximize:
        if (!window.maximized())
          window.pos_size(&norm_x, &norm_y, &norm_w, &norm_h);  // 捕获还原矩形（拖还原跟手用）
        window.toggle_maximized();
        break;
      case UiIntent::Kind::DragMove: {
        // d68 §2：拖动指令统一入口，消费与否按区域决定。目前唯一消费者 = TopBar。
        if (it.region != UiIntent::DragRegion::TopBar) break;  // Stage/Widget：已识别，无动作
        // 边缘缩放带（N 带与顶栏重叠）已接管本次按住 → 不能再启动移动拖拽
        if (win_move_drag || win_resize_drag) break;
        if (window.fullscreen()) break;                        // 全屏无标题栏语义
        if (win_move_system) break;                            // d90：本次已交给合成器
        // d90：Wayland 下窗口位置由合成器掌管 —— 优先请它接管一次原生移动抓取
        // （xdg_toplevel_move）。最大化态传 unmaximize=true：先请求还原再拖，等价于
        // GNOME 原生「拖最大化标题栏 = 还原并跟手」，比客户端拼矩形更稳。
        // 成功即整体接管：下面那些 set_pos 矩形数学在 Wayland 上全是空转。
        // 失败（X11/Win32 后端、或拿不到 serial/toplevel）自动回退原逻辑，零回归。
        if (window.begin_system_move(window.maximized())) {
          win_move_drag = true;
          win_move_system = true;
          publish_wake();
          break;
        }
        if (window.maximized()) {
          // §7（采纳）：最大化态拖标题栏 = 还原并跟手。抓取点在最大化宽度上的
          // 相对位置映射到还原宽度，标题栏保持在光标下不跳变。
          int gx = 0, gy = 0;
          const bool global = WindowGLFW::global_cursor(&gx, &gy);
          float fx = 0.5f;
          int grab_dy = std::max(0, (int)in.my);
          if (global) {
            int wx, wy, ww, wh;
            window.pos_size(&wx, &wy, &ww, &wh);
            if (ww > 0) fx = std::clamp((float)(gx - wx) / (float)ww, 0.f, 1.f);
            grab_dy = std::max(0, gy - wy);
          }
          window.toggle_maximized();  // 还原（Win 同步生效；X11 异步 → 下面的锚定矩形兜底）
          const int tx = gx - (int)(fx * (float)norm_w);
          const int ty = gy - grab_dy;
          window.set_pos(tx, ty);
          // 接管 d39 跟手机：锚 = 计算出的还原矩形 + 当前光标
          win_move_drag = true;
          drag_global = global;
          drag_gx0 = gx; drag_gy0 = gy;
          drag_px0 = (float)gx; drag_py0 = (float)gy;
          drag_wx0 = tx; drag_wy0 = ty; drag_ww0 = norm_w; drag_wh0 = norm_h;
        } else {
          win_drag_start_move();
        }
        break;
      }
      case UiIntent::Kind::ToggleFullscreen:
        window.toggle_fullscreen();
        show_osd(3, window.fullscreen() ? 3 : 4);   // d32：全屏动作反馈
        break;
      case UiIntent::Kind::Pip: {
        if (pip_mode) {
          // 退出小窗：还原矩形与置顶
          pip_mode = false;
          render.set_pip(false);
          window.set_floating(false);
          window.set_pos(pip_x, pip_y);
          window.set_window_size(pip_w, pip_h);
          show_osd(3, 6);                           // d32：退出画中画反馈
        } else {
          // 全屏下直接进画中画是未定义行为：窗口还挂在显示器上（fullscreen 态），
          // 对全屏窗口 set_window_size 实测把整个显示器变黑（d24 用户实测）。
          // 故必须先退全屏（还原到进入前的窗口矩形），再取矩形、再缩进小窗。
          if (window.fullscreen()) window.toggle_fullscreen();
          pip_mode = true;
          render.set_pip(true);
          window.pos_size(&pip_x, &pip_y, &pip_w, &pip_h);
          window.set_floating(true);
          window.set_window_size(kPipW, kPipH);
          window.move_to_workarea_corner(kPipW, kPipH);
          show_osd(3, 5);                           // d32：进入画中画反馈
        }
        break;
      }
      case UiIntent::Kind::PlayPause: {
        PlaybackSnapshot s = bus.snapshot();
        DmrCommand c;
        c.type = s.state == TransportState::Playing ? DmrCommand::Type::Pause
                                                    : DmrCommand::Type::Play;
        player.on_command(c);
        show_osd(3, s.state == TransportState::Playing ? 2 : 1);  // d32：动作反馈
        break;
      }
      case UiIntent::Kind::Prev:
        // 无 playlist 概念（DMR 侧 Previous 恒 710），本地按钮无动作
        break;
      case UiIntent::Kind::Next: {
        // 与 DMR 层 Next action 同语义：有 next_uri 则切过去，否则无动作
        // （本地按钮不做 710 —— 那是 SOAP 侧的约定；按钮点了没反应即可）
        const PlaybackSnapshot s = bus.snapshot();
        if (!s.next_uri.empty()) {
          DmrCommand c;
          c.type = DmrCommand::Type::SetUri;
          c.uri = s.next_uri;
          c.metadata = s.next_metadata;
          player.on_command(c);
        }
        break;
      }
      case UiIntent::Kind::Stop: {
        // d158：停止键（无上/下一集数据时替换 prev/next 两键）——与 DMR 层 Stop action
        // 同语义：走 handle_stop（暂停 + STOPPED + 保持期）；无会话时按钮本就禁用。
        DmrCommand c;
        c.type = DmrCommand::Type::Stop;
        player.on_command(c);
        show_osd(3, 7);  // d32 动作反馈：7 = 停止
        break;
      }
      case UiIntent::Kind::Seek: {
        const PlaybackSnapshot s = bus.snapshot();
        DmrCommand c;
        c.type = DmrCommand::Type::Seek;
        c.seek_to = it.value;
        player.on_command(c);
        show_osd(2, it.value, it.value > s.position + 0.25 ? 1 : 0);  // d32：方向触发时定格
        break;
      }
      case UiIntent::Kind::Volume: {
        DmrCommand c;
        c.type = DmrCommand::Type::SetVolume;
        c.volume = (int)std::lround(it.value * 100);
        player.on_command(c);
        show_osd(1, c.volume);    // d28：音量条拖拽释放同样弹 OSD
        break;
      }
      case UiIntent::Kind::Mute: {
        DmrCommand c;
        c.type = DmrCommand::Type::SetMute;
        c.mute = it.flag;
        player.on_command(c);
        break;
      }
      case UiIntent::Kind::Speed: {
        // d169：倍速选择 —— 落盘 player.speed、下发播放器、OSD 反馈。
        cfg.player_speed = it.value;
        {
          char raw[16];
          std::snprintf(raw, sizeof(raw), "%g", it.value);
          Config::patch(paths::config_file(), {{"player", "speed", raw}});
        }
        DmrCommand c;
        c.type = DmrCommand::Type::SetSpeed;
        c.speed = it.value;
        player.on_command(c);
        show_osd(4, it.value);  // d169：倍速反馈面板（kind4）
        break;
      }
      case UiIntent::Kind::Preview: {
        // 悬停进度条：请求目标位置缩略图（worker latest-wins，高频投递安全）
        const PlaybackSnapshot s = bus.snapshot();
        if (s.duration > 0 && !s.uri.empty() && thumb.ready())
          thumb.request(it.value * s.duration, s.uri);
        break;
      }
      case UiIntent::Kind::DismissMenu:
        // d45：ESC 关菜单（菜单没开 → 渲染线程自己回投 Close，不在主线程判断）
        render.request_menu_dismiss();
        break;
      case UiIntent::Kind::ClipboardPlay: {
        // d74：提示条「播放」——与投屏 SetUri 完全同管线（R1 语义：无条件
        // 接受 + 自动开播）。URL 真值 = 主线程持有的 clip_last_ok。
        if (clip_last_ok.empty()) break;
        DmrCommand c;
        c.type = DmrCommand::Type::SetUri;
        c.uri = clip_last_ok;
        player.on_command(c);
        clip_played_url = clip_last_ok;  // 10 分钟免打扰只记「播过的」
        clip_played_at = mono_now();
        in.clip_visible = false;  // 动作完成即收提示条（regions 侧同帧生效）
        publish_wake();
        break;
      }
      case UiIntent::Kind::ClipboardDismiss:
        in.clip_visible = false;
        publish_wake();
        break;
      case UiIntent::Kind::UiPrefChanged:
        // d146 R1：渲染线程已即时翻转勾选（本帧反馈）；这里补三件事 —— 运行值、
        // 落盘、日志。Config::patch 文本级修补（不洗掉用户手写的未知键），主线程唯一调用方。
        if (it.pref == (int)UiIntent::UiPref::ShowInfo) {
          cfg.ui_show_info = it.flag;
          Config::patch(paths::config_file(),
                        {{"ui", "show_info", it.flag ? "true" : "false"}});
          FR_LOG_INFO("[APP] 媒体信息徽章 = {}（已落盘）", it.flag);
        } else if (it.pref == (int)UiIntent::UiPref::CastAutoFullscreen) {
          cfg.ui_cast_auto_fullscreen = it.flag;
          Config::patch(paths::config_file(),
                        {{"ui", "cast_auto_fullscreen", it.flag ? "true" : "false"}});
          render.set_cast_auto_fullscreen(it.flag);
          FR_LOG_INFO("[APP] 投屏自动全屏 = {}（已落盘）", it.flag);
        }
        break;
    }
    render.wake();
  };

  // —— d81 托盘（platform/tray；Win=Shell_NotifyIcon，Linux=SNI→XEmbed 探测）——
  // 回调全部在主线程触发（Win：GLFW 主循环泵托盘消息窗口）。菜单每次右键
  // 实时构建（标签随播放态/窗口可见态变化）；「退出」= 真退出，关窗 = 隐托盘。
  auto tray_toggle_window = [&] {
    if (glfwGetWindowAttrib(window.raw(), GLFW_VISIBLE)) {
      window.hide_window();
    } else {
      if (glfwGetWindowAttrib(window.raw(), GLFW_ICONIFIED)) glfwRestoreWindow(window.raw());
      window.show_window();
    }
    publish_wake();  // 隐/显改变渲染可见性（隐窗后渲染线程可能沉睡）
  };
  window.on_close_request = [&] {
    // Alt+F4 / 系统关机广播：托盘可用 = 隐到托盘（同关窗语义）；否则默认关闭
    if (!tray_active) return false;
    window.hide_window();
    return true;
  };
  // d117：真退出的统一入口（托盘菜单「退出」与外部协议 tools/quit_app.py 共用）：
  // 先隐窗（d71 视觉秒退）再置 should_close，主循环退出后收尾段照常执行。
  window.on_quit_request = [&] {
    window.hide_window();
    glfwSetWindowShouldClose(window.raw(), GLFW_TRUE);
  };
  // FR_DBG_NO_TRAY=1：跳过托盘安装 → 走"常规窗口模式"（窗口常显）。
  // 只为诊断用：托盘模式下窗口隐着、swap 被 d91 闸门挡住，任何窗口截图都是黑的，
  // 没法肉眼核对画面（尤其"软件渲染到底出没出画"）。与 FR_DBG_SIZE/FR_DBG_DAMAGE 同族。
  // ★副作用（2026-09-17 用户实测踩到）：它同时改变**关闭语义** —— 没有托盘 ⇒
  //   `on_close_request` 返回 false、自绘 ✕ 走 `glfwSetWindowShouldClose` ⇒ **关窗即真退出**，
  //   而不是"隐到托盘"。诊断完记得去掉这个开关再让用户碰。
  const bool dbg_no_tray = std::getenv("FR_DBG_NO_TRAY") != nullptr;
  tray_active = !dbg_no_tray && tray::create(
      window.raw(), "FlashRec 投屏接收", paths::asset_file("icons/app_32.png").c_str(),
      TrayCallbacks{
          .menu_provider =
              [&] {
                std::vector<TrayMenuItem> items;
                TrayMenuItem show;
                show.id = 1;
                show.label = glfwGetWindowAttrib(window.raw(), GLFW_VISIBLE)
                                 ? std::string("隐藏主窗口")
                                 : std::string("显示主窗口");
                items.push_back(show);
                items.push_back(TrayMenuItem::Sep());
                PlaybackSnapshot s = bus.snapshot();
                TrayMenuItem pp;
                pp.id = 2;
                pp.label = s.state == TransportState::Playing ? std::string("暂停")
                                                              : std::string("播放");
                pp.enabled = !s.uri.empty();
                items.push_back(pp);
                TrayMenuItem nx;
                nx.id = 3;
                nx.label = "下一集";
                items.push_back(nx);
                items.push_back(TrayMenuItem::Sep());
                {  // d146 R4：投屏窗口 —— 每块可用屏一项（选屏判断失手时的人工兜底硬保障）
                  MonitorInfo ms[8];
                  const int n2 = collect_monitors(ms, 8);
                  for (int i = 0; i < n2; ++i) {
                    TrayMenuItem mw;
                    mw.id = 100 + ms[i].idx;
                    char lbl[96];
                    std::snprintf(lbl, sizeof(lbl), "投到屏幕 %d（%d×%d）%s", i + 1,
                                  ms[i].w, ms[i].h, ms[i].primary ? "· 主屏" : "");
                    mw.label = lbl;
                    items.push_back(mw);
                  }
                  if (n2 > 0) items.push_back(TrayMenuItem::Sep());
                }
                TrayMenuItem quit;
                quit.id = -1;
                quit.label = "退出";
                items.push_back(quit);
                return items;
              },
          .on_menu_action =
              [&](int id) {
                switch (id) {
                  case 1:
                    tray_toggle_window();
                    break;
                  case 2:
                    play_pause();
                    break;
                  case 3:
                    exec_intent({UiIntent::Kind::Next});
                    break;
                  case -1:  // 退出：统一走 on_quit_request（隐窗 + 置关闭，d117）
                    if (window.on_quit_request) window.on_quit_request();
                    break;
                  default:
                    if (id >= 100) {  // d146 R4：托盘「投到屏幕 N」→ 迁移窗口到指定屏
                      MonitorInfo ms[8];
                      const int n2 = collect_monitors(ms, 8);
                      for (int i = 0; i < n2; ++i) {
                        if (ms[i].idx != id - 100) continue;
                        if (window.fullscreen()) window.toggle_fullscreen();
                        if (window.maximized()) window.toggle_maximized();
                        apply_placement(window, ms[i], mem_for(cfg, ms[i].fp));
                        ensure_window_shown(window);
                        publish_wake();
                        break;
                      }
                    }
                    break;
                }
              },
          .on_toggle_window =
              [&] {
                tray_toggle_window();
              },
      });
  if (tray_active) {
    FR_LOG_INFO("[APP] 托盘模式：启动即隐窗（托盘唤回），关窗=隐托盘，退出走托盘菜单");
  } else {    // 托盘没装上：恢复显示窗口（启动隐窗只为消除初始化闪现）
    window.show_window();
    FR_LOG_INFO("[APP] 无托盘：常规窗口模式");
  }

  // —— d146 R2/R3/R4：投屏呈现管线（cast 事件 → 2s 防抖 → P1 选屏 → P2 摆位
  //     → P3 显示 → P4 自动全屏；P5 记忆写回在主循环几何去抖段 + 退出兜底）——
  std::string cast_last_uri;   // 上次管线触发的 URI（去 #fragment）
  double cast_last_at = -1e9;
  auto run_cast_pipeline = [&](const CastEvent& ce) {
    const double tnow = mono_now();
    const size_t frag = ce.uri.find('#');
    const std::string u = frag == std::string::npos ? ce.uri : ce.uri.substr(0, frag);
    if (!u.empty() && u == cast_last_uri && tnow - cast_last_at < 2.0) {
      FR_LOG_INFO("[CAST] 2s 内重复推送，合并（duplicate={}）", ce.duplicate);
      return;  // d144 实证控制点 390ms 连推；合并后不重复触发显窗/全屏
    }
    cast_last_uri = u;
    cast_last_at = tnow;
    FR_LOG_INFO("[CAST] 呈现管线触发 duplicate={} uri={}", ce.duplicate, ce.uri);
    if (pip_mode) {  // 画中画 → 先退回窗口再走管线（Pip↔全屏互斥同款）
      UiIntent it{UiIntent::Kind::Pip};
      exec_intent(it);
    }
    MonitorInfo ms[8];
    const int n = collect_monitors(ms, 8);
    const int cur = window.monitor_index_of_window();
    const bool visible = glfwGetWindowAttrib(window.raw(), GLFW_VISIBLE) &&
                         !glfwGetWindowAttrib(window.raw(), GLFW_ICONIFIED);
    const MonitorInfo* target = pick_target(cfg, ms, n, cur);
    if (window.fullscreen()) {
      // 已全屏：目标屏 = 当前屏 ⇒ 不动作（不闪屏）；目标屏不同 ⇒ 退全屏再迁移
      if (!target || target->idx == cur) {
        FR_LOG_INFO("[CAST] 已全屏且目标屏未变，无动作");
        return;
      }
      window.toggle_fullscreen();
    }
    if (!target) {
      if (!visible) window.show_window();  // 无可用屏：至少把窗口显出来（R3）
      publish_wake();
      return;
    }
    if (!visible || target->idx != cur) {
      if (window.maximized()) window.toggle_maximized();  // 最大化先还原再摆位
      apply_placement(window, *target, mem_for(cfg, target->fp));
    }
    ensure_window_shown(window);  // P3：显窗无条件（R3）；面板不弹（d36 语义保持）
    if (cfg.ui_cast_auto_fullscreen && !window.fullscreen())
      window.enter_fullscreen_on(target->idx);  // P4：已在目标屏全屏 = no-op（不闪屏）
    publish_wake();  // d47：显窗/形态变化必须自行发布+唤醒
    FR_LOG_INFO("[CAST] 目标屏 {}（{}×{}）摆位完成", target->idx, target->w, target->h);
  };
  // d146：显示器拓扑变化自愈 —— 窗口可见但所在屏已被拔/失效 → 按 prefer 链迁移
  //（glfwSetMonitorCallback 在主线程 poll_events 内回调，可安全触碰窗口）。
  window.on_monitors_changed = [&](int) {
    if (!glfwGetWindowAttrib(window.raw(), GLFW_VISIBLE)) return;
    if (window.monitor_index_of_window() >= 0) return;  // 所在屏仍可用
    MonitorInfo ms[8];
    const int n = collect_monitors(ms, 8);
    const MonitorInfo* tgt = pick_target(cfg, ms, n, -1);
    if (!tgt) return;
    if (window.maximized()) window.toggle_maximized();
    apply_placement(window, *tgt, mem_for(cfg, tgt->fp));
    publish_wake();
    FR_LOG_WARN("[APP] 所在屏已失效 → 迁移到屏 {}（{}×{}）", tgt->idx, tgt->w, tgt->h);
  };

  // —— 主循环（消息线程）——
  // d47 按需发布基准：上次发布给渲染线程的输入与快照版本（见主循环步骤 8）
  ViewInput last_pub_{};
  uint64_t last_pub_ver = 0;
  // d52：主循环快照也按版本缓存（d50 渲染侧同款）——每轮 bus.snapshot() 的
  // mutex + 多字符串整份拷贝改为版本不变时复用缓存。
  PlaybackSnapshot snap_cache{};
  uint64_t snap_ver_seen = 0;
  // d146 P5：窗口几何去抖持久化状态（变化后静止 800ms 才落盘，拖动过程不写盘）
  int geo_x = 0, geo_y = 0, geo_w = 0, geo_h = 0;
  bool geo_fs = false, geo_mx = false, geo_valid = false, geo_dirty = false;
  double geo_settle_at = 0;
  // d163：音量/静音去抖持久化（同 P5 口径：变化后静止 800ms 才落盘）
  int vol_last = cfg.player_volume;
  bool mute_last = cfg.player_muted;
  bool vol_dirty = false;
  double vol_settle_at = 0;
  while (!window.should_close()) {
    const double now = mono_now();
    in.now = now;  // 无输入也推进（idle 淡出用）
    double dt = now - prev_now;
    prev_now = now;
    if (dt < 0 || dt > 0.1) dt = 0.1;
    in.dt = (float)dt;

    // 1) DMR 命令 → 状态机（先命令后泵，保证 SetURI 当帧生效）
    //    d36：DMR 命令（含投屏进入）**不再唤醒面板** —— 面板唯一唤醒 = 鼠标移动。
    //    旧逻辑刷 last_input 会让手机一投屏/一控制就弹两栏，违背沉浸设计；
    //    用户感知反馈由 OSD 承担（手机调音量有音量 OSD）。
    for (auto& c : bus.drain_commands()) {
      player.on_command(c);
    }
    // 2) 状态机泵（mpv 事件 → 转移 + 快照发布）
    player.tick(now);
    // 3) DMR 差量 NOTIFY
    dmr.tick();
    // 4) 窗口事件（模态循环会在此阻塞，但渲染线程不受影响）
    window.poll_events();
    tray::poll();  // d81：Linux 托盘图标窗口 X 事件（Win 空操作）

    // 5) 渲染线程投来的用户意图
    for (auto& it : channel.drain_intents()) exec_intent(it);

    // 5.5) d146：投屏呈现管线（P1–P4；P5 写回在 7.5 几何去抖段 + 退出兜底）
    for (const auto& ce : bus.drain_casts()) run_cast_pipeline(ce);

    // 6) 画中画：单击（原地快速弹起）恢复窗口的判定在 on_mouse_btn（经意图队列回来）；
    //    这里只做两件事：按住 200ms 未弹起 → 判定拖动；出现过画面后又消失也退回大窗。
    //    d47：快照本轮只取一次（tick 之后），pip 判定 / 忙碌判定 / 等待时长共用。
    pip_hold_check();
    // 6.5) d74 剪贴板：探测结果入池 → 提示条；无事件平台 1s 轮询兜底
    if (cfg.clipboard_play) {
      if (!clip_event_mode && now - clip_poll_at > 1.0) {
        clip_poll_at = now;
        clip_on_text(clipboard::get_text(window.raw()));
      }
      std::vector<UrlProbe::Result> probe_out;
      url_probe.drain_results(probe_out);
      for (const auto& r : probe_out) {
        if (!r.playable) {
          FR_LOG_INFO("[CLIP] 探测不可播（分享页/死链/无轨道）{}", r.url);
          continue;
        }
        clip_last_ok = r.url;
        std::snprintf(in.clip_url, sizeof(in.clip_url), "%s", r.url.c_str());
        in.clip_url[sizeof(in.clip_url) - 1] = '\0';
        in.clip_visible = true;
        in.clip_at = now;   // 重新计时（新链接替换旧提示条）
        publish_wake();
        FR_LOG_INFO("[CLIP] 可播链接 → 提示条 {}", r.url);
      }
    }
    // d52：版本不变复用缓存（版本由 publish_player 唯一写点推进，版本同 = 内容同）
    PlaybackSnapshot snap;
    {
      const uint64_t ver = bus.snapshot_version();
      if (ver == snap_ver_seen) {
        snap = snap_cache;
      } else {
        snap = bus.snapshot();
        snap_cache = snap;
        snap_ver_seen = ver;
      }
    }
    {
      const bool has_picture = snap.picture_ready && !snap.uri.empty();
      if (pip_mode && pip_had_picture && !has_picture) {
        UiIntent it{UiIntent::Kind::Pip};
        exec_intent(it);
      }
      pip_had_picture = has_picture;
    }

    // 7) 面板醒睡推进 + 光标显隐（d36）
    //    转睡：idle 超过显示+淡出时长后进入睡眠，防抖基准点重置为当前位置。
    //    光标隐藏前提（用户定值，两条件叠加）：全屏 && 鼠标在窗口客户区内；
    //    且面板已 idle、不在底栏热区带、无按住交互。窗口态永不藏光标。
    if (chrome_awake && now - in.last_input >= theme.layout.idleHideSec + theme.layout.fadeSec) {
      chrome_awake = false;
      wake_ax = in.mx;
      wake_ay = in.my;
    }
    {
      const auto& L = theme.layout;
      const bool over_bar = in.my >= (float)window.height() - 2.f * L.botBarH;
      const bool busy = in.down || pip_pressed;
      const bool want_hide = window.fullscreen() && mouse_inside &&
                             now - in.last_input >= L.idleHideSec &&
                             !over_bar && !busy;
      if (want_hide != cursor_hidden) {
        cursor_hidden = want_hide;
        window.set_cursor_visible(!want_hide);
      }
    }

    // 7.5) d146 P5：几何变化检测 + 去抖落盘（画中画小窗不记，避免污染按屏记忆；
    //      变化帧只刷新基准与结算时刻，静止 800ms 后的下一轮才真正写盘）
    if (!pip_mode) {
      int wx, wy, ww, wh;
      window.pos_size(&wx, &wy, &ww, &wh);
      const bool fs = window.fullscreen(), mx = window.maximized();
      if (!geo_valid || wx != geo_x || wy != geo_y || ww != geo_w || wh != geo_h ||
          fs != geo_fs || mx != geo_mx) {
        geo_x = wx;
        geo_y = wy;
        geo_w = ww;
        geo_h = wh;
        geo_fs = fs;
        geo_mx = mx;
        geo_valid = true;
        geo_dirty = true;
        geo_settle_at = now + 0.8;
      } else if (geo_dirty && now >= geo_settle_at &&
                 window.monitor_index_of_window() >= 0) {
        geo_dirty = false;
        save_window_geometry(cfg, window, paths::config_file());
      }
    }

    // 7.6) d163：音量/静音变化检测 + 去抖落盘（音量条拖动每秒几十个 SetVolume，
    //      静止 800ms 写一次；Config::patch 键不变不写盘）
    if (snap.volume != vol_last || snap.muted != mute_last) {
      vol_last = snap.volume;
      mute_last = snap.muted;
      vol_dirty = true;
      vol_settle_at = now + 0.8;
    } else if (vol_dirty && now >= vol_settle_at) {
      vol_dirty = false;
      cfg.player_volume = vol_last;
      cfg.player_muted = mute_last;
      Config::patch(paths::config_file(),
                    {{"player", "volume", std::to_string(vol_last)},
                     {"player", "muted", mute_last ? "true" : "false"}});
      FR_LOG_INFO("[APP] 音量 = {}{}（已落盘）", vol_last, mute_last ? " 静音" : "");
    }

    // 8) 发布输入给渲染线程 —— d47 改按需：渲染线程每帧自算 in.now/dt/fullscreen/
    //    maximized/win_radius/窗口尺寸，主线程循环里唯一会变的是 `in`（所有改动点
    //    已各自 publish_wake）与播放快照（tick 推进，版本号检测）。静默期零发布
    //    零唤醒 —— 这就是原"每 8ms 无条件 publish+wake"双线程空转的主消减点。
    //    last_pub_ = 上次发布时的主线程输入；input_changed 与渲染线程脏判定共用
    //    字段表，另补 down_right（主线程→渲染线程的唯一携带者，菜单边沿依赖）。
    {
      const uint64_t ver = bus.snapshot_version();
      if (ver != last_pub_ver || input_changed(in, last_pub_) ||
          in.down_right != last_pub_.down_right) {
        channel.publish();
        render.wake();
        last_pub_ver = ver;
        last_pub_ = in;
      }
    }

    // 9) 等事件（不 busy loop）。d47 改自适应：输入/窗口事件与 publish_wake 的
    //    post 都会即时打断等待，放宽的只是"主线程巡检"粒度（命令处理/状态机泵/
    //    防抖判定）。播放/转场中保持 8ms（手机控制的反馈延迟不变）；
    //    面板睡眠且无播放的静默期 50ms（命令/位置感知延迟 ≤50ms，无感）。
    const bool main_busy =
        chrome_awake || pip_mode || snap.state == TransportState::Playing ||
        snap.state == TransportState::Transitioning;
    glfwWaitEventsTimeout(main_busy ? 0.008 : 0.05);
  }

  // —— 收尾 ——
  // d68 §5：Alt+F4 / 系统关机等不经 Close 意图的关闭路径同样先隐窗（幂等；
  // Close 意图路径在 exec_intent 里已隐），网络收尾照旧在后台执行。
  window.hide_window();
  tray::remove();        // d81：撤托盘图标（真退出的唯一路径已到，隐托盘态同达此处）
  url_probe.shutdown();  // d74：探测 worker + mpv 句柄（在 player.shutdown 前后皆可，无共享状态）
  // d146 P5：退出兜底 —— 几何再落盘一次（去抖可能还没到点）
  if (geo_valid && window.monitor_index_of_window() >= 0)
    save_window_geometry(cfg, window, paths::config_file());
  // d163：音量/静音兜底再落盘一次（去抖可能还没到点）
  if (vol_dirty) {
    Config::patch(paths::config_file(),
                  {{"player", "volume", std::to_string(vol_last)},
                   {"player", "muted", mute_last ? "true" : "false"}});
  }
  // 顺序关键：先停渲染线程（释放 GL 资源、交还上下文），再动窗口/播放器。
  render.stop();
  thumb.shutdown();  // 渲染线程已停，不再读结果槽；先停 worker 再销毁 mpv

  // 任何形式的窗口关闭都推送"投屏结束"：
  // Stop（R5 进度快照固化）→ STOPPED → dmr.tick 冲出最终 NOTIFY → BYEBYE
  {
    DmrCommand stop_cmd;
    stop_cmd.type = DmrCommand::Type::Stop;
    player.on_command(stop_cmd);
    player.tick(mono_now());
    dmr.tick();
  }
  keep_awake::set(false);
  dmr.stop();
  player.shutdown();
  window.destroy();
  FR_LOG_INFO("[APP] 退出");
  shutdown_logger();
  return 0;
}
