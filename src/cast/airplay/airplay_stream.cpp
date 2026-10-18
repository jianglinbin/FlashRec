// airplay_stream 实现 —— 见头文件（数据面通道 + §5.3 同步策略 + M2b 复用线程）。
//
// 与方案逐条对应：§5.1 通道矩阵 / §5.3 音频锚定轴 + 恒定偏移兜底 / §5.4 预缓冲
// 仅启动窗口、稳态直通；TCP_NODELAY 在 stream_local 侧已设（细则⑤）。
// d269：M2a 双流路径已裁剪（M2b 单 TS 流恒定），备份骨架的 d248/d249 真机
// 踩坑注释（音频先到 / numSubFrames 位宽）逐条保留。
//
// 线程分工（每流单写者，无锁竞争）：
//   · lib mirror/rtp 线程 → feed_video/feed_audio（入队或直接写）；
//   · 复用线程（M2b）是 socket 的唯一写者：drain 队列 → 算 PTS → TsMuxer → 写；
//   · lib httpd 线程 → stop_streams（经 source 的回调）；内部用互斥收口，幂等。

#include "cast/airplay/airplay_stream.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>  // std::getenv（诊断开关）
#include <cstring>
#include <filesystem>
#include <utility>

#include "app/config.h"
#include "app/log.h"
#include "cast/airplay/annexb_scan.h"
#include "cast/cast_contract.h"
#include "platform/paths.h"
#include "platform/stream_local.h"

namespace fr::cast {
namespace {

// 90kHz tick 换算：1 tick = 1/90000 s ⇒ ns × 9 / 100000
// （入参必须是**相对** ns，绝对值（NTP 域 ~4e18）会溢出 uint64）
inline uint64_t ns_to_90k(uint64_t ns) { return ns * 9u / 100000u; }

inline uint64_t wall_ns() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count());
}

inline double mono_sec() {
  using namespace std::chrono;
  return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

// d277：本地单调纳秒（帧到达钟；远端钟缺失时的兜底时间轴源）
inline uint64_t mono_ns() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

// EMA 步长（到达偏差测量；α=0.1 慢速跟踪防泵动）
constexpr double kLagAlpha = 0.1;
// 垫音频（§5.3 恒定偏移兜底）：>250ms 触发、上限 500ms、超限只告警
constexpr double kSkewStartSec = 0.250;
constexpr double kSkewMaxSec = 0.500;
constexpr size_t kPrebufferCap = 4u * 1024 * 1024;  // 4MiB ≈ 2–3s 1080p 镜像流
// 队列上限（背压：mpv 长时间不消费时丢最旧，防内存无界增长）
constexpr size_t kQueueCap = 256;
// ★d312：原「回环写停滞 3s 判死」时限已废除 —— 停滞 = 对端活着只是不读（mpv 暂停
//   是预期场景、可持续任意久；IDR 码率浪是瞬态），一律丢片续跑（stall_policy）；
//   真死（对端关闭/复位）由 fr_stream_write 返回 ERROR 走 on_peer_closed 重连收尾。
//   判死的实际后果（真机 21:42 现场）：恢复播放后 mux 消费恒 0、队列 256 顶满全丢、
//   mpv 只能播 TCP 存量直到 EOF 重建 —— 正是用户实测「分钟级延迟 + 卡死」的来源。
// 复用线程空转周期（顺便承担 PCR pacing，细则①：≤100ms）
constexpr int kMuxIdleMs = 10;
// 「待确认纯音频」窗口（d248）：镜像时音频先到，等这么久没见视频才敢确认是纯音频。
// 取值权衡：太长 → 纯音频投送起播慢；太短 → 镜像被误判成纯音频（会出现双发布）。
// 500ms 远大于"音频先起→视频首帧"的实测间隔（同一次 SETUP/RECORD 内），又短到听不出延迟。
constexpr double kAudioOnlyConfirmSec = 0.5;

constexpr const char* kTag = "[AIR]";

// d273 原始流落盘（ts_dump_file 内懒初始化打开；写点在 feed_video/feed_audio）
// d275：音频 dump 改记裸 ELD 帧（[u16 le 长度][帧] 记录流），与镜像 PES 载荷同源
FILE* g_eld_dump = nullptr;    // 裸 ELD 帧（TS 音频 PES 载荷真值，含帧边界）
FILE* g_video_dump = nullptr;  // 视频 AnnexB AU 顺序拼接（lib mirror 输出真值）

// TS 落盘诊断（d249）：`FR_AIRPLAY_TS_DUMP` 指向文件时，把复用器写出的 TS 流
// 原样另存一份，供事后解析（PAT/PMT/PES/PCR/PTS 都能查）。
// d273：默认开到 logs/airplay-dump/（排障期常开，IO 开销可接受）——
//   audio_eld.bin   = 裸 ELD 帧记录流（[u16 le 长度][帧]，TS 音频 PES 载荷真值）
//   video_annexb.h264 = 视频 AU 顺序拼接（lib mirror 输出真值，ffprobe -f h264 可试解）
//   ts_dump.ts      = 复用器 TS 输出（mpv 实际收到的字节）
//   音频/视频原始字节用 ffprobe 逐层对照，把「生成 vs 封装 vs mpv」的断点钉死。
// 设 FR_AIRPLAY_NO_DUMP=1 可关闭。
FILE* ts_dump_file() {
  static FILE* fp = nullptr;
  static bool tried = false;
  if (!tried) {
    tried = true;
    if (std::getenv("FR_AIRPLAY_NO_DUMP") && *std::getenv("FR_AIRPLAY_NO_DUMP")) return fp;
    static std::string dir;
    dir = fr::paths::log_dir() + "/airplay-dump";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string ts_path = dir + "/ts_dump.ts";
    const std::string eld_path = dir + "/audio_eld.bin";
    const std::string video_path = dir + "/video_annexb.h264";
    fp = std::fopen(ts_path.c_str(), "wb");
    g_eld_dump = std::fopen(eld_path.c_str(), "wb");
    g_video_dump = std::fopen(video_path.c_str(), "wb");
    if (fp) {
      static std::vector<char> buf(1u << 20);
      std::setvbuf(fp, buf.data(), _IOFBF, buf.size());
    }
    if (g_eld_dump) {
      static std::vector<char> lbuf(1u << 16);
      std::setvbuf(g_eld_dump, lbuf.data(), _IOFBF, lbuf.size());
    }
    if (g_video_dump) {
      static std::vector<char> vbuf(1u << 20);
      std::setvbuf(g_video_dump, vbuf.data(), _IOFBF, vbuf.size());
    }
    FR_LOG_INFO("{} 原始流落盘：{}（{}）", kTag, dir,
                (fp && g_eld_dump && g_video_dump) ? "ts+eld+video 全开" : "部分失败");
  }
  return fp;
}

// ct → demuxer 提示（player 侧转 mpv demuxer-lavf-format）
const char* audio_load_format(uint8_t ct) {
  switch (ct) {
    case 8: return "latm";
    case 2: return "caf";
    case 4: return "aac";
    default: return nullptr;
  }
}

}  // namespace

// ————————————————————————————— Channel ————————————————————————————

