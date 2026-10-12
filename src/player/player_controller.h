#pragma once
// 播放状态机：全项目唯一真值（DMR_SOAP_SPEC §2 六不变量 + AGENTS.md 规则 3）。
// 快照 PlaybackSnapshot 经事件总线对外发布；UI 与 SOAP 层只读，禁止第二份副本。
#include <deque>
#include <memory>
#include <string>

#include "app/event_bus.h"
#include "player/mpv_bridge.h"

namespace fr {

struct Config;

class PlayerController {
 public:
  PlayerController(EventBus& bus, const Config& cfg);
  ~PlayerController();

  bool init();   // mpv 初始化 + 属性订阅
  void shutdown();

  // 主线程每帧：泵 mpv 事件 → 转移；看门狗；保持期清理；节流发布
  void tick(double now_seconds);

  // 总线 DmrCommand 的执行入口（也在主线程）
  void on_command(const DmrCommand& cmd);

  // 渲染透传：mpv 画面渲到外部 FBO（渲染线程调用；mpv 未就绪时空操作）
  void render_frame(int fbo, int w, int h);
  // d50：mpv 是否有新视频帧要渲染（渲染线程在出帧路径调用；false 时可整体跳过
  // mpv 渲染、直接复用 FBO 上一帧内容 —— 暂停/无新帧时 UI 重绘不再白跑视频链）
  bool video_needs_render();
  // swap 完成通知：mpv 官方要求每次 render 后成对调用，否则内部同步退化
  // （在 resize 这种高频重渲场景下会放大卡顿）。渲染线程在 swap_buffers 后调用。
  void report_swap();
  // 创建 mpv OpenGL 渲染上下文（须在 GL 上下文就绪后、持有该上下文的线程调用一次）
  bool init_render(void* (*get_proc)(void* user, const char* name), void* user);
  // mpv 实测视频信息整包（渲染线程 2Hz 轮询读，d44/d45；无视频各字段回 -1）
  VideoStats video_stats();
  // v0.6.1 d343：视频显示尺寸（SAR 修正；数字键缩放档位锚定基准）。mpv API 仍
  // 收口在本层（规则：mpv 只准出现在 src/player/），main 经此转发调用。
  bool video_display_size(int* w, int* h);

