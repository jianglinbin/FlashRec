#include "player/mpv_bridge.h"

#include "cast/cast_contract.h"  // d269：kLoopbackUriPrefix（回流 URI 不进 last_uri_）

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>

#include <glad/gl.h>

#include "app/log.h"
#include "app/str_util.h"

namespace fr {

MpvBridge::~MpvBridge() { shutdown(); }

bool MpvBridge::init() {
  mpv_ = mpv_create();
  if (!mpv_) {
    FR_LOG_ERROR("[MPV] mpv_create 失败");
    return false;
  }
  mpv_set_option_string(mpv_, "vo", "libmpv");
  mpv_set_option_string(mpv_, "hwdec", "auto-safe");
  mpv_set_option_string(mpv_, "keep-open", "no");
  mpv_set_option_string(mpv_, "terminal", "no");
  mpv_set_option_string(mpv_, "input-default-bindings", "no");
  mpv_set_option_string(mpv_, "input-vo-keyboard", "no");
  mpv_set_option_string(mpv_, "input-cursor", "no");
  mpv_set_option_string(mpv_, "osc", "no");
  mpv_set_option_string(mpv_, "audio-display", "no");
  mpv_set_option_string(mpv_, "demuxer-max-bytes", "64MiB");
  // ★d283 取证开关：真机 loadfile→file-loaded 空档 7.577s，期间主线程正常 tick、
  //   mpv 只读到流时间 0.267s —— 离线 vo=null 复现不出（同选项 0.01s 就绪），
  //   只能让 mpv 自己把这段时间讲了。FR_MPV_TRACE=1 抬 msg-level 并放开日志级别。
  //   纯诊断，不改任何播放行为；默认关。
  const bool mpv_trace = [] {
    const char* v = std::getenv("FR_MPV_TRACE");
    return v != nullptr && *v != '\0' && *v != '0';
  }();
  if (mpv_trace)
    mpv_set_option_string(mpv_, "msg-level", "cplayer=v,demux=v,demux_lavf=v,ffmpeg=v,stream=v");
  if (mpv_initialize(mpv_) < 0) {
    FR_LOG_ERROR("[MPV] mpv_initialize 失败");
    mpv_terminate_destroy(mpv_);
    mpv_ = nullptr;
    return false;
  }
  mpv_request_log_messages(mpv_, mpv_trace ? "v" : "info");
  if (mpv_trace) {
    log_trace_ = true;  // d287：真正打通 —— 否则 v 级请求来了也被这里的分级丢弃
    FR_LOG_WARN("[MPV] FR_MPV_TRACE 已开：mpv 内部日志按 v 级转发");
  }
  // d125：配置开关 —— 强制软件渲染，完全不碰视频 GL 链。
  // 用途：① 老显卡驱动（已知会闪退/白屏）的自救开关；② 启动探测结论的落地通道。
  // 取值：FR_FORCE_SW=1/true/yes 生效；未设或 0 不生效。
  if (const char* fs = std::getenv("FR_FORCE_SW"); fs != nullptr && *fs != '\0' && *fs != '0') {
    gl_broken_.store(true, std::memory_order_release);
    FR_LOG_WARN("[MPV] FR_FORCE_SW={} → 强制软件渲染", fs);
  }
  // 观测属性：状态机唯一真值的输入源
  const char* doubles[] = {"time-pos", "duration", nullptr};
  const char* flags[] = {"pause", "mute", "seekable", "seeking", nullptr};
  for (auto* p = doubles; *p; ++p)
    mpv_observe_property(mpv_, 0, *p, MPV_FORMAT_DOUBLE);
  for (auto* p = flags; *p; ++p)
    mpv_observe_property(mpv_, 0, *p, MPV_FORMAT_FLAG);
  FR_LOG_INFO("[MPV] mpv 初始化完成 client-api={}", mpv_client_api_version() >> 16, "");
  return true;
}

void MpvBridge::shutdown() {
  if (render_) {
    mpv_render_context_free(render_);
    render_ = nullptr;
  }
  if (mpv_) {
    mpv_terminate_destroy(mpv_);
    mpv_ = nullptr;
  }
}

bool MpvBridge::init_render(void* (*get_proc)(void* user, const char* name), void* user) {
  if (!mpv_) return false;
  // d125：已知视频 GL 链不可用（配置开关强制 / 启动探测结论）→ 起步就建 SW 上下文，
  // 一次 GL 视频调用都不发生 —— 这是老驱动（AMD HD 6570 / Catalyst 15.7.1）上
  // 唯一"根本不崩"的姿势；运行时 SEH 兜底（render_loop）只是它的后手。
  if (gl_broken_.load(std::memory_order_acquire)) {
    FR_LOG_WARN("[MPV] 软件渲染模式（配置开关/启动探测）→ 直接使用 SW 后端");
    return create_sw_context();
  }
  mpv_opengl_init_params gl_init{};
  gl_init.get_proc_address = get_proc;
  gl_init.get_proc_address_ctx = user;
  mpv_render_param params[2] = {
      {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
      {MPV_RENDER_PARAM_INVALID, nullptr},
  };
  params[1].data = &gl_init;
  params[1].type = MPV_RENDER_PARAM_OPENGL_INIT_PARAMS;
  if (mpv_render_context_create(&render_, mpv_, params) < 0) {
    FR_LOG_ERROR("[MPV] mpv_render_context_create 失败");
    render_ = nullptr;
    return false;
  }
  FR_LOG_INFO("[MPV] render context 就绪（OpenGL FBO 合成）");
  return true;
}

void MpvBridge::render_to_fbo(int fbo, int w, int h, bool flip_y) {
  if (!render_ || backend_ != VideoBackend::GL) return;  // d124：软件后端不走 GL 分支
  // 翻转不靠 FLIP_Y 参数：mpv 头文件明确写着
  //   "MPV_RENDER_PARAM_FLIP_Y is currently ignored (unsupported)"（render.h）
  // 传它纯属无效负担。**且根本不需要翻**：d18 用四象限图实测确认
  // "mpv 渲染进 FBO 的内容已经是 nanovg 采样所需的朝向"，消费端
  // （render_loop.cpp 的画面贴图段）用默认 pattern、不做任何翻转，三种翻转写法
  // 全是错的（历史教训见该段注释与 tools/flip_math_check.py）。
  (void)flip_y;
  mpv_opengl_fbo f{};
  f.fbo = fbo;
  // w/h 必须是 **framebuffer 的尺寸**（render_gl.h: "This must refer to the size of
  // the framebuffer"），即 FBO 容量，不是内容尺寸 —— 传小画面只落在纹理一角。
  f.w = w;
  f.h = h;
  f.internal_format = 0;
  mpv_render_param rp[2] = {
      {MPV_RENDER_PARAM_OPENGL_FBO, &f},
      {MPV_RENDER_PARAM_INVALID, nullptr},
  };
  mpv_render_context_render(render_, rp);
}

bool MpvBridge::create_sw_context() {
  // SW 路径按定义只在 CPU 出帧，硬解必须关掉（否则拿不到 CPU 侧帧）
  mpv_set_property_string(mpv_, "hwdec", "no");
  mpv_render_param params[2] = {
      {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_SW)},
      {MPV_RENDER_PARAM_INVALID, nullptr},
  };
  if (mpv_render_context_create(&render_, mpv_, params) < 0) {
    render_ = nullptr;
    FR_LOG_ERROR("[MPV] 软件渲染上下文创建失败 —— 画面仍无法显示");
    return false;
  }
  backend_ = VideoBackend::Software;
  FR_LOG_INFO("[MPV] 已切到软件渲染（MPV_RENDER_API_TYPE_SW，CPU 出帧，分辨率无关约 8ms/帧）");
  return true;
}

bool MpvBridge::switch_to_software_if_needed() {
  // ★渲染线程专用（mpv_render_context_free 要求与 create 同线程、同 GL 上下文）
  if (backend_ == VideoBackend::Software) return false;
  if (!gl_broken_.load(std::memory_order_acquire) || !mpv_) return false;

  FR_LOG_WARN("[MPV] GL 渲染后端不可用（着色器/纹理/GL 调用失败）→ 切换软件渲染兜底");
  if (render_) {
    mpv_render_context_free(render_);
    render_ = nullptr;
  }
  if (!create_sw_context()) return false;
  // d151：切完必须重载才能恢复出帧（d127：切换掐断播放；且 hwdec=no 只对后续 loadfile 生效）。
  // 置位交给主线程 tick 消费——渲染线程不碰播放语义。
  needs_reload_.store(true, std::memory_order_release);
  return true;
}

bool MpvBridge::render_sw(int w, int h) {
  if (!render_ || backend_ != VideoBackend::Software || w <= 0 || h <= 0) return false;
  if (sw_w_ != w || sw_h_ != h) {
    sw_w_ = w;
    sw_h_ = h;
    sw_buf_.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0);
  }
  int size[2] = {w, h};
  int stride = w * 4;
  // 格式取 rgba：与本项目视频 FBO 的 GL_RGBA8 纹理逐字节对齐，可直接 glTexSubImage2D 上传。
  // FLIP_Y 在 SW 路径被 mpv 忽略（render.h 明示），输出即"行 0 = 图像顶部"。
  // 已实测确认这与 GL 路径的 FBO 朝向一致（自造"上白下黑"图过 SW 渲染：上 1/4 纯白、
  // 下 1/4 纯黑）⇒ 上传时**不需要任何翻转**，消费端共用 d18 的"不翻"贴图即可。
  mpv_render_param rp[5] = {
      {MPV_RENDER_PARAM_SW_SIZE, size},
      {MPV_RENDER_PARAM_SW_FORMAT, const_cast<char*>("rgba")},
      {MPV_RENDER_PARAM_SW_STRIDE, &stride},
      {MPV_RENDER_PARAM_SW_POINTER, sw_buf_.data()},
      {MPV_RENDER_PARAM_INVALID, nullptr},
  };
  mpv_render_context_render(render_, rp);
  return true;
}