bool AirplayStream::Channel::open() {
  std::lock_guard<std::mutex> lk(mu_);
  if (lfd_ >= 0) return true;
  unsigned short p = 0;
  int fd = fr_stream_listen(&p);
  if (fd < 0) {
    FR_LOG_ERROR("{} 回环监听创建失败", kTag);
    return false;
  }
  lfd_ = fd;
  port_ = p;
  return true;
}

void AirplayStream::Channel::close() {
  std::lock_guard<std::mutex> lk(mu_);
  if (conn_ >= 0) fr_stream_close(conn_);
  conn_ = -1;
  conn_pub_.store(-1, std::memory_order_relaxed);  // d282：与 conn_ 同步维护无锁镜像
  if (lfd_ >= 0) fr_stream_close(lfd_);
  lfd_ = -1;
  port_ = 0;
  pre_.clear();
  pre_total_ = 0;
  pre_pub_.store(0, std::memory_order_relaxed);
  stall_since_ = 0.0;
}

// ★d282：禁止后续写，为收尾铺路。conn_pub_ 是 conn_ 的原子镜像 ⇒ 本函数**不取 mu_**。
// 这是硬要求：写阻塞时 mu_ 正被数据线程攥着（offer 持锁调 fr_stream_write），
// 在这里取锁就等于和阻塞的写一起挂死。
//
// ⚠️别记反（build/test_ts/test_shutdown_wake.cpp 实测）：Win32 的 shutdown **唤不醒**
//   正在阻塞的 send()（POSIX 才会）。所以 join() 真正靠的是 FR_STREAM_WRITE_TIMEOUT_MS
//   =250ms 的写超时；本函数的价值是让 mux 线程在超时后的**下一轮** send 立刻拿到
//   WSAESHUTDOWN → _ERROR 收尾，省掉又一次 250ms 的等待。
void AirplayStream::Channel::shutdown_conn() {
  const int c = conn_pub_.load(std::memory_order_relaxed);
  if (c >= 0) fr_stream_shutdown(c);
}

void AirplayStream::Channel::drop_prebuffer() {
  std::lock_guard<std::mutex> lk(mu_);
  pre_.clear();
  pre_total_ = 0;
  pre_pub_.store(0, std::memory_order_relaxed);
}

bool AirplayStream::Channel::try_accept() {
  // 超时 0：数据线程绝不能为等 mpv 连接而阻塞（lib 的 rtp/mirror 线程有实时性要求）
  int c = fr_stream_accept(lfd_, 0);
  if (c < 0) return false;
  conn_ = c;
  conn_pub_.store(c, std::memory_order_relaxed);
  stall_since_ = 0.0;
  // ★d316：对端关闭后的重连成功 ⇒ 通知 mux 线程做视频 IDR 对齐（参数集重注入 +
  //   丢弃到下一关键帧）。会话首个连接不置位（那本来就是从 IDR 起播的正常路径）。
  if (peer_closed_) {
    reconn_flag_ = true;
    peer_closed_ = false;
    // ★d431：重连抑制窗。断连窗口积下的预缓冲是「中 GOP 陈旧片」（平均单帧
    //   ~18KB、可更大）—— accept 后先冲它就会抢在 PAT/PMT 之前占满 fresh
    //   demuxer 的 16384B 探测窗（d430 真机 41.114 现场：probe 判死 Unknown
    //   codec）。这里整体丢弃 + 置抑制：suppress 期间 offer 丢弃一切写出，
    //   直到 mux 线程 clear_suppress()（其时 PSI 已发出）。
    suppress_ = true;
    pre_.clear();
    pre_total_ = 0;
    pre_pub_.store(0, std::memory_order_relaxed);
  }
  FR_LOG_INFO("{} mpv 已接入回环流（port={}）", kTag, port_);
  return true;
}

// d282：停滞策略（★d312 修订：**停滞不再判死**）。
//   STALL 的语义 = 对端活着只是不读。两种场景：①mpv 被暂停 —— 预期行为、可持续
//   任意久（真机 21:42 现场：暂停 83s → 3s 判死 → 恢复后 mux 消费恒 0、队列 256
//   顶满全丢、mpv 只能播 TCP 存量直到 EOF）；②IDR 码率浪瞬态（d282 原场景）。
//   两者都应「丢片续跑到底」。真死（对端关闭/复位）走 FR_STREAM_WRITE_ERROR 路径
//   （offer 内：关旧 fd、清陈旧预缓冲、回 accept 等新连接），不经过这里。
//   暂停期丢弃的片无关紧要 —— 恢复播放由 player 的「跳直播前沿重载」兜底（d312）。
bool AirplayStream::Channel::stall_policy() {
  if (stall_since_ <= 0.0) {
    stall_since_ = mono_sec();
    FR_LOG_WARN("{} [{}] 回环写停滞（对端不读：mpv 暂停/缓存满），丢弃后续分片", kTag, tag_);
  }
  stall_drops_++;  // ★d320：停滞丢片计数（恢复时随时长一并输出）
  return true;
}

bool AirplayStream::Channel::offer(const uint8_t* data, size_t len) {
  std::lock_guard<std::mutex> lk(mu_);
  if (lfd_ < 0) return false;
  if (conn_ < 0) try_accept();  // 持锁下 accept：本通道只有一个数据线程会走到这里
  if (conn_ < 0) {
    enqueue_pre(data, len);
    return true;
  }
  // ★d431：重连抑制窗 —— 重连 accept 起丢弃写出（本片也丢），直到 mux 线程
  //   clear_suppress()。抑制窗 ≤ 一个 mux 迭代（ms 级），丢弃量 ≤ 数帧，
  //   直播前沿语义下本就是过期数据；换取「新连接第一批字节 = PAT/PMT」。
  if (suppress_) return true;
  // 先冲预缓冲：冲不动就**整段丢弃**。陈旧数据留着没有任何价值 —— 恢复瞬间把
  // 积压的过期帧灌进 mpv 只会把播放位置顶到更早（延迟/错位的来源）。
  if (!pre_.empty()) {
    const int rc = fr_stream_write(conn_, pre_.data(), static_cast<int>(pre_.size()));
    pre_.clear();
    pre_total_ = 0;
    pre_pub_.store(0, std::memory_order_relaxed);
    if (rc == FR_STREAM_WRITE_ERROR) on_peer_closed();  // ★d312：对端已关
    if (rc == FR_STREAM_WRITE_STALL) return stall_policy();  // 预缓冲都冲不动 ⇒ 本片也丢
  }
  const int rc = fr_stream_write(conn_, data, static_cast<int>(len));
  if (rc == FR_STREAM_WRITE_OK) {
    if (stall_since_ > 0.0) {
      FR_LOG_INFO("{} [{}] 回环写恢复（停滞 {:.0f}ms，期间丢 {} 片）", kTag, tag_,
                  (mono_sec() - stall_since_) * 1000.0, stall_drops_);
      stall_since_ = 0.0;
      stall_drops_ = 0;
    }
    return true;
  }
  if (rc == FR_STREAM_WRITE_ERROR) {
    // ★d312：对端连接消亡（mpv 重载/退出/close）：关 fd、清陈旧预缓冲。
    // ★d314：**mux 线程绝不能因对端断开而退出** —— 22:13 真机现场：on_peer_closed
    //   返回 false ⇒ mux 线程 break（「复用输出失败…退出」）⇒ 没人 accept mpv
    //   重载（跳直播前沿）的新连接 ⇒ mpv probe 读阻塞 ⇒ 15s transition-timeout
    //   ⇒「恢复播放后实际未恢复」。对端断开只是「暂时无连接」，与启动窗口同
    //   语义：本片直落预缓冲，新连接接入后随 pre_ 冲出。
    on_peer_closed();
    enqueue_pre(data, len);
    return true;
  }
  return stall_policy();                          // 停滞：丢本片续跑
}

