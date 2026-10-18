// airplay_source.cpp —— AirPlay 控制面门面（d269 M0 实装：启动序列 + 控制面）
//
// == 启动序列（顺序敏感，对照 AIRPLAY_EMBED_PLAN.md §4.2；源自 uxplay.cpp 的
//    start_raop_server 与 dnssd 段，d238 实锤，从零重写逐条保留依据注释）==
//   1. ntp_global_init()           —— Windows QPC 时基，必须最先（uxplay.cpp:2964）
//   2. dnssd_init(name, mac, ...)  —— mDNS 身份（device_id 需 MAC；net_if hwaddr）
//   3. raop_init(&cbs)             —— 回调表装配（36 字段全赋值）
//   4. raop_set_log_callback/level —— ★必须紧跟 raop_init：logger.c:111 的
//      assert 在未注入回调时任何 lib 日志即 abort
//   5. raop_init2(nohold=1, mac, keyfile=app_data_dir()/airplay.pem)
//   6. raop_set_plist ×N           —— 1920x1080@60 / maxFPS 30（h265 开 4K）
//   7. raop_set_tcp_ports / raop_set_udp_ports（0 = lib 动态分配）
//   8. raop_start_httpd → raop_set_port → raop_set_dnssd
//   9. dnssd_register_raop / dnssd_register_airplay —— mDNS 双服务上线
//
// 与 UxPlay 宿主的刻意差异（不是遗漏）：
//   · 不重启 httpd：conn_reset 只发 Stop（UxPlay 的重启大循环是 gstreamer 管道
//     重建的需要；FlashRec 是常驻接收端 + mpv loadfile replace）；
//   · 心跳失联由 tick 判定后收尾（UxPlay 依赖 gstreamer 侧超时）。
//
// M0 范围：序列 1–9 走通、mDNS 可被发现、控制面事件上抛；数据面回调
//（audio/video_process 等）空实现仅首包打点，M1 接回环流。
#include "cast/airplay/airplay_source.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "app/log.h"
#include "cast/airplay/airplay_stream.h"
#include "platform/net_if.h"
#include "platform/paths.h"

// 第三方头只出现在本文件（规则 8：只读，不改一行）。
// ★Windows 下 UxPlay 的 raop_ntp.h 用了 POSIX 的 ssize_t，MSVC 没有；
//   cast 侧只有一个文件 include lib 头，就地补 typedef 比给整个 exe 加 /FI 干净。
#ifdef _WIN32
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif
extern "C" {
#include "dnssd.h"
#include "logger.h"
#include "raop.h"  // 内含 stream.h / raop_ntp.h（audio/video_decode_struct 等）
}

