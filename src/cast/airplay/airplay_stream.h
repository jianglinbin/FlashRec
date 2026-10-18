#pragma once
// airplay_stream：AirPlay 数据面（AIRPLAY_EMBED_PLAN.md §5.1/§5.3；d269 回收实装）。
//
// 把 UxPlay lib 工作线程产出的裸流搬进 mpv。d269 拍板只走 **M2b 单 TS 流**：
//   · 镜像会话：视频 AU + 音频 LOAS 帧进 TsMuxer（专属复用线程 = socket 唯一
//     写者），单 URI（load_format="mpegts"）；
//   · 纯音频会话：音频单流（ct=8→"latm"，ct=2→"caf"，ct=4→"aac"）。
//   （备份骨架里的 M2a 双流+外挂轨路径已裁剪：M2b 是 d248/d249 真机调试的
//    实测路径，双流路径从未通过真机验收。）
//
// 关键机制：
//   · 预缓冲 4MiB：盖住「listen → publish SetUri → mpv connect → probe」窗口；
//     socket 就绪后先冲缓冲再实时写；稳态直通（§5.3 细则⑥）。
//   · 音频锚定轴（§5.3）：PTS_90k = ntp_local_ns × 9 / 100000（本地墙钟域），
//     音频即主轴；视频同域直映；差分到达偏差经 EMA 检测，>250ms 持续时
//     垫音频（上限 500ms，超限只告警）——恒定偏移兜底，不做帧级丢删。
//   · 复用线程是 socket 唯一写者（每流单写者纪律）；lib 回调线程只入队。
//
// 线程模型：start_*/feed_* 由 lib 回调线程调用（audio= rtp 线程、video= mirror
// 线程）；stop/reset 由 source 的 tick 或回调触发（主线程/回调线程皆可，
// 内部互斥收口）。publish_dmr 线程安全（EventBus 自带锁）。

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/event_bus.h"
#include "cast/airplay/audio_wrap.h"
#include "cast/airplay/ts_muxer.h"

namespace fr {

struct Config;

namespace cast {

// 会话的音频编解码类型（lib audio_get_format / audio_decode_struct.ct 传来）
enum class AudioCt : uint8_t { Alac = 2, AacLc = 4, AacEld = 8 };

class AirplayStream {
 public:
  AirplayStream(EventBus& bus, const Config& cfg);
  ~AirplayStream();

  AirplayStream(const AirplayStream&) = delete;
  AirplayStream& operator=(const AirplayStream&) = delete;

  // —— lib 回调线程入口 ——

  // 镜像视频 AU（已解密 Annex-B；data[0]!=0 = 解密失败标记由 source 层判定）。
  // 首帧：建通道 + 经 bus 投 SetUri（source=AirPlay, load_format="mpegts"）。
  void feed_video(const uint8_t* data, size_t len, uint64_t ntp_local_ns,
                  uint64_t ntp_remote_ns, const char* session_title);

  // 音频帧（已解密裸帧；ct 决定包装器）。首次调用时若尚无任何会话（纯音频模式）
  // 则记"待确认"（d248：镜像时音频先到是常态）；镜像会话中静默入队。
  // ★d277：ntp_remote_ns 是**时间轴真源**（客户端钟），ntp_local_ns 仅作锚点兜底。
  void feed_audio(const uint8_t* data, size_t len, uint8_t ct, uint64_t ntp_local_ns,
                  uint64_t ntp_remote_ns, const char* session_title);

  // —— source 控制面（主线程 / lib 回调线程皆可）——

  // 会话收尾（video_reset / conn_reset / watchdog / snapshot Stopped）：
  // 停复用线程、关 socket（对端 EOF → mpv END_FILE）、清预缓冲与轴状态。幂等。
  void stop_streams(const char* why);