bool AirplayStream::Channel::take_reconnected() {
  std::lock_guard<std::mutex> lk(mu_);
  const bool v = reconn_flag_;
  reconn_flag_ = false;
  return v;
}

// ★d431：解除重连抑制窗（mux 线程在发出 PSI 之前调用）。
void AirplayStream::Channel::clear_suppress() {
  std::lock_guard<std::mutex> lk(mu_);
  suppress_ = false;
}

// 预缓冲插入（带上限丢最旧）—— offer 的未连接/重连窗口共用。
void AirplayStream::Channel::enqueue_pre(const uint8_t* data, size_t len) {
  if (pre_total_ + len > kPrebufferCap) {
    pre_.clear();
    pre_total_ = 0;
  }
  pre_.insert(pre_.end(), data, data + len);
  pre_total_ += len;
  pre_pub_.store(pre_total_, std::memory_order_relaxed);
}

// ★d312/d314：对端连接消亡收尾（关 fd、清陈旧预缓冲、回 accept 等重连）。
//   **不给调用方返回失败** —— mux 线程必须活下来 accept mpv 重载的新连接
//   （直播前沿恢复的正常路径）。
bool AirplayStream::Channel::on_peer_closed() {
  fr_stream_close(conn_);
  conn_ = -1;
  conn_pub_.store(-1, std::memory_order_relaxed);
  pre_.clear();
  pre_total_ = 0;
  pre_pub_.store(0, std::memory_order_relaxed);
  stall_since_ = 0.0;
  peer_closed_ = true;  // ★d316：重连成功后由 try_accept 转成 reconn_flag_
  FR_LOG_WARN("{} 回环对端已关闭，回到 accept 等重连（mpv 重载/退出）", kTag);
  return false;
}

// ————————————————————————————— AirplayStream —————————————————————————————

AirplayStream::AirplayStream(EventBus& bus, const Config& cfg) : bus_(bus), cfg_(cfg) {
  video_ch_.tag_ = "视频";
  audio_ch_.tag_ = "音频";
}

AirplayStream::~AirplayStream() { stop_streams("dtor"); }

bool AirplayStream::session_active() const {
  std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(streams_mu_));
  return session_ != Session::None;
}

bool AirplayStream::mirror_active() const {
  std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(streams_mu_));
  return session_ == Session::Mirror;
}

// —— 主线程每轮：确认"待确认纯音频"会话（见 feed_audio 开头的 d248 说明）——
// 只有音频、且在窗口期内始终没有视频 ⇒ 才算纯音频，这时才投 SetUri。
// 音频帧在这期间一直写进通道（未连接时进预缓冲），所以不会丢首帧。
void AirplayStream::tick(double now) {
  std::lock_guard<std::mutex> lk(streams_mu_);
  // d273：数据面 5s 汇总（主线程；放在会话判定**之前**——镜像/纯音频/无会话都要看，
  // 「镜像开始后 video_in 恒 0」这类断链只有汇总能暴露。队列深度在锁内读）。
  // ★d282 铁律（AppHang 根因，勿破）：本段在**主线程**跑，不得取任何会被数据面
  //   长时间持有的锁。队列深度走 q_mu_（mux 线程只瞬时持有），预缓冲量走原子镜像
  //   （prebuffer_size() 已改无锁读）。曾经的 video_ch_.prebuffer_size() 取
  //   Channel::mu_，而该锁在写阻塞（当时 send 无超时，可无限期）期间被数据线程
  //   攥着 ⇒ 主线程卡在这一行 ⇒ 消息泵停摆 ⇒ 系统判 AppHang 杀进程。
  //   以后新增任何"从数据面取数"的字段，一律走原子量，不在主线程取锁。
  if (now - st_last_at_ >= 5.0) {
    const uint64_t v_in = st_v_in_.load(), v_in_b = st_v_in_b_.load();
    const uint64_t a_in = st_a_in_.load(), a_in_b = st_a_in_b_.load();
    const uint64_t mux_v = st_mux_v_.load(), mux_a = st_mux_a_.load();
    const uint64_t out = st_out_pkts_.load(), out_b = st_out_bytes_.load();
    const uint64_t wfail = st_w_fail_.load(), dec = st_dec_drop_.load();
    const uint64_t vdrop = st_v_drops_.load(), adrop = st_a_drops_.load();
    const uint64_t brk = st_broken_.load();
    StatsSnap& s = st_snap_;
    // ★d279 延迟归属探针：生产侧积压 =（本地经过 − 轴前沿媒体时间）。
    //   ★d289：轴前沿 = 实时基线（首帧以来的单调到达钟），不再是「最近一帧视频 PTS」
    //   —— 主锚已换回音频，生产积压读数口径随之变为「数据面自身是否跟得上实时」。
    //   该值含「客户端编码 + 网络 + 入队 + TCP 写阻塞」，不含 mpv 侧；
    //   与 player 侧「接收端积压」对读即可定位延迟归属（见 player_controller 探针）。
    // 无会话时 base_mono_=0：elapsed 会等于「系统开机秒数」，纯噪声，显式归零。
    const bool has_stream = (base_mono_ > 0.0);
    const double elapsed = has_stream ? (now - base_mono_) : 0.0;
    const double front = static_cast<double>(st_axis_90k_.load()) / 90000.0;
    const double prod_backlog = (has_stream && front > 0.0) ? elapsed - front : 0.0;
    // 末尾四个实参 = ★d303 传输延迟：视频瞬时/峰值、音频瞬时/峰值（ms）
    FR_LOG_INFO(
        "{} 数据面(5s): 视频 +{}帧/+{}KB(累计{}) mux消费+{} | 音频 +{}帧/+{}KB(累计{}) "
        "mux消费+{} | TS输出 +{}包/+{}KB(累计{}) | 写失败+{} 队列丢v/a +{}/+{} 解密丢+{} "
        "muxer坏+{} | 队列v/a={}/{} 预缓v/a={}/{}B | 轴 经过={:.1f}s 前沿={:.1f}s "
        "生产积压={:.2f}s lagv={:.0f}ms laga={:.0f}ms | 对齐 视频丢帧+{} 轴纠偏+{} PTS补步+{} IDR等待丢+{} "
        "| ★传输延迟(客户端→我们) 视频 {:.0f}ms(峰{:.0f}) 音频 {:.0f}ms(峰{:.0f})",
        kTag, v_in - s.v_in, (v_in_b - s.v_in_b) / 1024, v_in, mux_v - s.mux_v,
        a_in - s.a_in, (a_in_b - s.a_in_b) / 1024, a_in, mux_a - s.mux_a,
        out - s.out, (out_b - s.out_b) / 1024, out, wfail - s.wfail,
        vdrop - s.vdrop, adrop - s.adrop, dec - s.dec, brk - s.broken,
        vq_.size(), aq_.size(), video_ch_.prebuffer_size(), audio_ch_.prebuffer_size(),
        elapsed, front, prod_backlog, video_lag_ema_ * 1000.0, audio_lag_ema_ * 1000.0,
        mux_ ? mux_->dropped_video() : 0, mux_ ? mux_->axis_corrections() : 0,
        mux_ ? mux_->video_pts_bumps() : 0,  // d311：重复帧补步进计数（累计值，同上两口径）
        st_idr_drop_.load(std::memory_order_relaxed),  // d316：重连等 IDR 丢帧（累计）
        probe_now_v_ * 1000.0, probe_transit_v_ * 1000.0,
        probe_now_a_ * 1000.0, probe_transit_a_ * 1000.0);
    s = {v_in, v_in_b, a_in, a_in_b, mux_v, mux_a, out, out_b, wfail, dec, vdrop, adrop, brk};
    st_last_at_ = now;
  }
  if (session_ != Session::AudioOnly) return;
  if (audio_uri_published_) return;
  if (audio_only_since_ <= 0.0) return;
  if (now - audio_only_since_ < kAudioOnlyConfirmSec) return;
  if (!audio_ch_.open()) return;

  char uri[64];
  std::snprintf(uri, sizeof(uri), "%s%u", kLoopbackUriPrefix, audio_ch_.port());
  DmrCommand c;
  c.type = DmrCommand::Type::SetUri;
  c.source = CastSource::AirPlay;
  c.uri = uri;
  c.title = title_;
  const char* fmt = audio_load_format(ct_);
  if (fmt) c.load_format = fmt;
  bus_.publish_dmr(c);
  audio_uri_published_ = true;
  FR_LOG_INFO("{} 音频管道就绪：{}（ct={} format={}）—— 纯音频会话（{}ms 内未见视频）", kTag,
              uri, static_cast<int>(ct_), fmt ? fmt : "auto",
              static_cast<int>(kAudioOnlyConfirmSec * 1000));
}

