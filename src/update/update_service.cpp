#include "update/update_service.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <utility>

#include "app/log.h"
#include "update/release_info.h"
#include "update/version.h"

namespace fr::update {
namespace {

constexpr auto kManualDebounce = std::chrono::seconds(30);
constexpr auto kWallWakeCap = std::chrono::seconds(60);
constexpr long long kCheckBudgetMs = 45000;

long long backoff_seconds(int fail_count) {
  if (fail_count <= 1) return 15 * 60;
  if (fail_count == 2) return 60 * 60;
  if (fail_count == 3) return 6 * 60 * 60;
  return 24 * 60 * 60;
}

std::string endpoint_order_text(const std::vector<RankedEndpoint>& endpoints) {
  std::string line;
  for (const RankedEndpoint& ranked : endpoints) {
    char buf[192];
    if (ranked.metrics.bytes_per_sec > 0) {
      std::snprintf(buf, sizeof(buf), "%s%s (%.1f KB/s)", line.empty() ? "" : " → ",
                    ranked.endpoint.id.c_str(), ranked.metrics.bytes_per_sec / 1024.0);
    } else {
      std::snprintf(buf, sizeof(buf), "%s%s (未测速)", line.empty() ? "" : " → ",
                    ranked.endpoint.id.c_str());
    }
    line += buf;
  }
  return line.empty() ? "无" : line;
}

template <size_t N>
void copy_fixed(char (&dst)[N], const std::string& src) {
  const size_t n = std::min(src.size(), N - 1);
  if (n > 0) std::memcpy(dst, src.data(), n);
  std::memset(dst + n, 0, N - n);
}

template <size_t N>
void copy_fixed_utf8(char (&dst)[N], const std::string& src) {
  std::string tmp = src;
  truncate_utf8(&tmp, N - 1);
  copy_fixed(dst, tmp);
}

const ReleaseAsset* select_asset(const ReleaseManifest& manifest,
                                 fr::platform::InstallShape shape) {
  const ReleaseAsset* asset = nullptr;
  switch (shape) {
    case fr::platform::InstallShape::WindowsSetup: asset = &manifest.windows_setup; break;
    case fr::platform::InstallShape::WindowsPortable: asset = &manifest.windows_zip; break;
    case fr::platform::InstallShape::LinuxAppImage: asset = &manifest.linux_appimage; break;
    case fr::platform::InstallShape::LinuxDeb: asset = &manifest.linux_deb; break;
    default: return nullptr;
  }
  return asset->valid ? asset : nullptr;
}

UpdateError download_error(DownloadStatus status) {
  switch (status) {
    case DownloadStatus::DiskFull:
    case DownloadStatus::IoError: return UpdateError::Disk;
    case DownloadStatus::HashMismatch:
    case DownloadStatus::InvalidManifest: return UpdateError::Verify;
    case DownloadStatus::NetworkFailed:
    case DownloadStatus::NoAsset: return UpdateError::Network;
    default: return UpdateError::Unknown;
  }
}

std::string json_quote(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  out.push_back('"');
  return out;
}

}  // namespace

UpdateService::UpdateService() = default;
UpdateService::UpdateService(ServiceDeps deps) : deps_(std::move(deps)) {}
UpdateService::~UpdateService() { stop(); }

void UpdateService::start(const UpdateConfig& cfg, std::string current_version,
                          std::string config_path) {
  if (worker_.joinable()) return;
  std::lock_guard<std::mutex> lk(wait_mu_);
  cfg_ = cfg;
  current_version_ = std::move(current_version);
  config_path_ = std::move(config_path);
  config_pending_ = false;
  check_requested_ = false;
  manual_pending_ = false;
  download_requested_ = false;
  had_manual_request_ = false;
  manual_upgrade_.store(false);
  stop_.store(false);
  cancel_.store(false);

  const bool auto_on = cfg_.enabled && cfg_.auto_check;
  const int delay_sec = std::max(0, cfg_.startup_delay_sec);
  next_due_ = auto_on
                  ? std::chrono::system_clock::now() + std::chrono::seconds(delay_sec)
                  : std::chrono::system_clock::time_point::max();
  {
    std::lock_guard<std::mutex> slk(snap_mu_);
    snap_ = UpdateSnapshot{};
    copy_fixed(snap_.current_version, current_version_);
    snap_.last_check_at = cfg_.last_check_at;
    snap_.fail_count = cfg_.fail_count;
    snap_.next_check_at = auto_on ? wall_now() + delay_sec : 0;
  }
  worker_ = std::thread(&UpdateService::worker_main, this);
}

void UpdateService::stop() {
  {
    std::lock_guard<std::mutex> lk(wait_mu_);
    if (!worker_.joinable()) return;
    stop_.store(true);
    cancel_.store(true);
    check_requested_ = false;
    download_requested_ = false;
  }
  cv_.notify_all();
  worker_.join();
  cancel_.store(false);
  std::lock_guard<std::mutex> lk(snap_mu_);
  if (snap_.state == UpdateState::Checking || snap_.state == UpdateState::Downloading)
    snap_.state = UpdateState::Idle;
  snap_.next_check_at = 0;
}

void UpdateService::request_check(bool manual) {
  if (stop_.load() || !worker_.joinable()) return;

  UpdateState state;
  {
    std::lock_guard<std::mutex> lk(snap_mu_);
    state = snap_.state;
  }
  // 已就绪/正在下载时不再检查：避免把待安装包覆盖成“已是最新”。
  if (state == UpdateState::Downloading || state == UpdateState::Installing ||
      state == UpdateState::Ready)
    return;

  if (manual) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(wait_mu_);
    if (had_manual_request_ && now - last_manual_request_ < kManualDebounce) return;
    had_manual_request_ = true;
    last_manual_request_ = now;
  }

