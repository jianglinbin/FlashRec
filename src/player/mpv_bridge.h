#pragma once
// mpv_bridge：全项目唯一 mpv API 出入口（AGENTS.md 规则 1/4）。
// 只做"翻译"：mpv 命令/事件 ↔ MpvEvent；状态机逻辑一律在 player_controller。
#include <atomic>
#include <functional>
#include <string>
#include <vector>

struct mpv_handle;
struct mpv_render_context;

namespace fr {

// d124：视频渲染后端。GL 是首选；老显卡驱动编译不了 mpv 的 GLSL 时降级到软件路径。
enum class VideoBackend {
  GL,        // mpv_render_context(OPENGL) 渲到外部 FBO
  Software,  // mpv_render_context(SW) 出 CPU RGBA 缓冲，由 UI 层上传纹理
};

// d45：视频信息徽章用的一次性查询结果（渲染线程 2Hz 轮询）
struct VideoStats {
  double fps = -1;        // estimated-vf-fps 实测帧率（无视频/不可用 -1）
  long long bitrate = -1; // 总码率 bps（无数据 -1）
  int w = -1, h = -1;     // 视频分辨率（无视频 -1）
  // d146 R5：徽章三合一扩展（属性名已用 tools/mpv_load_probe.py 实测确认：
  // video-format / video-bitrate / hwdec-current / audio-codec / audio-bitrate /
  // audio-params/samplerate / audio-params/channel-count）
  bool hwdec = false;          // hwdec-current 非空且 != "no"（true=硬解）
  std::string hwdec_name;      // d146：hwdec-current 原值（"no"=软解，""=取不到；徽章附后端名）
  std::string video_codec;     // video-format 短名（h264/vp9/...；取不到为空）
  long long video_br = -1;     // video-bitrate bps（取不到 -1）
  std::string audio_codec;     // audio-codec（无音轨为空）
  long long audio_br = -1;     // audio-bitrate bps（无音轨/取不到 -1）
  int audio_sr = -1;           // audio-params/samplerate Hz
  int audio_ch = -1;           // audio-params/channel-count
};

struct MpvEvent {
  enum class Kind {
    FileLoaded,      // 媒体加载完成（TRANSITIONING → PLAYING/READY 的触发源）
    EndFile,         // 播放结束（reason 区分 eof/error/stop/quit/redirect，R2/R6）
    IdleEntered,     // 内部清空（防御性）
    PropertyChanged, // 观测属性
    LogMessage,      // mpv 日志（转 spdlog [MPV]，反查链）
  };
  Kind kind = Kind::FileLoaded;
  int reason = 0;          // EndFile: MPV_END_FILE_REASON_*
  std::string prop_name;   // PropertyChanged
  double num = 0.0;        // 数值属性（time-pos/duration）
  int64_t flag = 0;        // flag 属性（pause/mute/seekable/seeking）
  std::string text;        // LogMessage
};

class MpvBridge {
 public:
  using EventCb = std::function<void(const MpvEvent&)>;

  MpvBridge() = default;
  ~MpvBridge();
  MpvBridge(const MpvBridge&) = delete;
  MpvBridge& operator=(const MpvBridge&) = delete;

  bool init();
  void shutdown();

  // render API：get_proc 来自 GLFW 的 glfwGetProcAddress 适配（见 window_glfw/main）
  bool init_render(void* (*get_proc)(void* user, const char* name), void* user);
  // 渲到外部 FBO（UI 层拿纹理做合成）；fbo=0 表示默认帧缓冲
  void render_to_fbo(int fbo, int w, int h, bool flip_y);
  void report_swap();
  // d50：mpv 更新旗标查询（须在持有 GL 上下文的渲染线程调用）。
  // MPV_RENDER_UPDATE_FRAME 置位 = 有新视频帧/画面变化必须渲染；未置位时
  // mpv_render_context_render 可以整体跳过 —— FBO 里仍是上一帧的有效内容，
  // UI 重绘直接复用该纹理（暂停时操作 UI 不再白跑视频渲染链）。
  bool frame_update_pending();

