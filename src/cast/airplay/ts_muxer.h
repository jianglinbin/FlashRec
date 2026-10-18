#pragma once
// 最小 MPEG-TS 复用器 —— AirPlay 镜像数据面 M2b 生产路径（AIRPLAY_EMBED_PLAN.md §5.3）。
//
// 它是「音画同步契约」的忠实执行件，只做四件事，策略全部在调用方（airplay_stream）：
//   ① PCR 自产连续：内部 PCR 按 tick(now_90k) 以 ≤100ms 间隔发出（PCR-only 包，
//      **不绑视频帧** —— 视频迟到不连累 mpv 的时钟刻度）。
//   ② 音频时间轴自产连续（d278 重写）：音频 PID 上有一个**单调轴**
//      `audio_axis_`，每发一帧（真实帧或静音帧）只前进一帧时长（980 tick）。
//      tick(now_90k) 用**真静音 ELD 帧**把轴补到实时时间（缺口无上限——客户端
//      可能几十秒不送音频，甚至比视频晚几十秒才开音频流），feed_audio 把真实帧
//      放到轴上（与轴偏差 > 90ms 的一律丢弃其 PTS、改用轴，见下）。
//      ★为什么必须铺满（d278 定案）：mpv `filters/f_decoder_wrapper.c:
//      correct_audio_pts()` 用插值 PTS 预测下一帧，`diff > 0.1s` 告警
//      `Invalid audio PTS`、**`diff >= 5s` 直接 `pts_reset` → `Reset playback
//      due to audio timestamp reset` → `reset_playback_state()`（连视频一起复位）**。
//      旧实现"缺口 > 250ms 就停止填充、允许 PTS 跳变"对渲染器消费者成立，
//      对 mpv 就是每隔几秒复位一次播放 → 表现为"没有声音"。
//   ③ 开局声明双 PID：PAT/PMT 同时含视频与音频 elementary 流。
//     ★d275：PMT 携带 MPEG-4 描述符（IOD + SL），音频以「AAC + ASC extradata
//     + sample_rate=44100」声明——ER(ELD) 解码的容器供参链，详见 ts_muxer.cpp。
//   ④ **音频是基准流**（d289 落地契约 §5.3/§D1）：`audio_axis_` 由 feed_audio
//      推进，是全流唯一时间轴基准；视频向它对齐、落后超阈值即丢（见下）。
//      ★d288 判决：此前本组件是**反向**的 —— 视频 PTS 原样写出、轴被
//      fill_silence 按视频 PTS 拖着走，即「视频当基准、音频追视频」，与契约相反；
//      真机实测 a−v 于视频帧 #600 单跳 +2.778s 后恒定 150s。
//
// ★d289「音频为基准」的三条实现细则（改本文件前必读）：
//   ① 主锚：feed_audio 每帧把 audio_axis_ 推进 980 tick；**视频写出的 PTS 取
//      audio_axis_**（feed_video 内），故视频/音频同域。
//      tick(now_90k) 的 now 仍是**实时基线**（不是音频轴）—— 它管 PCR pacing 与
//      静音填充目标，必须独立于音频帧到达（d278：真静音期几十秒不送音频，
//      轴靠 tick 铺满，否则 mpv 遇 ≥5s 空洞直接复位播放）。
//   ② 丢帧只丢**视频**、只在落后超kVideoDropTh 时丢（feed_video）。
//      ★为什么丢视频不丢音频（用户原话）：「视频丢几帧很难感受到，但音频丢失
//      直接无法正常播放」。⇒ 音频永不丢，缺口一律用真静音 ELD 帧补。
//   ③ 轴**可回拉**（feed_audio 内，阈值 kAxisCorrectionTh=100ms）：此前轴只进退，
//      一次跳变即永久冻结（d288 的 2.78s 就是这么留下的）。回拉幅度远小于 mpv 的
//      5s 复位线（d278），且只在落后时前移⇒ 轴始终单调、不产生倒挂。

//
// 打包约定：188B TS 包；PAT PID 0x0000 → PMT PID 0x1000；视频 ES PID 0x100
//（stream_type 0x1b AVC，PCR 载体）、音频 ES PID 0x101（stream_type 0x0f）；
// 视频 stream_id 0xe0（PES_packet_length 按实值，>64KB 场景写 0）、音频 0xc0；
// 视频 AU 透传（不加 AUD）；单 AU = 单 PES。
//
// 线程前提：feed_video / feed_audio / tick 由同一数据线程串行调用（调用方保证），
// 内部状态无锁；Sink 在该线程内同步调用（返回 false = 下游不可写，调用方收尾）。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace fr::cast {

class TsMuxer {
 public:
  using Sink = std::function<bool(const uint8_t*, size_t)>;  // false = 下游不可写