  // —— d124：GL 后端失败 → 软件渲染兜底（渲染线程专用，见 mpv_bridge.h）——
  // 返回 true = 本次发生了后端切换，调用方应强制重渲一帧
  bool switch_video_backend_if_needed();
  bool software_video() const;
  // 软件路径渲染一帧到内部 RGBA 缓冲；pixels 在下次渲染前有效
  bool render_sw_frame(int w, int h);
  const unsigned char* sw_pixels() const;
  // d125：渲染线程用 SEH 接住显卡驱动异常后上报（见 platform/seh_guard.h），
  // 使下帧的 switch_video_backend_if_needed() 生效
  void report_gl_crash();
  // v0.6.1 d350：画面缩放/平移（mpv video-zoom / video-pan-x|y，渲染侧会话级；
  // main 经此转发——mpv API 收口规则）。新会话在 mpv_bridge::load_uri 内重置。
  // d351：zoom 入参为 mpv 原生 2^z 标度（0=适配基准），百分比换算在 main 侧。
  void set_video_zoom(double z);
  void set_video_pan(double x, double y);
  // v0.6.1 d353：解码方式选择（右键「解码」子菜单）。0 软解 / 1 自动 / 2 NVDEC /
  // 3 DXVA2。内部按位重载续播（对已加载流确定生效），重载重置的渲染属性由 main 重放。
  void set_hwdec_mode(int mode);
  // d184：视频填充（cover=1 / contain=0）
  void set_panscan(double p);
  // ★d295：当前是否处于「镜像自适应填充」模式（回环镜像流 = d281 的按比例自适应）。
  //   切皮肤时需要它做闸门 —— 见 main.cpp 的切皮肤逻辑。
  bool fit_auto_active() const { return fit_auto_; }
  // ★d295：切皮肤后把 panscan 交还给自适应判决。
  //   病灶：切皮肤会无条件 set_panscan(皮肤 fit) ⇒ 把 d281 已下发的自适应判决**覆盖掉**。
  //   实测（16:24:03）：panscan 由 1 变0，render 日志从
  //   `(75,0) 1770x884 -> (0,0) 960x480`（裁两侧、上下铺满）
  //   退回 `(0,0) 1920x884 -> (0,19) 960x442`（留边、上下黑边）。
  //   ⇒ 自适应期间切皮肤**不应**动 panscan；非自适应期间才听皮肤的。
  void restore_fit_after_skin_change();
  // d196：留边底色（letterbox）= 皮肤 stageBg，避免 contain 恒黑边
  void set_background_color(const std::string& rgb_hex);
  // ★d281：镜像自适应填充 —— 主循环每轮把**舞台宽高比**（逻辑坐标，顶栏可见时不计入）
  // 推给播放器。**只对回环（AirPlay 镜像/音频）流生效**：视频比舞台宽 → cover
  // （铺满，先裁掉的正是源内烧录黑边）；比舞台窄 → contain（竖屏镜像保住整屏，上下已满）。
  // DLNA 直链不适用 —— 电影 2.39:1 被 cover 会裁掉三成画面，那些继续由皮肤 fit 决定。
  void set_stage_aspect(double aspect);
  // d151：后端降级后按位重载续播（主线程 tick 内调用；切换本身由渲染线程做）。
  // 返回 true = 本次真的重载了当前文件。
  bool reload_after_backend_switch();

 private:
  // —— 状态机核心：唯一转移入口（不变量 1/3）——
  void apply_state(TransportState next, const char* why, int req_id = 0);
  void enter_stopped(const char* why, int req_id);
  void clear_session(const char* why);

  void handle_set_uri(const DmrCommand& c, double now);
  void handle_set_next_uri(const DmrCommand& c, double now);
  void handle_play(const DmrCommand& c, double now);
  void handle_pause(const DmrCommand& c, double now);
  void handle_stop(const DmrCommand& c, double now);
  void handle_seek(const DmrCommand& c, double now);
  void handle_volume(const DmrCommand& c, double now);
  void handle_mute(const DmrCommand& c, double now);
  void handle_speed(const DmrCommand& c, double now);  // d169
  void handle_preset(const DmrCommand& c, double now);

  // —— mpv 事件 → 转移 ——
  void on_mpv_event(const MpvEvent& e, double now);
  void on_file_loaded(double now);
  void on_end_file(int reason, double now);
  void on_property(const MpvEvent& e, double now);
  void run_pending_after_load();

  // —— 发布 ——
  void publish_state(const char* why);
  void publish_position(bool force);
  void publish_media();
  void publish_volume();
  void publish_speed();  // d169

  double calibrate_target(const DmrCommand& c) const;  // R1 重复 URI 进度校准

  EventBus& bus_;
  const Config& cfg_;
  std::unique_ptr<MpvBridge> mpv_;