void AirplayStream::flush() {
  video_ch_.drop_prebuffer();
  audio_ch_.drop_prebuffer();
}

// —— 视频（lib mirror 线程）——
// d277 时间戳换算（详见 airplay_stream.h 的字段注释）：
//   远端钟可用 → 本地锚点 + (remote − remote_anchor)，精确复刻客户端节奏；
//   远端钟缺失（如音频首个 RTP sync 包之前为 0）→ 用帧到达时刻。
// 两者同为 steady_clock 域的绝对 ns，故可混用而不错轴。
uint64_t AirplayStream::stamp_ns(uint64_t remote_ns) {
  const uint64_t now = mono_ns();
  if (!remote_ns) return now;
  std::lock_guard<std::mutex> lk(clock_mu_);
  if (!clock_anchor_remote_) {
    clock_anchor_remote_ = remote_ns;
    clock_anchor_local_ = now;
  }
  if (remote_ns < clock_anchor_remote_) return clock_anchor_local_;  // 乱序防御
  return clock_anchor_local_ + (remote_ns - clock_anchor_remote_);
}

void AirplayStream::feed_video(const uint8_t* data, size_t len, uint64_t ntp_local_ns,
                               uint64_t ntp_remote_ns, const char* session_title) {
  if (!accepting_.load(std::memory_order_relaxed)) return;  // ★d435：接管收尾中
  // d277：时间轴改由远端钟（+本地锚点）产生；ntp_local_ns 仅在远端钟缺失时兜底。
  // 原实现 `(void)ntp_remote_ns` 直接丢弃客户端钟，而 lib 的 ntp_local_ns 本机恒 0
  // → 所有 PES PTS=0（mpv "Invalid video timestamp"）→ 无画面。
  ntp_local_ns = stamp_ns(ntp_remote_ns);
  // d273 诊断：到达计数（首帧打一条，之后靠 5s 汇总看速率）
  if (st_v_in_.fetch_add(1) == 0)
    FR_LOG_INFO("{} 视频首帧到达：{}B（ts={}ns remote={}）", kTag, len, ntp_local_ns,
                ntp_remote_ns);
  st_v_in_b_.fetch_add(len);
  if (g_video_dump) std::fwrite(data, 1, len, g_video_dump);  // 原始 AnnexB 真值
  std::lock_guard<std::mutex> lk(streams_mu_);
  if (session_ != Session::Mirror) {
    // ★收到视频 ⇒ 这就是镜像会话（纯音频会话不会有 video_process）。
    //   若此前已按"待确认纯音频"建了通道，这里**升级**：镜像走 TS 单流（M2b），
    //   那条音频单流通道必须关掉，否则 mpv 会同时挂着两个流（d248 实测现象）。
    const bool upgraded = (session_ == Session::AudioOnly);
    session_ = Session::Mirror;
    audio_only_since_ = 0.0;  // 取消"待确认纯音频"
    title_ = session_title ? session_title : "AirPlay 镜像";
    if (upgraded) {
      if (audio_uri_published_) {
        audio_uri_published_ = false;  // 已按纯音频投过 SetUri 的情况：重投视频 URI 接管
      }
      audio_ch_.close();  // M2b：改单 TS 流，音频通道不再需要
      video_uri_published_ = false;
      FR_LOG_WARN("{} 会话由纯音频升级为镜像（音频先到），改用单 TS 流", kTag);
    }
    if (!base_ntp_) base_ntp_ = ntp_local_ns;
    if (!base_mono_) base_mono_ = mono_sec();
  } else if (!base_ntp_) {
    base_ntp_ = ntp_local_ns;
  }
  if (ntp_local_ns && !base_ntp_) base_ntp_ = ntp_local_ns;

  if (!video_uri_published_) {
    if (!video_ch_.open()) return;
    char uri[64];
    std::snprintf(uri, sizeof(uri), "%s%u", kLoopbackUriPrefix, video_ch_.port());
    DmrCommand c;
    c.type = DmrCommand::Type::SetUri;
    c.source = CastSource::AirPlay;
    c.uri = uri;
    c.title = title_;
    c.load_format = "mpegts";
    bus_.publish_dmr(c);
    video_uri_published_ = true;
    FR_LOG_INFO("{} 视频管道就绪：{}（format=mpegts）", kTag, uri);
    mux_stop_.store(false);
    mux_ = std::make_unique<TsMuxer>();
    mux_->start(
        [this](const uint8_t* d, size_t n) {
          st_out_pkts_.fetch_add(1);       // d273 诊断：TS 输出包计数
          st_out_bytes_.fetch_add(n);
          const bool ok = video_ch_.offer(d, n);  // 正式通路
          if (!ok) st_w_fail_.fetch_add(1);       // d273：写失败（EPIPE 等）
          if (FILE* fp = ts_dump_file()) std::fwrite(d, 1, n, fp);  // 诊断旁路（默认关）
          return ok;
        },
        true, true,
        video_hevc_);  // ★d292：PMT 的 stream_type 按客户端实际编码取 0x1b/0x24
    mux_running_.store(true);
    mux_thr_ = std::thread(&AirplayStream::mux_thread_main, this);
  }

  {
    std::lock_guard<std::mutex> qlk(q_mu_);
    if (vq_.size() >= kQueueCap) {
      vq_.pop_front();
      st_v_drops_.fetch_add(1);  // d273
    }
    Frame f;
    f.data.assign(data, data + len);
    f.ntp_ns = ntp_local_ns;
    // ★d279d：recv_ns 改用**单调钟**（steady_clock）——它现在是 PES 时间轴的来源，
    //   若用 system_clock，会话期间的系统授时微调会直接把跳变写进 PTS。
    f.recv_ns = mono_ns();
    f.remote_ns = ntp_remote_ns;   // ★d303：客户端发送时刻（测传输延迟用）
    vq_.push_back(std::move(f));
  }
  q_cv_.notify_one();
}