  // 立即发出 PAT/PMT。has_video=false 时 PCR 挂到音频 PID。
  // ★d292：hevc_video=true 时 PMT 的视频 stream_type 取0x24(HEVC) 而非 0x1b(AVC)。
  //   客户端送 H.265 时若仍声明 AVC，mpv 会用 AVC 解析器解 HEVC ⇒ 解不出参数集、
  //   `non-existing PPS` / `demux-w/h=null` ⇒ 有帧无画面。
  bool start(Sink sink, bool has_video, bool has_audio, bool hevc_video = false);
  // 单个 H.264 Access Unit（Annex-B，00 00 00 01 起始码流）。
  // ★d289：pts_90k 改为「视频帧自己的到达钟」，仅作**诊断与丢帧判据**用；
  //   实际写出的 PTS 取**音频轴现值**（契约 §5.3：音频为基准、视频跟随）。
  //   落后轴超过 kVideoDropTh 即丢这一帧（见 feed_video 实现）。
  bool feed_video(const uint8_t* annexb_au, size_t len, uint64_t pts_90k);
  // 单个裸 AAC-ELD 帧（d275 起不再 LOAS 包装；一帧一 PES——裸帧无重同步，
  // 帧跨 PES 边界即碎。供参（sample_rate/ASC）由 PMT 描述符承担）。
  // ★d289：**音频轴由本函数推进**，是全流时间轴的基准（视频/PCR/静音填充以它为准）。
  bool feed_audio(const uint8_t* eld_frame, size_t len, uint64_t pts_90k);
  // 主时钟推进：PCR 连续性保障（无帧也发 PCR-only 包）+ 音频缺口填充策略。
  // ★d289：now_90k 是**实时基线**（调用方按单调到达钟算出），**不是**视频 PTS。
  //   它有两个用途：① PCR pacing ② fill_silence 的填充目标 —— 必须独立于音频帧
  //   到达（d278：真静音期可能几十秒不送音频，轴要靠 tick 铺满，音频永不丢）。
  //   ⇒ 「音频为基准」指的是**视频 PTS 取音频轴**，不是把tick 喂成音频轴。
  void tick(uint64_t now_90k);
  // ★d429：请求下一轮 tick **立即重发 PAT/PMT**（不等 100ms 周期）。用途 = 回环
  //   重连接入（mpv 按位重载/暂停恢复的新 demuxer 实例）：其探测窗仅 16384B
  //  （d285），而周期 PSI 与重连点的间隔可达 100ms 轴时（10~20Mbps 下 125~250KB）
  //   ⇒ 窗内无 PMT ⇒ 音频 PID 被盲探成 mp3、视频轨建不出（真机 23:48:31 实证）。
  //   跨线程安全（数据线程消费，try_accept/解码切换路径置位）。
  void request_psi_resync();
  // 复位内部状态（可再次 start）。
  void stop();
  // ★d289：音频轴现值（下一个要发出的音频 PTS）。调用方用它驱动 tick()。
  uint64_t audio_axis() const { return audio_axis_; }
  // ★d289：实时基线（tick 的入参口径）。仅供定向回归探针比对"轴 vs 真实时间"。
  uint64_t real_90k() const { return real_90k_; }
  // ★d289：已发出的最后一枚音频 PTS（定向回归探针用：看纠偏的第二道保护是否卡住）。
  uint64_t last_audio_pts() const { return last_audio_pts_; }
  // ★d289 诊断：因落后音频轴而被丢的视频帧数 / 音频轴回拉次数。
  uint64_t dropped_video() const { return dropped_video_; }
  uint64_t axis_corrections() const { return axis_corrections_; }
  // ★d311：视频 PTS 补步进次数（重复帧数）。
  uint64_t video_pts_bumps() const { return video_pts_bumps_; }

  // d273 诊断：broken_ 置位时的最后一次原因（"" = 未 broken；sink 写失败不在此列——
  // 那由调用方按"下游断开"处理）。
  const char* broken_reason() const { return broken_reason_ ? broken_reason_ : ""; }

 private:
  // CC 槽位：0=视频 PID，1=音频 PID，2=PAT，3=PMT
  static constexpr int kCcVideo = 0, kCcAudio = 1, kCcPat = 2, kCcPmt = 3;

  // 唯一的 TS 打包口：body（≤184B）或 PCR-only 包（body=nullptr/0）。
  // stuffing 走 adaptation field；CC 只对有 payload 的包递增（ISO 13818-1）。
  bool write_packet(uint16_t pid, int cc_slot, bool pusi, const uint8_t* body,
                    size_t body_len, bool with_pcr, uint64_t pcr_90k);
  bool emit_psi(bool pat);  // PAT/PMT：单 section 单包（pointer_field + CRC + stuffing）
  bool emit_pes(uint16_t pid, int cc_slot, uint8_t stream_id, const uint8_t* payload,
                size_t len, uint64_t pts_90k);
  bool fill_silence(uint64_t now_90k);  // 细则②：缺口重发上帧

  bool broken_ = false;  // sink 写失败后所有 feed/tick 快速返回 false
  const char* broken_reason_ = nullptr;  // d273 诊断：broken 的最近一次原因