void MpvBridge::report_swap() {
  if (render_) mpv_render_context_report_swap(render_);
}

bool MpvBridge::frame_update_pending() {
  if (!render_) return false;
  // mpv render.h 的标准模式（eglrender 示例同款）：update() 返回旗标位集，
  // MPV_RENDER_UPDATE_FRAME 置位才需要渲染，否则跳过（跳过时无须 report_swap）。
  return (mpv_render_context_update(render_) & MPV_RENDER_UPDATE_FRAME) != 0;
}

void MpvBridge::pump() {
  if (!mpv_) return;
  for (;;) {
    mpv_event* ev = mpv_wait_event(mpv_, 0);
    if (!ev || ev->event_id == MPV_EVENT_NONE) break;
    MpvEvent out;
    switch (ev->event_id) {
      case MPV_EVENT_FILE_LOADED:
        out.kind = MpvEvent::Kind::FileLoaded;
        break;
      case MPV_EVENT_END_FILE: {
        auto* ef = static_cast<mpv_event_end_file*>(ev->data);
        out.kind = MpvEvent::Kind::EndFile;
        out.reason = ef ? ef->reason : 0;
        break;
      }
      case MPV_EVENT_IDLE:
        out.kind = MpvEvent::Kind::IdleEntered;
        break;
      case MPV_EVENT_LOG_MESSAGE: {
        auto* m = static_cast<mpv_event_log_message*>(ev->data);
        if (m) {
          std::string line(m->text ? m->text : "");
          while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
          if (!line.empty()) {
            // d124：判定 GL 渲染后端是否已实质失效 —— 老显卡驱动（如 AMD HD 6570 /
            // Catalyst 15.7.1）编译不了 mpv 的 NV12→RGB 转换 pass：
            //   ERROR: 0:27: error(#444) The specified offset of a UBO or SSBO member
            //   causes it to overlap another member.   → shader link log (status=0)
            // 后果是"每帧只有底色"，而 mpv 仍照常报新帧、时间轴照常推进 —— 一切正常
            // 除了画面本身。这里据此置位，由渲染线程切到软件渲染（见 switch_to_software_if_needed）。
            // 只认 libmpv_render 模块，避免误伤其它模块出现同名文本。
            // d151：libmpv_render 的日志从 TRACE 提为 INFO——TRACE 因未定义
            // SPDLOG_ACTIVE_LEVEL 是空宏，白屏复现时驱动实际报了什么完全看不到，
            // 两道兜底（SEH + 特征串）都没触发就成了盲区。render 模块全量落 INFO，
            // 其它模块只落 warn/error，避免 demuxer/decoder 的 info 刷屏。
            const bool is_render_mod = m->prefix && strncmp(m->prefix, "libmpv_render", 14) == 0;
            if (log_trace_ && m->log_level > MPV_LOG_LEVEL_WARN) {
              // ★d287：取证通道专用 —— WARN 以上仍走下面的原分级，避免把错误降级成 info。
              FR_LOG_INFO("[MPV][{}] {}", m->prefix ? m->prefix : "?", line);
            } else if (is_render_mod)
              FR_LOG_INFO("[MPV][{}] {}", m->prefix ? m->prefix : "?", line);
            else if (m->log_level <= MPV_LOG_LEVEL_WARN)
              FR_LOG_WARN("[MPV][{}] {}", m->prefix ? m->prefix : "?", line);
            // ★触发条件必须覆盖三类失败（实测见 d151）：
            //   ① shader failed to compile            —— #444 类着色器编译失败
            //   ② shader link log (status=0)          —— 着色器链接失败（同上，常成对出现）
            //   ③ libmpv_render 报出的任何 OpenGL error —— ★本次白屏真凶：
            //      "after creating texture: OpenGL error INVALID_OPERATION"，帧照出、
            //      fps 照报、进度照走，唯独纹理建不出来 ⇒ 画面全白而两道兜底全哑。
            //      凡 render 模块自报 GL error 都是致命的（拿不到画面），一律降级。
            if (is_render_mod && !gl_broken_.load(std::memory_order_relaxed) &&
                (line.find("shader failed to compile") != std::string::npos ||
                 line.find("shader link log (status=0)") != std::string::npos ||
                 line.find("OpenGL error") != std::string::npos)) {
              gl_broken_.store(true, std::memory_order_release);
              FR_LOG_WARN("[MPV] GL 渲染后端失败（着色器/纹理/GL 调用）→ 将降级软件渲染: {}", line);
            }
            FR_LOG_TRACE("[MPV][{}] {}", m->prefix ? m->prefix : "?", line);
          }
        }
        continue;  // 日志不上抛
      }
      case MPV_EVENT_PROPERTY_CHANGE: {
        auto* p = static_cast<mpv_event_property*>(ev->data);
        if (!p || !p->name) continue;
        out.kind = MpvEvent::Kind::PropertyChanged;
        out.prop_name = p->name;
        if (p->format == MPV_FORMAT_DOUBLE)
          out.num = *static_cast<double*>(p->data);
        else if (p->format == MPV_FORMAT_FLAG)
          out.flag = *static_cast<int*>(p->data);
        else
          continue;  // 未观测格式
        break;
      }
      case MPV_EVENT_SHUTDOWN:
        FR_LOG_ERROR("[MPV] 核心关闭事件");
        out.kind = MpvEvent::Kind::EndFile;
        out.reason = MPV_END_FILE_REASON_ERROR;
        break;
      default:
        continue;
    }
    if (event_cb_) event_cb_(out);
  }
}

