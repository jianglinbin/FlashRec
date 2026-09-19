#include "player/mpv_bridge.h"

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
  if (mpv_initialize(mpv_) < 0) {
    FR_LOG_ERROR("[MPV] mpv_initialize 失败");
    mpv_terminate_destroy(mpv_);
    mpv_ = nullptr;
    return false;
  }
  mpv_request_log_messages(mpv_, "info");
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
            if (is_render_mod)
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

void MpvBridge::load_uri(const std::string& uri, double start_sec) {
  if (!mpv_) return;
  std::string clean = uri_without_fragment(uri);
  double start = start_sec;
  if (start < 0) start = uri_start_fragment(uri);  // R1：#t=xx 进度载体
  std::vector<std::string> cmd = {"loadfile", clean, "replace"};
  std::string opts;
  if (start > 0.0) {
    char buf[40];
    snprintf(buf, sizeof(buf), "start=%.3f", start);
    opts = buf;
    cmd.push_back("0");
    cmd.push_back(opts);
  }
  std::vector<const char*> args;
  for (auto& s : cmd) args.push_back(s.c_str());
  args.push_back(nullptr);
  int rc = mpv_command(mpv_, args.data());
  FR_LOG_INFO("[MPV] loadfile {} start={} rc={}", clean, start > 0 ? start : 0.0, rc);
  // d151：记住最近一次点播（已去 fragment 的净 URL），供后端降级后按位重载复用。
  last_uri_ = clean;
}

bool MpvBridge::reload_after_backend_switch() {
  // 主线程专用（tick 内）。渲染线程已把上下文切成 SW；这里只负责"让新后端拿到帧"。
  if (!needs_reload_.load(std::memory_order_acquire)) return false;
  needs_reload_.store(false, std::memory_order_release);
  if (!mpv_ || !render_ || backend_ != VideoBackend::Software) return false;
  if (last_uri_.empty()) return false;

  // 位置/暂停态从 mpv 现取（唯一真值），不依赖主线程缓存的 snap_
  double pos = 0.0;
  if (mpv_get_property(mpv_, "time-pos", MPV_FORMAT_DOUBLE, &pos) < 0) pos = 0.0;
  int paused = 0;
  bool was_paused = mpv_get_property(mpv_, "pause", MPV_FORMAT_FLAG, &paused) >= 0 && paused != 0;

  FR_LOG_WARN("[MPV] 后端降级后按位重载续播 pos={:.2f}s paused={}", pos, was_paused ? 1 : 0);
  load_uri(last_uri_, pos > 0.0 ? pos : 0.0);  // start=pos 精确续播；内部记录 last_uri_ 不变
  if (was_paused) set_pause(true);             // 暂停态一并恢复（loadfile 不重置 pause 属性）
  return true;
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
