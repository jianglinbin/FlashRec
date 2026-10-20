#include "player/player_controller.h"

#include <chrono>
#include <cmath>
#include <cstdlib>  // d285：FR_LIVE_FORMAT（起播取证台）
#include <string>
#include <utility>
#include <vector>

#include <mpv/client.h>

#include "app/config.h"
#include "app/log.h"
#include "app/str_util.h"
#include "cast/cast_contract.h"  // d281：kLoopbackUriPrefix（回流 URI 判据，与 mpv_bridge 同源）

namespace fr {

namespace {
double mono_now() {
  using namespace std::chrono;
  return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

const char* hwdec_mode_name(int mode) {
  switch (mode) {
    case 0: return "软解(no)";
    case 2: return "NVDEC(nvdec-copy)";
    case 3: return "DXVA2(dxva2-copy)";
    default: return "自动(auto-safe)";
  }
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

  // ★d279 端到端延迟探针（只对回环实时流）；★d283 改由 tick 驱动 —— 旧实现挂在
  //   time-pos 属性事件上，而该事件要等 mpv 开播才发，于是「起播 7.5s」整段落在观测
  //   盲区里，且把「mpv 就绪耗时」与「开播排队耗时」折成一个数。现在 TRANSITIONING
  //   期间按 250ms 采样（time-pos 未到打 no-pos），起播后回落到 1s。
  //   接收端积压 =（当前墙钟 − 这次 loadfile 墙钟）− time-pos；time-pos 的 0 点 = TS
  //   首帧 PTS，故该值只含接收端侧（生产队列 + TCP + mpv demuxer + 解码显示），不含
  //   客户端编码与网络。判断依据：
  //     积压 ≈ 起播缺口且不再涨 → 起播一次性损失（追帧/提前起播可解）
  //     积压随流长度持续爬升     → 接收端某处真积压（配合 cache-dur 细分）
  // ★d479：门控扩展 —— 回环探针（live_start_wall_）与 DLNA 直播遥测（dl_probe_）
  //   共用采样计时与断流判据，输出分流（见下方 dl_probe_ 早退分支）。
  if ((live_start_wall_ > 0.0 || dl_probe_) && now >= live_probe_next_) {
    const bool transitional = snap_.state == TransportState::Transitioning;
    live_probe_next_ = now + (transitional ? 0.25 : 1.0);
    const double elapsed = live_start_wall_ > 0.0 ? now - live_start_wall_ : 0.0;
    const double pos = snap_.position;
    char posbuf[32];
    if (pos >= 0.0) snprintf(posbuf, sizeof(posbuf), "%.3f", pos);
    else snprintf(posbuf, sizeof(posbuf), "no-pos");
    // ★d299 断流检测：客户端断开后 `流经过`仍按墙钟走表，而 time-pos 冻住
    //   ⇒ 「接收端积压」会线性涨到几十秒，**那是记账假象、不是延迟**（d299 定性）。
    //   判据：apts 连续 3 次采样不动（≤0.001s）且 eof 未到 ⇒ 已无数据流入。
    //   此时**停打积压**并明确输出「已断流」，避免后人拿它当延迟证据。
    //   ★d479 补充：paused-for-cache=yes（HLS 缓冲中）不算断流 —— 分片拉取期
    //   apts 冻结是常态，3s 缓冲不该误判成断流停测。
    const double apts_now = std::atof(mpv_->get_prop("audio-pts").c_str());
    const bool frozen_apts = live_probe_last_apts_ >= 0.0 &&
                             std::fabs(apts_now - live_probe_last_apts_) <= 0.001;
    // ★d492：用户暂停（pause=yes）时 apts 冻结是**合法状态**，不得计入断流——
    //   d491 复测实证：手机端暂停 3s ⇒ 断流判定误触发 ⇒ dl_probe_ 停测且本会话
    //   无人复位（file_loaded 才再置位）⇒ 暂停过一次遥测全程失明。pause 期间
    //   stall 计数清零；真正的客户端断流（pause=no 而 apts 冻结）判据不变。
    const bool user_paused = mpv_->get_prop("pause") == "yes";
    live_probe_stall_ = (frozen_apts && !user_paused) ? live_probe_stall_ + 1 : 0;
    live_probe_last_apts_ = apts_now;
    const bool buffering =
        mpv_->get_prop("paused-for-cache") == "yes";  // ★d479：HLS 拉片缓冲中
    const bool stream_dead =
        live_probe_stall_ >= 3 && std::string(mpv_->get_prop("eof-reached")) != "yes" &&
        !buffering;
    if (stream_dead) {
      FR_LOG_INFO("[MPV] 延迟探针: 已断流（apts 连续 {}s 未推进，eof未到）—— 停止累计积压"
                  "（此前的『接收端积压』是记账假象，不是延迟）", live_probe_stall_);
      live_start_wall_ = 0.0;  // 复位，下次会话重新起算（d299）
      dl_probe_ = false;       // ★d479：DLNA 遥测同停，重连成功后 file_loaded 再置位
      return;
    }
    // ★d479：DLNA 直播瘦身遥测（早退，不落入回环分解）。「流经过 − time-pos」
    //   墙钟差对 HLS 无物理意义（PTS 起点任意，与墙钟不对齐），只打可信量：
    //   B = demuxer-cache-time − time-pos ≈ 距直播边缘延迟（预读顶到边缘时）。
    if (dl_probe_) {
      const double demux_pos = std::atof(mpv_->get_prop("demuxer-cache-time").c_str());
      const double b = (demux_pos > 0.0 && pos >= 0.0) ? (demux_pos - pos) : -1.0;
      FR_LOG_INFO("[MPV] DLNA直播遥测: B={:.3f}s cache-dur={} core-idle={} pause={} "
                  "vfps={} apts={:.3f} hwdec={} 追赶={:.2f}x",
                  b, mpv_->get_prop("demuxer-cache-duration"), mpv_->get_prop("core-idle"),
                  mpv_->get_prop("pause"), mpv_->get_prop("estimated-vf-fps"),
                  apts_now, mpv_->get_prop("hwdec-current"), dl_catchup_speed_);
      return;
    }
    // ★d302 延迟分解：把「数据侧」与「显示侧」的差直接打出来，替代只能推算的
    //   「接收端积压」。实测（17:37:15~17:17 三连采样）：
    //     time-pos=163.688（冻住）demuxer-cache=166.748 接收端积压 1.480→2.486→3.489
    //   ⇒ **数据已到 166.7s、只显示到 163.7s，正好差 3 秒且每秒涨 1 秒** ——
    //     这就是用户观察到的「手机与播放器差好几秒」的直接来源。
    //
    //   三个量的含义（分工明确，不重复、不遗漏）：
    //     已收数据位= demuxer-cache-time（demuxer 读到的**流内**位置）
    //     显示位置   = time-pos（当前真正显示到哪）
    //     ⇒ **数据侧延迟 = 已收数据位 − 显示位置**（要等多少秒才追得上）
    //     ⇒ **墙钟差     = 流经过 − 已收数据位**（客户端+网络+我们自己的排队的总和）
    //   两者相加 = 现有的「接收端积压」，但现在**每一段都能单独读**、可归因。
    const double demux_pos = std::atof(mpv_->get_prop("demuxer-cache-time").c_str());
    const bool has_demux = demux_pos > 0.0 && pos >= 0.0;
    const double data_side = has_demux ? (demux_pos - pos) : -1.0;   // 数据侧延迟
    const double wall_gap = has_demux ? (elapsed - demux_pos) : -1.0;  // 墙钟侧延迟
    // 数据侧延迟取滑动最大值：用户感知的是「最坏的那几秒」，不是瞬时值。
    if (data_side >= 0.0) {
      live_data_side_ = data_side > live_data_side_ ? data_side : live_data_side_ * 0.98;
    }
    // ★d322→d323：追赶控制器已移出探针块（独立 200ms 采样段，见下方）——
    //   1s 采样对追赶而言太钝（响应慢 + 档位切换粒度粗）。
    FR_LOG_INFO("[MPV] 延迟探针: 流经过={:.3f}s time-pos={} 接收端积压={:.3f}s "
                "demuxer-cache={} cache-dur={} core-idle={} 加载={} 开播排队={} "
                "pause={} vfps={} vpts={} apts={} eof={} vo-delayed={} "
                "hwdec={} 解码丢帧={} 显示丢帧={} "
                "| ★延迟分解 数据侧={}s 墙钟侧={}s 峰值={}s",
                elapsed, posbuf,
                pos >= 0.0 ? elapsed - pos : elapsed, mpv_->get_prop("demuxer-cache-time"),
                mpv_->get_prop("demuxer-cache-duration"), mpv_->get_prop("core-idle"),
                live_loaded_wall_ > 0.0 ? "yes" : "no",
                live_firstpos_wall_ > 0.0 ? "yes" : "no",
                // ★d287 冻结取证四件套：真机镜像「画面静止、没有声音」时 time-pos 恒 0，
                //   而上面几个值分不清是三种死法中的哪一种 ——
                //   pause=yes            → 根本没播（协议面/Play 未落到 mpv）
                //   vfps/apts 在涨而 time-pos 不动 → 帧在出但时钟没推（同步源问题）
                //   vfps=null 且 vo-delayed 不涨   → 显示链断在 mpv 之外（渲染泵没抽帧）
                // ★vfps=estimated-vf-fps 是「视频是否真在显示」的权威闸：只有帧被显示
                //   后才出值。video-pts 在同一条健康会话里实测就是 (null)（离线台
                //   23:07 复测：apts 有值、画面正常，vpts 仍空），故不能单看它。
                mpv_->get_prop("pause"), mpv_->get_prop("estimated-vf-fps"),
                mpv_->get_prop("video-pts"),
                mpv_->get_prop("audio-pts"), mpv_->get_prop("eof-reached"),
                mpv_->get_prop("vo-delayed-frame-count"),
                // ★d319（重落地）：卡顿归因三件套——hwdec 后端 / 解码丢帧（涨=
                // 解码跟不上）/ 显示丢帧（涨=显示跟不上），与数据洞互相独立。
                mpv_->get_prop("hwdec-current"),
                mpv_->get_prop("decoder-frame-drop-count"),
                mpv_->get_prop("frame-drop-count"),
                data_side >= 0.0 ? std::to_string(data_side).c_str() : "n/a",
                wall_gap >= 0.0 ? std::to_string(wall_gap).c_str() : "n/a",
                live_data_side_ > 0.0 ? std::to_string(live_data_side_).c_str() : "n/a");
  }

  // ★d323→d325：阈值细化（用户拍板数值）——触发阈值 = **缓冲垫厚度**：越低延迟
  //   越低、抗网络抖动越弱（0.5s 级到达停顿会触底瞬停）。**非对称滞回**消边界
  //   抖动：进入 0.6s / 退出 0.3s（200ms 采样下 B 抖动 ±0.3-0.5s，对称阈值会
  //   在边界频繁切档）。
  //   等级（B = 数据侧延迟 = demuxer-cache-time − time-pos；读窗 2s；★d325 首档
  //   改 1.25×——1.1× 追动力不足，追赶期拖长）：
  //     1.0×（未追赶）：B > 0.6s 进入 → 1.25×；B > 1.5s → 1.50×
  //     追赶中：B ≤ 0.3s 退出 → 1.00×；0.3 < B ≤ 1.5 维持 1.25×
  //   200ms 采样 = 追赶响应与视频帧距同量级，档位切换平滑无感。
  if (now >= catchup_next_ && snap_.state == TransportState::Playing &&
      snap_.source == CastSource::AirPlay &&
      snap_.uri.rfind(cast::kLoopbackUriPrefix, 0) == 0) {
    catchup_next_ = now + 0.2;
    const double demux_pos = std::atof(mpv_->get_prop("demuxer-cache-time").c_str());
    const double backlog = (demux_pos > 0.0 && snap_.position >= 0.0)
                               ? (demux_pos - snap_.position)
                               : 0.0;
    constexpr double kEnterTh = 0.6;  // 进入追赶（滞回上沿）
    constexpr double kExitTh = 0.3;   // 退出追赶（滞回下沿）
    constexpr double kEscTh = 1.5;    // 1.25 → 1.50
    double target;
    if (catchup_speed_ > 1.0) {
      // 追赶中：B ≤ 0.3s 才退出（滞回下沿）；未退出按 B 维持/升档
      if (backlog <= kExitTh)
        target = 1.0;
      else if (backlog > kEscTh)
        target = 1.5;
      else
        target = 1.25;
    } else {
      // 1.0×：B > 0.6s 进入追赶（先 1.25×，升档由后续采样按 B 升）
      if (backlog > kEscTh)
        target = 1.5;
      else if (backlog > kEnterTh)
        target = 1.25;
      else
        target = 1.0;
    }
    if (target != catchup_speed_) {
      mpv_->set_speed(target);
      FR_LOG_INFO("[MPV] 追赶控制器: 积压 {:.2f}s → 倍速 {:.2f}x{}", backlog,
                  target, target > catchup_speed_ ? "（追赶）" : "（已追平）");
      catchup_speed_ = target;
    }
  }

  // ★d490：DLNA 直播复判——WeChat HLS 连接瞬间片单可能极短（实测 0.8045s 且
  //   seekable ⇒ 首判误 VOD）。宽限 5s 后重查一次，任何「边缘仍在推进」的证据
  //   （时长增长 / 播放越过初始时长 / 缓冲越过初始片单 / 不可 seek）即翻案接管。
  //   未在 Playing 则不消费，待恢复播放后再判；on_file_loaded（新文件）重置挂起。
  if (live_recheck_at_ > 0.0 && now >= live_recheck_at_ &&
      snap_.state == TransportState::Playing && snap_.source == CastSource::Dlna) {
    live_recheck_at_ = -1.0;  // 只判一次
    const double dur = mpv_->get_duration();
    const bool seekable = mpv_->get_seekable();
    const double cache_dur = std::atof(mpv_->get_prop("demuxer-cache-duration").c_str());
    const bool grew = dur > live_recheck_dur0_ + 1.0;              // 时长仍在长
    const bool past = snap_.position > live_recheck_dur0_ + 0.5;   // 播过初始时长
    const bool bufover = cache_dur > live_recheck_dur0_ + 1.0;     // 缓冲越过片单
    if (!seekable || grew || past || bufover) {
      engage_dlna_live("复判：初始片单短，直播边缘在推进");
    }
  }

  // ★d479：DLNA 直播窄带追赶控制器（行业口径：hls.js maxLiveSyncPlaybackRate /
  //   dash.js liveCatchup.playbackRate）。与上方回环控制器平行互斥（source 门控
  //   分账），判据同源 B = demuxer-cache-time − time-pos。阈值 d491 复判修正
  //   （方案 A 内收敛，d489 的 4.0s 水位实证失效：readahead ≥ 分片时长后字节顶
  //   4MiB 接管 ≈10.4s@3.2Mbps，带顶漂出可控范围）——分片整片入缓 ⇒ B 稳态必锯齿
  //   （水位 2.0 带顶 ≈ 恢复点+一个分片 ≈4.2s），**唯一无抖动驻留 = 排水到带底后
  //   整带落回门限之下**，即「驻留条件 exit + 分片 < enter」：
  //     1.00×：B ≤ 4.8s（带顶 4.3 = 退出点 1.0 + 分片 3.3 之上 0.5s 余量）
  //     1.10×：B > 4.8s（窄带软追赶——仅启动排水/包络上移时出现，稳态静默）
  //     1.25×：B > 10s（兜底档——若稳态常驻 = 水位选项未生效，回查不是调参）
  //     退出滞回：追赶中 B ≤ 1.0s 回 1.00×（1.0 + 3.3 = 4.3 < 4.8 ⇒ 排水到位后
  //     锯齿整带 [~0.5↔4.3] 落入门限之下，控制器静默驻留，均值 ~2.4s）
  //     防追崩（dash.js playbackBufferMin 等价）：cache-dur < 1.0s 不加速——
  //     缓冲太薄时加速只会制造下一次卡顿。
  //   采样 200ms 与回环控制器同口径。倍速权归控制器：直播会话内用户 SetSpeed
  //   被 handle_speed 忽略、UI 倍速按钮灰显（d479 同批）。
  if (now >= dl_catchup_next_ && snap_.state == TransportState::Playing &&
      snap_.source == CastSource::Dlna && snap_.is_live) {
    dl_catchup_next_ = now + 0.2;
    const double demux_pos = std::atof(mpv_->get_prop("demuxer-cache-time").c_str());
    const double cache_dur = std::atof(mpv_->get_prop("demuxer-cache-duration").c_str());
    const double b = (demux_pos > 0.0 && snap_.position >= 0.0)
                         ? (demux_pos - snap_.position)
                         : 0.0;
    constexpr double kEnterTh = 4.8;  // 进入软追赶（带顶 4.3 之上，d491 修正）
    constexpr double kEscTh = 10.0;   // 兜底档（水位钉死失效信号）
    constexpr double kExitTh = 1.0;   // 排水到带底（1.0+3.3=4.3 < 4.8 驻留条件，d491）
    constexpr double kBufMin = 1.0;   // 缓冲下限：太薄不加速（防追崩）
    const bool buf_ok = cache_dur >= kBufMin;
    double target;
    if (dl_catchup_speed_ > 1.0) {
      if (b <= kExitTh || !buf_ok)
        target = 1.0;
      else if (dl_catchup_speed_ > 1.10)
        // ★d493：1.25x 档退出滞回 1.0s——d491 复测实证启动排水期 B 在 esc=10
        //   边界反复穿越（10.16→1.25 / 9.96→1.10 每 1-2s 翻一次），档间抖动。
        target = (b > kEscTh - 1.0) ? 1.25 : 1.10;
      else if (b > kEscTh && buf_ok)
        target = 1.25;
      else
        target = 1.10;
    } else {
      if (b > kEscTh && buf_ok)
        target = 1.25;
      else if (b > kEnterTh && buf_ok)
        target = 1.10;
      else
        target = 1.0;
    }
    if (target != dl_catchup_speed_) {
      mpv_->set_speed(target);
      FR_LOG_INFO("[MPV] DLNA直播追赶: B {:.2f}s → 倍速 {:.2f}x{}", b, target,
                  target > dl_catchup_speed_ ? "（追赶）" : "（已贴水位）");
      dl_catchup_speed_ = target;
    }
  }

  // d151：GL→SW 降级后按位重载续播（渲染线程刚切的上下文拿不到帧，必须重载；
  // 且 hwdec=no 只对后续 loadfile 生效）。放在 pump 之后：先消化既有事件，再发起重载。
  reload_after_backend_switch();

  // TRANSITIONING 看门狗（不变量 5：15s 强制 STOPPED + ERROR）
  if (snap_.state == TransportState::Transitioning && transition_at_ > 0 &&
      now - transition_at_ > 15.0) {
    FR_LOG_ERROR("[DMR] [req#-] TRANSITIONING 超 15s，强制 STOPPED（防卡死）");
    // d273：强停前抓 mpv 诊断快照——流是否到达 mpv（filename/缓存）、轨是否建出
    //（track-list）、解码器是否认出编码（video-format/audio-codec）。黑屏无声排障关键。
    FR_LOG_ERROR("[DMR] mpv 诊断快照: {}", mpv_->diag_snapshot());
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

  // ★d281/d425：镜像自适应填充 —— 现为 FitMode::Auto 在**回环镜像流**下的内部判据
  // （Auto + 非镜像会话恒 contain，会话初值见 handle_set_uri；其余模式 tick 不介入）。
  // 实测背景：
  //   iPhone 横屏镜像 = 1920x884（19.5:9），舞台 = 1920x1146（1.675）。
  //   contain ⇒ 按宽填满 + 上下 131px letterbox（用皮肤底色画）；而源视频左右
  //   还自带手机播放器烧录的纯黑边（截图实测 RGB(0,0,0)，左右各 175px）
  //   ⇒ 观感"四边皆不满"。改 cover ⇒ 裁掉两侧，最先裁掉的就是那些烧录黑边，
  //   16:9 真内容铺满整个舞台。
  //   竖屏镜像（498x1080 = 0.461）反过来：contain 恰好上下满、整屏不丢；cover 会把
  //   手机屏幕裁掉大半 —— 所以判决取"谁宽听谁"，不是无脑 cover。
  // 只在**判决变化**时下发 panscan，避免反复 reconfigure 抖动。
  if (fit_mode_ == (int)FitMode::Auto && fit_auto_ && stage_aspect_ > 0.01 &&
      now >= fit_probe_next_) {
    fit_probe_next_ = now + 0.5;
    const double va = mpv_->video_aspect();
    if (va > 0.0) {
      // 阈值 0.5%：近平局时取 cover —— 此时两侧裁切量极小，且优先消灭源内黑边。
      const double want = (va >= stage_aspect_ * 0.995) ? 1.0 : 0.0;
      if (want != fit_applied_) {
        fit_applied_ = want;
        mpv_->set_panscan(want);
        FR_LOG_INFO("[MPV] 镜像填充自适应: 视频比例={:.3f} 舞台比例={:.3f} → {}", va,
                    stage_aspect_, want >= 0.5 ? "cover（铺满，裁两侧）" : "contain（保整屏，上下满）");
      }
    }
  }

  publish_position(false);  // 1s 节流的位置事件
}

// ★d281：镜像自适应填充判据 —— 舞台宽高比由主循环推送（窗口逻辑尺寸；顶栏可见时排除）。
// 只记录并让下一轮 tick 立刻重判；比例不变（同一窗口）时不触发任何动作。
void PlayerController::set_stage_aspect(double aspect) {
  if (aspect <= 0.01) return;
  if (std::fabs(aspect - stage_aspect_) > 0.001) {
    stage_aspect_ = aspect;
    fit_probe_next_ = 0.0;  // 舞台变了（缩放/全屏）→ 立即重判一次
  }
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

// v0.6.1 d343：视频显示尺寸转发（数字键缩放档位）。mpv 未就绪/无视频 = false。
bool PlayerController::video_display_size(int* w, int* h) {
  return mpv_ && mpv_->video_display_size(w, h);
}

// v0.6.1 d350：画面缩放/平移转发（数字键/菜单窗口缩放之外的渲染侧能力）。
void PlayerController::set_video_zoom(double z) {
  if (mpv_) mpv_->set_video_zoom(z);
}

void PlayerController::set_video_pan(double x, double y) {
  if (mpv_) mpv_->set_video_pan(x, y);
}

// v0.6.1 d353：解码方式选择（mpv API 收口规则）。
// ★d432：AirPlay 会话一律挂起到下次会话 —— 镜像 H.264 每会话仅开头 1 个 IDR，
//   d428~d431 已证明任何重载后的新解码器都只能从 P 帧接入，wait_idr 永不释放，
//   画面永久冻结。此处连 hwdec 属性都不写，彻底不碰活动解码链。
bool PlayerController::set_hwdec_mode(int mode) {
  if (mode < 0 || mode > 3) return true;
  if (!mpv_) return true;

  if (snap_.has_session && snap_.source == CastSource::AirPlay) {
    if (mode == active_hwdec_mode_.load(std::memory_order_relaxed)) {
      pending_hwdec_mode_.store(-1, std::memory_order_relaxed);  // 点当前档 = 取消挂起
      FR_LOG_INFO("[DMR] AirPlay 会话中选当前解码档 {}，无需挂起", hwdec_mode_name(mode));
      return true;
    }
    pending_hwdec_mode_.store(mode, std::memory_order_release);
    FR_LOG_WARN("[DMR] AirPlay 会话中：解码方式 → {} 挂起，下次会话生效（本次不重载）",
                hwdec_mode_name(mode));
    return false;
  }

  pending_hwdec_mode_.store(-1, std::memory_order_release);
  if (snap_.has_session) {
    mpv_->set_hwdec_mode(mode);   // DLNA/点播：立即生效（属性 + 按位重载）
  } else {
    mpv_->apply_hwdec_mode(mode);  // 无会话：只落属性，不重载残留 URI
  }
  active_hwdec_mode_.store(mode, std::memory_order_release);
  return true;
}

int PlayerController::active_hwdec_mode() const {
  return active_hwdec_mode_.load(std::memory_order_relaxed);
}

int PlayerController::pending_hwdec_mode() const {
  return pending_hwdec_mode_.load(std::memory_order_relaxed);
}

int PlayerController::take_hwdec_applied_notice() {
  return hwdec_applied_notice_.exchange(-1, std::memory_order_acq_rel);
}

void PlayerController::set_panscan(double p) {
  if (mpv_) mpv_->set_panscan(p);
}

// ★d425：画面填充模式（FIT_MODE_PLAN.md）。持久偏好：落盘与重启恢复在 main 侧，
// 这里只管「立即应用到 mpv」——属性跨 loadfile 持久，随时可改；新会话由
// handle_set_uri 按模式重下初值。
//   Cover/Contain：keepaspect=yes + panscan 1/0。
//   Stretch：keepaspect=no + panscan=0（变形填充本体在 mpv keepaspect）。
//   Auto：keepaspect=yes，先按 contain 落底；镜像会话复位判决态让下一轮 tick
//   （≤0.5s）按「视频比例 vs 舞台比例」重判，非镜像会话 tick 不轮询（恒 contain）。
void PlayerController::set_fit_mode(int mode) {
  if (mode < (int)FitMode::Auto || mode > (int)FitMode::Stretch) return;
  fit_mode_ = mode;
  if (!mpv_) return;
  if (mode == (int)FitMode::Stretch) {
    mpv_->set_keep_aspect(false);
    mpv_->set_panscan(0.0);
  } else {
    mpv_->set_keep_aspect(true);
    mpv_->set_panscan(mode == (int)FitMode::Cover ? 1.0 : 0.0);
    if (mode == (int)FitMode::Auto) {
      fit_applied_ = -1.0;  // 复位 → 下一轮 tick 重判（仅镜像会话轮询）
      fit_probe_next_ = 0.0;
    }
  }
  FR_LOG_INFO("[MPV] 画面填充模式 → {}", mode == 0   ? "自动"
                                        : mode == 1 ? "铺满"
                                        : mode == 2 ? "完整画面"
                                                    : "拉伸");
}

void PlayerController::set_background_color(const std::string& rgb_hex) {
  if (mpv_) mpv_->set_background_color(rgb_hex);
}

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
    case DmrCommand::Type::SetSpeed: handle_speed(cmd, now); break;
    case DmrCommand::Type::SelectPreset: handle_preset(cmd, now); break;
    case DmrCommand::Type::AddAudioTrack:
      // d267 引入的镜像外挂配音轨指令；v0.6.3 退回后当前无人投递（枚举保留占位）。
      // 显式接住以满足 -Wswitch，未来恢复音轨并入时在此接线。
      FR_LOG_WARN("[MPV] 忽略 AddAudioTrack（音轨并入当前未接线）");
      break;
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
  fit_auto_ = false;   // d281：会话结束，填充策略回到皮肤 fit
  fit_applied_ = -1.0;
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
  dl_probe_ = false;  // ★d479：DLNA 直播遥测随会话终止（防停止后 apts 冻结被误报断流）
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
  // d310：来源徽章真值 —— 此前漏拷 c.source，snap_.source 恒为默认 Dlna，
  // AirPlay 会话在 UI 侧永远显示不出「AirPlay 投屏」（AIRPLAY_EMBED_PLAN.md:172 M2 接线）。
  snap_.source = c.source;
  // d310：会话名 —— event_bus.h 设计口径：AirPlay 会话名走 c.title（客户端名 /"AirPlay 镜像"），
  // DIDL 提取只归 DLNA 路径；此前无条件 extract_dc_title ⇒ AirPlay 标题恒为空。
  snap_.title = c.title.empty() ? extract_dc_title(c.metadata) : c.title;
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
  // ★d285 起播取证台：FR_LIVE_FORMAT=mpegts 时，把**回环 URI**（tcp://127.0.0.1:）也按
  //   「回流实时流」加载 —— 于是 `ts_feed.py 喂切片 + 生产 player（libmpv render+GL）`
  //   能在**无手机**条件下复现并度量 AirPlay 的起播门（A 段）。
  //   动机：离线裸 libmpv（vo=null）已实测复现不出起播空档（A=0.01s vs 真机 1.6~7.6s），
  //   而每个假设都消耗一次真机投屏太贵。★只认回环前缀 ⇒ DLNA 的 http:// 直链零影响；
  //   默认不设 = 行为与今天完全一致。纯诊断通道。
  std::string live_fmt = c.load_format;
  if (live_fmt.empty() && c.uri.rfind(cast::kLoopbackUriPrefix, 0) == 0) {
    if (const char* lf = std::getenv("FR_LIVE_FORMAT"); lf != nullptr && *lf != '\0') {
      live_fmt = lf;
      FR_LOG_WARN("[MPV] FR_LIVE_FORMAT={} → 回环 URI 按实时流加载（取证台）", live_fmt);
    }
  }
  // ★d425：画面填充模式会话初值（FIT_MODE_PLAN §二/§三.5）。必须在 load_uri 之前
  //   下发 —— load_uri 从桥层成员重申 panscan/keepaspect（跨 loadfile 持久属性，
  //   防上会话残留），set_panscan/set_keep_aspect 会同步成员，重申即取到本值。
  //   Auto 先按 contain 起步：镜像流由 tick 在首个轮询周期内按比例重判（横屏镜像
  //   通常立即跳 cover，d281 同款）；非镜像会话（DLNA 直链/HLS 直载）tick 不轮询，
  //   恒 contain —— 电影 2.39:1 不被默认裁切。
  switch ((FitMode)fit_mode_) {
    case FitMode::Cover:
      mpv_->set_keep_aspect(true);
      mpv_->set_panscan(1.0);
      break;
    case FitMode::Stretch:
      mpv_->set_keep_aspect(false);
      mpv_->set_panscan(0.0);
      break;
    case FitMode::Contain:
    case FitMode::Auto:
    default:
      mpv_->set_keep_aspect(true);
      mpv_->set_panscan(0.0);
      break;
  }
  // ★d432：上一次 AirPlay 会话挂起的解码档，在本会话 loadfile **之前**落属性。
  //   此处不重载（新会话本来就要 loadfile，属性随新解码器初始化生效）。
  {
    const int pm = pending_hwdec_mode_.exchange(-1, std::memory_order_acq_rel);
    if (pm >= 0) {
      const int prev = active_hwdec_mode_.exchange(pm, std::memory_order_acq_rel);
      if (pm != prev) {
        mpv_->apply_hwdec_mode(pm);
        hwdec_applied_notice_.store(pm, std::memory_order_release);
        FR_LOG_INFO("[DMR] [req#{}] 应用挂起解码方式 {}（随本次 loadfile 生效）", c.req_id,
                    hwdec_mode_name(pm));
      }
    }
  }
  mpv_->load_uri(c.uri, -1.0, live_fmt);
  live_format_ = live_fmt;  // ★d312：暂存回环加载格式，暂停恢复跳前沿时复用
  // ★d322：新会话恢复**用户倍速**（d169 档位）——AirPlay 追赶控制器可能把 mpv
  //   speed 改写在 1.0~1.5 之间，切换到 DLNA（或新会话）时必须还原，否则点播
  //   会以追赶倍速播放。DLNA 会话恢复 d169 用户档位；AirPlay 会话从 1.0 由
  //   控制器接管（控制器仅在回环 Playing 期驱动，此处归零其状态基准）。
  mpv_->set_speed(snap_.speed);
  catchup_speed_ = 1.0;
  dl_catchup_speed_ = 1.0;  // ★d479：DLNA 窄带追赶档位同步归零（file_loaded 再按 isLive 接管）
  // ★d279 端到端延迟探针锚点：load_format 非空 = AirPlay 回流（DLNA 点播不设，
  //   与 mpv_bridge.cpp 的 P1 低延迟选项门控同一判据）。用 mono_now() 取值，
  //   与 on_property 里的采样点**同一时钟**，不依赖 tick 参数来源。
  if (!live_fmt.empty()) {
    live_start_wall_ = mono_now();
    live_probe_next_ = live_start_wall_;  // ★d283：立刻起测（旧值 +3.0 会把起播过程整段糊掉）
    live_loaded_wall_ = -1.0;
    live_firstpos_wall_ = -1.0;
  } else {
    live_start_wall_ = -1.0;  // DLNA：关掉探针
    live_loaded_wall_ = -1.0;
    live_firstpos_wall_ = -1.0;
  }
  // ★d281/d425：填充策略分流。回环 URI（tcp://127.0.0.1:）= AirPlay 镜像/音频回流 →
  //   FitMode::Auto 下走「按比例自适应」（判据见 tick）：这类流的形状 = **手机屏幕形状**，
  //   源内还可能自带播放器烧录的黑边，不能只死办 contain。DLNA 直链与 AirPlay 的 HLS
  //   直载（location 直给 mpv）在 Auto 下恒 contain —— 电影 2.39:1 被 cover 会裁掉约
  //   三成画面；用户可经右键「画面填充」显式选铺满/拉伸。前缀常量与 mpv_bridge
  //   （回流 URI 不进 last_uri_）同源，见 cast_contract.h。
  fit_auto_ = base.rfind(cast::kLoopbackUriPrefix, 0) == 0;
  if (fit_mode_ == (int)FitMode::Auto) {
    fit_applied_ = -1.0;   // 本会话尚未判定 → 下一轮 tick 首次取样
    fit_probe_next_ = 0.0;
  }
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
    // ★d312：AirPlay 直播回流的暂停恢复必须**跳直播前沿**——重载回环 URI = 新建连接
    //   从当前数据解复用，天然落在直播前沿。原地 set_pause(false) 只会从暂停时刻的
    //   旧画面播起：暂停期内容已流逝，用户实测延迟可达分钟级（「手机上几分钟前播过
    //   的画面，投屏上几分钟后才显示」）。DLNA URL 播放流保持原地继续（点播语义，
    //   用户明确要求区分两者）。跳转走 TRANSITIONING，file-loaded(auto-play) 自动续播。
    if (snap_.uri.rfind(cast::kLoopbackUriPrefix, 0) == 0) {
      mpv_->load_uri(snap_.uri, -1.0, live_format_);
      apply_state(S::Transitioning, "play-live-edge", c.req_id);
      transition_at_ = now;
    } else {
      mpv_->set_pause(false);
      apply_state(S::Playing, "play", c.req_id);
    }
  } else if (snap_.state == S::Playing) {
    mpv_->set_pause(false);  // 幂等
  } else if (snap_.state == S::Stopped && snap_.has_session) {
    // R5 保持期内 Play：重推当前 URI 继续（部分 App 行为）
    // ★d312：直播回流同样跳直播前沿（旧位置对直播无意义——seekable=no、内容已流逝，
    //   按冻结 position 重推 = 从过去起播）；DLNA 点播仍按 snap_.position 续播。
    if (snap_.uri.rfind(cast::kLoopbackUriPrefix, 0) == 0) {
      mpv_->load_uri(snap_.uri, -1.0, live_format_);
    } else {
      mpv_->load_uri(snap_.uri, snap_.position);
    }
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
  // ★d318：AirPlay 镜像为直播流（中途不发关键帧，ts_dump 65.3s 仅首帧 1 个
  //   IDR）⇒ 不支持暂停——UI 按钮已禁用，此处兜底键盘/媒体键等其它路径。
  //   DLNA URL 播放流不受影响（点播语义，真暂停）。
  if (snap_.source == CastSource::AirPlay && snap_.state == S::Playing) {
    FR_LOG_INFO("[DMR] [req#{}] Pause 忽略（AirPlay 直播流不支持暂停）", c.req_id);
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

void PlayerController::handle_speed(const DmrCommand& c, double now) {
  // ★d322：AirPlay 会话的用户倍速由追赶控制器接管（UI 按钮已禁用，此处兜底
  //   DLNA 控制器命令等路径）。DLNA 会话不受影响。
  if (snap_.source == CastSource::AirPlay &&
      snap_.uri.rfind(cast::kLoopbackUriPrefix, 0) == 0) {
    FR_LOG_INFO("[DMR] [req#{}] SetSpeed {:.2f}x 忽略（AirPlay 倍速由追赶控制器接管）",
                c.req_id, c.speed);
    return;
  }
  // ★d479：DLNA 直播流同口径 —— 倍速权归窄带追赶控制器（直播无用户倍速，
  //   行业口径；UI 按钮已灰显，此处兜底 SOAP 控制点路径）。
  if (snap_.source == CastSource::Dlna && snap_.is_live) {
    FR_LOG_INFO("[DMR] [req#{}] SetSpeed {:.2f}x 忽略（DLNA 直播倍速由追赶控制器接管）",
                c.req_id, c.speed);
    return;
  }
  // d169：本地倍速（控制栏选择）。夹到 [0.25, 4.0]；倍速是 mpv 全局属性，
  // 跨 loadfile / 后端重载保持，无需在文件加载后重设。
  double s = c.speed;
  if (s < 0.25) s = 0.25;
  if (s > 4.0) s = 4.0;
  snap_.speed = s;
  mpv_->set_speed(s);
  publish_speed();
  (void)now;
  FR_LOG_INFO("[DMR] [req#{}] SetSpeed {:.2f}x", c.req_id, s);
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
  // ★d283 判决 A 段：mpv 从 loadfile 到 file-loaded 花了多久（回环流才有值）。
  if (live_start_wall_ > 0.0 && live_loaded_wall_ < 0.0) {
    live_loaded_wall_ = mono_now();
    FR_LOG_INFO("[MPV] 起播取证 A 段: loadfile→file-loaded={:.3f}s（demuxer 就绪）",
                live_loaded_wall_ - live_start_wall_);
  }
  // R6：isLive 判定 —— seekable=no 或 duration<=0
  double dur = mpv_->get_duration();
  bool seekable = mpv_->get_seekable();
  snap_.is_live = (!seekable || dur <= 0);
  snap_.seekable = seekable;
  snap_.duration = snap_.is_live ? 0.0 : dur;  // R6：TrackDuration 恒 0
  // ★d479：DLNA 直播低延迟三件套（水位钉死 + 遥测 + 窄带追赶）的判定锚点。
  //   is_live 此刻才可判 ⇒ 缓冲水位只能运行时下发。直播 = 钉死水位 + 遥测置位 +
  //   倍速归 1.0（直播无用户倍速，倍速权归追赶控制器，行业口径）；
  //   其余（点播/回环）恒还原默认 —— 回环有 per-file 选项集不受影响，这里只保证
  //   上一场直播的水位不泄漏进本场。
  //   ★d490 复判挂起：WeChat HLS 连接瞬间片单可能极短（d488 复测实证 0.8045s 且
  //   seekable ⇒ 首判误 VOD、三件套全部未接管）。仅 DLNA 且 0 < dur ≤ 5s 才挂；
  //   宽限 5s 后在 tick 重查（见 live_recheck 块），点播不受影响。
  dl_probe_ = false;
  live_recheck_at_ = -1.0;
  if (snap_.is_live && snap_.source == CastSource::Dlna) {
    engage_dlna_live("isLive 判定");
  } else {
    mpv_->apply_live_buffer_cap(false);
    if (snap_.source == CastSource::Dlna && dur > 0.0 && dur <= 5.0) {
      live_recheck_dur0_ = dur;
      live_recheck_at_ = mono_now() + 5.0;
      FR_LOG_INFO("[MPV] DLNA 直播复判挂起: dur={:.3f}s ≤5s，宽限 5s 后重查", dur);
    }
  }
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
  // ★d301 修「只显示第一帧然后卡死」：开播时把 **mpv 实际状态与状态机对齐**。
  //   病灶（本条是真机首现，v0.1.0 起就存在）：`loadfile` **不重置 mpv 的 pause 属性**
  //   （mpv_bridge.cpp:458 的注释已写明这一点），而 `stop()` / `clear_session()` 会
  //   `set_pause(true)`（保持最后画面，见 R5）。⇒ **同一进程内第二次投屏**时，
  //   新会话带着上一会话遗留的 `pause=true` 开播：状态机已是 PLAYING、mpv 却仍暂停
  //   ⇒ 不读回环 ⇒ 只显示首帧后卡死 + 数据面 `mux消费+0 / TS输出+0包 / 队列丢v/a` 暴涨。
  //   触发条件：**App 复用会话**（先发 Stop 再 SetURI）。此前用 iPhone17,1 每次都是
  //   全新会话，故从未踩到；换 iPhone15,4 后 App 复用了会话，立刻暴露。
  //   ⇒ 判据：`next == Playing` 时必须显式解除暂停。
  if (next == TransportState::Playing) mpv_->set_pause(false);
  apply_state(next, auto_play ? "file-loaded(auto-play)" : "file-loaded", -1);
  publish_media();
  FR_LOG_INFO("[DMR] [req#-] file-loaded：isLive={} dur={} title=\"{}\"", snap_.is_live, dur,
              snap_.title);
  // d273：file-loaded 即抓一次轨/编码快照——回流裸流「加载成功但没建出视频轨」
  //（TS 里视频 PES 缺失/坏）在这条日志一眼可辨。
  FR_LOG_INFO("[DMR] mpv 轨信息: {}", mpv_->diag_snapshot());
  run_pending_after_load();
  (void)now;
}

// ★d490：DLNA 直播三件套接管（水位钉死 + 遥测置位 + 倍速归 1.0）。
//   on_file_loaded 首判（isLive=true）与 tick 复判（初始片单极短误判 VOD 后翻案）
//   共用此唯一进入路径，保证两条路径的置位集合完全一致。
void PlayerController::engage_dlna_live(const char* why) {
  snap_.is_live = true;
  snap_.duration = 0.0;  // R6：TrackDuration 恒 0
  dl_probe_ = true;
  mpv_->apply_live_buffer_cap(true);
  // 倍速无条件归 1.0：SetUri 已按用户档恢复过 speed，排队期用户点过倍速的
  // 情况也在此时压回（排队的 SetSpeed 之后由 handle_speed 的直播忽略兜底）。
  mpv_->set_speed(1.0);
  dl_catchup_speed_ = 1.0;
  live_probe_stall_ = 0;
  live_probe_last_apts_ = -1.0;
  const double t0 = mono_now();
  live_probe_next_ = t0;   // 立刻起测
  dl_catchup_next_ = t0;
  FR_LOG_INFO("[MPV] DLNA 直播流：缓冲水位钉死 + 遥测/追赶接管（{}）", why);
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
    // ★d283 判决 B 段：首个有效 time-pos = mpv 真正开始推进播放位置的时刻。
    //   与 A 段（file-loaded）之差 = 主线程 auto-play 冲队/开播握手耗时。
    if (live_start_wall_ > 0.0 && live_firstpos_wall_ < 0.0 && e.num >= 0.0) {
      live_firstpos_wall_ = mono_now();
      FR_LOG_INFO("[MPV] 起播取证 B 段: file-loaded→首个 time-pos={:.3f}s，"
                  "loadfile→time-pos 合计={:.3f}s（旧探针量到的就是这个合计）",
                  live_firstpos_wall_ - (live_loaded_wall_ > 0.0 ? live_loaded_wall_ : live_start_wall_),
                  live_firstpos_wall_ - live_start_wall_);
    }
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

void PlayerController::publish_speed() {
  PlayerEvent e;
  e.kind = PlayerEvent::Kind::Speed;
  e.snap = snap_;
  bus_.publish_player(std::move(e));
}

}  // namespace fr