  // —— d124：GL 后端失败 → 软件渲染兜底 ——
  // 背景：老 AMD 驱动（如 HD 6570 / Catalyst 15.7.1）编译不了 mpv 的 NV12→RGB
  // 转换 pass（`layout(offset=…) mat3` 被误判 overlap，报 error #444）⇒ 链接失败
  // ⇒ 每帧只有底色 ⇒ 透明窗口透出桌面，表现为"白屏"。驱动还会把
  // GL_ARB_enhanced_layouts 等扩展谎报为支持，所以降 GL 版本也绕不过去。
  // 兜底：改用 MPV_RENDER_API_TYPE_SW，全程 CPU 出帧，完全不依赖 GPU 驱动。
  //
  // 主线程在 pump() 里从 mpv 日志判定失败并置位；渲染线程消费。
  bool backend_gl_broken() const { return gl_broken_.load(std::memory_order_acquire); }
  // d125：由**渲染线程**在 SEH 接住显卡驱动异常后调用（见 platform/seh_guard.h）。
  // 与 pump() 里的日志检测互为补充：那条路只在驱动"善良地报错"（打 error 日志）时有效；
  // 驱动直接抛访问违例时进程当场就死了，根本来不及产生日志 —— 只能靠 SEH 现场置位。
  void report_gl_crash() { gl_broken_.store(true, std::memory_order_release); }
  VideoBackend backend() const { return backend_; }
  bool software_backend() const { return backend_ == VideoBackend::Software; }
  // 若已判定 GL 失败且尚未切换，则切到软件后端。
  // ★必须在持有 GL 上下文的渲染线程调用（mpv_render_context_free 要求同线程同上下文）。
  // 返回 true = 本次真的发生了切换，调用方应据此强制重渲一帧。
  bool switch_to_software_if_needed();  // 软件路径：渲染一帧到内部 RGBA 缓冲（渲染线程）。缓冲由本类持有，尺寸变化时重分配。
  // d151：GL 后端降级后「按位重载续播」。背景：①mpv_render_context_free + 新建 SW 上下文
  // 会掐断播放（d127 实测）；②create_sw_context 内的 hwdec=no 只对后续 loadfile 生效。
  // 两者都要求重载当前文件才能恢复出帧——不重载的表现是"降级成功但画面全黑、video fps 为空"。
  // 由**主线程**调用（tick 内）：位置/暂停态从 mpv 现取，loadfile start=<pos> 精确续播。
  bool reload_after_backend_switch();
  // 注意：不在内部查 update() 旗标（帧首已 peek 过一次，二次查询会吃掉下一帧旗标）。
  bool render_sw(int w, int h);
  const unsigned char* sw_pixels() const { return sw_buf_.empty() ? nullptr : sw_buf_.data(); }

  void set_event_cb(EventCb cb) { event_cb_ = std::move(cb); }
  void pump();  // 主线程每帧：非阻塞取空 mpv 事件队列

  // —— 命令（mpv 自身线程安全；实际调用点全在主线程）——
  void load_uri(const std::string& uri, double start_sec = -1.0);  // R1：replace，不重建句柄
  void set_pause(bool p);
  void seek_abs(double sec);
  void set_volume(int vol);  // 0-100
  void set_mute(bool m);
  void set_speed(double s);  // d169：倍速（mpv speed 属性；1.0 = 原速）
  // d184：视频填充 —— cover=1（短边填满/居中/裁切，不变型无空白）/ contain=0（留黑边）
  void set_panscan(double p);

  // —— 查询（主线程）——
  double get_time_pos();
  double get_duration();
  bool get_seekable();
  // mpv 实测视频帧率（estimated-vf-fps；无视频/不可用返回 -1）。
  // mpv client API 线程安全，渲染线程 2Hz 轮询读取（d44）。
  double get_video_fps();
  // d45：视频信息整包查询（帧率/码率/分辨率），mpv_get_property 逐项、缺项回 -1。
  VideoStats get_video_stats();
  bool valid() const { return mpv_ != nullptr; }

 private:
  // d125：建软件渲染上下文（关硬解 + MPV_RENDER_API_TYPE_SW）。
  // 由 init_render（已知 GL 不可用时直接起步）与 switch_to_software_if_needed 共用。
  bool create_sw_context();

  struct mpv_handle* mpv_ = nullptr;
  // GL 或 SW 二选一的渲染上下文（切换后端时先 free 再重建，同一指针复用）
  struct mpv_render_context* render_ = nullptr;
  EventCb event_cb_;

  // d124：软件兜底状态。gl_broken_ 由主线程（pump）置位、渲染线程读；
  // 其余字段只在渲染线程被触碰。
  std::atomic<bool> gl_broken_{false};
  VideoBackend backend_ = VideoBackend::GL;
  std::vector<unsigned char> sw_buf_;  // RGBA8，sw_w_ * sw_h_
  int sw_w_ = 0;
  int sw_h_ = 0;

  // d151：降级后按位重载。needs_reload_ 渲染线程置位（切后端处）、主线程消费（tick）；
  // last_uri_ 主线程写（load_uri 记录，已去 fragment），重载时复用。
  std::atomic<bool> needs_reload_{false};
  std::string last_uri_;
  // d184：视频填充（panscan）= cover|contain 映射；load 时应用。
  double panscan_ = 1.0;
};

}  // namespace fr