// d273：诊断快照（强停/异常时由 player_controller 落日志）
std::string MpvBridge::diag_snapshot() {
  if (!mpv_) return "(mpv 未初始化)";
  static const char* keys[] = {
      "filename",
      "seekable",
      "duration",
      "eof-reached",
      "core-idle",
      "pause",              // d287：冻结取证（yes=压根没播）
      "estimated-vf-fps",   // d287：视频是否真在显示（无帧可显示时取不到值）
      "video-pts",          // d287：显示链是否真在出帧
      "audio-pts",          // d287：音频时钟（无声缺陷的分界）
      "video-format",
      "width",
      "height",
      "video-codec",
      "video-bitrate",
      "audio-codec-name",
      "audio-params/samplerate",
      "audio-bitrate",
      "track-list",
      "demuxer-cache-time",
      "demuxer-cache-duration",
      "paused-for-cache",
      "cache-buffering-state",
  };
  std::string out;
  for (const char* k : keys) {
    char* v = mpv_get_property_string(mpv_, k);
    out += k;
    out += "=";
    out += (v && *v) ? v : "(null)";
    if (v) mpv_free(v);
    if (std::strcmp(k, "track-list") == 0) out += "\n  ";  // JSON 较长，换行
    else out += "  ";
  }
  return out;
}