// —— 音频（lib rtp 线程）——
void AirplayStream::feed_audio(const uint8_t* data, size_t len, uint8_t ct, uint64_t ntp_local_ns,
                               uint64_t ntp_remote_ns, const char* session_title) {
  if (!accepting_.load(std::memory_order_relaxed)) return;  // ★d435：接管收尾中
  // d277：与 feed_video 同一换算（音频远端钟由 RTP 时间戳换算；首个 RTP sync
  //   包之前为 0 → 自动退化为到达时刻轴，后续切回远端钟轴仍同域）。
  ntp_local_ns = stamp_ns(ntp_remote_ns);
  std::lock_guard<std::mutex> lk(streams_mu_);
  ct_ = ct;
  // d273 诊断：到达计数（首帧打一条，之后靠 5s 汇总看速率）
  if (st_a_in_.fetch_add(1) == 0)
    FR_LOG_INFO("{} 音频首帧到达：{}B ct={}（ts={}ns remote={}）", kTag, len,
                static_cast<int>(ct), ntp_local_ns, ntp_remote_ns);
  st_a_in_b_.fetch_add(len);
  if (session_ == Session::None) {
    // ★不要在这里就认定"纯音频会话"（d248 实测教训）：
    //   屏幕镜像时 lib 也是**音频先起、视频随后**（AirPlay 先建音频 RTP 再建镜像流），
    //   若此刻按纯音频 publish SetUri，等视频到了就成了"双发布 + 标题错成 AirPlay 音频"，
    //   mpv 被 loadfile 两次、音频那条还会被当 mp3 解。
    //   所以先记为"待确认"，音频照常进通道（未连接时进预缓冲，不会丢），
    //   由 tick() 在窗口期内没等到视频才确认成纯音频。
    session_ = Session::AudioOnly;
    audio_only_since_ = mono_sec();
    title_ = session_title ? session_title : "AirPlay 音频";
    base_ntp_ = ntp_local_ns;
    base_mono_ = mono_sec();
  }
  if (ntp_local_ns && !base_ntp_) base_ntp_ = ntp_local_ns;

  // 包装（§5.2：裸帧不可探测——AudioOnly 仍走 LOAS+latm demuxer；
  // ★d275：Mirror 会话不再包装，裸 ELD 帧直通 TS（供参走 PMT 描述符，
  //   见 ts_muxer.cpp emit_psi——LOAS+aac_latm 的容器无法供 sample_rate，
  //   ER(ELD) 输出每帧被判 invalid = 音频全灭的根因）。
  uint8_t buf[8192];
  const uint8_t* payload = data;
  size_t payload_len = len;
  if (ct == 8) {  // AAC-ELD
    if (session_ != Session::Mirror) {
      // AudioOnly：保持 LOAS 包装（latm demuxer 可探测）。
      // 注：AudioOnly 的 ELD 解码同样受 sample_rate 缺口影响（d276 处理）。
      const size_t n = latm_wrap(data, len, buf, sizeof(buf));
      if (n == 0) {
        FR_LOG_WARN("{} LATM 包装失败（帧 {}B），丢弃", kTag, len);
        return;
      }
      payload = buf;
      payload_len = n;
    } else if (g_eld_dump) {
      // 裸帧真值：[u16 le 长度][帧] 记录流（保帧边界，供事后重包验证）
      const uint8_t hdr[2] = {(uint8_t)(len & 0xFF), (uint8_t)((len >> 8) & 0xFF)};
      std::fwrite(hdr, 1, 2, g_eld_dump);
      std::fwrite(data, 1, len, g_eld_dump);
    }
  } else if (ct == 2) {  // ALAC → CAF（首包带文件头 + magic cookie，之后透传）
    if (!caf_header_sent_) {
      const size_t n = caf_.write_header(buf, sizeof(buf));
      if (n == 0) {
        FR_LOG_WARN("{} CAF 头生成失败", kTag);
        return;
      }
      payload = buf;
      payload_len = n;
      caf_header_sent_ = true;
    } else {
      const size_t n = caf_.wrap(data, len, buf, sizeof(buf));
      if (n == 0) return;
      payload = buf;
      payload_len = n;
    }
  }
  // ct == 4（AAC-LC）：透传（极少出现）

  // 纯音频会话：单通道自己 publish SetUri —— 但**只在待确认窗口过后**才发
  //（d248：镜像的音频先到时不能抢跑，见 feed_audio 开头的注释）。这里只管把
  //  音频写进通道：未连接时进预缓冲，等 tick 确认后再 publish，首帧不会丢。
  if (session_ == Session::AudioOnly) {
    if (audio_ch_.open()) {
      // ★d434：纯音频无 mux 线程，d431 重连抑制窗在此无人解除（suppress_ 只在
      //   mux_thread_main 清）——重连一次即永久哑火。纯音频是单通道裸流，没有
      //   「PSI 必须先于数据」的顺序约束，直接解除即可；代价仅重连首帧丢弃。
      audio_ch_.clear_suppress();
      if (!audio_ch_.offer(payload, payload_len)) {
        st_w_fail_.fetch_add(1);  // d273
        FR_LOG_DEBUG("{} 音频管道已断，丢弃帧", kTag);
      }
    }
    return;  // 待确认期间不投 SetUri、也不走镜像入队
  }

  if (ct == 8 && session_ == Session::Mirror) {
    // M2b 镜像：音频入队，由复用线程与视频合成单 TS 流（音频是基准主轴）。
    //（ct 非 8 的镜像会话未实测过——iOS 镜像恒 AAC-ELD；非 LATM 帧静默丢弃。）
    {
      std::lock_guard<std::mutex> qlk(q_mu_);
      if (aq_.size() >= kQueueCap) {
        aq_.pop_front();
        st_a_drops_.fetch_add(1);  // d273
      }
      Frame f;
      f.data.assign(payload, payload + payload_len);
      f.ntp_ns = ntp_local_ns;
      f.recv_ns = mono_ns();  // ★d279d：PES 时间轴来源 → 必须单调钟（见 feed_video 同注）
      f.remote_ns = ntp_remote_ns;  // ★d303：客户端发送时刻（测传输延迟用）
      aq_.push_back(std::move(f));
    }
    q_cv_.notify_one();
    return;
  }
}