  // ★d435：收尾/接管期间暂停 lib 回调入口（feed_*）。stop_streams 复位会话后、
  //   raop_destroy 完成前若有在途回调，会重新建会话并投 SetUri 把刚接管的
  //   DLNA 会话抢回去。stop()/被接管路径先暂停，start() 成功后恢复。
  void pause_feeds() { accepting_.store(false, std::memory_order_relaxed); }
  void resume_feeds() { accepting_.store(true, std::memory_order_relaxed); }

  // d273：source 侧视频解密失败计数转发（首条 WARN 由 source 打，之后静默累计）
  void notify_decrypt_drop() { st_dec_drop_.fetch_add(1); }

  // audio_flush / video_flush：清预缓冲（管道内已写入的帧由 mpv 快速消费，
  // 无需干预 —— 对照 UxPlay 的 flush 为空实现）
  void flush();
  // ★d292：客户端送来的视频编码（H.265 = true）。由 AirplaySource 的
  //   video_set_codec 回调写入，TsMuxer::start() 用它决定 PMT 的 stream_type
  //   （AVC 0x1b / HEVC 0x24）。声明错编码 ⇒ mpv 用错解析器 ⇒ 有帧无画面。
  void set_video_hevc(bool hevc) { video_hevc_ = hevc; }
  bool video_hevc() const { return video_hevc_; }

  // 主线程每轮（由 source::tick 调用）：① 确认"待确认纯音频"会话（d248：
  // 镜像场景音频先到，不能一收到音频就投 SetUri；等 500ms 窗口过了仍无视频，
  // 才认作纯音频并投流）② 复用线程死后的兜底清理（下游断开时线程自行退出）。
  void tick(double now);

  bool session_active() const;          // 有活跃会话（镜像或音频）
  bool mirror_active() const;           // 镜像中（MirrorStarted 事件判据）

  // ★d303 传输延迟测量状态（锚点法，音视频分开）
  uint64_t probe_anchor_remote_ns_ = 0;  // 首帧的客户端钟（0=未锚定）
  uint64_t probe_anchor_local_ns_ = 0;   // 首帧的本地到达钟
  double probe_transit_v_ = 0.0;         // 视频传输延迟：瞬时/峰值分开存
  double probe_transit_a_ = 0.0;
  double probe_now_v_ = 0.0;
  double probe_now_a_ = 0.0;

 private:
  struct Frame {
    std::vector<uint8_t> data;
    uint64_t ntp_ns = 0;       // 帧内时间戳（本地轴）
    uint64_t recv_ns = 0;      // 到达时刻（**单调钟**；d279d 起为 PES 时间轴来源）
    // ★d303：客户端发送时刻（UxPlay 的 NTP 远端钟，1900 纪元纳秒）。
    //   实测**两个钟完全同速**（视频/音频两帧的 remote 差与本地钟差都是 2.193s，
    //   偏差 0.000s，见台账 d303）⇒ **两钟相减恒为 0，测不出传输延迟**；
    //   正确测法是「取首帧做固定锚点，之后每帧 `remote − (锚点remote + 本地经过)`」
    //   ⇒ 得到的正是「客户端已发出、但数据还没到我们」的那段延迟（传输+客户端排队）。
    uint64_t remote_ns = 0;    // 0 = 本帧无远端钟（不可用）
  };