// d279：单个属性取值（字符串形式），探针用。
std::string MpvBridge::get_prop(const char* name) {
  if (!mpv_ || !name) return "(null)";
  char* v = mpv_get_property_string(mpv_, name);
  std::string out = (v && *v) ? v : "(null)";
  if (v) mpv_free(v);
  return out;
}

// d281：视频显示宽高比（含 PAR 修正）。取不到/无视频/离谱值一律 0 —— 判定侧据此跳过。
// 为什么不用 demux-w/h：那是**解码尺寸**，镜像流常见 SAR≠1，必须看显示比例。
double MpvBridge::video_aspect() {
  if (!mpv_) return 0.0;
  double a = 0.0;
  if (mpv_get_property(mpv_, "video-params/aspect", MPV_FORMAT_DOUBLE, &a) < 0) return 0.0;
  return (a > 0.01 && a < 100.0) ? a : 0.0;
}

// v0.6.1 d343：视频显示尺寸（含 SAR/PAR 修正），数字键缩放档位的锚定基准。
//   首选 video-out-params/dw/dh —— mpv 按显示口径算好的整型（含 rotation/容器
//   SAR 修正），与 d281 video_aspect 同源但更直接（不用自己乘）；
//   回退：width × video-params/aspect 估算（老 mpv 属性缺失时）；
//   都取不到（无视频/音频流）返回 false，调用方无动作。
bool MpvBridge::video_display_size(int* w, int* h) {
  if (!mpv_ || !w || !h) return false;
  int64_t dw = 0, dh = 0;
  if (mpv_get_property(mpv_, "video-out-params/dw", MPV_FORMAT_INT64, &dw) >= 0 &&
      mpv_get_property(mpv_, "video-out-params/dh", MPV_FORMAT_INT64, &dh) >= 0 &&
      dw > 0 && dh > 0) {
    *w = (int)dw;
    *h = (int)dh;
    return true;
  }
  // 回退：解码宽 × 显示宽高比（d281 同口径；aspect 取不到按 1.0 = 方像素）。
  int64_t wid = 0;
  if (mpv_get_property(mpv_, "width", MPV_FORMAT_INT64, &wid) >= 0 && wid > 0) {
    const double a = video_aspect();
    const double disp_w = (double)wid * (a > 0.01 ? a : 1.0);
    int64_t hei = 0;
    if (mpv_get_property(mpv_, "height", MPV_FORMAT_INT64, &hei) >= 0 && hei > 0) {
      *w = (int)disp_w;
      *h = (int)hei;
      return true;
    }
  }
  return false;
}