  bool merge_into_running = false;
  {
    std::lock_guard<std::mutex> lk(wait_mu_);
    if (manual && state == UpdateState::Checking) {
      merge_into_running = true;
      manual_upgrade_.store(true);
    } else {
      check_requested_ = true;
      manual_pending_ = manual_pending_ || manual;
    }
  }
  if (merge_into_running) {
    std::lock_guard<std::mutex> lk(snap_mu_);
    snap_.last_check_manual = true;
    return;
  }
  cv_.notify_all();
}

void UpdateService::request_download() {
  if (stop_.load() || !worker_.joinable()) return;
  {
    std::lock_guard<std::mutex> lk(snap_mu_);
    if (!snap_.download_available) return;
    if (snap_.state == UpdateState::Downloading || snap_.state == UpdateState::Installing ||
        snap_.state == UpdateState::Ready)
      return;
  }
  {
    std::lock_guard<std::mutex> lk(wait_mu_);
    download_requested_ = true;
  }
  cv_.notify_all();
}

void UpdateService::skip_current_version() {
  if (stop_.load() || !worker_.joinable()) return;
  {
    std::lock_guard<std::mutex> lk(wait_mu_);
    skip_requested_ = true;
  }
  cv_.notify_all();
}

void UpdateService::apply_config(const UpdateConfig& cfg) {
  {
    std::lock_guard<std::mutex> lk(wait_mu_);
    pending_cfg_ = cfg;
    config_pending_ = true;
  }
  cv_.notify_all();
}

UpdateSnapshot UpdateService::snapshot() const {
  std::lock_guard<std::mutex> lk(snap_mu_);
  return snap_;
}

