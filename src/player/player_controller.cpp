#include "player/player_controller.h"

#include <chrono>
#include <cmath>

#include <mpv/client.h>

#include "app/config.h"
#include "app/log.h"
#include "app/str_util.h"

namespace fr {

namespace {
double mono_now() {
  using namespace std::chrono;
  return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}
}  // namespace

PlayerController::PlayerController(EventBus& bus, const Config& cfg) : bus_(bus), cfg_(cfg) {}

PlayerController::~PlayerController() { shutdown(); }

bool PlayerController::init() {
  mpv_ = std::make_unique<MpvBridge>();
  if (!mpv_->init()) return false;
  mpv_->set_event_cb([this](const MpvEvent& e) { on_mpv_event(e, mono_now()); });
  snap_.volume = 100;
  FR_LOG_INFO("[DMR] 状态机就绪（Idle）");
  return true;
}

void PlayerController::shutdown() {
  if (mpv_) mpv_->shutdown();
}

// ---------------- 主循环入口 ----------------

void PlayerController::tick(double now) {
  now_ = now;
  if (!mpv_) return;
  mpv_->pump();  // mpv 事件 → on_mpv_event → 转移（不变量 4：全程主线程）

  // d151：GL→SW 降级后按位重载续播（渲染线程刚切的上下文拿不到帧，必须重载；
  // 且 hwdec=no 只对后续 loadfile 生效）。放在 pump 之后：先消化既有事件，再发起重载。
  reload_after_backend_switch();

  // TRANSITIONING 看门狗（不变量 5：15s 强制 STOPPED + ERROR）
  if (snap_.state == TransportState::Transitioning && transition_at_ > 0 &&
      now - transition_at_ > 15.0) {
    FR_LOG_ERROR("[DMR] [req#-] TRANSITIONING 超 15s，强制 STOPPED（防卡死）");
    enter_stopped("transition-timeout", -1);
  }

  // EOF 无 NextURI：R2 要求 PLAYING→TRANSITIONING→STOPPED 两条事件
  if (pending_stop_ && snap_.state == TransportState::Transitioning && now - transition_at_ > 0.05) {
    pending_stop_ = false;
    enter_stopped("eof-no-next", -1);
  }

  // 断流重连（R6：指数退避）
  if (snap_.state == TransportState::Transitioning && !reconnect_uri_.empty() &&
      reconnect_at_ > 0 && now >= reconnect_at_) {
    double delay = reconnect_at_;
    reconnect_at_ = 0;
    FR_LOG_INFO("[DMR] [req#-] 直播重连尝试（剩余 {} 次，退避 {}s）", reconnect_left_,
                (int)last_reconnect_delay_);
    (void)delay;
    mpv_->load_uri(reconnect_uri_);
  }

  // R5：进度保持期到期 → 清零回 Idle
  if (snap_.state == TransportState::Stopped && snap_.has_session && stop_at_ > 0 &&
      now - stop_at_ > cfg_.dlna_progress_retention_sec) {
    clear_session("retention-expired");
  }

  publish_position(false);  // 1s 节流的位置事件
}

void PlayerController::render_frame(int fbo, int w, int h) {
  if (mpv_ && snap_.picture_ready) mpv_->render_to_fbo(fbo, w, h, true);
}

// —— d124：软件渲染兜底（全在渲染线程；mpv_bridge 内已注释为什么必须这个线程）——
bool PlayerController::switch_video_backend_if_needed() {
  return mpv_ ? mpv_->switch_to_software_if_needed() : false;
}

// —— d151：降级后的按位重载（主线程；切换由渲染线程完成，但播放语义只在此处改）——
bool PlayerController::reload_after_backend_switch() {
  return mpv_ ? mpv_->reload_after_backend_switch() : false;
}

bool PlayerController::software_video() const { return mpv_ && mpv_->software_backend(); }

bool PlayerController::render_sw_frame(int w, int h) {
  if (!mpv_ || !snap_.picture_ready) return false;
  return mpv_->render_sw(w, h);
}

const unsigned char* PlayerController::sw_pixels() const {
  return mpv_ ? mpv_->sw_pixels() : nullptr;
}

void PlayerController::report_gl_crash() {
  if (mpv_) mpv_->report_gl_crash();
}

bool PlayerController::video_needs_render() {
  return mpv_ ? mpv_->frame_update_pending() : false;
}

void PlayerController::report_swap() {
  if (mpv_) mpv_->report_swap();
}

bool PlayerController::init_render(void* (*get_proc)(void* user, const char* name), void* user) {
  if (!mpv_) return false;
  return mpv_->init_render(get_proc, user);
}

VideoStats PlayerController::video_stats() { return mpv_ ? mpv_->get_video_stats() : VideoStats{}; }

// ---------------- 指令处理 ----------------

void PlayerController::on_command(const DmrCommand& cmd) {
  double now = mono_now();
  now_ = now;
  if (!mpv_) return;
  switch (cmd.type) {
    case DmrCommand::Type::SetUri: handle_set_uri(cmd, now); break;
    case DmrCommand::Type::SetNextUri: handle_set_next_uri(cmd, now); break;
    case DmrCommand::Type::Play: handle_play(cmd, now); break;
    case DmrCommand::Type::Pause: handle_pause(cmd, now); break;
    case DmrCommand::Type::Stop: handle_stop(cmd, now); break;
    case DmrCommand::Type::Seek: handle_seek(cmd, now); break;
    case DmrCommand::Type::SetVolume: handle_volume(cmd, now); break;
    case DmrCommand::Type::SetMute: handle_mute(cmd, now); break;
    case DmrCommand::Type::SelectPreset: handle_preset(cmd, now); break;
  }
}

void PlayerController::apply_state(TransportState next, const char* why, int req_id) {
  if (snap_.state == next) return;
  FR_LOG_INFO("[DMR] [req#{}] 状态转移 {} -> {}（{}）", req_id, transport_state_name(snap_.state),
              transport_state_name(next), why);
  snap_.state = next;
  publish_state(why);
}

void PlayerController::enter_stopped(const char* why, int req_id) {
  // R5：Stop 先把当前 RelTime/Duration/URI 固化为会话快照，再进 STOPPED
  stop_at_ = now_;
  snap_.has_session = true;
  snap_.picture_ready = false;
  snap_.next_uri.clear();
  snap_.next_metadata.clear();
  pending_.clear();
  seek_pending_ = -1;
  reconnect_left_ = 0;
  reconnect_uri_.clear();
  mpv_->set_pause(true);  // 停止后 mpv 静默挂起，保持最后画面 2s（R5）
  apply_state(TransportState::Stopped, why, req_id);
  publish_media();
  FR_LOG_INFO("[DMR] [req#{}] 进度快照固化 pos={} dur={} 保持期 {}s", req_id,
              format_hms_seconds(snap_.position), format_hms_seconds(snap_.duration),
              cfg_.dlna_progress_retention_sec);
}

void PlayerController::clear_session(const char* why) {
  snap_ = PlaybackSnapshot{};  // 全部清零回 Idle（新 SetURI 或超时，R5）
  stop_at_ = 0;
  pending_.clear();
  mpv_->set_pause(true);
  publish_media();
  publish_state(why);
  FR_LOG_INFO("[DMR] [req#-] 会话清零（{}）", why);
}

double PlayerController::calibrate_target(const DmrCommand& c) const {
  // R1：重复 URI 的进度载体 —— #t= fragment；metadata 里的 res@duration 是总时长，
  // 不能当目标位置，这里只认 fragment（显式 Seek 走 handle_seek 的正常通道）。
  double t = uri_start_fragment(c.uri);
  if (t > 0) return t;
  return -1.0;
}

void PlayerController::handle_set_uri(const DmrCommand& c, double now) {
  const std::string base = uri_without_fragment(c.uri);
  bool playing_like = snap_.state == TransportState::Playing ||
                      snap_.state == TransportState::PausedPlayback ||
                      (snap_.state == TransportState::Ready && snap_.has_session);
  if (playing_like && snap_.has_session && base == uri_without_fragment(snap_.uri)) {
    // R1：重复 URI（== 当前且在播）→ 进度校准，绝不重载
    // d146：DuplicateURI 分支同样发 cast 信号（"用户在投屏"的第二次确认；
    // 上层 2s 同 URI 防抖自行去重，呈现管线对它幂等）
    bus_.publish_cast({c.uri, true});
    double target = calibrate_target(c);
    if (target >= 0) {
      FR_LOG_INFO("[DMR] [req#{}] DuplicateURI calibrate pos={}", c.req_id,
                  format_hms_seconds(target));
      DmrCommand seek = c;
      seek.type = DmrCommand::Type::Seek;
      seek.seek_to = target;
      handle_seek(seek, now);
    } else {
      FR_LOG_WARN("[DMR] [req#{}] DuplicateURI ignored（无进度载体，幂等成功）", c.req_id);
    }
    return;
  }

  // 任何状态无条件接受新 URI（R1），保持会话存活
  stop_at_ = 0;
  snap_.uri = c.uri;
  snap_.metadata = c.metadata;
  snap_.title = extract_dc_title(c.metadata);
  snap_.next_uri.clear();
  snap_.next_metadata.clear();
  snap_.position = 0;
  snap_.duration = 0;
  snap_.is_live = false;
  snap_.seekable = true;
  snap_.picture_ready = false;
  snap_.has_session = true;
  pending_.clear();
  seek_pending_ = -1;
  reconnect_uri_.clear();
  publish_media();

  // metadata 里的 #t 也可能藏在 URI 本身；统一交给 load_uri 处理
  // d146：接受新 URI = 标准投屏信号（呈现管线 P1–P5 在主循环执行）
  bus_.publish_cast({c.uri, false});
  apply_state(TransportState::Transitioning, "set-uri", c.req_id);
  transition_at_ = now;
  mpv_->load_uri(c.uri);
  FR_LOG_INFO("[DMR] [req#{}] SetURI {} title=\"{}\"", c.req_id, uri_without_fragment(c.uri),
              snap_.title);
  // ★d143：DIDL 原文落盘（受 log.trace_http_body 控制）。起始进度的候选载体之一就藏在
  // <res> 的扩展属性里（如 sec:InitialPosition），而此前全项目只保留了 extract_dc_title()
  // 的产物 ⇒ **metadata 原文从未落过盘**，这条线索一直不可见。
  if (cfg_.log_trace_http_body && !c.metadata.empty())
    FR_LOG_INFO("[DMR] [req#{}] SetURI metadata[{}B] = {}", c.req_id, c.metadata.size(),
                c.metadata);
}

void PlayerController::handle_set_next_uri(const DmrCommand& c, double now) {
  (void)now;
  snap_.next_uri = c.uri;
  snap_.next_metadata = c.metadata;
  FR_LOG_INFO("[DMR] [req#{}] SetNextURI {}", c.req_id, uri_without_fragment(c.uri));
  publish_media();
}

void PlayerController::handle_play(const DmrCommand& c, double now) {
  using S = TransportState;
  if (snap_.state == S::Transitioning) {
    pending_.push_back(c);  // 不变量 6：排队暂存，file-loaded 后按序执行
    FR_LOG_DEBUG("[DMR] [req#{}] Play 在 TRANSITIONING 期入队", c.req_id);
    return;
  }
  if (snap_.state == S::Ready || snap_.state == S::PausedPlayback) {
    mpv_->set_pause(false);
    apply_state(S::Playing, "play", c.req_id);
  } else if (snap_.state == S::Playing) {
    mpv_->set_pause(false);  // 幂等
  } else if (snap_.state == S::Stopped && snap_.has_session) {
    // 保持期内 Play：重推当前 URI 继续（部分 App 行为）
    mpv_->load_uri(snap_.uri, snap_.position);
    apply_state(S::Transitioning, "play-after-stop", c.req_id);
    transition_at_ = now;
  } else {
    FR_LOG_WARN("[DMR] [req#{}] Play 在 {} 非法（SOAP 层回 701）", c.req_id,
                transport_state_name(snap_.state));
  }
}

void PlayerController::handle_pause(const DmrCommand& c, double now) {
  using S = TransportState;
  if (snap_.state == S::Transitioning) {
    pending_.push_back(c);
    FR_LOG_DEBUG("[DMR] [req#{}] Pause 在 TRANSITIONING 期入队", c.req_id);
    return;
  }
  if (snap_.state == S::Playing) {
    mpv_->set_pause(true);
    apply_state(S::PausedPlayback, "pause", c.req_id);
  } else if (snap_.state == S::PausedPlayback) {
    // 幂等
  } else {
    FR_LOG_WARN("[DMR] [req#{}] Pause 在 {} 非法（SOAP 层回 701）", c.req_id,
                transport_state_name(snap_.state));
  }
  (void)now;
}

void PlayerController::handle_stop(const DmrCommand& c, double now) {
  if (snap_.state == TransportState::Transitioning) {
    pending_.push_back(c);  // R1：刚推完 URI 就 Stop —— file-loaded 后执行
    FR_LOG_DEBUG("[DMR] [req#{}] Stop 在 TRANSITIONING 期入队", c.req_id);
    return;
  }
  if (snap_.state == TransportState::Idle) {
    FR_LOG_DEBUG("[DMR] [req#{}] Stop 在 Idle，幂等", c.req_id);
    return;
  }
  mpv_->set_pause(true);
  apply_state(TransportState::Stopped, "stop", c.req_id);
  enter_stopped("stop", c.req_id);
  (void)now;
}

void PlayerController::handle_seek(const DmrCommand& c, double now) {
  using S = TransportState;
  if (snap_.state == S::Transitioning) {
    pending_.push_back(c);
    FR_LOG_DEBUG("[DMR] [req#{}] Seek 在 TRANSITIONING 期入队", c.req_id);
    return;
  }
  if (snap_.is_live && !snap_.seekable) {
    // R6：非 DVR 直播禁止 seek（强 seek 断流），SOAP 层负责回 710
    FR_LOG_WARN("[DMR] [req#{}] 直播流不可 seek，忽略（710）", c.req_id);
    return;
  }
  double target = c.seek_to;
  double dur = snap_.duration;
  if (dur > 0 && target > dur) target = dur;
  if (target < 0) target = 0;
  if (snap_.state == S::Stopped && snap_.has_session && !snap_.uri.empty()) {
    // R5 保持期内 Seek（EOF/Stop 后快照固化，mpv 已无文件）：按 handle_play 的
    // play-after-stop 同语义重载 URI 定位到目标。直接 seek_abs 会打在空 mpv 上
    // 回 MPV_ERROR_COMMAND(-12)（实测：86.92s 短片播完后连按方向键 → -12 风暴）。
    if (dur > 0 && target >= dur) {
      FR_LOG_WARN("[DMR] [req#{}] 保持期内 Seek 目标已达片尾 {}，忽略", c.req_id,
                  format_hms_seconds(target));
      return;
    }
    FR_LOG_INFO("[DMR] [req#{}] 保持期内 Seek：重载 URI 定位 pos={}", c.req_id,
                format_hms_seconds(target));
    mpv_->load_uri(snap_.uri, target);
    apply_state(S::Transitioning, "seek-after-stop", c.req_id);
    transition_at_ = now;
    return;
  }
  if (snap_.state == S::Idle || !snap_.has_session) {
    FR_LOG_WARN("[DMR] [req#{}] Seek 在 {} 态无会话文件，忽略", c.req_id,
                transport_state_name(snap_.state));
    return;
  }
  seek_pending_ = target;
  (void)now;
  mpv_->seek_abs(target);
  // R3：位置以 seeking=false 确认为准（见 on_property），发起时不刷新缓存
}

void PlayerController::handle_volume(const DmrCommand& c, double now) {
  int v = c.volume;
  if (v < 0) v = 0;
  if (v > 100) v = 100;
  snap_.volume = v;
  mpv_->set_volume(v);
  publish_volume();
  (void)now;
  FR_LOG_INFO("[DMR] [req#{}] SetVolume {} ({}dB)", c.req_id, v, -6000 + 60 * v / 100);
}

void PlayerController::handle_mute(const DmrCommand& c, double now) {
  snap_.muted = c.mute;
  mpv_->set_mute(c.mute);
  publish_volume();
  (void)now;
  FR_LOG_INFO("[DMR] [req#{}] SetMute {}", c.req_id, c.mute ? "1" : "0");
}

void PlayerController::handle_preset(const DmrCommand& c, double now) {
  // RC：FactoryDefaults → 音量回默认、解除静音（DMR_SOAP_SPEC §1.2）
  // ★d134：官方预置名是**复数** `FactoryDefaults`（RCS:1 表 2-17），SCPD 的
  // A_ARG_TYPE_PresetName 也只列了复数 ⇒ 按 SCPD 发名的控制点原先会被静默忽略
  // （只打一条"未知预设"警告，没有任何反馈）。两个拼写都收（单数是历史兼容）。
  if (c.preset == "FactoryDefaults" || c.preset == "FactoryDefault") {
    snap_.volume = 100;
    snap_.muted = false;
    mpv_->set_volume(100);
    mpv_->set_mute(false);
    publish_volume();
    FR_LOG_INFO("[DMR] [req#{}] SelectPreset {} → 音量回默认并解除静音", c.req_id, c.preset);
  } else {
    FR_LOG_WARN("[DMR] [req#{}] SelectPreset 未知预设 {}", c.req_id, c.preset);
  }
  (void)now;
}

// ---------------- mpv 事件 ----------------

void PlayerController::run_pending_after_load() {
  while (!pending_.empty()) {
    DmrCommand c = pending_.front();
    pending_.pop_front();
    FR_LOG_INFO("[DMR] [req#{}] 消费 TRANSITIONING 期排队指令 {}", c.req_id,
                (int)c.type);
    on_command(c);  // 状态已非 TRANSITIONING，走正常分支
  }
}

void PlayerController::on_file_loaded(double now) {
  // R6：isLive 判定 —— seekable=no 或 duration<=0
  double dur = mpv_->get_duration();
  bool seekable = mpv_->get_seekable();
  snap_.is_live = (!seekable || dur <= 0);
  snap_.seekable = seekable;
  snap_.duration = snap_.is_live ? 0.0 : dur;  // R6：TrackDuration 恒 0
  snap_.picture_ready = true;
  snap_.position = 0;
  bool auto_play = cfg_.dlna_auto_play_on_set_uri;  // R1：默认自动开播

  // 直播重连成功：回到 PLAYING，App 无感（R6）
  TransportState next =
      (auto_play || !reconnect_uri_.empty()) ? TransportState::Playing : TransportState::Ready;
  if (!reconnect_uri_.empty()) {
    reconnect_uri_.clear();
    reconnect_left_ = 0;
  }
  apply_state(next, auto_play ? "file-loaded(auto-play)" : "file-loaded", -1);
  publish_media();
  FR_LOG_INFO("[DMR] [req#-] file-loaded：isLive={} dur={} title=\"{}\"", snap_.is_live, dur,
              snap_.title);
  run_pending_after_load();
  (void)now;
}

void PlayerController::on_end_file(int reason, double now) {
  if (snap_.state == TransportState::Stopped && !snap_.has_session) return;
  if (reason == MPV_END_FILE_REASON_STOP || reason == MPV_END_FILE_REASON_QUIT) return;

  if (reason == MPV_END_FILE_REASON_ERROR) {
    // R6：直播断流 → 指数退避重连
    if (snap_.is_live && reconnect_left_ > 0) {
      reconnect_left_--;
      last_reconnect_delay_ = std::pow(2.0, 3 - reconnect_left_);  // 1,2,4s 级退避
      FR_LOG_ERROR("[DMR] [req#-] 直播断流，{}s 后重连（剩余 {} 次）",
                   (int)last_reconnect_delay_, reconnect_left_);
      if (reconnect_uri_.empty()) reconnect_uri_ = snap_.uri;
      apply_state(TransportState::Transitioning, "live-reconnect", -1);
      transition_at_ = now;
      reconnect_at_ = now + last_reconnect_delay_;
      return;
    }
    FR_LOG_ERROR("[DMR] [req#-] mpv error → STOPPED（不再重连）");
    enter_stopped("mpv-error", -1);
    return;
  }

  // R2：EOF 双路径连播
  if (!snap_.next_uri.empty()) {
    std::string next_uri = snap_.next_uri;
    std::string next_meta = snap_.next_metadata;
    snap_.next_uri.clear();
    snap_.next_metadata.clear();
    snap_.uri = next_uri;
    snap_.metadata = next_meta;
    snap_.title = extract_dc_title(next_meta);
    snap_.position = 0;
    snap_.duration = 0;
    snap_.picture_ready = false;
    apply_state(TransportState::Transitioning, "eof-next", -1);
    transition_at_ = now;
    publish_media();
    mpv_->load_uri(next_uri);
    FR_LOG_INFO("[DMR] [req#-] EOF → 连播 {}", uri_without_fragment(next_uri));
    return;
  }

  // 无 NextURI：PLAYING → TRANSITIONING → STOPPED（保持会话存活等 App 重推）
  apply_state(TransportState::Transitioning, "eof-drain", -1);
  transition_at_ = now;
  pending_stop_ = true;
}

void PlayerController::on_property(const MpvEvent& e, double now) {
  if (e.prop_name == "time-pos") {
    // R3：seek 期间不刷位置缓存（等 seeking=false 确认），否则进度条来回弹
    if (seek_pending_ >= 0) return;
    snap_.position = e.num;
    publish_position(false);
  } else if (e.prop_name == "seeking") {
    if (seek_pending_ >= 0 && e.flag == 0) {
      // R3：seeking=false = seek 确认完成，刷新位置并立即发布
      snap_.position = seek_pending_;
      FR_LOG_INFO("[DMR] [req#-] Seek 完成确认 pos={}", format_hms_seconds(seek_pending_));
      seek_pending_ = -1;
      publish_position(true);
    }
  } else if (e.prop_name == "duration") {
    if (!snap_.is_live) snap_.duration = e.num;
    publish_media();
  } else if (e.prop_name == "pause") {
    using S = TransportState;
    if (e.flag && snap_.state == S::Playing) apply_state(S::PausedPlayback, "pause-prop", -1);
    else if (!e.flag && snap_.state == S::PausedPlayback)
      apply_state(S::Playing, "resume-prop", -1);
  } else if (e.prop_name == "volume") {
    int v = (int)std::lround(e.num);
    if (v >= 0 && v <= 100 && v != snap_.volume) {
      snap_.volume = v;
      publish_volume();
    }
  } else if (e.prop_name == "mute") {
    bool m = e.flag != 0;
    if (m != snap_.muted) {
      snap_.muted = m;
      publish_volume();
    }
  } else if (e.prop_name == "seekable") {
    snap_.seekable = e.flag != 0;
    if (snap_.is_live) snap_.duration = 0;  // R6
  }
  (void)now;
}

void PlayerController::on_mpv_event(const MpvEvent& e, double now) {
  now_ = now;
  switch (e.kind) {
    case MpvEvent::Kind::FileLoaded: on_file_loaded(now); break;
    case MpvEvent::Kind::EndFile: on_end_file(e.reason, now); break;
    case MpvEvent::Kind::PropertyChanged: on_property(e, now); break;
    default: break;
  }
}

// ---------------- 发布 ----------------

void PlayerController::publish_state(const char* why) {
  PlayerEvent e;
  e.kind = PlayerEvent::Kind::StateChanged;
  e.snap = snap_;
  bus_.publish_player(std::move(e));
  (void)why;
}

void PlayerController::publish_position(bool force) {
  if (!force && last_pos_pub_ > 0 && now_ - last_pos_pub_ < 1.0) return;
  last_pos_pub_ = now_;
  PlayerEvent e;
  e.kind = PlayerEvent::Kind::Position;
  e.snap = snap_;
  bus_.publish_player(std::move(e));
}

void PlayerController::publish_media() {
  PlayerEvent e;
  e.kind = PlayerEvent::Kind::MediaInfo;
  e.snap = snap_;
  bus_.publish_player(std::move(e));
}

void PlayerController::publish_volume() {
  PlayerEvent e;
  e.kind = PlayerEvent::Kind::Volume;
  e.snap = snap_;
  bus_.publish_player(std::move(e));
}

}  // namespace fr