void MpvBridge::load_uri(const std::string& uri, double start_sec,
                         const std::string& load_format) {  if (!mpv_) return;
  // d184：每次加载应用视频填充（cover=1/contain=0；等价 CSS object-fit）
  mpv_set_property(mpv_, "panscan", MPV_FORMAT_DOUBLE, &panscan_);
  // d350：画面缩放是会话级状态——新会话（任何协议的 SetURI）重置回 100%/零平移，
  // 防止上一会话的 video-zoom/pan 残留到新投屏（main 侧经快照边沿同步重置）。
  video_zoom_ = 1.0;
  video_pan_x_ = 0.0;
  video_pan_y_ = 0.0;
  mpv_set_property(mpv_, "video-zoom", MPV_FORMAT_DOUBLE, &video_zoom_);
  mpv_set_property(mpv_, "video-pan-x", MPV_FORMAT_DOUBLE, &video_pan_x_);
  mpv_set_property(mpv_, "video-pan-y", MPV_FORMAT_DOUBLE, &video_pan_y_);
  // d196：留边底色随皮肤（contain 时 letterbox 用 stageBg，而非 mpv 默认黑）
  if (!bg_color_.empty())
    mpv_set_property_string(mpv_, "background-color", bg_color_.c_str());
  std::string clean = uri_without_fragment(uri);
  double start = start_sec;
  if (start < 0) start = uri_start_fragment(uri);  // R1：#t=xx 进度载体
  // d269：AirPlay 回流裸流按 load_format 钉 demuxer（"latm"/"caf"/"mpegts"；
  // 空 = 自动探测）。用 per-loadfile option，无属性残留，不影响 DLNA 播放。
  std::vector<std::string> cmd = {"loadfile", clean, "replace"};
  std::string opts;
  if (start > 0.0) {
    char buf[40];
    snprintf(buf, sizeof(buf), "start=%.3f", start);
    opts = buf;
  }
  // ★d279：回流实时流必须走**低延迟**选项集（只加在 load_format 非空时 —— 已核实
  //   全工程只有 AirPlay 回流设它：airplay_stream.cpp 的 "mpegts"/"latm"/"caf"/"aac"，
  //   DLNA 点播不设，故 mpv_bridge.cpp:38 的全局 demuxer-max-bytes=64MiB 对 DLNA 仍生效）。
  //   取证（d279 受控实验，真机 dump 切片按实时速率喂 + 量 demuxer-cache-duration
  //   = 已读前沿 PTS − 播放位置 PTS = 端到端延迟）：
  //     生产基线（全局 64MiB）：10.5s → **38.4s 仍在爬**（终点 ~123s）
  //     cache=no                       ：1.00s（被 demuxer-readahead-secs 默认 1s 卡住）
  //     cache=no,demuxer-readahead-secs=0.1：**0.10s**（实测线性可控）
  //   机理：mpv 把 tcp://127.0.0.1 当网络流 ⇒ cache=auto ⇒ 读游标全速跑到播放位置
  //   前面 demuxer-max-bytes 那么多字节 —— 实时流下这段预读量就是纯延迟。
  //   ★per-file 生效性已实测（同一 mpv、同一 URI，仅 per-file 不同，行为迥异）。
  // ★d279c：起播耗时才是延迟的**主项**（P1 只治了稳态缓冲，治不了这个）。
  //   真机探针（player_controller 的「延迟探针」）实测：接收端积压恒定 4.35s
  //   （= 流经过 − time-pos），且**生产积压 = −0.01s、demuxer 缓存 = 0.100s** ⇒
  //   积压既不在生产侧也不在 demuxer 缓存，而在「mpv 起播慢」：
  //     20:04:15.084 loadfile → 20:04:18.994 file-loaded（**3.91s**），
  //     起播那一刻 time-pos 仅 0.05s ⇒ mpv 从 PTS=0 起播却晚了 3.9s，
  //     之后以实时速率播放（追不上），于是永久落后 3.9s。
  //   受控实验（build/test_ts/startup_test.sh，离线重放真机切片）：
  //     现状（cache=no,readahead=0.1）           → 起播 ~3.8s（与真机 3.91s 吻合）
  //     +probesize=200000,analyzeduration=1     → 起播 **<0.5s**
  //   机理：lavf demuxer 默认 probesize=5MB / analyzeduration=5s，网络实时流下
  //   要收满这么多数据才肯 file-loaded —— 在 ~10Mbps 镜像上就是 4s 的死等。
  //   我们的格式是已知的（mpegts/latm/caf/aac，见 load_format），无需大探测。
  //   audio-buffer=0.1：默认 0.2s 的音频输出缓冲同样计入端到端延迟。
  // ★d285：200000 这一档**本身又是新的门**，必须再砍一个小数量级。
  //   真机 A 段（loadfile→file-loaded）实测 7.577s；离线用**生产播放器**（tools/live_bench.py
  //   + FR_LIVE_FORMAT=mpegts，真机抓的 d282_live.ts 按可控码率回喂）同流同选项 A/B：
  //     喂速 0.29MB/s + probesize=200000 → A=1.086s
  //     喂速 0.042MB/s + probesize=200000 → A=7.456s / 7.687s（两次复现）
  //     喂速 0.042MB/s + probesize=16384  → A=0.064s
  //     喂速 0.29MB/s  + probesize=16384  → A=0.231s，B=0.271s，轨道表 video/audio **仍在**
  //   ⇒ A ∝ probesize / 喂速：这就是**字节闸门**，不是时间闸门。镜像起始常落在近乎
  //   静止的画面上（iPhone 只发稀疏 P 片，~42KB/s），200000B 就要 5s+；而 mpv 起播后
  //   按实时速率走 ⇒ 永远追不回，这 7.5s 直接变成用户看到的「10s+ 延迟」的主项。
  //   16384 ≈ 87 个 TS 包，覆盖 3~4 个 PSI 周期（PAT/PMT 100ms 重发，d282）足以建轨。
  //   ★顺带否证两条候选（d283 留档）：①「主线程被卡」—— 取证期间主循环 tick 日志间隔
  //   正常；②「PMT 声明了空音频 ES 卡住探测」—— A 段里轨道表已建好，空 ES 是**另一个**
  //   缺陷（file-loaded 后 time-pos 恒 0 + core-idle 空转），与本门无关，另行处置。
  if (!load_format.empty()) {
    if (!opts.empty()) opts += ",";
    opts += "demuxer-lavf-format=" + load_format;
    // ★d322：readahead 0.1 → 2.0 —— 追赶控制器（player_controller）的**缓冲深度**
    //   配套。0.1s 窗口下 mpv 手里没有可加速消费的积压（积压滞留在我们的发送
    //   缓冲/网络侧），倍速控制器无从追起；2.0s 窗口让积压进入 mpv demuxer，
    //   控制器以倍速把延迟压回阈值。稳态延迟 ~1-2s（控制器压制），换取突发
    //   不丢片、解码链不断。d279 的 0.1s 是「无控制器时代」的取舍，本条是
    //   有控制器后的重新取舍。
    opts += ",cache=no,demuxer-readahead-secs=2.0";
    opts += ",demuxer-lavf-probesize=16384,demuxer-lavf-analyzeduration=1";
    opts += ",audio-buffer=0.1";
    // ★d283 取证：整串可被环境变量替换（默认不设 = 上面这套生产值）。
    //   用途 = 真机上逐项 A/B「A 段 7.577s 到底卡在哪个选项」—— 每次只改环境变量、
    //   重启、复投，不需要重编。纯诊断通道，出厂默认不生效。
    if (const char* ov = std::getenv("FR_LIVE_OPTS"); ov != nullptr && *ov != '\0') {
      opts = "demuxer-lavf-format=" + load_format;
      opts += std::string(",") + ov;
      FR_LOG_WARN("[MPV] FR_LIVE_OPTS 覆盖回流 per-file 选项: {}", ov);
    }
  }
  if (!opts.empty()) {
    cmd.push_back("0");
    cmd.push_back(opts);
  }
  std::vector<const char*> args;
  for (auto& s : cmd) args.push_back(s.c_str());
  args.push_back(nullptr);
  int rc = mpv_command(mpv_, args.data());
  FR_LOG_INFO("[MPV] loadfile {} start={} fmt={} rc={}", clean, start > 0 ? start : 0.0,
              load_format.c_str(), rc);
  // d151：记住最近一次点播（已去 fragment 的净 URL），供后端降级后按位重载复用。
  // d269：回环流 URI（tcp://127.0.0.1:）不进 last_uri_ —— 端口随会话销毁，
  // 降级重载必然失败，且对实时流"重连到开头"语义本身不成立。
  if (clean.rfind(fr::cast::kLoopbackUriPrefix, 0) != 0) last_uri_ = clean;
}