// ————————————————————————————— 复用线程（M2b） —————————————————————————————

void AirplayStream::mux_thread_main() {
  // d271：base_ntp_/base_mono_ 由 lib 线程在 streams_mu_ 下写（feed_* 首帧置位），
  // mux 线程原先无锁直读是数据竞争（UB）。启动时锁内快照一次：mux 线程跑动期间
  // base 一旦非零即不再变（feed_* 仅在为 0 时补写），快照值与启动值一致。
  uint64_t base_ntp = 0;
  double base_mono = 0.0;
  {
    std::lock_guard<std::mutex> lk(streams_mu_);
    base_ntp = base_ntp_;
    base_mono = base_mono_;
  }
  // ★d279：PES 时间轴锚点 —— 「最近一帧（视频）PTS + 自那以后的墙钟增量」= 时间轴现值。
  //   ★为什么不再用裸墙钟：d277 之后**视频 PTS 锚在客户端远端钟**，而 d278 的音频轴
  //   与 PCR pacing 却锚在**到达墙钟**；真机实测两者**恒差 2.8s**（远端钟比墙钟慢 ~2.8s，
  //   音频 lag EMA 2686ms 与视频映射值互相印证）⇒ 发出去的 TS 里 a−v = **+2.799s**
  //   （真机 dump 逐 PES 实测，build/test_ts/av_offset.py）⇒ **音画错位 2.8 秒**。
  //   统一到 PES 时间轴后：PCR / 音频轴 / 视频 PTS 三者同域，音画自动对齐，且 PCR
  //   与 PTS 不再差那一截。（远端钟与墙钟的恒定差本身无害 —— mpv 从首个 PTS 起播，
  //   恒定偏移不产生额外延迟。）
  // ★d289：PES 时间轴主锚换回**音频**（契约 §5.3/§D1「音频为基准流（永不丢、
  //   不拉伸）；视频跟随、赶不上就丢」）。实现落在 TsMuxer 内（音频轴由
  //   feed_audio 推进、视频写出的 PTS 取该轴），本函数只提供**到达钟参考**。
  //   ★d288 判决的根因之一：`axis_pts_90k` 原先**只在 is_video 时更新** ⇒ 主锚
  //   是视频、音频轴被 fill_silence 按视频 PTS 拖着走 ⇒ 与契约完全相反。
  //   ★为什么这不是"推翻 d279b"：d279b「轴现值取视频 PTS、禁墙钟外推」的前提是
  //   「客户端远端钟与墙钟推进速率不等」；d284 之后两轴同用单调到达钟，前提已消失。
  uint64_t axis_prev_90k = 0;   // 上一轮 tick 的实时基线（单调钳制用，d279b）

  // ★d284：归零点改回「音/视频**共用**」= 任一支首帧到达时刻。
  //   d279d 的「各自归零」是为绕开客户端音频轴 2.6s 基准偏（那取证仍成立：PTS 只信
  //   单调到达钟），但它顺手把**两支轨的起点**也各自拉回 0 —— 而真机上镜像的音频会话
  //   是**迟到**的（iOS 只在有声音时才发音频）。实测一次 20s 静屏起投的会话：
  //     14.989 镜像开始 → 21.38:29.863 音频首帧（晚 14.87s）
  //     各自归零 ⇒ 音频 PTS 从 0 起、视频 PTS 已到 14.4s ⇒ laga 恒 =14383ms 永不收敛，
  //     且「垫音频」兜底 500ms 上限直接判「网络病态」放弃。
  //   两支轨是同一路媒体时间轴的两个 ES，共用零点后：音频首帧 PTS = 14.87s =
  //   当时的轴现值 ⇒ 音画天然对齐，晚到本身不再是错位（真静音期由 fill_silence 补）。
  uint64_t axis_base_recv_ns = 0;
  // ★d316→d317：重连后视频「丢弃对齐到 IDR」状态（mux 线程私有）。
  //   参数集缓存已升级为 AirplayStream 成员 vparams_（跨线程/跨会话重建保留）。
  bool wait_idr = false;
  while (!mux_stop_.load()) {
    Frame f;
    bool is_video = false;
    bool got = false;
    {
      std::unique_lock<std::mutex> lk(q_mu_);
      q_cv_.wait_for(lk, std::chrono::milliseconds(kMuxIdleMs),
                     [this] { return !vq_.empty() || !aq_.empty() || mux_stop_.load(); });
      if (mux_stop_.load()) break;
      // 音频优先：它是基准主轴（§5.3：视频跟随音频，音频永不丢）
      if (!aq_.empty()) {
        f = std::move(aq_.front());
        aq_.pop_front();
        is_video = false;
        got = true;
      } else if (!vq_.empty()) {
        f = std::move(vq_.front());
        vq_.pop_front();
        is_video = true;
        got = true;
      }
    }
    if (got) (is_video ? st_mux_v_ : st_mux_a_).fetch_add(1);  // d273

    // ★d303 传输延迟测量（锚点法）。位置：**取到帧之后**（此处 f 才有内容）。
    //   前提实测：远端钟与本地钟**完全同速**（视频/音频两帧的 remote 差与本地钟差
    //   都是 2.193s、偏差 0.000s）⇒ **两钟直接相减恒为 0，测不出任何东西**。
    //   正确测法：取**首帧**做固定锚点，之后每帧算
    //     transit = remote_ns − (锚点remote + 本地经过)
    //   即「客户端已经把这一帧发出去、但数据还没到我们」的延迟（传输 + 客户端排队）。
    //   音视频**分开**统计（用户诉求：可能单侧数据延迟拖垮整体）。
    if (got && f.remote_ns != 0) {
      if (probe_anchor_remote_ns_ == 0) {
        probe_anchor_remote_ns_ = f.remote_ns;
        probe_anchor_local_ns_ = f.recv_ns;
      } else {
        const int64_t expect = static_cast<int64_t>(probe_anchor_remote_ns_) +
                               static_cast<int64_t>(f.recv_ns - probe_anchor_local_ns_);
        const double transit =
            (static_cast<double>(f.remote_ns) - static_cast<double>(expect)) / 1e9;
        if (transit > -5.0 && transit < 60.0) {  // 合理范围，挡开锚点不匹配
          double& acc = is_video ? probe_transit_v_ : probe_transit_a_;
          double& now_v = is_video ? probe_now_v_ : probe_now_a_;
          now_v = transit;
          acc = transit > acc ? transit : acc * 0.98;  // 滑动峰值（用户感知最坏值）
        }
      }
    }

    // 相对轴：首帧以来的墙钟 = **实时基线**（tick 的输入）
    const uint64_t wall_90k = static_cast<uint64_t>((mono_sec() - base_mono) * 90000.0);
    // ★d289：tick() 的入参改回**实时基线**（d288 前这里是"最近一帧视频 PTS"）。
    //   理由：tick 干两件事 —— ① PCR pacing ② fill_silence 的填充目标。②必须
    //   独立于音频帧到达：真静音期可能几十秒不送音频（iOS 只在有声时开音频通道），
    //   此时轴要靠 tick 用静音帧铺满，否则 mpv 遇 ≥5s 空洞直接 reset_playback_state()
    //   （d278 定案）。若改喂音频轴，音频一停填充目标就冻结 ⇒ d278 的保障被架空。
    //   ⇒「音频为基准」= **视频写出的 PTS 取音频轴**（在TsMuxer::feed_video 内），
    //   不是把 tick 喂成音频轴。这是本条最容易搞反的一处。
    uint64_t axis_now_90k = wall_90k;
    if (axis_now_90k < axis_prev_90k) axis_now_90k = axis_prev_90k;  // tick 契约：必须单调
    axis_prev_90k = axis_now_90k;
    st_axis_90k_.store(axis_now_90k, std::memory_order_relaxed);  // d279：发布轴前沿

    // ★d431：无条件解除重连抑制窗（先于 PSI 发出）。防御性：suppress 只在
    //   try_accept 重连路径与 take_reconnected 配对置位/消费，但若出现任何
    //   意外错配，逐迭代清零可保证通道永不因抑制窗而永久哑火。
    audio_ch_.clear_suppress();
    video_ch_.clear_suppress();

    // ★d430：重连检测提升到循环顶部（d429 补救太晚的根因修复）。
    //   d317/d429 把「PSI 重发 + 参数集重注入」挂在**视频分支**（取到下一枚视频帧
    //   才执行），而镜像视频 25fps ⇒ 帧间隔 ~40ms；mpv 新 demuxer 的 16384B 探测窗
    //   在重连后 ~5ms 内即判死（d429 真机日志：23:53:23.853 接入 → .858 probed
    //   stream 0 failed → .886 才轮到视频分支补救，晚了 28ms）。另外两个定案：
    //   ① accept 起到本块为止写出的字节由 ★d431 抑制窗丢弃 ⇒ 本块的 PAT/PMT 就是
    //     新连接的第一批字节（d430 真机实证残余：单枚陈旧预缓冲帧 ~18KB 即可
    //     抢先占满 16KB 窗，probe 判死 Unknown codec）；
    //   ② 参数集 PES 必须在 PSI **之后**写出（fresh demuxer 对 PMT 前的未知 PID
    //     直接丢弃），所以先 tick() 冲 PSI（resync 标志被本次 tick 消费），再喂
    //     参数集——与 d429 的「request 后靠下一 tick 顺带」顺序相反。
    //   ★注：AirPlay 发送端没有任何重连协议（2026-10-19 用户定案）——mpv reload
    //     一旦判死走到 STOPPED，镜像就永久终止。故此处必须抢在探测窗判死之前。
    if (video_ch_.take_reconnected()) {
      wait_idr = true;
      if (mux_) {
        mux_->request_psi_resync();
        mux_->tick(axis_now_90k);  // 立即冲出 PAT/PMT（resync 标志本次 tick 消费）
        std::vector<uint8_t> vp;
        {
          std::lock_guard<std::mutex> lk(vparams_mu_);
          vp = vparams_;
        }
        // 参数集 PES 的 PTS 用「当前单调钟 − 轴基准」，与帧 PTS 同域
        // （f.recv_ns = mono_ns()，见入队处）。轴基准未定时退化为 0 —— 参数集
        // PES 无画面载荷，PTS 误差对解码无害。
        uint64_t ppts = 0;
        if (axis_base_recv_ns != 0) {
          const uint64_t now_ns = mono_ns();
          if (now_ns > axis_base_recv_ns) ppts = ns_to_90k(now_ns - axis_base_recv_ns);
        }
        if (!vp.empty()) (void)mux_->feed_video(vp.data(), vp.size(), ppts);
        FR_LOG_INFO("{} 重连：顶部立即重发 PSI + 参数集（{}B），视频对齐到下一 IDR", kTag,
                    vp.size());
      }
    }

    if (!got) {
      if (mux_) mux_->tick(axis_now_90k);
      continue;
    }

    // ★d279d：PTS 改用「到达墙钟」而非客户端远端钟（归零点见下方 d284）。
    //   取证（真机 dump，build/test_ts/ts_av_trace.py 逐 PES 实测）：
    //     00~16s：a−v 稳定 **±50ms**（对齐）；视频帧#480→#510（流 16.2s 处）
    //     音频轴**单次跳变 +2.6s**，此后恒定 2.6s，并叠加 0.58%/s 的缓慢漂移。
    //     起点对齐（音频/视频首 PES PTS 均为 0.000）。
    //   机理：lib 的音频轴有两套来源 —— 首个 RTP sync 包到达前走「兜底轴」
    //     （UxPlay raop_rtp.c:623，= 本地到达时刻），到达后切到「客户端 RTP sync 轴」
    //     （raop_rtp.c:388 rtp_time_to_client_ntp）。真机上后者与前者**基准差 2.6s**
    //     （客户端音频 NTP 域自身带偏，UxPlay 用 add_secs_1900_to_1970=true/false
    //     分两路校正，两轴基准不同源）⇒ 切换瞬间音画错位。视频轴则始终在客户端域。
    //   前 16s 之所以对齐，恰恰因为那时音频还没切到那条有偏的轴。
    //   ⇒ 正解：两条轴都改用**到达墙钟**、且**各自以首帧归零**：
    //     ① 彻底绕开客户端音频轴的 2.6s 基准偏；② 同轴同源 ⇒ 无跨钟速率漂移；
    //     ③ 到达时刻经实测能正确反映真实内容时间（兜底轴下 a−v 仅 ±50ms）。
    //   代价：网络抖动直接写进 PTS（局域网抖动小，风险可控）。
    if (!axis_base_recv_ns) axis_base_recv_ns = f.recv_ns;
    const uint64_t base_recv = axis_base_recv_ns;
    uint64_t pts = 0;
    if (f.recv_ns > base_recv) pts = ns_to_90k(f.recv_ns - base_recv);
    if (!is_video) pts += audio_shift_90k_;  // 恒定偏移兜底：垫音频
    (void)base_ntp;  // d279d：PTS 已不再用远端钟基准，留快照仅为兼容/诊断
    // ★d289：pts 现在只是**该帧自己的到达时刻**（喂进TsMuxer 后由它按音频轴对齐），
    //   不再在这里充当全流时间轴。原先 `if (is_video) axis_pts_90k = pts` 那段
    //   已删除 —— 那正是 d288 判定的根因（主锚是视频）。音频轴与视频对齐全部
    //   收敛到 TsMuxer::feed_video / feed_audio 内部，调用方只提供到达钟参考。

    // 到达偏差 EMA（差分延迟测量）：持续 >250ms → 垫音频，上限 500ms
    const double lag = (mono_sec() - base_mono) - static_cast<double>(pts) / 90000.0;
    double& ema = is_video ? video_lag_ema_ : audio_lag_ema_;
    if (ema < 0.0) ema = lag;
    else ema = ema + kLagAlpha * (lag - ema);
    if (!is_video && ema > kSkewStartSec &&
        audio_shift_90k_ < static_cast<uint64_t>(kSkewMaxSec * 90000.0)) {
      // d272：add 夹上限——原实现一次加满 (ema-250ms) 会越过 500ms 上限
      //（昨晚真机日志"垫音频至 840ms（上限 500ms）"即此）
      const uint64_t cap = static_cast<uint64_t>(kSkewMaxSec * 90000.0);
      uint64_t add = static_cast<uint64_t>((ema - kSkewStartSec) * 90000.0);
      if (add == 0) add = 9000u;
      if (audio_shift_90k_ + add > cap) add = cap - audio_shift_90k_;
      audio_shift_90k_ += add;
      FR_LOG_WARN("{} 音画恒定偏移 {:.0f}ms > 250ms，垫音频至 {}ms（上限 500ms）", kTag,
                  ema * 1000.0, audio_shift_90k_ / 90);
    }
    if (!is_video && ema > kSkewMaxSec && !shift_warned_) {
      shift_warned_ = true;
      FR_LOG_WARN("{} 音画偏移已超 500ms 兜底上限，不再垫音频（网络病态）", kTag);
    }

    bool ok = true;
    if (is_video) {
      // ★d316/d317/d429 的重连处理（参数集重注入 + PSI 重发）已上移到循环顶部
      //   （★d430，见上）—— 原地消费 take_reconnected() 必须等下一枚视频帧
      //   （25fps ⇒ ~40ms），跑不过 mpv 的 5ms 探测判死。此处仅保留 wait_idr
      //   的丢弃对齐逻辑（顶部置位，这里消费）。
      if (wait_idr) {
        const unsigned fl =
            annexb_scan(f.data.data(), f.data.size(), video_hevc_);
        if (!(fl & kAnnexbIdr)) {
          // P 帧：丢弃，但若携带参数集（流中途补发）则顺手刷新缓存
          if (fl & kAnnexbParams) {
            auto p = annexb_extract_params(f.data.data(), f.data.size(), video_hevc_);
            std::lock_guard<std::mutex> lk(vparams_mu_);
            vparams_ = std::move(p);
          }
          st_idr_drop_.fetch_add(1, std::memory_order_relaxed);
          mux_->tick(axis_now_90k);
          continue;  // 丢到 IDR：本帧不入复用器（tick 照跑，PCR 不停摆）
        }
        wait_idr = false;
        FR_LOG_INFO("{} IDR 对齐完成（等待期共丢视频 {} 帧）", kTag,
                    st_idr_drop_.load(std::memory_order_relaxed));
      } else {
        // 会话首帧 / 任何携带参数集的 AU：刷新缓存（UxPlay 把 SPS/PPS 前插在
        // 首个视频 AU；换设备/换分辨率由新参数集覆盖旧值）。
        auto p = annexb_extract_params(f.data.data(), f.data.size(), video_hevc_);
        if (!p.empty()) {
          std::lock_guard<std::mutex> lk(vparams_mu_);
          if (vparams_ != p) {
            vparams_ = std::move(p);
            FR_LOG_INFO("{} 已缓存视频参数集（{}B）", kTag, vparams_.size());
          }
        }
      }
      ok = mux_->feed_video(f.data.data(), f.data.size(), pts);
    } else {
      ok = mux_->feed_audio(f.data.data(), f.data.size(), pts);
    }
    mux_->tick(axis_now_90k);  // d279：tick 的时间基 = PES 时间轴现值（不再是裸墙钟）
    if (!ok) {
      // d273：区分「下游断开」与「复用器自身判坏」——后者要带原因
      if (mux_->broken_reason() && *mux_->broken_reason()) {
        st_broken_.fetch_add(1);
        FR_LOG_ERROR("{} 复用器判坏（{}），帧丢失，复用线程退出", kTag, mux_->broken_reason());
      } else {
        FR_LOG_WARN("{} 复用输出失败（下游断开），复用线程退出", kTag);
      }
      break;
    }
  }
  mux_running_.store(false);
}

