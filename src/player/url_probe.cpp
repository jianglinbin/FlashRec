#include "player/url_probe.h"

#include <condition_variable>

#include "app/log.h"

// mpv API 只准出现在 src/player/（AGENTS.md 规则 4）。
#include <mpv/client.h>

namespace fr {

namespace {
// 探测上限：HLS 主/媒体 playlist 慢源留足余量；超时判不可播（静默，不打扰）。
constexpr double kProbeTimeoutSec = 12.0;
// 每次 mpv_wait_event 的步长：足够小让「顶掉/退出」请求及时生效。
constexpr double kEventWaitSec = 0.3;
}  // namespace

UrlProbe::~UrlProbe() { shutdown(); }

void UrlProbe::submit(std::string url) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    pending_.clear();          // latest-wins：旧探测作废（worker 比对 gen 自行放弃）
    pending_.push_back(std::move(url));
    ++gen_;
    if (!started_) {
      started_ = true;
      thread_ = std::thread([this] { run(); });
    }
  }
}

void UrlProbe::drain_results(std::vector<Result>& out) {
  std::lock_guard<std::mutex> lk(mu_);
  out.swap(results_);
}

void UrlProbe::destroy_handle() {
  if (mpv_) {
    mpv_destroy(mpv_);  // 探测句柄无渲染上下文，直接销毁
    mpv_ = nullptr;
  }
}

void UrlProbe::shutdown() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    quit_ = true;
    ++gen_;
  }
  if (thread_.joinable()) thread_.join();
  destroy_handle();
}

bool UrlProbe::ensure_handle() {
  // 每次探测全新句柄（d74b）：复用句柄在 stop→loadfile 间有状态残留，
  // 实测同 URL 第二次探测 24ms 即 END_FILE 误判不可播（第三次又正常）。
  // 探测频率 = 用户复制频率，create/destroy 开销可忽略，换确定性。
  destroy_handle();
  mpv_ = mpv_create();
  if (!mpv_) return false;
  // 纯探测实例：无输出、无用户配置、无 ytdl（分享页脚本直接禁——它若能
  // 解析出来的链接我们本来也会走 mpv 正式管线重开，探测只要 demux 结论）。
  mpv_set_option_string(mpv_, "config", "no");
  mpv_set_option_string(mpv_, "terminal", "no");
  mpv_set_option_string(mpv_, "vo", "null");
  mpv_set_option_string(mpv_, "ao", "null");
  mpv_set_option_string(mpv_, "ytdl", "no");
  mpv_set_option_string(mpv_, "pause", "yes");  // 探测态：loaded 即停，不出声不出帧
  if (mpv_initialize(mpv_) < 0) {
    FR_LOG_WARN("[CLIP] 探测 mpv 实例初始化失败");
    mpv_destroy(mpv_);
    mpv_ = nullptr;
    return false;
  }
  return true;
}

void UrlProbe::run() {
  std::unique_lock<std::mutex> lk(mu_);
  for (;;) {
    if (quit_) return;
    if (pending_.empty()) {
      mu_.unlock();
      // 轻量等待：无 condvar 唤醒的退化轮询（探测频率 = 用户复制频率，量级极低）
      std::this_thread::sleep_for(std::chrono::milliseconds(120));
      mu_.lock();
      continue;
    }
    const std::string url = std::move(pending_.back());
    pending_.clear();
    const uint64_t mygen = gen_;
    mu_.unlock();

    bool playable = false;
    if (ensure_handle()) {
      // argv 数组式命令：URL 原样透传，不经任何转义/解析（mpv_command_string
      // 的 CLI 风格解析会被引号/特殊字符打穿——与 R1「URI 原样透传」同纪律）。
      const char* cmd[] = {"loadfile", url.c_str(), "replace", nullptr};
      mpv_command(mpv_, cmd);
      const double deadline = mpv_get_time_us(mpv_) / 1e6 + kProbeTimeoutSec;
      for (;;) {
        const mpv_event* ev = mpv_wait_event(mpv_, kEventWaitSec);
        {  // 顶掉/退出检查（每步一次；命中则中止本次探测且不产结果）
          std::lock_guard<std::mutex> chk(mu_);
          if (quit_ || gen_ != mygen) break;
        }
        if (!ev) continue;
        if (ev->event_id == MPV_EVENT_FILE_LOADED) {
          playable = true;  // demux 成功 = mpv 真能打开（探测的唯一定义）
          break;
        }
        if (ev->event_id == MPV_EVENT_END_FILE) break;  // 解析失败/网络失败
        if (ev->event_id == MPV_EVENT_SHUTDOWN) break;
        if (mpv_get_time_us(mpv_) / 1e6 > deadline) break;
      }
    }
    destroy_handle();  // 每次探测即用即毁（见 ensure_handle 注）

    {
      std::lock_guard<std::mutex> chk(mu_);
      const bool superseded = quit_ || gen_ != mygen;
      if (!superseded) results_.push_back({url, playable});
    }
    mu_.lock();
  }
}

}  // namespace fr