UpdateService::Action UpdateService::wait_for_action(bool* manual) {
  std::unique_lock<std::mutex> lk(wait_mu_);
  for (;;) {
    if (stop_.load(std::memory_order_relaxed)) return Action::Stopped;

    if (config_pending_) {
      const bool was_auto = auto_enabled();
      const std::string old_skipped = cfg_.skipped_version;
      UpdateConfig next = pending_cfg_;
      config_pending_ = false;
      // 运行时字段归工作线程所有（主线程的配置副本可能是旧的），热更新不得覆盖。
      next.last_check_at = cfg_.last_check_at;
      next.next_retry_at = cfg_.next_retry_at;
      next.fail_count = cfg_.fail_count;
      cfg_ = std::move(next);
      policy_.configure(cfg_);

      const auto now_wall = std::chrono::system_clock::now();
      const auto interval = std::chrono::hours(std::max(1, cfg_.check_interval_hours));
      if (!auto_enabled()) {
        next_due_ = std::chrono::system_clock::time_point::max();
      } else if (!was_auto || next_due_ == std::chrono::system_clock::time_point::max() ||
                 next_due_ > now_wall + interval) {
        next_due_ = now_wall + interval;
      }

      const bool skip_changed = cfg_.skipped_version != old_skipped;
      const std::string skipped = cfg_.skipped_version;
      lk.unlock();
      if (skip_changed) persist_skipped(skipped);
      lk.lock();
      continue;
    }

    if (skip_requested_) {
      skip_requested_ = false;
      return Action::Skip;
    }
    if (download_requested_) {
      download_requested_ = false;
      return Action::Download;
    }
    if (check_requested_) {
      check_requested_ = false;
      *manual = manual_pending_;
      manual_pending_ = false;
      return Action::Check;
    }

    const auto now_wall = std::chrono::system_clock::now();
    if (auto_enabled() && next_due_ != std::chrono::system_clock::time_point::max()) {
      if (now_wall >= next_due_) {
        UpdateState state;
        {
          std::lock_guard<std::mutex> slk(snap_mu_);
          state = snap_.state;
        }
        // 就绪包等待用户处理期间不做自动检查（下一次检查顺延一个间隔）。
        if (state == UpdateState::Ready || state == UpdateState::Installing) {
          next_due_ = now_wall + std::chrono::hours(std::max(1, cfg_.check_interval_hours));
          continue;
        }
        return Action::Check;
      }
      auto wait = std::min(
          std::chrono::duration_cast<std::chrono::system_clock::duration>(next_due_ - now_wall),
          std::chrono::duration_cast<std::chrono::system_clock::duration>(kWallWakeCap));
      if (wait <= std::chrono::system_clock::duration::zero())
        wait = std::chrono::milliseconds(1);
      cv_.wait_for(lk, wait, [&] {
        return stop_.load(std::memory_order_relaxed) || check_requested_ || download_requested_ ||
               config_pending_ || skip_requested_;
      });
    } else {
      cv_.wait(lk, [&] {
        return stop_.load(std::memory_order_relaxed) || check_requested_ || download_requested_ ||
               config_pending_ || skip_requested_;
      });
    }
  }
}

void UpdateService::worker_main() {
  shape_ = deps_.detect_shape ? deps_.detect_shape() : fr::platform::detect_install_shape();
  policy_.configure(cfg_);

  SemVer current;
  if (!parse_semver(current_version_, &current)) {
    FR_LOG_WARN("[UPD] 当前版本号非法（{}），自动更新停用", current_version_);
    cfg_.enabled = false;
    std::lock_guard<std::mutex> lk(snap_mu_);
    snap_.next_check_at = 0;
  } else {
    FR_LOG_INFO("[UPD] 工作线程启动：当前 {}，形态 {}，间隔 {}h，启动延迟 {}s",
                current_version_, fr::platform::install_shape_name(shape_),
                cfg_.check_interval_hours, cfg_.startup_delay_sec);
  }

  for (;;) {
    bool manual = false;
    const Action action = wait_for_action(&manual);
    if (action == Action::Stopped || stop_.load()) break;
    if (action == Action::Check)
      run_check(manual);
    else if (action == Action::Download)
      run_pending_download();
    else if (action == Action::Skip)
      run_skip();
  }
  FR_LOG_INFO("[UPD] 工作线程退出");
}