void AirplayStream::request_stop_mux() {
  mux_stop_.store(true);
  q_cv_.notify_all();
  if (mux_thr_.joinable()) mux_thr_.join();
  if (mux_) {
    mux_->stop();
    mux_.reset();
  }
  mux_running_.store(false);
}

void AirplayStream::stop_streams(const char* why) {
  // d271：两线程域并发进入的真实风险点——lib 回调线程（conn_destroy 等）与
  // 主线程（snapshot stopped / source stop / dtor）可能在「用户停止投屏」时
  // 交叠（mpv Stopped 与 TEARDOWN 常在几百 ms 内先后到达）。无锁时并发 join
  // + 并发 mux_.reset() = double free = 堆损坏（21:36/22:08 两起 quit 期崩溃
  // 的头号嫌疑）。串行化 + 幂等。
  std::lock_guard<std::mutex> lk(stop_mu_);
  // ★d282 顺序铁律：**先 shutdown（不取锁）→ 再 join → 最后 close**。
  //   旧序（request_stop_mux 内的 join 排在 close 之前）在「mux 线程卡在 send()」
  //   时必然死锁：join 永远等不到线程退出。而本函数会被**主线程**调到
  //   （AirplaySource::check_snapshot ← airplay.tick），一旦 join 挂住就是 AppHang
  //   （2026-09-30 真机 20:20 / 20:36 两起）。
  //   shutdown 走 conn_pub_ 原子镜像、不取 mu_，所以在写阻塞时也调得动。
  //
  //   ⚠️join 的实际上界 = **写超时 250ms**，不是 shutdown：Win32 的 shutdown 不中断
  //     正在阻塞的 send（判决实验 test_shutdown_wake：8s 窗口下 shutdown 后仍等满
  //     7504ms）。shutdown 只让 mux 线程下一轮 send 立刻拿到 WSAESHUTDOWN 收尾。
  video_ch_.shutdown_conn();
  audio_ch_.shutdown_conn();
  request_stop_mux();  // 写超时(≤250ms) + shutdown ⇒ mux 线程本轮内必定退出
  video_ch_.close();   // 关 conn → 对端 EOF → mpv END_FILE → player 进 Stopped
  audio_ch_.close();
  {
    std::lock_guard<std::mutex> lk(streams_mu_);
    session_ = Session::None;
    video_uri_published_ = false;
    audio_uri_published_ = false;
    caf_header_sent_ = false;
    audio_only_since_ = 0.0;
    title_.clear();
    base_ntp_ = 0;
    base_mono_ = 0.0;
    {
      std::lock_guard<std::mutex> clk(clock_mu_);  // d277：时间轴锚点随会话清零
      clock_anchor_remote_ = 0;
      clock_anchor_local_ = 0;
    }
    audio_shift_90k_ = 0;
    video_lag_ema_ = -1.0;
    audio_lag_ema_ = -1.0;
    shift_warned_ = false;
  }
  {
    std::lock_guard<std::mutex> lk(q_mu_);
    vq_.clear();
    aq_.clear();
  }
  FR_LOG_INFO("{} 管道关闭（{}）", kTag, why ? why : "");
}

}  // namespace fr::cast