namespace fr::cast {
namespace {

// 与主循环同源的单调秒（main.cpp 的 now 也是 steady_clock 的 double 秒）
double mono_sec() {
  using namespace std::chrono;
  return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

constexpr const char* kTag = "[AIR]";

// —— 日志桥：lib 的 logger 走 FR_LOG_*（与 DMR 同一份日志文件，便于对照）——
// FR_AIRPLAY_LIB_DEBUG=1 时把 lib 的 DEBUG 级也转发（默认关）：NTP/timing 握手
// 全在 DEBUG 级，"PTS 全 0 / 无声"这类问题的证据恰恰在那一层。⚠️ 打开后非常
// 吵（逐帧），只在取证时临时开。发布构建里 FR_LOG_DEBUG 是空宏 ⇒ DEBUG 走 INFO。
bool lib_debug_enabled() {
  static int cached = -1;
  if (cached < 0) {
    const char* p = std::getenv("FR_AIRPLAY_LIB_DEBUG");
    cached = (p && *p && *p != '0') ? 1 : 0;
  }
  return cached == 1;
}

void log_bridge(void* cls, int level, const char* msg) {
  (void)cls;
  if (!msg) return;
  switch (level) {
    case LOGGER_DEBUG:
      if (lib_debug_enabled()) FR_LOG_INFO("{} lib(dbg): {}", kTag, msg);
      break;
    case LOGGER_INFO:    FR_LOG_INFO("{} lib: {}", kTag, msg); break;
    case LOGGER_WARNING: FR_LOG_WARN("{} lib: {}", kTag, msg); break;
    case LOGGER_ERR:     FR_LOG_ERROR("{} lib: {}", kTag, msg); break;
    default: break;
  }
}

// —— 静态回调：cls 恒为 AirplaySource*（回调里只投事件/打点，规则 4）——

void cb_conn_init(void* cls) { static_cast<AirplaySource*>(cls)->on_conn_init(); }
void cb_conn_destroy(void* cls) { static_cast<AirplaySource*>(cls)->on_conn_destroy(); }
void cb_conn_feedback(void* cls) { static_cast<AirplaySource*>(cls)->on_conn_feedback(); }

void cb_conn_reset(void* cls, int reason) {
  static_cast<AirplaySource*>(cls)->on_conn_reset(reason);
}

void cb_video_reset(void* cls, reset_type_t type) {
  static_cast<AirplaySource*>(cls)->on_video_reset(static_cast<int>(type));
}

void cb_audio_process(void* cls, raop_ntp_t* ntp, audio_decode_struct* data) {
  (void)ntp;
  auto* self = static_cast<AirplaySource*>(cls);
  if (!data || !data->data || data->data_len <= 0) return;
  // M1：裸 AAC 帧 → airplay_stream 包装（latm/caf）→ 回环流 → mpv。
  // d277：时间戳真源是 **ntp_time_remote**（客户端钟，由 RTP 时间戳换算）；
  //   ntp_time_local 在未建立 NTP 校时（本机环境）时恒为 0，只作锚点兜底。
  self->stream().feed_audio(data->data, static_cast<size_t>(data->data_len), data->ct,
                            data->ntp_time_local, data->ntp_time_remote, "AirPlay 音频");
}

void cb_video_process(void* cls, raop_ntp_t* ntp, video_decode_struct* data) {
  (void)ntp;
  auto* self = static_cast<AirplaySource*>(cls);
  if (!data || !data->data || data->data_len <= 0) return;
  // raop_rtp_mirror.c:535 —— data[0] != 0 是解密失败标记，整帧丢弃（出花屏不如不出）。
  // d273：首条 WARN 保留，之后静默累计（5s 汇总的「解密丢」列可见频次），防刷屏。
  if (data->data[0] != 0) {
    static thread_local bool first_drop_warned = false;
    self->stream().notify_decrypt_drop();
    if (!first_drop_warned) {
      first_drop_warned = true;
      FR_LOG_WARN("{} 视频帧解密失败，开始丢弃（{}B；后续在数据面汇总看频次）", kTag,
                  data->data_len);
    }
    return;
  }
  self->stream().feed_video(data->data, static_cast<size_t>(data->data_len),
                            data->ntp_time_local, data->ntp_time_remote, "AirPlay 镜像");
}

void cb_audio_get_format(void* cls, unsigned char* ct, unsigned short* spf, bool* using_screen,
                         bool* is_media, uint64_t* audio_format) {
  static_cast<AirplaySource*>(cls)->on_audio_get_format(
      ct ? *ct : 0, spf ? *spf : 0, using_screen ? *using_screen : false,
      is_media ? *is_media : false, audio_format ? *audio_format : 0);
}

void cb_video_report_size(void* cls, float* width_source, float* height_source, float* width,
                          float* height) {
  (void)cls;
  // 透传源尺寸为输出尺寸（UxPlay 同义）：渲染侧按窗口缩放，无需虚拟定值。
  if (width && height && width_source && height_source) {
    *width = *width_source;
    *height = *height_source;
  }
}

void cb_audio_set_volume(void* cls, float volume) {
  static_cast<AirplaySource*>(cls)->on_audio_set_volume(volume);
}

double cb_audio_get_client_volume(void* cls) {
  return static_cast<AirplaySource*>(cls)->on_audio_get_client_volume();
}

void cb_video_pause(void* cls) { static_cast<AirplaySource*>(cls)->on_video_pause(); }
void cb_video_resume(void* cls) { static_cast<AirplaySource*>(cls)->on_video_resume(); }
void cb_audio_flush(void* cls) { static_cast<AirplaySource*>(cls)->stream().flush(); }
void cb_video_flush(void* cls) { static_cast<AirplaySource*>(cls)->stream().flush(); }

void cb_mirror_video_running(void* cls, bool is_running) {
  static_cast<AirplaySource*>(cls)->on_mirror_video_running(is_running);
}

void cb_report_client_request(void* cls, char* deviceid, char* model, char* name, bool* admit) {
  static_cast<AirplaySource*>(cls)->on_report_client_request(deviceid, model, name, admit);
}

void cb_display_pin(void* cls, char* pin) {
  static_cast<AirplaySource*>(cls)->on_display_pin(pin);
}

void cb_register_client(void* cls, const char* device_id, const char* pk_str, const char* name) {
  // M2：凭据持久化（client_registry JSON）。M0 记一条即可。
  (void)cls;
  FR_LOG_INFO("{} 客户端注册：id={} name={}", kTag, device_id ? device_id : "?",
              name ? name : "?");
  (void)pk_str;
}

bool cb_check_register(void* cls, const char* pk_str) {
  // M0：无持久化 ⇒ 一律"未注册"（与 UxPlay 首次配对路径一致；M2 接 registry）。
  (void)cls; (void)pk_str;
  return false;
}

const char* cb_passwd(void* cls, int* len) {
  auto* self = static_cast<AirplaySource*>(cls);
  const std::string& pw = self->password();
  // 空密码 / "pin" 模式：返回 NULL ⇒ lib 走内部 PIN 生成 + display_pin 回调；
  // 固定口令模式：返回口令字符串（lib 在回调返回后立即取用）。
  if (pw.empty() || pw == "pin") return nullptr;
  if (len) *len = static_cast<int>(pw.size());
  return pw.c_str();
}

void cb_export_dacp(void* cls, const char* active_remote, const char* dacp_id) {
  // DACP 遥控（iPhone 锁屏控制）M1 之后再接；M0 忽略。
  (void)cls; (void)active_remote; (void)dacp_id;
}

int cb_video_set_codec(void* cls, video_codec_t codec) {
  // 声明支持（h264 恒可；h265 由 features bit42 决定是否对外宣称）。M0 恒 1。
  // ★d292：记录客户端**实际选定的**编码 —— 开了 airplay.h265 后 iPhone 可能送
  //   HEVC，PMT 必须如实声明 0x24(HEVC)，否则 mpv 用 AVC 解析器解 HEVC ⇒
  //   `non-existing PPS` / demux-w/h=null ⇒ 有帧无画面。
  //   UxPlay 的 video_set_codec 在解出 SPS 后调用（h265_video 已判定）⇒ 时机可靠。
  if (cls) static_cast<AirplaySource*>(cls)->stream().set_video_hevc(codec == VIDEO_CODEC_H265);
  return 1;
}

void cb_on_video_play(void* cls, const char* location, const float start_position) {
  // M3：HLS / 视频 URL 投送（lib 已把 m3u8 改写为本地 http URL）。
  (void)cls;
  FR_LOG_INFO("{} 视频投送请求（M3 接）：{} start={}", kTag, location ? location : "?",
              static_cast<double>(start_position));
}

void cb_on_video_scrub(void* cls, const float position) {
  static_cast<AirplaySource*>(cls)->on_video_scrub(position);
}
void cb_on_video_rate(void* cls, const float rate) {
  static_cast<AirplaySource*>(cls)->on_video_rate(rate);
}
void cb_on_video_stop(void* cls) { static_cast<AirplaySource*>(cls)->on_video_stop(); }

void cb_on_video_acquire_playback_info(void* cls, playback_info_t* playback_video) {
  static_cast<AirplaySource*>(cls)->on_video_acquire_playback_info(playback_video);
}

float cb_on_video_playlist_remove(void* cls) {
  return static_cast<AirplaySource*>(cls)->on_video_playlist_remove();
}

}  // namespace

// ————————————————————————————— AirplaySource —————————————————————————————

AirplaySource::AirplaySource(EventBus& bus, const Config& cfg) : bus_(bus), cfg_(cfg) {
  stream_ = std::make_unique<AirplayStream>(bus, cfg);
}

AirplaySource::~AirplaySource() { stop(); }

bool AirplaySource::make_hw_addr(unsigned char out[6]) {
  if (fr_net_if_hwaddr(out) == 0) return true;
  // 无物理网卡 MAC（如只有 VPN）：随机 + 本地管理位（0x02），与 UxPlay find_mac
  // 的回退同义；每次重启会变（iOS 会当成新设备，可接受）。
  std::srand(static_cast<unsigned>(std::time(nullptr)));
  for (int i = 0; i < 6; ++i) out[i] = static_cast<unsigned char>(std::rand() & 0xff);
  out[0] = static_cast<unsigned char>((out[0] | 0x02) & ~0x01);
  return false;
}

bool AirplaySource::start() {
  if (running_.load(std::memory_order_relaxed)) return true;

  // —— 步骤 1：NTP 时基（Windows QPC 基线），必须最先 ——
  ntp_global_init();

  unsigned char hw[6] = {0};
  if (!make_hw_addr(hw)) {
    FR_LOG_WARN("{} 取不到物理网卡 MAC，回退随机本地管理地址（iOS 每次重启视为新设备）",
                kTag);
  }
  char mac_str[18];
  std::snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x", hw[0], hw[1], hw[2],
                hw[3], hw[4], hw[5]);

