#pragma once
// 剪贴板链接可播性探测（d74）：独立 mpv 实例「暂停态 loadfile」验证。
//
// 为什么用 mpv 当探测引擎：可播放的定义 = **mpv 真能打开**（demux 成功、
// 有轨道）——TLS / HLS / DASH / 各种封装全自动覆盖；短视频分享页落地是
// text/html，mpv 打不开 → 天然判死。pupnp 未编 TLS 不可用；引入 curl 是
// 新依赖且仍要猜 content-type，均不如 mpv 语义精确。
//
// 线程模型：内部 worker 线程持有独立 mpv 句柄（vo/ao 全 null，不碰 GL、
// 不进 DMR 状态机——探测对协议层完全不可见）。同一时刻至多一个探测在跑，
// 新提交顶掉旧探测（latest-wins）；结果入池，主线程 drain（与
// bus.drain_commands 同款模式，回调不跨线程直呼主线程）。句柄每次探测
// 即用即毁（复用会在 stop→loadfile 间残留状态，误判可播性）。
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct mpv_handle;

namespace fr {

class UrlProbe {
 public:
  struct Result {
    std::string url;
    bool playable = false;
  };

  UrlProbe() = default;
  ~UrlProbe();
  UrlProbe(const UrlProbe&) = delete;
  UrlProbe& operator=(const UrlProbe&) = delete;

  // 提交探测（latest-wins：顶掉在跑的旧探测）。URL 形式校验由调用方负责。
  void submit(std::string url);

  // 主线程每轮取结果（线程安全）。结果按完成顺序给出。
  void drain_results(std::vector<Result>& out);

  // 停 worker 并销毁 mpv 句柄（退出路径调用一次）。
  void shutdown();

 private:
  void run();
  bool ensure_handle();
  void destroy_handle();

  struct mpv_handle* mpv_ = nullptr;
  std::thread thread_;
  bool started_ = false;

  std::mutex mu_;                    // 保护以下共享字段

  // 以下由 mu_ 保护
  std::deque<std::string> pending_;  // 待探测 URL（仅取最新）
  std::vector<Result> results_;      // 结果池（主线程 drain）
  bool quit_ = false;
  uint64_t gen_ = 0;                 // 提交代数（worker 快照比对，防旧结果误报）
};

}  // namespace fr