void UpdateService::run_check(bool manual) {
  if (stop_.load()) return;
  if (manual_upgrade_.exchange(false)) manual = true;

  {
    std::lock_guard<std::mutex> lk(snap_mu_);
    snap_.state = UpdateState::Checking;
    snap_.error = UpdateError::None;
    snap_.last_check_manual = manual;
  }

  const long long now = wall_now();
  if (!cfg_.enabled) {
    std::lock_guard<std::mutex> lk(snap_mu_);
    snap_.state = UpdateState::Idle;
    snap_.error = UpdateError::None;
    snap_.download_available = false;
    snap_.last_check_at = now;
    snap_.check_seq = ++check_seq_;
    return;
  }

  const auto started = std::chrono::steady_clock::now();
  DetectionResult result;
  bool used_cache = false;
  if (!manual && cfg_.probe.cache_sec > 0 && cache_valid_) {
    const auto age =
        std::chrono::duration_cast<std::chrono::seconds>(started - cached_at_).count();
    if (age >= 0 && age <= cfg_.probe.cache_sec) {
      result = cached_detection_;
      used_cache = true;
      FR_LOG_INFO("[UPD] 复用 {} 秒内的检测结果（自动检查）", age);
    }
  }
  if (!used_cache) {
    DetectionDeps dd;
    dd.fetch = deps_.fetch;
    dd.manual = manual;
    dd.budget_ms = kCheckBudgetMs;
    FR_LOG_INFO("[UPD] 开始{}检查（候选端点 {} 个）", manual ? "手动" : "自动",
                policy_.endpoints().size());
    result = detect_update(cfg_, current_version_, &policy_, &cancel_, dd);
    if (stop_.load()) return;
    cached_detection_ = result;
    cached_at_ = started;
    cache_valid_ = result.outcome != DetectionOutcome::Error;
  }
  manual_upgrade_.store(false);

  cfg_.last_check_at = now;
  {
    std::lock_guard<std::mutex> lk(snap_mu_);
    snap_.last_check_at = now;
    snap_.last_check_manual = manual;
    snap_.check_seq = ++check_seq_;
  }

  switch (result.outcome) {
    case DetectionOutcome::Error: {
      ++cfg_.fail_count;
      // 保留上一次已知的更新信息与待下载资产：网络失败不该把用户可重试的包抹掉。
      {
        std::lock_guard<std::mutex> lk(snap_mu_);
        snap_.state = UpdateState::Backoff;
        snap_.error = UpdateError::Network;
        snap_.fail_count = cfg_.fail_count;
      }
      schedule_after(false, now);
      persist_runtime(cfg_.last_check_at, cfg_.next_retry_at, cfg_.fail_count);
      FR_LOG_WARN("[UPD] 检查失败（连续 {} 次）：{}", cfg_.fail_count, result.error);
      break;
    }
    case DetectionOutcome::UpToDate: {
      cfg_.fail_count = 0;
      update_manifest_fields(result);
      has_pending_update_ = false;
      {
        std::lock_guard<std::mutex> lk(snap_mu_);
        snap_.state = UpdateState::UpToDate;
        snap_.error = UpdateError::None;
        snap_.download_available = false;
        snap_.fail_count = 0;
      }
      schedule_after(true, now);
      persist_runtime(cfg_.last_check_at, 0, 0);
      FR_LOG_INFO("[UPD] 已是最新（远端 {}）", result.latest_version);
      break;
    }
    case DetectionOutcome::Available: {
      cfg_.fail_count = 0;
      update_manifest_fields(result);
      if (result.api_fallback)
        FR_LOG_INFO("[UPD] 未取得 latest.json，已用 API 资产 + digest/SHA256SUMS 合成清单（{}）",
                    result.latest_version);
      const bool skipped =
          !cfg_.skipped_version.empty() && result.latest_version == cfg_.skipped_version;
      const ReleaseAsset* asset = select_asset(result.manifest, shape_);
      {
        std::lock_guard<std::mutex> lk(snap_mu_);
        snap_.fail_count = 0;
        snap_.download_available = asset != nullptr && !skipped;
        snap_.asset_size = asset != nullptr ? asset->size : 0;
        if (asset != nullptr) copy_fixed(snap_.asset_name, asset->name);
      }
      if (skipped) {
        has_pending_update_ = false;
        set_state(UpdateState::Skipped, UpdateError::None);
        schedule_after(true, now);
        persist_runtime(cfg_.last_check_at, 0, 0);
        FR_LOG_INFO("[UPD] {} 已被用户跳过", result.latest_version);
        break;
      }
      if (asset == nullptr) {
        has_pending_update_ = false;
        set_state(UpdateState::ManualOnly, UpdateError::None);
        schedule_after(true, now);
        persist_runtime(cfg_.last_check_at, 0, 0);
        FR_LOG_INFO("[UPD] 发现 {}，但当前形态无可用资产，只提示发布页",
                    result.latest_version);
        break;
      }
      pending_update_ = result;
      has_pending_update_ = true;
      if (cfg_.auto_download) {
        perform_download();
      } else {
        set_state(UpdateState::ManualOnly, UpdateError::None);
        schedule_after(true, now);
        persist_runtime(cfg_.last_check_at, 0, 0);
        FR_LOG_INFO("[UPD] 发现 {}，等待用户确认下载", result.latest_version);
      }
      break;
    }
    case DetectionOutcome::NotifyOnly: {
      cfg_.fail_count = 0;
      update_manifest_fields(result);
      has_pending_update_ = false;
      {
        std::lock_guard<std::mutex> lk(snap_mu_);
        snap_.state = UpdateState::ManualOnly;
        snap_.error = UpdateError::None;
        snap_.download_available = false;
        snap_.fail_count = 0;
      }
      schedule_after(true, now);
      persist_runtime(cfg_.last_check_at, 0, 0);
      FR_LOG_INFO("[UPD] 发现 {}，无可用清单/资产，只提示发布页", result.latest_version);
      break;
    }
  }
}