  const std::string name =
      cfg_.airplay_friendly_name.empty() ? cfg_.friendly_name : cfg_.airplay_friendly_name;
  // pin_pw（dnssd.h 注释）：0 无 / 1 屏上 PIN（客户端记住）/ 2 固定口令 /
  // 3 每次随机口令。合并逻辑：pin_auth 开关优先（屏上 PIN），其次 password：
  // "pin"=每次随机、其余非空=固定口令。
  unsigned char pin_pw = 0;
  if (cfg_.airplay_pin_auth) {
    pin_pw = 1;
  } else if (cfg_.airplay_password == "pin") {
    pin_pw = 3;
  } else if (!cfg_.airplay_password.empty()) {
    pin_pw = 2;
  }

  // —— 步骤 2：mDNS 身份 ——
  int dnssd_err = 0;
  dnssd_ = dnssd_init(name.c_str(), static_cast<int>(name.size()),
                      reinterpret_cast<const char*>(hw), 6, pin_pw, &dnssd_err);
  if (!dnssd_) {
    FR_LOG_ERROR("{} dnssd_init 失败（错误码 {}）：AirPlay 不可用，DLNA 继续运行", kTag,
                 dnssd_err);
    return false;
  }

  // —— features：照 uxplay.cpp:2071-2150 的默认集逐位设置（覆盖 dnssdint.h 默认值）——
  const int on_bits[] = {1, 2, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
                         19, 20, 21, 22, 25, 27, 28, 30};
  for (int bit : on_bits) dnssd_set_airplay_features(dnssd_, bit, 1);
  const int off_bits[] = {0, 3, 4, 8, 23, 24, 26, 29, 31};
  for (int bit : off_bits) dnssd_set_airplay_features(dnssd_, bit, 0);
  if (cfg_.airplay_hls) {  // HLS 需要 bit0/bit4（uxplay.cpp:2148-2149）
    dnssd_set_airplay_features(dnssd_, 0, 1);
    dnssd_set_airplay_features(dnssd_, 4, 1);
  }
  if (cfg_.airplay_h265) dnssd_set_airplay_features(dnssd_, 42, 1);