bool MpvBridge::reload_after_backend_switch() {
  // 主线程专用（tick 内）。渲染线程已把上下文切成 SW；这里只负责"让新后端拿到帧"。
  if (!needs_reload_.load(std::memory_order_acquire)) return false;
  needs_reload_.store(false, std::memory_order_release);
  if (!mpv_ || !render_ || backend_ != VideoBackend::Software) return false;
  if (last_uri_.empty()) return false;
  return reload_current_preserve_pos();
}

// v0.6.1 d352：从 reload_after_backend_switch 抽出的公共段（无门槛，调用方自判）。
bool MpvBridge::reload_current_preserve_pos() {
  if (!mpv_ || last_uri_.empty()) return false;
  // 位置/暂停态从 mpv 现取（唯一真值），不依赖主线程缓存的 snap_
  double pos = 0.0;
  if (mpv_get_property(mpv_, "time-pos", MPV_FORMAT_DOUBLE, &pos) < 0) pos = 0.0;
  int paused = 0;
  bool was_paused = mpv_get_property(mpv_, "pause", MPV_FORMAT_FLAG, &paused) >= 0 && paused != 0;

  FR_LOG_WARN("[MPV] 按位重载续播 pos={:.2f}s paused={}", pos, was_paused ? 1 : 0);
  load_uri(last_uri_, pos > 0.0 ? pos : 0.0);  // start=pos 精确续播；内部记录 last_uri_ 不变
  if (was_paused) set_pause(true);             // 暂停态一并恢复（loadfile 不重置 pause 属性）
  return true;
}