void UpdateService::run_pending_download() {
  if (stop_.load() || !has_pending_update_) return;
  perform_download();
}

// v0.6.4 d460：跳过当前版本 —— 落盘 skipped_version（下次检查命中即进 Skipped），
// 清空待安装状态与缓存（.part/.ready 都不再需要）。
void UpdateService::run_skip() {
  if (stop_.load()) return;
  std::string version;
  {
    std::lock_guard<std::mutex> lk(snap_mu_);
    version = snap_.latest_version;
    snap_.state = UpdateState::Skipped;
    snap_.error = UpdateError::None;
    snap_.download_available = false;
    snap_.asset_path[0] = '\0';
    snap_.downloaded = 0;
    snap_.total = 0;
    snap_.bytes_per_sec = 0.0;
    snap_.progress_percent = -1;
  }
  has_pending_update_ = false;
  if (!version.empty()) {
    persist_skipped(version);
    cleanup_update_cache("", "", deps_.cache_dir);
    FR_LOG_INFO("[UPD] 用户跳过版本 {}（已落盘并清理缓存）", version);
  } else {
    FR_LOG_INFO("[UPD] 用户跳过更新（当前无候选版本）");
  }
  schedule_after(true, wall_now());
  persist_runtime(cfg_.last_check_at, 0, 0);
}

void UpdateService::perform_download() {
  if (stop_.load() || !has_pending_update_) return;
  const ReleaseAsset* asset = select_asset(pending_update_.manifest, shape_);
  if (asset == nullptr) {
    has_pending_update_ = false;
    set_state(UpdateState::ManualOnly, UpdateError::None);
    return;
  }

  const ReleaseManifest& manifest = pending_update_.manifest;
  const std::string asset_name = asset->name;
  const long long asset_size = asset->size;
  cancel_.store(false);
  {
    std::lock_guard<std::mutex> lk(snap_mu_);
    snap_.state = UpdateState::Downloading;
    snap_.error = UpdateError::None;
    snap_.download_available = true;
    snap_.asset_size = asset_size;
    copy_fixed(snap_.asset_name, asset_name);
    snap_.asset_path[0] = '\0';
    snap_.asset_sha256[0] = '\0';  // ★d495：新下载开始，清掉上一场的摘要
    snap_.downloaded = 0;
    snap_.total = asset_size;
    snap_.bytes_per_sec = 0.0;
    snap_.progress_percent = asset_size > 0 ? 0 : -1;
  }
  FR_LOG_INFO("[UPD] 开始下载 {}（{} 字节，端点 {} 个）", asset_name, asset_size,
              pending_update_.download_endpoints.size());
  FR_LOG_INFO("[UPD] 下载端点顺序：{}",
              endpoint_order_text(pending_update_.download_endpoints));

  DownloadDeps dd;
  dd.cache_dir = deps_.cache_dir;
  dd.progress = [this](const DownloadProgress& p) {
    update_download_fields(p.downloaded, p.total, p.bytes_per_sec);
  };
  const DownloadResult result =
      deps_.download
          ? deps_.download(manifest, *asset, pending_update_.download_endpoints, dd, &cancel_)
          : download_asset(manifest, *asset, pending_update_.download_endpoints, dd, &cancel_);

  if (stop_.load()) return;  // 退出/取消：.part 保留，状态由 stop() 收尾
  if (result.status == DownloadStatus::Ok) {
    has_pending_update_ = false;
    {
      std::lock_guard<std::mutex> lk(snap_mu_);
      snap_.state = UpdateState::Ready;
      snap_.error = UpdateError::None;
      snap_.download_available = false;
      copy_fixed(snap_.asset_path, result.file_path);
      copy_fixed(snap_.asset_sha256, result.sha256);  // ★d495：便携换装安装前复核用
      snap_.downloaded = asset_size;
      snap_.total = asset_size;
      snap_.progress_percent = 100;
    }
    schedule_after(true, wall_now());
    persist_runtime(cfg_.last_check_at, 0, 0);
    cleanup_update_cache(manifest.version, asset_name, deps_.cache_dir);
    FR_LOG_INFO("[UPD] 下载完成并校验通过：{}（胜出端点 {}）", asset_name, result.endpoint_id);
  } else if (result.status == DownloadStatus::Cancelled) {
    set_state(UpdateState::Idle, UpdateError::None);
  } else {
    ++cfg_.fail_count;
    {
      std::lock_guard<std::mutex> lk(snap_mu_);
      snap_.state = UpdateState::Backoff;
      snap_.error = download_error(result.status);
      snap_.download_available = true;
      snap_.fail_count = cfg_.fail_count;
    }
    schedule_after(false, wall_now());
    persist_runtime(cfg_.last_check_at, cfg_.next_retry_at, cfg_.fail_count);
    FR_LOG_WARN("[UPD] 下载失败（{}）：{}", download_status_name(result.status), result.error);
  }
}