  // —— 步骤 3：回调表装配 + raop_init ——
  raop_callbacks_t cbs;
  std::memset(&cbs, 0, sizeof(cbs));
  cbs.cls = this;
  cbs.conn_init = cb_conn_init;
  cbs.conn_destroy = cb_conn_destroy;
  cbs.conn_feedback = cb_conn_feedback;
  cbs.conn_reset = cb_conn_reset;
  cbs.video_reset = cb_video_reset;
  cbs.audio_process = cb_audio_process;
  cbs.video_process = cb_video_process;
  cbs.audio_flush = cb_audio_flush;
  cbs.video_flush = cb_video_flush;
  cbs.video_pause = cb_video_pause;
  cbs.video_resume = cb_video_resume;
  cbs.audio_set_client_volume = cb_audio_get_client_volume;
  cbs.audio_set_volume = cb_audio_set_volume;
  cbs.audio_get_format = cb_audio_get_format;
  cbs.video_report_size = cb_video_report_size;
  cbs.audio_set_metadata = nullptr;   // M1：封面/元数据簇（空回调，lib 有判空）
  cbs.audio_set_coverart = nullptr;
  cbs.audio_stop_coverart_rendering = nullptr;
  cbs.audio_remote_control_id = nullptr;
  cbs.audio_set_progress = nullptr;
  cbs.mirror_video_running = cb_mirror_video_running;
  cbs.report_client_request = cb_report_client_request;
  cbs.display_pin = cb_display_pin;
  cbs.register_client = cb_register_client;
  cbs.check_register = cb_check_register;
  cbs.passwd = cb_passwd;
  cbs.export_dacp = cb_export_dacp;
  cbs.video_set_codec = cb_video_set_codec;
  cbs.on_video_play = cb_on_video_play;
  cbs.on_video_scrub = cb_on_video_scrub;
  cbs.on_video_rate = cb_on_video_rate;
  cbs.on_video_stop = cb_on_video_stop;
  cbs.on_video_acquire_playback_info = cb_on_video_acquire_playback_info;
  cbs.on_video_playlist_remove = cb_on_video_playlist_remove;