  // 单流回环通道（listen + 预缓冲 + accept + 写）
  // 完整定义放在头文件：它是值成员，嵌套类型必须先完整才能声明成员。
  struct Channel {
    const char* tag_ = "chan";  // ★d320：日志标识（视频/音频）
    bool open();                     // listen（*port=0 自动分配），回填 port_
    void close();                    // 关 conn + listen，清预缓冲
    unsigned short port() const { return port_; }
    // 数据线程调用：未连接/重连窗口时入预缓冲（4MiB 上限，丢最旧）；已连接时先冲
    // 缓冲再直写。返回 false 仅当 listen fd 已关（通道整体收尾）。
    // ★d312/d314：对端"活着但不读"（mpv 暂停/缓存满）丢片续跑、不判死；对端连接
    //   消亡走 on_peer_closed 后本片落入预缓冲、等 mpv 重载的新连接 —— **mux 线程
    //   不因对端断开退出**（22:13 现场：退出 ⇒ 没人 accept 新连接 ⇒ mpv probe
    //   阻塞 15s ⇒ transition-timeout ⇒「恢复播放后实际未恢复」）。
    bool offer(const uint8_t* data, size_t len);
    // ★d316：mux 线程取走「重连成功」标记（取走即清零；瞬时持锁，与 offer 同
    //   线程域，无 AppHang 风险 —— d282 铁律针对的是主线程取此锁，mux 线程本来就是
    //   offer 的调用方）。
    bool take_reconnected();
    // ★d431：解除「重连抑制窗」。重连 accept（try_accept 的 peer_closed_ 路径）
    //   起到本调用为止，offer() 丢弃一切写出（含预缓冲）——保证新连接写出的第一批
    //   字节是 mux 线程紧接着发出的 PAT/PMT（fresh demuxer 的 16384B 探测窗必须以
    //   PSI 开头，否则 probe 判死；d430 真机实证：单枚陈旧视频帧 ~18KB 即可独占窗口）。
    //   mux 线程在消费重连标志、发出 PSI 前调用；抑制窗 ≤ 一个 mux 迭代。
    void clear_suppress();
    bool listen_ok() const { return lfd_ >= 0; }
    bool connected() const { return conn_ >= 0; }
    // 唤醒阻塞在 send() 上的数据线程（shutdown）。★不取 mu_，任意线程可调 ——
    // stop 路径必须先调它，阻塞的写才会返回、mux 线程才能退出、join 才不会挂死。
    void shutdown_conn();
    // flush 回调（audio_flush/video_flush）：只清预缓冲，管道内已写的帧由 mpv 消费
    void drop_prebuffer();
    // d273 诊断：当前预缓冲字节。★d282：**读原子量，不取 mu_** —— 本函数在
    // 主线程（5s 汇总）里调用，取锁 = 让主线程可被数据面的写阻塞拖死（AppHang 根因）。
    size_t prebuffer_size() const { return pre_pub_.load(std::memory_order_relaxed); }

   private:
    bool try_accept();               // 非阻塞/限时 accept 一个连接
    // 停滞策略（d282/★d312 修订）：对端活着但不消费 ⇒ 丢片续跑、恒返回 true
    //（不再有「超上限判死」—— 暂停可持续任意久，判死只会打断暂停恢复）。
    bool stall_policy();
    // ★d312/d314：对端连接消亡收尾（关 fd、清陈旧预缓冲、回 accept 等重连）。
    // ★d314：调用方（offer）收到它后**不得再向上传播失败** —— 对端断开 = 暂时
    //   无连接，mux 线程必须活下来 accept mpv 重载的新连接（22:13 现场教训）。
    bool on_peer_closed();
    // 预缓冲插入（带上限丢最旧）—— 未连接/重连窗口共用（★d314 抽取）。
    void enqueue_pre(const uint8_t* data, size_t len);