void UpdateService::schedule_after(bool success, long long wall_now_sec) {
  long long next_wall = 0;
  if (!auto_enabled()) {
    next_due_ = std::chrono::system_clock::time_point::max();
  } else if (success) {
    cfg_.fail_count = 0;
    cfg_.next_retry_at = 0;
    const auto interval = std::chrono::hours(std::max(1, cfg_.check_interval_hours));
    next_due_ = std::chrono::system_clock::now() + interval;
    next_wall =
        wall_now_sec + std::chrono::duration_cast<std::chrono::seconds>(interval).count();
  } else {
    if (cfg_.fail_count < 1) cfg_.fail_count = 1;
    const long long wait_sec = backoff_seconds(cfg_.fail_count);
    cfg_.next_retry_at = wall_now_sec + wait_sec;
    next_due_ = std::chrono::system_clock::now() + std::chrono::seconds(wait_sec);
    next_wall = cfg_.next_retry_at;
  }
  std::lock_guard<std::mutex> lk(snap_mu_);
  snap_.next_check_at = auto_enabled() ? next_wall : 0;
}

void UpdateService::set_state(UpdateState state, UpdateError error) {
  std::lock_guard<std::mutex> lk(snap_mu_);
  snap_.state = state;
  snap_.error = error;
}

void UpdateService::update_manifest_fields(const DetectionResult& result) {
  std::lock_guard<std::mutex> lk(snap_mu_);
  copy_fixed(snap_.current_version, current_version_);
  copy_fixed(snap_.latest_version, result.latest_version);
  copy_fixed(snap_.release_url, result.release_url);
  copy_fixed_utf8(snap_.notes, result.notes);
  snap_.atom = result.atom;
  snap_.asset_name[0] = '\0';
  snap_.asset_path[0] = '\0';
  snap_.asset_size = 0;
  snap_.downloaded = 0;
  snap_.total = 0;
  snap_.bytes_per_sec = 0.0;
  snap_.progress_percent = -1;
}

void UpdateService::update_download_fields(long long downloaded, long long total, double bps) {
  std::lock_guard<std::mutex> lk(snap_mu_);
  if (snap_.state != UpdateState::Downloading) return;
  snap_.downloaded = downloaded;
  snap_.total = total;
  snap_.bytes_per_sec = bps;
  if (total > 0) {
    const double pct = 100.0 * static_cast<double>(downloaded) / static_cast<double>(total);
    snap_.progress_percent = static_cast<int>(std::clamp(pct, 0.0, 100.0));
  } else {
    snap_.progress_percent = -1;
  }
}

void UpdateService::persist_runtime(long long last_check_at, long long next_retry_at,
                                    int fail_count) {
  cfg_.last_check_at = std::max<long long>(0, last_check_at);
  cfg_.next_retry_at = std::max<long long>(0, next_retry_at);
  cfg_.fail_count = std::max(0, fail_count);
  if (config_path_.empty()) return;
  Config::patch(config_path_,
                {Config::PatchKV{"update", "last_check_at", std::to_string(cfg_.last_check_at)},
                 Config::PatchKV{"update", "next_retry_at", std::to_string(cfg_.next_retry_at)},
                 Config::PatchKV{"update", "fail_count", std::to_string(cfg_.fail_count)}});
}

void UpdateService::persist_skipped(const std::string& version) {
  cfg_.skipped_version = version;
  if (config_path_.empty()) return;
  Config::patch(config_path_,
                {Config::PatchKV{"update", "skipped_version", json_quote(version)}});
}

long long UpdateService::wall_now() const {
  if (deps_.wall_now) return deps_.wall_now();
  return static_cast<long long>(std::time(nullptr));
}

bool UpdateService::auto_enabled() const { return cfg_.enabled && cfg_.auto_check; }

}  // namespace fr::update