  raop_ = raop_init(&cbs);
  if (!raop_) {
    FR_LOG_ERROR("{} raop_init 失败", kTag);
    dnssd_destroy(dnssd_);
    dnssd_ = nullptr;
    return false;
  }
  // —— 步骤 4：日志注入，★必须紧跟 raop_init（logger.c:111 assert 铁律）——
  raop_set_log_callback(raop_, log_bridge, nullptr);
  raop_set_log_level(raop_, lib_debug_enabled() ? LOGGER_DEBUG : LOGGER_INFO);

  // —— 步骤 5：init2（nohold=1：新客户端可接管；pthreads4w 为 DLL 版，无需 attach）——
  const std::string keyfile = paths::app_data_dir() + "/airplay.pem";
  if (raop_init2(raop_, 1, mac_str, keyfile.c_str())) {
    FR_LOG_ERROR("{} raop_init2 失败（keyfile={}）", kTag, keyfile);
    raop_destroy(raop_);
    raop_ = nullptr;
    dnssd_destroy(dnssd_);
    dnssd_ = nullptr;
    return false;
  }

  // —— 步骤 6：显示参数（规划 §4.1-6）：1080p@60 / maxFPS 30；h265 开 4K ——
  raop_set_plist(raop_, "width", cfg_.airplay_h265 ? 3840 : 1920);
  raop_set_plist(raop_, "height", cfg_.airplay_h265 ? 2160 : 1080);
  raop_set_plist(raop_, "refreshRate", 60);
  raop_set_plist(raop_, "maxFPS", 30);
  raop_set_plist(raop_, "overscanned", 0);
  raop_set_plist(raop_, "clientFPSdata", 0);
  if (cfg_.airplay_hls) raop_set_plist(raop_, "hls", 1);

  // —— 步骤 7：端口（0 = lib 动态分配）。固定端口撞车自检：与 DLNA 端口相同则回退自动 ——
  unsigned short tcp[2] = {static_cast<unsigned short>(cfg_.airplay_mirror_port),
                           static_cast<unsigned short>(cfg_.airplay_rtsp_port)};
  unsigned short udp[3] = {static_cast<unsigned short>(cfg_.airplay_udp_timing_port),
                           static_cast<unsigned short>(cfg_.airplay_udp_control_port),
                           static_cast<unsigned short>(cfg_.airplay_udp_data_port)};
  if (tcp[1] != 0 && tcp[1] == cfg_.dlna_port) {
    FR_LOG_WARN("{} airplay.rtsp_port 与 dlna.port 相同（{}），回退自动分配", kTag, tcp[1]);
    tcp[1] = 0;
  }
  raop_set_tcp_ports(raop_, tcp);
  raop_set_udp_ports(raop_, udp);

  // —— 步骤 8：httpd 起监听 → 回填端口 → 绑 dnssd ——
  unsigned short port = tcp[1];
  raop_start_httpd(raop_, &port);
  FR_LOG_INFO("{} httpd 已起（port={}）", kTag, port);
  raop_set_port(raop_, port);
  raop_set_dnssd(raop_, dnssd_);

  // —— 步骤 9：mDNS 双服务上线 ——
  int rc = dnssd_register_raop(dnssd_, port);
  if (rc) FR_LOG_ERROR("{} dnssd_register_raop 失败（{}）", kTag, rc);
  rc = dnssd_register_airplay(dnssd_, port);
  if (rc) FR_LOG_ERROR("{} dnssd_register_airplay 失败（{}）", kTag, rc);