  Sink sink_;
  bool has_video_ = false, has_audio_ = false;
  // ★d292：视频编码（false=H.264/AVC、true=H.265/HEVC），仅影响 PMT stream_type。
  //   feed_video 路径两者不变—— HEVC 裸流同样是 Annex-B（三字节起始码），原样透传即可。
  bool hevc_ = false;
  // ★d278：CC 初值 15 ⇒ 每路第一包写出 0；语义 = 「上次写出去的值」，
  //   有载荷包**先自增后写**，无载荷包（afc=2，PCR-only）原值写出。
  //   与 FFmpeg mpegtsenc.c 逐字一致（见 ts_muxer.cpp write_packet 注释）。
  uint8_t cc_[4] = {15, 15, 15, 15};
  uint64_t last_pcr_at_ = 0;  // 自产 PCR pacing 基准（90kHz，细则①）
  bool pcr_seeded_ = false;
  // ★d282：PSI 由 tick 周期重发（不再只在开局发一次）—— 写路径改为有界超时后，
  //   停滞期会主动丢分片，流中间出现断层，mpv 必须靠 PSI 重新同步（FFmpeg
  //   mpegtsenc 默认 pat_period=0.1s，行业基线）。
  //   ★用独立的 psi_seeded_ 而非 `last_psi_at_ == 0` 哨兵：轴时间可以合法地就是 0，
  //     用 0 当"未锚定"会让首轮 tick(now=0) 把锚写回 0 ⇒ 之后每轮都重发 PSI。
  //     （同 pcr_seeded_ 的做法，见 tick。）
  uint64_t last_psi_at_ = 0;
  bool psi_seeded_ = false;
  // ★d429：PSI 立即重发请求（回环重连接入时置位，下一轮 tick 消费并清零）。
  //   atomic：置位方可能在数据线程之外（解码切换走 mux 线程、try_accept 走数据线程），
  //   消费方 = mux 线程 tick。
  std::atomic<bool> psi_resync_{false};

  // 音频时间轴（细则②，d278 重写）：轴 = 下一个要发出的音频 PTS（90kHz），
  // 每发一帧 +980；由 tick(now_90k) 用静音帧补到实时时间。
  bool have_audio_ = false;     // 轴已启用（start 即置，铺头段空头）
  uint64_t audio_axis_ = 0;
  // ★d289：**已发出的最后一枚音频 PTS**（含 start() 前置静音与 fill_silence 补的
  //   静音帧）。d278 的红线要求音频 PTS 严格单调，而 d289 引入的轴回拉会拿到
  //   「抖动后的到达钟」，可能落在已发出 PTS 之后 ⇒ 靠这个基准兜住单调。
  //   无需哨兵：has_audio_ 时 start() 必先发 3 帧静音，本值必然已有效。
  uint64_t last_audio_pts_ = 0;
  // ★d311：**已写出的最后一枚视频 PTS**（视频自己的单调基准，与音频基准同域但不混用）。
  //   d297 定案：视频帧落在同一音频 tick 内（音频 92fps vs 视频 24fps，网络突发时
  //   同 tick 多帧）⇒ 拿到相同 PTS ⇒ mpv 判零增量丢帧 ⇒ 停转⇒追帧跳变（真机实测
  //   `Invalid video timestamp` 1816 条/轮、vfps 29.9→3.7、time-pos 冻 3s）。
  //   修法 = 亚帧级最小步进（kVideoPtsMinStep=1ms），**不拆两个时钟**（d298 铁律：
  //   a−v=0 恰恰因为共基准；锚仍是 last_audio_pts_，只补步进）。
  uint64_t last_video_pts_ = 0;

  // ★d289：**实时基线**（由 tick(now_90k) 持续更新，单调到达钟口径）。
  //   它是"真实媒体时间"的唯一客观参照 —— 音频轴回拉判据必须拿它比，
  //   不能拿调用方传的 pts_90k（那条链在 d288 病灶里与轴一起被污染，
  //   互相比较差值恒为 0 ⇒ 纠偏永不触发，定向回归场景 B 判为假绿灯抓出）。
  uint64_t real_90k_ = 0;
  bool real_seeded_ = false;

  // ★d289 诊断计数器（契约 §5.3「视频跟随、赶不上就丢」的执行量）：
  //   dropped_video_   —— 因落后音频轴超阈值而被丢的视频帧数
  //   axis_corrections_ —— 音频轴回拉次数（d288 的 2.78s 永久错位若复发，
  //     这个计数会持续增长；正常应为 0 或极低）
  uint64_t dropped_video_ = 0;
  uint64_t axis_corrections_ = 0;
  // ★d311 诊断计数器：视频 PTS 因重复而补步进的次数（正信号：突发时 >0；
  //   负信号：a−v 仍须为 0 —— 步进只发生在重复帧上，亚毫秒级不破坏对齐）。
  uint64_t video_pts_bumps_ = 0;
  std::vector<uint8_t> pes_buf_;  // PES 组包暂存（复用，避免反复分配）
};

}  // namespace fr::cast