    int lfd_ = -1;             // listen fd
    int conn_ = -1;            // mpv 连接 fd
    // ★d316：对端关闭 → 重连成功 的一次性标记（mux 线程据此做视频 IDR 对齐）。
    //   peer_closed_ 在 on_peer_closed 置位；try_accept 接到新连接时转成
    //   reconn_flag_；take_reconnected() 由 mux 线程取走并清零（同线程域，加锁
    //   只为与 offer 的 mu_ 一致——持锁窗口是瞬时的，无 AppHang 风险）。
    bool peer_closed_ = false;
    bool reconn_flag_ = false;
    // ★d431：重连抑制窗（mu_ 保护）。try_accept 重连路径置位；mux 线程
    //   clear_suppress() 解除。置位期间 offer() 直接丢弃写出。
    bool suppress_ = false;
    // d282：conn_ 的无锁镜像 —— shutdown_conn() 必须能在**不持 mu_** 的前提下
    // 拿到 fd（写阻塞时 mu_ 正被数据线程攥着，取锁就等于一起挂）。
    std::atomic<int> conn_pub_{-1};
    unsigned short port_ = 0;
    std::vector<uint8_t> pre_;   // 预缓冲（约 4MiB 上限，丢最旧）
    size_t pre_total_ = 0;
    std::atomic<size_t> pre_pub_{0};  // d282：pre_total_ 的对外镜像（主线程无锁读）
    double stall_since_ = 0.0;   // d282：本轮停滞起点（mono 秒；0 = 未在停滞）
    int stall_drops_ = 0;        // ★d320：本轮停滞期间丢弃的分片数（恢复时清零）
    std::mutex mu_;
  };

  // 复用线程主体（M2b）：drain 双队列 → PTS 计算 → TsMuxer → 写 socket
  void mux_thread_main();
  void request_stop_mux();

  Channel video_ch_;
  Channel audio_ch_;

  // 会话状态（streams_mu_ 保护）
  std::mutex streams_mu_;
  enum class Session { None, AudioOnly, Mirror };
  Session session_ = Session::None;
  uint8_t ct_ = 0;                  // 音频编解码（AirPlay ct 值）

  bool video_hevc_ = false;      // ★d292：客户端实际送来的编码
  // ★d317：视频参数集缓存（Annex-B：H264 SPS+PPS / HEVC VPS+SPS+PPS，会话首帧
  //   缓存）。**跨 mux 线程/跨会话重建保留** —— d316 版是 mux 线程局部，会话重建
  //   即丢（真机 22:41:57 重建会话 0B ⇒ 探测窗无视频参数集 ⇒ 视频轨建不出来）。
  //   同一手机同一流参数不变，跨会话保留是收益；换设备由新会话首帧覆盖。
  std::mutex vparams_mu_;
  std::vector<uint8_t> vparams_;
  bool video_uri_published_ = false;
  bool audio_uri_published_ = false;
  // d248：只收到音频时的"待确认"起点（mono 秒；0 = 不在待确认态）。
  // 镜像会话里音频先到是常态，所以要给视频一个短暂的追赶窗口再定会话类型。
  double audio_only_since_ = 0.0;
  std::string title_;

  // M2b 复用管线
  std::unique_ptr<TsMuxer> mux_;
  // ★d435：feed_* 入口开关（见 pause_feeds/resume_feeds）。
  std::atomic<bool> accepting_{true};
  // d271：stop_streams 的收口锁。调用方横跨 lib 回调线程（conn_destroy/
  // conn_reset/video_reset）与主线程（source stop / snapshot stopped / dtor），
  // 两路交叠时并发 join 同一线程（UB）+ 并发 mux_.reset()（double free → 堆
  // 损坏，炸点随机）。request_stop_mux 只可经持锁的 stop_streams 进入。
  std::mutex stop_mu_;
  std::deque<Frame> vq_, aq_;       // 双 SPSC（每流单生产者，复用线程单消费者）
  std::mutex q_mu_;
  std::condition_variable q_cv_;
  std::atomic<bool> mux_running_{false};
  std::atomic<bool> mux_stop_{false};
  std::thread mux_thr_;
  // 时间轴基准：NTP 域绝对值（~4e18 ns）直接 ×9/100000 会溢出 uint64，
  // 故统一平移到「首帧 = 0」的相对轴（§5.3 音频锚定轴）。
  uint64_t base_ntp_ = 0;
  double base_mono_ = 0.0;           // 首帧对应的单调秒（PCR pacing 的相对轴）

