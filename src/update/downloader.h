#pragma once
// 静默下载：<cache>/<version>/<asset>.part → 校验 SHA-256 → <asset>（真实文件名，
// 可直接交给安装器/文件管理器）+ <asset>.ready（空标记，缓存复用判据）。
// 全部 I/O 由调用线程（更新工作线程）执行；进度回调最多 4Hz，绝不触碰 UI。
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "update/endpoint_policy.h"
#include "update/http_client.h"
#include "update/release_info.h"

namespace fr::update {

enum class DownloadStatus {
  Ok = 0,
  Cancelled,
  NoAsset,
  DiskFull,
  NetworkFailed,
  HashMismatch,
  IoError,
  InvalidManifest,
};

const char* download_status_name(DownloadStatus status);

struct DownloadProgress {
  long long downloaded = 0;  // 含续传起点
  long long total = -1;      // 清单声明大小；未知为 -1
  double bytes_per_sec = 0.0;
  std::string endpoint_id;
};

struct DownloadDeps {
  // 空 = http_get。测试注入时可完全不碰网络。
  std::function<HttpResult(const HttpRequest&, const HttpCallbacks&, const std::atomic<bool>*)>
      fetch;
  std::function<void(const DownloadProgress&)> progress;
  std::function<long long(const std::string& path)> free_bytes;  // 空 = paths::free_disk_bytes
  std::string cache_dir;                                        // 空 = paths::update_cache_dir()
};

struct DownloadResult {
  DownloadStatus status = DownloadStatus::NetworkFailed;
  std::string error;
  std::string file_path;  // 成功时为已校验资产（真实文件名）的绝对/缓存路径
  std::string sha256;
  std::string endpoint_id;
  bool reused_ready = false;
};

// endpoints 按吞吐降序（detector 的 download_endpoints）。每端点最多 2 次尝试
// （首次 + 续传），连续 30 秒无进度即中止；校验失败换下一端点。
DownloadResult download_asset(const ReleaseManifest& manifest, const ReleaseAsset& asset,
                              const std::vector<RankedEndpoint>& endpoints,
                              const DownloadDeps& deps = {},
                              const std::atomic<bool>* cancel = nullptr);

// 缓存目录清理：删除 keep_version 之外的版本目录；keep_version 内最多保留一个
// 已校验资产（真实文件名 + .ready 标记 + 可选 .part）。
// 取消/退出时 .part 必须保留（服务层不得在下载中断时调用本函数）。
// cache_dir 空 = paths::update_cache_dir()（测试可注入临时目录）。
void cleanup_update_cache(const std::string& keep_version, const std::string& keep_asset_name,
                          const std::string& cache_dir = {});

}  // namespace fr::update