// v0.6.1 d353：解码方式选择（右键「解码」子菜单；取代 d352 二值开关）。
// ① mpv 属性运行时改 hwdec 对**已加载流**不保证重初始化解码器（d151 实测 hwdec=no
//   只对后续 loadfile 生效的教训），故有会话时一律按位重载，确定生效；
// ② 重载会连带重置渲染属性（zoom/pan），main 侧在调用后按自身状态重放（见 main）；
// ③ 无会话时只落属性，对后续 loadfile 生效；
// ④ 模式表与 render_loop 子菜单条目一一对应，改一处必须改另一处。
void MpvBridge::set_hwdec_mode(int mode) {
  if (!mpv_) return;
  const char* val = "auto-safe";
  const char* name = "自动(auto-safe)";
  if (mode == 0) { val = "no"; name = "软解(no)"; }
  else if (mode == 2) { val = "nvdec-copy"; name = "NVDEC(nvdec-copy)"; }
  else if (mode == 3) { val = "dxva2-copy"; name = "DXVA2(dxva2-copy)"; }
  mpv_set_property_string(mpv_, "hwdec", val);
  FR_LOG_WARN("[MPV] 解码切换 → {}（重载当前流生效）", name);
  if (!last_uri_.empty()) reload_current_preserve_pos();
}

void MpvBridge::set_pause(bool p) {
  if (!mpv_) return;
  int v = p ? 1 : 0;
  mpv_set_property(mpv_, "pause", MPV_FORMAT_FLAG, &v);
}

void MpvBridge::seek_abs(double sec) {
  if (!mpv_) return;
  char t[40];
  snprintf(t, sizeof(t), "%.3f", sec);
  const char* cmd[] = {"seek", t, "absolute", "exact", nullptr};
  int rc = mpv_command(mpv_, cmd);
  FR_LOG_INFO("[MPV] seek {} rc={}", t, rc);
}

void MpvBridge::set_volume(int vol) {
  if (!mpv_) return;
  double v = vol;
  mpv_set_property(mpv_, "volume", MPV_FORMAT_DOUBLE, &v);
}

void MpvBridge::set_mute(bool m) {
  if (!mpv_) return;
  int v = m ? 1 : 0;
  mpv_set_property(mpv_, "mute", MPV_FORMAT_FLAG, &v);
}

void MpvBridge::set_speed(double s) {
  if (!mpv_) return;
  if (s < 0.25) s = 0.25;
  if (s > 4.0) s = 4.0;
  mpv_set_property(mpv_, "speed", MPV_FORMAT_DOUBLE, &s);
}

void MpvBridge::set_panscan(double p) {
  panscan_ = p < 0 ? 0 : (p > 1 ? 1 : p);
  if (mpv_) mpv_set_property(mpv_, "panscan", MPV_FORMAT_DOUBLE, &panscan_);
}

// v0.6.1 d350：画面缩放（渲染侧，窗口不动）。mpv `video-zoom` 是 **2 的幂指数标度**：
// 0 = 适配基准，1.0 = 放大 2 倍，-1.0 = 缩小一半。★d351 修正：d350 误按线性倍率
// 传入（100% → 1.0 = 2 倍），用户实测「100% 已放大、50% 反而不缩小」即此因；
// 现入参直接收 mpv 原生标度，百分比 → log2 换算归调用方（main 侧统一做）。
// 与 panscan（d184 填充/d281 自适应）正交叠加，互不覆盖——DLNA 直链 / AirPlay
// 镜像回流 / HLS 三路径同为 mpv 渲染，统一生效；纯渲染属性，不触碰 SOAP/GENA
// 状态变量与镜像数据流，协议面零感知。
void MpvBridge::set_video_zoom(double z) {
  // ★d351：夹取范围同步换到 log 域（[-2, 3] = 25%..800%，罩住 UI 的 50–500%
  //（d354 上限扩展；log2(5)≈2.32 仍在界内，夹取无需再动）。
  // 旧 [0.1, 10] 线性夹会把 -1.0（50%）顶回 0.1，缩不下去。
  video_zoom_ = z < -2.0 ? -2.0 : (z > 3.0 ? 3.0 : z);
  if (mpv_) mpv_set_property(mpv_, "video-zoom", MPV_FORMAT_DOUBLE, &video_zoom_);
}

