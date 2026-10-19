#pragma once
// 更新服务：唯一工作线程 + 状态快照 + 启动/4h 调度。
// 硬约束（d454）：网络、清单解析、SHA-256、磁盘检查与写盘全部在本线程；
// 主线程只做 start/stop、O(1) 快照复制、request_check/request_download 投递。
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "app/config.h"
#include "platform/update_install.h"
#include "update/detector.h"
#include "update/downloader.h"
#include "update/endpoint_policy.h"

namespace fr::update {

enum class UpdateState : int {
  Idle = 0,
  Checking,
  UpToDate,
  Downloading,
  Ready,
  ManualOnly,
  Backoff,
  Skipped,
  Installing,
};

enum class UpdateError : int {
  None = 0,
  Network,
  Verify,
  Disk,
  Permission,
  Install,
  Unknown,
};

struct UpdateSnapshot {
  UpdateState state = UpdateState::Idle;
  UpdateError error = UpdateError::None;
  bool atom = false;              // 仅 Atom 版本信息，只能打开发布页
  bool download_available = false;  // 有清单资产，可手动触发下载
  bool last_check_manual = false;
  char current_version[32] = {};
  char latest_version[32] = {};
  char release_url[512] = {};
  char notes[608] = {};
  char asset_name[128] = {};
  char asset_path[520] = {};
  long long asset_size = 0;
  long long downloaded = 0;
  long long total = 0;
  double bytes_per_sec = 0.0;
  long long last_check_at = 0;  // wall epoch
  long long next_check_at = 0;  // wall epoch；0 = 未排期
  int fail_count = 0;
  int progress_percent = -1;
  long long check_seq = 0;  // 每完成一次检查 +1（UI 用来识别“本次手动检查已结束”）
};

static_assert(std::is_trivially_copyable<UpdateSnapshot>::value,
              "UpdateSnapshot 必须可平凡复制：主线程只做一次结构体复制");

using DownloadFn = std::function<DownloadResult(
    const ReleaseManifest&, const ReleaseAsset&, const std::vector<RankedEndpoint>&,
    const DownloadDeps&, const std::atomic<bool>*)>;

struct ServiceDeps {
  BatchFetch fetch;   // 空 = http_get_many
  DownloadFn download;  // 空 = download_asset
  std::function<long long()> wall_now;  // 空 = time(nullptr)，单测注入
  std::function<fr::platform::InstallShape()> detect_shape;  // 空 = detect_install_shape()
  std::string cache_dir;  // 空 = paths::update_cache_dir()；单测注入临时目录
};

class UpdateService {
 public:
  UpdateService();
  explicit UpdateService(ServiceDeps deps);
  ~UpdateService();
  UpdateService(const UpdateService&) = delete;
  UpdateService& operator=(const UpdateService&) = delete;

  // 主线程调用；线程启动后立即返回。
  void start(const UpdateConfig& cfg, std::string current_version, std::string config_path);
  // 主线程退出路径调用：取消网络/下载并 join（不含任何 I/O 等待以外的阻塞）。
  void stop();

  // 非阻塞投递；检查/下载进行中自动合并，不并发。
  void request_check(bool manual);
  // auto_download=false 时用户确认下载（ManualOnly + download_available）。
  void request_download();
  // 用户点「跳过此版本」：工作线程落盘 skipped_version 并清理该版本缓存。
  void skip_current_version();
  // 配置热更新（下次检查生效；skipped_version 变化由工作线程落盘）。
  void apply_config(const UpdateConfig& cfg);

  UpdateSnapshot snapshot() const;

 private:
  enum class Action { None, Check, Download, Skip, Stopped };

  Action wait_for_action(bool* manual);
  void worker_main();
  void run_check(bool manual);
  void run_pending_download();
  void run_skip();
  void perform_download();
  void schedule_after(bool success, long long wall_now_sec);
  void set_state(UpdateState state, UpdateError error);
  void update_manifest_fields(const DetectionResult& result);
  void update_download_fields(long long downloaded, long long total, double bps);
  void persist_runtime(long long last_check_at, long long next_retry_at, int fail_count);
  void persist_skipped(const std::string& version);
  long long wall_now() const;
  bool auto_enabled() const;

  ServiceDeps deps_;
  UpdateConfig cfg_;
  std::string current_version_;
  std::string config_path_;
  fr::platform::InstallShape shape_ = fr::platform::InstallShape::Unknown;
  EndpointPolicy policy_;

  mutable std::mutex snap_mu_;
  UpdateSnapshot snap_;

  std::mutex wait_mu_;
  std::condition_variable cv_;
  std::thread worker_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> cancel_{false};
  // 自动检查进行中用户又点「手动检查」：不重复发请求，但强制本次绕过测速缓存/冷却。
  std::atomic<bool> manual_upgrade_{false};
  bool check_requested_ = false;
  bool manual_pending_ = false;
  bool download_requested_ = false;
  bool skip_requested_ = false;
  bool config_pending_ = false;
  bool had_manual_request_ = false;
  std::chrono::steady_clock::time_point last_manual_request_{};
  UpdateConfig pending_cfg_;

  // —— 以下仅工作线程访问 ——
  std::chrono::system_clock::time_point next_due_{};
  DetectionResult pending_update_;
  bool has_pending_update_ = false;
  DetectionResult cached_detection_;
  std::chrono::steady_clock::time_point cached_at_{};
  bool cache_valid_ = false;
  long long check_seq_ = 0;
};

}  // namespace fr::update
