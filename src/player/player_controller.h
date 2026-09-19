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
  // d184：视频填充（cover=1 / contain=0）
  void set_panscan(double p);
  // d196：留边底色（letterbox）= 皮肤 stageBg，避免 contain 恒黑边
  void set_background_color(const std::string& rgb_hex);
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
