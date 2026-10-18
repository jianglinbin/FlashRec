// airplay_source.h —— AirPlay 控制面门面（v0.6.0 d269，从零重写；M0 范围）
//
// 职责（M0）：UxPlay lib 的装配与生命周期（启动序列九步顺序敏感，见 .cpp）、
// 连接计数 / 心跳看门狗、控制回调 → EventBus 三通道上抛（DmrCommand /
// AirplayEvent / 内部状态）。数据面（audio/video → 回环流）M1/M2 落地：
// 对应回调当前为空实现（首包打点）。
//
// 形状比照 DmrDevice：start() 失败仅 WARN 不致命（main.cpp 同 dmr.start 待遇）；
// stop() 幂等；tick() 主线程每帧。
//
// 线程纪律（规则 4）：lib 的 httpd/rtp/mirror 线程进回调 —— 回调里只做
// 「投事件 / 发 DmrCommand」，绝不碰 mpv、UI、状态机。tick() 只在主线程。
//
// 本头不 include 任何 UxPlay 头（第三方头只出现在 .cpp，规则 8）：raop_t /
// dnssd_t 由 .cpp 里 lib 头 typedef，这里只前向声明占位。
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "app/config.h"
#include "app/event_bus.h"

struct raop_s;
struct dnssd_s;
typedef struct raop_s raop_t;    // 与 raop.h 的 typedef 同型（C++ 允许重复声明）
typedef struct dnssd_s dnssd_t;  // 同上

namespace fr::cast {

class AirplayStream;

class AirplaySource {
 public:
  AirplaySource(EventBus& bus, const Config& cfg);
  ~AirplaySource();
  AirplaySource(const AirplaySource&) = delete;
  AirplaySource& operator=(const AirplaySource&) = delete;

  // 装配并启动 UxPlay（mDNS 双服务：_raop._tcp + _airplay._tcp）。
  // 失败逐级清理并返回 false，仅 WARN 不致命（同 dmr.start 语义）。
  bool start();

  // 停止并释放 lib 资源（幂等；退出序列在 dmr.stop() 之前调）。
  // 顺序：raop_destroy（内含 httpd 停止）→ dnssd unregister（发 goodbye）→
  // dnssd_destroy。
  void stop();

  // 心跳看门狗 + 会话收尾轮询（主循环每帧调）：conn_feedback 超时判定客户端
  // 失联；mpv 侧先 EOF（网络断）→ Stopped 时同步收尾 lib 侧。
  void tick();

  bool running() const { return running_.load(std::memory_order_relaxed); }

  // —— 静态回调（.cpp）访问器 ——
  const std::string& password() const { return cfg_.airplay_password; }
  AirplayStream& stream() { return *stream_; }

  // —— 以下仅供 lib 回调调用（public：静态回调在 .cpp 需要访问）——
  void on_conn_init();
  void on_conn_destroy();
  void on_conn_feedback();
  void on_conn_reset(int reason);
  void on_video_reset(int reset_type);
  void on_audio_get_format(unsigned char ct, unsigned short spf, bool using_screen,
                           bool is_media, uint64_t audio_format);
  void on_audio_set_volume(float volume_db);
  double on_audio_get_client_volume();
  void on_video_pause();
  void on_video_resume();
  void on_mirror_video_running(bool running);
  void on_display_pin(const char* pin);
  void on_report_client_request(const char* deviceid, const char* model, const char* name,
                                bool* admit);
  // —— M3：HLS / 视频 URL 投送的控制面 ——
  void on_video_scrub(float position);
  void on_video_rate(float rate);
  void on_video_stop();
  void on_video_acquire_playback_info(void* info);  // playback_info_t*（lib 类型只进 .cpp）
  float on_video_playlist_remove();

 private:
  void publish_stop();
  void publish_play();
  void reset_session(const char* why);
  bool check_watchdog(double now_mono);  // true = 本轮触发了失联收尾
  void check_snapshot();                 // mpv 侧 Stopped → 收尾 lib 侧会话
  void teardown_stack(const char* why);  // lib 栈收尾（不碰播放侧状态；stop/接管共用）
  void preempt_for_other_source();       // ★d435：DLNA/本地投屏接管 → 结束 AirPlay
  bool make_hw_addr(unsigned char out[6]);  // false = 回退随机本地管理地址

  EventBus& bus_;
  const Config& cfg_;

  std::unique_ptr<AirplayStream> stream_;  // 数据面（M1 回收；lib 头不进本头）

  raop_t* raop_ = nullptr;
  dnssd_t* dnssd_ = nullptr;

  std::atomic<bool> running_{false};
  std::atomic<int> conn_count_{0};
  // 心跳：lib 的 conn_feedback 在 httpd 线程写，tick 在主线程读 —— 单调秒 + 原子。
  std::atomic<double> last_feedback_at_{0.0};
  std::atomic<bool> watchdog_armed_{false};
  // ★d435：预停期抑制「Stop 指令」上抛。raop_destroy 会触发 conn_destroy 等回调，
  //   它们会 publish_stop；若在「被 DLNA 接管」收尾时放出去，[req] Stop 会杀掉
  //   刚接管的 DLNA 会话。teardown_stack 置位、下次 start() 成功后解除。
  std::atomic<bool> suppress_stop_{false};
  // 客户端名（report_client_request 拿到）：只在 lib httpd 线程写/读（M0 不跨线程）。
  std::string title_;
  // ★d435：曾确认过「当前活动会话归 AirPlay」（快照来源 = AirPlay）。只在主线程
  //   （check_snapshot/teardown）读写。用于区分「DLNA 接管」与「本会话 SetUri
  //   尚未被播放侧消化」两种来源非 AirPlay 的瞬时态。
  bool owned_ = false;
};

}  // namespace fr::cast