  // —— d277 帧时间戳换算（客户端远端钟 → 本地单调域）——
  // 背景：lib 的 ntp_time_local 依赖客户端 NTP 校时，本机环境
  //   raop_ntp_convert_remote_time() 恒返 0（client_time_received=false），
  //   而原先 PTS 只取它 → 所有帧 PTS=0、mpv 报 "Invalid video timestamp"
  //   → 无声无画。改与上游 uxplay 同构：以客户端远端钟（视频在包内、音频由
  //   RTP 时间戳换算）为源，用本地单调钟一次性锚定 → 单调、连续、抗丢校时。
  // 两分支都落在 steady_clock 域，混合使用不会错轴。
  uint64_t stamp_ns(uint64_t remote_ns);   // 帧时间戳（本地单调域 ns）
  std::mutex clock_mu_;                    // 锚点独占；与 streams_mu_ 无嵌套
  uint64_t clock_anchor_remote_ = 0;       // 首个有效远端钟值
  uint64_t clock_anchor_local_ = 0;        // 对应时刻的本地单调 ns
  // ALAC（ct=2）的 CAF 流：首包必须带文件头 + magic cookie，之后透传
  CafWriter caf_;
  bool caf_header_sent_ = false;
  // 同步策略状态（§5.3 恒定偏移兜底）：EMA 于到达时刻测量（mux 线程消费时更新）
  double video_lag_ema_ = -1.0, audio_lag_ema_ = -1.0;  // -1 = 未初始化
  uint64_t audio_shift_90k_ = 0;    // 垫音频量（0..500ms）
  bool shift_warned_ = false;

  EventBus& bus_;
  const Config& cfg_;

  // —— d273 数据面诊断计数器（原子；lib 线程 / mux 线程 / 主线程三方读写）——
  // 链路：lib 回调 feed_* → 队列 → mux 线程消费 → TsMuxer 输出（sink）→ Channel 写。
  // 每层都有计数，5s 汇总一条 INFO，断在哪一层一眼可辨。
  std::atomic<uint64_t> st_v_in_{0}, st_v_in_b_{0};        // 视频 AU 到达（帧/字节）
  std::atomic<uint64_t> st_a_in_{0}, st_a_in_b_{0};        // 音频帧到达（帧/字节）
  std::atomic<uint64_t> st_v_drops_{0}, st_a_drops_{0};    // 队列满丢最旧
  std::atomic<uint64_t> st_mux_v_{0}, st_mux_a_{0};        // mux 线程消费
  std::atomic<uint64_t> st_out_pkts_{0}, st_out_bytes_{0}; // TsMuxer 输出（sink 回调）
  std::atomic<uint64_t> st_w_fail_{0};                     // Channel 写失败（EPIPE 等）
  std::atomic<uint64_t> st_dec_drop_{0};                   // 视频解密失败丢弃（source 侧转发）
  std::atomic<uint64_t> st_idr_drop_{0};                   // ★d316：重连后等 IDR 期间丢弃的视频帧
  std::atomic<uint64_t> st_broken_{0};                     // TsMuxer broken 次数
  // d279：mux 线程发布「PES 轴现值」（90kHz）——主线程用它算生产侧积压
  //   =（本地经过 − 前沿媒体时间）；与 mpv 侧探针（time-pos）对读可一刀切开延迟归属。
  std::atomic<uint64_t> st_axis_90k_{0};
  double st_last_at_ = 0.0;                                // 上次汇总时刻（仅主线程 tick）
  // 5s 差值快照（仅主线程 tick 读写，无需原子）
  struct StatsSnap {
    uint64_t v_in = 0, v_in_b = 0, a_in = 0, a_in_b = 0;
    uint64_t mux_v = 0, mux_a = 0, out = 0, out_b = 0;
    uint64_t wfail = 0, dec = 0, vdrop = 0, adrop = 0, broken = 0;
  };
  StatsSnap st_snap_;
};

}  // namespace cast
}  // namespace fr