  PlaybackSnapshot snap_;            // 唯一真值快照
  std::deque<DmrCommand> pending_;   // TRANSITIONING 期排队（不变量 6：只排队不合并）
  double now_ = 0.0;
  // d279：端到端延迟探针（仅回环实时流；tcp://127.0.0.1:）。
  //   live_start_wall_ = 本次 loadfile 的墙钟（steady 秒），live_probe_next_ = 1s 限频。
  //   物理意义：端到端积压 =（当前墙钟 − live_start_wall_）− time-pos。
  //   它只含「接收端侧」积压（生产队列 + TCP + mpv demuxer + 解码显示），
  //   不含客户端编码/网络 —— 这正好能把「10s 在谁那边」一刀切开。
  double live_start_wall_ = -1.0;
  double live_probe_next_ = 0.0;
  // ★d312：本会话的回环加载格式（"mpegts"/"latm"/"caf"/"aac"，DLNA 恒空）。
  //   暂停恢复跳直播前沿时重载 URI 必须带上它（否则裸流探测重走字节门，d285）。
  std::string live_format_;
  // ★d322：直播回流延迟追赶控制器——积压超阈值自动倍速消费（scaletempo2 不变调），
  //   追平回 1.0。AirPlay 会话的用户倍速按钮/命令已禁用，倍速权归控制器。
  double catchup_speed_ = 1.0;
  // ★d323：控制器采样计时（200ms 周期，d322 的 1s 提频）。
  double catchup_next_ = 0.0;
  // ★d283 取证：真机报「延迟 10s+」，实测三段已排除生产积压(0.02s)与 demuxer 预读(0.10s)，
  //   但「loadfile → 第一条延迟探针」的 7.58s 无法定性 —— 旧探针只由 time-pos 事件驱动，
  //   而该事件要等 mpv 开播才发，且状态需 file-loaded 才翻 PLAYING，两个未知量（mpv 侧
  //   就绪耗时 / 主线程 auto-play 冲队耗时）被折成一条。下面两个墙钟把这一刀切成三段：
  //   loadfile →(A: mpv 就绪)→ file-loaded →(B: 开播排队)→ 首个 time-pos。
  //   ★A+B 都算进「接收端积压」，但修法完全不同，必须先分离。
  double live_loaded_wall_ = -1.0;      // 本次 loadfile 的 file-loaded 墙钟（-1=未到）
  double live_firstpos_wall_ = -1.0;    // 首个有效 time-pos 的墙钟（-1=未到）
  // ★d281：镜像自适应填充状态（全在主线程 tick 内读写，无并发）。
  //   fit_auto_       = 本次加载是不是回环镜像流（唯一开关，DLNA 恒 false）
  //   stage_aspect_   = 舞台宽高比（主循环推送；窗口缩放/全屏切换即时更新）
  //   fit_applied_    = 已下发的 panscan（-1 = 本会话尚未判定）
  //   fit_probe_next_ = 比例轮询限频（0.5s；流中途换比例/手机旋转也能跟上）
  bool fit_auto_ = false;
  // ★d299：探针断流检测（apts 连续不动的采样计数 + 上次 apts）。
  //   用于在客户端断开后停打「接收端积压」—— 它会随墙钟线性增长，是记账假象。
  // ★d302：数据侧延迟的滑动峰值（demuxer 已读位置 − time-pos）。
  //   用户感知的是「最坏的那几秒」而非瞬时值，故保留峰值；回落时按 0.98 缓慢衰减，
  //   避免单次尖峰永久钉住读数。
  double live_data_side_ = 0.0;
  double live_probe_last_apts_ = -1.0;
  int live_probe_stall_ = 0;
  double stage_aspect_ = 0.0;
  double fit_applied_ = -1.0;
  double fit_probe_next_ = 0.0;
  double last_pos_pub_ = -1.0;       // 位置节流 1s（GENA 位置类）
  double transition_at_ = 0.0;       // TRANSITIONING 起始（15s 看门狗，不变量 5）
  double stop_at_ = 0.0;             // STOPPED 时刻（保持期 R5）
  bool pending_stop_ = false;        // EOF 无 NextURI：先 TRANSITIONING 再 STOPPED（R2）
  double seek_pending_ = -1.0;       // 发起中的 seek 目标（R3：确认完成后才刷新位置）
  int reconnect_left_ = 0;
  double reconnect_at_ = 0.0;        // 指数退避重连时刻（R6）
  double last_reconnect_delay_ = 1.0;  // 上次退避时长（日志用）
  std::string reconnect_uri_;
};

}  // namespace fr