// d350：画面平移（绝对设置；单位 = viewport 宽高比）。缩放 >100% 时被裁部分
// 靠 pan 移入视野；100% 时调用方应传 0（无内容可平移）。
void MpvBridge::set_video_pan(double x, double y) {
  video_pan_x_ = x < -1.0 ? -1.0 : (x > 1.0 ? 1.0 : x);
  video_pan_y_ = y < -1.0 ? -1.0 : (y > 1.0 ? 1.0 : y);
  if (mpv_) {
    mpv_set_property(mpv_, "video-pan-x", MPV_FORMAT_DOUBLE, &video_pan_x_);
    mpv_set_property(mpv_, "video-pan-y", MPV_FORMAT_DOUBLE, &video_pan_y_);
  }
}

void MpvBridge::set_background_color(const std::string& rgb_hex) {
  bg_color_ = rgb_hex;
  if (mpv_) mpv_set_property_string(mpv_, "background-color", bg_color_.c_str());
}

double MpvBridge::get_time_pos() {
  if (!mpv_) return 0;
  double v = 0;
  mpv_get_property(mpv_, "time-pos", MPV_FORMAT_DOUBLE, &v);
  return v;
}

double MpvBridge::get_video_fps() {
  if (!mpv_) return -1;
  double v = 0;
  // estimated-vf-fps：mpv 按实际到达的视频帧测得的帧率（容器 fps 失真时更真实）
  if (mpv_get_property(mpv_, "estimated-vf-fps", MPV_FORMAT_DOUBLE, &v) < 0) return -1;
  return v > 0.5 ? v : -1;  // 无视频/暂停测不到 → 视为不可用
}

VideoStats MpvBridge::get_video_stats() {
  VideoStats st;
  if (!mpv_) return st;
  double fps = 0;
  if (mpv_get_property(mpv_, "estimated-vf-fps", MPV_FORMAT_DOUBLE, &fps) >= 0)
    st.fps = fps > 0.5 ? fps : -1;
  int64_t bitrate = 0;
  if (mpv_get_property(mpv_, "bitrate", MPV_FORMAT_INT64, &bitrate) >= 0)
    st.bitrate = bitrate > 0 ? bitrate : -1;
  int64_t w = 0, h = 0;
  if (mpv_get_property(mpv_, "width", MPV_FORMAT_INT64, &w) >= 0 && w > 0) st.w = (int)w;
  if (mpv_get_property(mpv_, "height", MPV_FORMAT_INT64, &h) >= 0 && h > 0) st.h = (int)h;
  // —— d146 R5：徽章三合一扩展（属性名已实测；字符串属性用完必须 mpv_free）——
  auto get_str = [&](const char* p) -> std::string {
    char* s = nullptr;
    if (mpv_get_property(mpv_, p, MPV_FORMAT_STRING, &s) < 0 || !s) return "";
    std::string out = s;
    mpv_free(s);
    return out;
  };
  auto get_i64 = [&](const char* p) -> long long {
    int64_t v = 0;
    if (mpv_get_property(mpv_, p, MPV_FORMAT_INT64, &v) < 0) return -1;
    return v > 0 ? (long long)v : -1;
  };
  const std::string hw = get_str("hwdec-current");  // "no"/空 = 软解
  st.hwdec = !hw.empty() && hw != "no";
  st.hwdec_name = hw;  // d146：原值出给徽章（"no"=软解；空=取不到不猜）
  st.video_codec = get_str("video-format");         // 短名（h264/vp9/...）
  st.video_br = get_i64("video-bitrate");
  st.audio_codec = get_str("audio-codec");          // 无音轨 = 空
  st.audio_br = get_i64("audio-bitrate");
  st.audio_sr = (int)get_i64("audio-params/samplerate");
  st.audio_ch = (int)get_i64("audio-params/channel-count");
  return st;
}

double MpvBridge::get_duration() {
  if (!mpv_) return 0;
  double v = 0;
  mpv_get_property(mpv_, "duration", MPV_FORMAT_DOUBLE, &v);
  return v;
}

bool MpvBridge::get_seekable() {
  if (!mpv_) return false;
  int v = 0;
  mpv_get_property(mpv_, "seekable", MPV_FORMAT_FLAG, &v);
  return v != 0;
}

}  // namespace fr