  running_.store(true, std::memory_order_relaxed);
  last_feedback_at_.store(0.0);
  watchdog_armed_.store(false);
  owned_ = false;            // ★d435：新栈尚未确认会话归属，等 SetUri 进快照
  suppress_stop_.store(false, std::memory_order_relaxed);  // ★d435：解除 Stop 抑制
  stream_->resume_feeds();   // ★d435：解除 teardown_stack 的入口暂停
  FR_LOG_INFO("{} 已启动：name=\"{}\" mac={} port={} pin_pw={} hls={} h265={}", kTag, name,
              mac_str, port, static_cast<int>(pin_pw), cfg_.airplay_hls ? 1 : 0,
              cfg_.airplay_h265 ? 1 : 0);
  return true;
}

void AirplaySource::stop() {
  if (!running_.exchange(false)) return;
  // 无会话也无妨：publish Stop 让播放侧收敛（幂等，mpv 侧空转）
  publish_stop();
  teardown_stack("source stop");
}

// ★d435：lib 侧收尾（不碰播放侧状态）——stop() 与「被其它来源接管」共用。
//   先 pause_feeds：stop_streams 复位会话后、raop_destroy 完成前若有在途 lib
//   回调，会重新建会话并投 SetUri，把刚接管的 DLNA 会话抢回去。
void AirplaySource::teardown_stack(const char* why) {
  // 先抑制 Stop 上抛：raop_destroy 触发的 conn_destroy/conn_reset 回调会
  // publish_stop，接管场景下必须挡住（见 suppress_stop_ 注释）。
  suppress_stop_.store(true, std::memory_order_relaxed);
  stream_->pause_feeds();
  stream_->stop_streams(why);
  if (raop_) {
    raop_destroy(raop_);  // 内含 httpd 停止（退出顺序 §6.4）
    raop_ = nullptr;
  }
  if (dnssd_) {
    dnssd_unregister_raop(dnssd_);  // 发 goodbye
    dnssd_unregister_airplay(dnssd_);
    dnssd_destroy(dnssd_);
    dnssd_ = nullptr;
  }
  conn_count_.store(0);
  watchdog_armed_.store(false);
  owned_ = false;
  FR_LOG_INFO("{} 已停止（{}）", kTag, why);
}

// ★d435：DLNA/本地投屏后到并已无条件接管（D6 会话互斥）⇒ 结束 AirPlay 会话。
//   绝不能走 publish_stop()——那会把刚接管的 DLNA 会话停掉。UxPlay 没有
//   接收端 → 手机的 TEARDOWN API（d321），raop_destroy 断 RTSP 是唯一手段；
//   随即 start() 重新注册，手机端退出后仍可再次 AirPlay 投屏。
void AirplaySource::preempt_for_other_source() {
  if (!running_.exchange(false)) return;
  FR_LOG_WARN("{} 其它投屏来源接管：结束 AirPlay 会话（不向播放侧发 Stop，断 RTSP 通知手机）",
              kTag);
  teardown_stack("preempted by other source");
  start();
}

void AirplaySource::tick() {
  if (!running_.load(std::memory_order_relaxed)) return;
  const double now = mono_sec();
  check_watchdog(now);
  check_snapshot();
  // d248：数据面也要 tick —— 纯音频与镜像的会话类型判定需要一点时间窗口
  //（镜像场景音频先到，见 airplay_stream::tick 的说明）。
  stream_->tick(now);
}

// —— 心跳看门狗：lib 的 conn_feedback 清零计数，超时即视为客户端失联 ——

bool AirplaySource::check_watchdog(double now_mono) {
  if (!watchdog_armed_.load(std::memory_order_relaxed)) return false;
  const double last = last_feedback_at_.load(std::memory_order_relaxed);
  if (last <= 0.0) return false;
  const double timeout = cfg_.airplay_feedback_timeout_sec > 0
                             ? static_cast<double>(cfg_.airplay_feedback_timeout_sec)
                             : 15.0;
  if (now_mono - last <= timeout) return false;

  FR_LOG_WARN("{} {}s 未收到客户端心跳，判定失联", kTag, static_cast<int>(timeout));
  publish_stop();
  stream_->stop_streams("watchdog");
  AirplayEvent e;
  e.kind = AirplayEvent::Kind::ClientDisconnected;
  bus_.publish_airplay(e);
  reset_session("watchdog");
  return true;
}

// —— snapshot 轮询：mpv 侧先 EOF（网络断）→ Stopped，这里同步收尾 lib 侧 ——
// ★d321：AirPlay 会话进入 Stopped（用户点停止/流自然结束）⇒ **整栈重启**
//   （raop_destroy 断开手机 RTSP ⇒ 手机端退出投屏；mDNS goodbye+重注册 ⇒ 接收端
//   重新出现在手机列表）。旧实现只关数据通道（stop_streams）—— 手机 RTSP 会话
//   仍活着，会继续推流并重新建立会话（真机实测「点停止手机端并未退出」的根因）。
//   UxPlay 没有「接收端→手机」的 TEARDOWN 发送 API，断连接是唯一手段。
// ★d435：另一条收尾条件 —— 快照来源已从 AirPlay 变成其它（DLNA/本地点播后到
//   并接管，D6）。旧逻辑只认 snap.source == AirPlay，来源被改写后永远不触发，
//   手机端一直不知道投屏已结束。owned_ 过滤「本会话 SetUri 尚未被播放侧消化」
//   的瞬时态（此时来源还停留在此前的 DLNA 会话）。
void AirplaySource::check_snapshot() {
  const PlaybackSnapshot snap = bus_.snapshot();
  const bool active = stream_->session_active();
  if (!active) {
    owned_ = false;
    return;
  }
  if (snap.has_session && snap.source == CastSource::AirPlay) {
    owned_ = true;
    if (snap.state == TransportState::Stopped) {
      FR_LOG_INFO("{} 播放侧已进入 Stopped，整栈重启（手机端退出投屏，接收端重新注册）", kTag);
      stop();
      start();
    }
    return;
  }
  if (owned_ && snap.has_session) preempt_for_other_source();
}

// ————————————————————————————— 会话与指令 —————————————————————————————

void AirplaySource::publish_stop() {
  // ★d435：收尾期间的 Stop 不得上抛（详见 suppress_stop_ 注释）。
  if (suppress_stop_.load(std::memory_order_relaxed)) {
    FR_LOG_DEBUG("{} 收尾中：抑制 Stop 指令上抛", kTag);
    return;
  }
  DmrCommand c;
  c.type = DmrCommand::Type::Stop;
  c.source = CastSource::AirPlay;
  bus_.publish_dmr(c);
}

void AirplaySource::publish_play() {
  DmrCommand c;
  c.type = DmrCommand::Type::Play;
  c.source = CastSource::AirPlay;
  bus_.publish_dmr(c);
}

void AirplaySource::reset_session(const char* why) {
  watchdog_armed_.store(false);
  title_.clear();
  FR_LOG_INFO("{} 会话复位（{}）", kTag, why);
}

// ————————————————————————————— lib 回调落地 —————————————————————————————

void AirplaySource::on_conn_init() {
  const int n = conn_count_.fetch_add(1) + 1;
  FR_LOG_INFO("{} 客户端连接建立（当前 {}）", kTag, n);
  watchdog_armed_.store(true);
  last_feedback_at_.store(mono_sec());
}

void AirplaySource::on_conn_destroy() {
  const int n = conn_count_.fetch_sub(1) - 1;
  FR_LOG_INFO("{} 客户端连接拆除（剩余 {}）", kTag, n);
  if (n <= 0) {
    publish_stop();
    stream_->stop_streams("conn_destroy");
    AirplayEvent e;
    e.kind = AirplayEvent::Kind::ClientDisconnected;
    bus_.publish_airplay(e);
    reset_session("conn_destroy");
  }
}

void AirplaySource::on_conn_feedback() { last_feedback_at_.store(mono_sec()); }

void AirplaySource::on_conn_reset(int reason) {
  // reason 1 = 断网；2 = HLS 源不支持（lib 侧已拒绝，给一条日志即可）
  FR_LOG_WARN("{} 连接被重置（reason={}）{}", kTag, reason, reason == 2 ? "：HLS 源不支持" : "");
  publish_stop();
  stream_->stop_streams("conn_reset");
  reset_session("conn_reset");
}

void AirplaySource::on_video_reset(int reset_type) {
  FR_LOG_INFO("{} video_reset type={}", kTag, reset_type);
  publish_stop();
  stream_->stop_streams("video_reset");
  // HLS 分支的 lib 副作用不可省（§6.2：对照 uxplay.cpp:2195-2252 的 fallthrough）
  if (raop_ && (reset_type == RESET_TYPE_HLS_SHUTDOWN || reset_type == RESET_TYPE_HLS_EOS ||
                reset_type == RESET_TYPE_RTP_TO_HLS_TEARDOWN)) {
    raop_destroy_airplay_video(raop_, -1);
    raop_remove_hls_connections(raop_);
  }
  reset_session("video_reset");
}

void AirplaySource::on_audio_get_format(unsigned char ct, unsigned short spf, bool using_screen,
                                        bool is_media, uint64_t audio_format) {
  FR_LOG_INFO("{} 音频格式：ct={} spf={} screen={} media={} format={}", kTag,
              static_cast<int>(ct), spf, using_screen ? 1 : 0, is_media ? 1 : 0, audio_format);
}

void AirplaySource::on_audio_set_volume(float volume_db) {
  // AirPlay 域 -30..0 dB；-144 视为静音（lib 语义）
  DmrCommand c;
  c.source = CastSource::AirPlay;
  if (volume_db <= -144.0f) {
    c.type = DmrCommand::Type::SetMute;
    c.mute = true;
  } else {
    c.type = DmrCommand::Type::SetVolume;
    c.volume = static_cast<int>((volume_db + 30.0f) / 30.0f * 100.0f);
    if (c.volume < 0) c.volume = 0;
    if (c.volume > 100) c.volume = 100;
  }
  bus_.publish_dmr(c);
}

double AirplaySource::on_audio_get_client_volume() {
  return bus_.snapshot().volume / 100.0;
}

void AirplaySource::on_video_pause() {
  DmrCommand c;
  c.type = DmrCommand::Type::Pause;
  c.source = CastSource::AirPlay;
  bus_.publish_dmr(c);
}

void AirplaySource::on_video_resume() { publish_play(); }

void AirplaySource::on_mirror_video_running(bool running) {
  AirplayEvent e;
  e.kind = running ? AirplayEvent::Kind::MirrorStarted : AirplayEvent::Kind::MirrorStopped;
  bus_.publish_airplay(e);
  FR_LOG_INFO("{} 镜像{}", kTag, running ? "开始" : "结束");
}

void AirplaySource::on_display_pin(const char* pin) {
  AirplayEvent e;
  e.kind = AirplayEvent::Kind::Pin;
  if (pin) std::snprintf(e.pin, sizeof(e.pin), "%s", pin);
  bus_.publish_airplay(e);
  FR_LOG_INFO("{} 配对码：{}（请在设备上输入）", kTag, pin ? pin : "?");
}

void AirplaySource::on_report_client_request(const char* deviceid, const char* model,
                                             const char* name, bool* admit) {
  // M0 恒放行（单客户端语义由 lib 的 nohold 兜底）；M2 可在此做黑名单。
  if (admit) *admit = true;
  if (name) title_ = name;
  FR_LOG_INFO("{} 客户端请求接入：name={} model={} deviceid={} → 放行", kTag,
              name ? name : "?", model ? model : "?", deviceid ? deviceid : "?");
}

// ————————————————————————————— M3：HLS 控制面 —————————————————————————————

void AirplaySource::on_video_scrub(float position) {
  DmrCommand c;
  c.type = DmrCommand::Type::Seek;
  c.source = CastSource::AirPlay;
  c.seek_to = position;
  bus_.publish_dmr(c);
}

void AirplaySource::on_video_rate(float rate) {
  if (rate <= 0.0f) {
    on_video_pause();
    return;
  }
  DmrCommand c;
  c.type = DmrCommand::Type::SetSpeed;
  c.source = CastSource::AirPlay;
  c.speed = rate;
  bus_.publish_dmr(c);
  publish_play();
}

void AirplaySource::on_video_stop() {
  publish_stop();
  reset_session("on_video_stop");
}

void AirplaySource::on_video_acquire_playback_info(void* info) {
  if (!info) return;
  auto* p = static_cast<playback_info_t*>(info);
  const PlaybackSnapshot snap = bus_.snapshot();
  p->duration = snap.duration;
  p->position = snap.position;
  p->seek_start = 0.0;
  p->seek_duration = snap.duration;
  p->rate = (snap.state == TransportState::Playing) ? 1.0f : 0.0f;
  p->ready_to_play = true;
  p->playback_likely_to_keep_up = true;
  // lib 语义：stopped_at > 0 时 duration = -1 表示会话已结束（http_handler_playback_info）
  if (snap.stopped_at > 0.0) p->duration = -1.0;
}

float AirplaySource::on_video_playlist_remove() {
  return static_cast<float>(bus_.snapshot().position);
}

}  // namespace fr::cast
