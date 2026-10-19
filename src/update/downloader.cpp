#include "update/downloader.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <system_error>

#include "platform/paths.h"
#include "update/verify.h"

namespace fs = std::filesystem;

namespace fr::update {
namespace {

constexpr long long kMiB = 1024LL * 1024LL;
constexpr long long kUnknownAssetSpace = 300LL * kMiB;
constexpr long long kExtraSpace = 50LL * kMiB;
constexpr long long kDownloadStallSec = 30;
constexpr auto kProgressInterval = std::chrono::milliseconds(250);  // 最多 4Hz

bool is_hex64(const std::string& s) {
  if (s.size() != 64) return false;
  for (unsigned char c : s) {
    const bool digit = c >= '0' && c <= '9';
    const bool lower = c >= 'a' && c <= 'f';
    if (!digit && !lower) return false;
  }
  return true;
}

long long file_size_or(const std::string& path, long long fallback) {
  std::error_code ec;
  const auto n = fs::file_size(path, ec);
  return ec ? fallback : static_cast<long long>(n);
}

bool remove_file(const std::string& path) {
  std::error_code ec;
  fs::remove(path, ec);
  return !ec;
}

long long required_space(long long asset_size) {
  if (asset_size <= 0) return kUnknownAssetSpace;
  if (asset_size > (std::numeric_limits<long long>::max() - kExtraSpace) / 2)
    return std::numeric_limits<long long>::max();
  return asset_size + asset_size / 5 + kExtraSpace;
}

void report_progress(const DownloadDeps& deps, long long downloaded, long long total,
                     double bps, const std::string& endpoint_id) {
  if (!deps.progress) return;
  DownloadProgress p;
  p.downloaded = downloaded;
  p.total = total;
  p.bytes_per_sec = bps;
  p.endpoint_id = endpoint_id;
  try {
    deps.progress(p);
  } catch (...) {
  }
}

struct AttemptState {
  std::ofstream out;
  std::string path;
  long long base_offset = 0;   // 本次响应写入前的续传起点
  long long written = 0;       // 本次响应已写字节
  long long asset_size = 0;
  bool truncated_to_zero = false;
  bool aborted = false;
  bool oversized = false;
  std::string endpoint_id;
  long long last_report_ms = 0;
};

long long now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool open_append(std::ofstream* out, const std::string& path) {
  out->open(path, std::ios::binary | std::ios::app);
  return out->is_open();
}

bool open_truncate(std::ofstream* out, const std::string& path) {
  out->open(path, std::ios::binary | std::ios::trunc);
  return out->is_open();
}

// 返回 false 表示本端点本轮失败（已记录 error）。
bool verify_and_publish(const std::string& part_path, const std::string& final_path,
                        const std::string& marker_path, const ReleaseAsset& asset,
                        DownloadResult* result) {
  std::string computed;
  std::string error;
  if (!sha256_file(part_path, &computed, &error)) {
    result->status = DownloadStatus::IoError;
    result->error = "read downloaded file: " + error;
    return false;
  }
  if (computed != asset.sha256) {
    result->status = DownloadStatus::HashMismatch;
    result->error = "sha256 mismatch: expected " + asset.sha256 + ", got " + computed;
    remove_file(part_path);
    return false;
  }
  std::error_code ec;
  // 发布名必须是真实资产名（.exe/.zip/.deb）：安装器与文件管理器按扩展名识别，
  // 旧实现的 "<asset>.ready" 作为唯一落盘名会让 Windows 无法直接执行安装包。
  fs::remove(marker_path, ec);  // 先撤标记：rename 完成前不可宣称"已校验"
  ec.clear();
  fs::remove(final_path, ec);
  ec.clear();
  fs::rename(part_path, final_path, ec);
  if (ec) {
    result->status = DownloadStatus::IoError;
    result->error = "rename .part -> asset: " + ec.message();
    return false;
  }
  {
    std::ofstream marker(marker_path, std::ios::binary | std::ios::trunc);
    // 标记只服务缓存复用；写不出也不影响本次结果（下次会重新校验真实文件）。
  }
  result->status = DownloadStatus::Ok;
  result->file_path = final_path;
  result->sha256 = computed;
  result->error.clear();
  return true;
}

}  // namespace

const char* download_status_name(DownloadStatus status) {
  switch (status) {
    case DownloadStatus::Ok: return "ok";
    case DownloadStatus::Cancelled: return "cancelled";
    case DownloadStatus::NoAsset: return "no-asset";
    case DownloadStatus::DiskFull: return "disk-full";
    case DownloadStatus::NetworkFailed: return "network-failed";
    case DownloadStatus::HashMismatch: return "hash-mismatch";
    case DownloadStatus::IoError: return "io-error";
    case DownloadStatus::InvalidManifest: return "invalid-manifest";
  }
  return "unknown";
}

DownloadResult download_asset(const ReleaseManifest& manifest, const ReleaseAsset& asset,
                              const std::vector<RankedEndpoint>& endpoints,
                              const DownloadDeps& deps, const std::atomic<bool>* cancel) {
  DownloadResult result;
  if (!manifest.valid || manifest.version.empty() || !asset.valid ||
      !is_safe_asset_name(asset.name) || !is_hex64(asset.sha256)) {
    result.status = DownloadStatus::InvalidManifest;
    result.error = "manifest or asset entry is invalid";
    return result;
  }
  if (endpoints.empty()) {
    result.status = DownloadStatus::NoAsset;
    result.error = "no download endpoints";
    return result;
  }

  const std::string root = deps.cache_dir.empty() ? paths::update_cache_dir() : deps.cache_dir;
  const std::string dir = root + "/" + manifest.version;
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    result.status = DownloadStatus::IoError;
    result.error = "create cache dir: " + ec.message();
    return result;
  }

  const std::string part_path = dir + "/" + asset.name + ".part";
  const std::string final_path = dir + "/" + asset.name;
  const std::string ready_path = final_path + ".ready";

  if (fs::exists(ready_path, ec)) {
    const long long ready_size = file_size_or(final_path, -1);
    if ((asset.size <= 0 || ready_size == asset.size) && ready_size >= 0) {
      std::string computed;
      std::string hash_error;
      if (sha256_file(final_path, &computed, &hash_error) && computed == asset.sha256) {
        result.status = DownloadStatus::Ok;
        result.file_path = final_path;
        result.sha256 = computed;
        result.reused_ready = true;
        return result;
      }
    }
    remove_file(final_path);
    remove_file(ready_path);
  }

  const long long free_now =
      deps.free_bytes ? deps.free_bytes(root) : paths::free_disk_bytes(root);
  if (free_now >= 0 && free_now < required_space(asset.size)) {
    result.status = DownloadStatus::DiskFull;
    result.error = "not enough disk space for " + asset.name;
    return result;
  }

  const std::string upstream = asset_download_url(manifest.version, asset.name);
  bool saw_hash_mismatch = false;

  for (const RankedEndpoint& ranked : endpoints) {
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
      result.status = DownloadStatus::Cancelled;
      return result;
    }
    const Endpoint& endpoint = ranked.endpoint;
    bool reset_after_416 = false;

    for (int attempt = 0; attempt < 2; ++attempt) {
      if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
        result.status = DownloadStatus::Cancelled;
        return result;
      }
      long long offset = file_size_or(part_path, 0);
      if (offset < 0) offset = 0;
      if (asset.size > 0 && offset > asset.size) {
        remove_file(part_path);
        offset = 0;
      }

      // .part 已完整（例如上次下载完但没来得及校验）：直接进入校验。
      if (asset.size > 0 && offset == asset.size) {
        result.endpoint_id = endpoint.id;
        if (verify_and_publish(part_path, final_path, ready_path, asset, &result)) return result;
        if (result.status == DownloadStatus::IoError) return result;
        saw_hash_mismatch = true;
        break;  // 换端点
      }

      AttemptState st;
      st.path = part_path;
      st.base_offset = offset;
      st.asset_size = asset.size;
      st.endpoint_id = endpoint.id;

      if (offset > 0) {
        if (!open_append(&st.out, part_path)) {
          result.status = DownloadStatus::IoError;
          result.error = "open .part for append: " + part_path;
          return result;
        }
      } else if (!open_truncate(&st.out, part_path)) {
        result.status = DownloadStatus::IoError;
        result.error = "open .part for write: " + part_path;
        return result;
      }

      HttpRequest req;
      req.url = endpoint_url(endpoint, upstream);
      // 拼装后的 URL 是本机 http 时才放行（远端代理仍强制 HTTPS）。
      req.allow_http_loopback = req.url.rfind("http://", 0) == 0;
      req.timeout_ms = 0;  // 大文件不设总超时，用停滞检测
      req.stall_timeout_sec = kDownloadStallSec;
      req.max_body_bytes = asset.size > 0 ? static_cast<std::size_t>(asset.size) : 0;
      req.range_from = offset > 0 ? offset : -1;

      long long last_progress_ms = 0;
      HttpCallbacks callbacks;
      callbacks.on_response = [&st](long code, bool resumed) {
        if (code == 200) {
          if (st.base_offset > 0) {
            // 服务器忽略 Range：必须从 0 重写，已有 .part 全部作废。
            st.out.close();
            if (!open_truncate(&st.out, st.path)) {
              st.aborted = true;
              return false;
            }
            st.base_offset = 0;
            st.truncated_to_zero = true;
          }
          return true;
        }
        if (code == 206) {
          if (!resumed && st.base_offset > 0) {
            st.aborted = true;
            return false;
          }
          return true;
        }
        st.aborted = true;
        return false;
      };
      callbacks.on_body = [&st](const char* data, std::size_t n) -> std::size_t {
        if (st.asset_size > 0 && st.base_offset + st.written + static_cast<long long>(n) >
                                    st.asset_size) {
          st.oversized = true;
          return 0;
        }
        st.out.write(data, static_cast<std::streamsize>(n));
        if (!st.out) return 0;
        st.written += static_cast<long long>(n);
        return n;
      };
      callbacks.on_progress = [&st, &deps, &last_progress_ms](long long dl_total,
                                                             long long dl_now) {
        const long long elapsed = now_ms();
        if (elapsed - last_progress_ms < kProgressInterval.count()) return true;
        last_progress_ms = elapsed;
        const long long downloaded = st.base_offset + dl_now;
        long long total = st.asset_size;
        if (total <= 0 && dl_total > 0) total = st.base_offset + dl_total;
        const double bps = elapsed > 0 ? static_cast<double>(dl_now) * 1000.0 / elapsed : 0.0;
        report_progress(deps, downloaded, total, bps, st.endpoint_id);
        return true;
      };

      const auto fetch = deps.fetch ? deps.fetch : http_get;
      HttpResult http = fetch(req, callbacks, cancel);

      st.out.flush();
      st.out.close();

      if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
        result.status = DownloadStatus::Cancelled;
        return result;
      }
      // 416：.part 起点超出服务器资源，删掉重下（本端点第二次尝试）。
      if (http.metrics.response_code == 416 && offset > 0 && !reset_after_416) {
        remove_file(part_path);
        reset_after_416 = true;
        --attempt;
        continue;
      }
      if (http.error == HttpError::RangeMismatch) {
        remove_file(part_path);
        continue;
      }
      if (http.error == HttpError::WriteFailed) {
        if (st.oversized) {
          remove_file(part_path);
          continue;
        }
        result.status = DownloadStatus::IoError;
        result.error = "write .part failed";
        return result;
      }
      if (http.error != HttpError::None) {
        result.error = http.error_text.empty() ? http_error_name(http.error) : http.error_text;
        continue;  // 保留 .part，下一次尝试续传
      }
      if (st.aborted) {
        result.error = "unexpected HTTP " + std::to_string(http.metrics.response_code);
        continue;
      }

      const long long final_size = file_size_or(part_path, -1);
      if (final_size < 0) {
        result.status = DownloadStatus::IoError;
        result.error = "stat .part failed";
        return result;
      }
      if (asset.size > 0 && final_size > asset.size) {
        remove_file(part_path);
        result.error = "downloaded file exceeds manifest size";
        continue;
      }
      if (asset.size > 0 && final_size < asset.size) {
        result.error = "downloaded file is short (" + std::to_string(final_size) + "/" +
                       std::to_string(asset.size) + ")";
        continue;
      }

      result.endpoint_id = endpoint.id;
      if (verify_and_publish(part_path, final_path, ready_path, asset, &result)) {
        long long total = asset.size > 0 ? asset.size : final_size;
        report_progress(deps, final_size, total, 0.0, endpoint.id);
        return result;
      }
      if (result.status == DownloadStatus::IoError) return result;
      saw_hash_mismatch = true;
      break;  // 哈希不符：删除后换端点
    }
  }

  result.status = saw_hash_mismatch ? DownloadStatus::HashMismatch : DownloadStatus::NetworkFailed;
  if (result.error.empty())
    result.error = saw_hash_mismatch ? "sha256 verification failed on all endpoints"
                                     : "all download endpoints failed";
  return result;
}

void cleanup_update_cache(const std::string& keep_version, const std::string& keep_asset_name,
                          const std::string& cache_dir) {
  const std::string root = cache_dir.empty() ? paths::update_cache_dir() : cache_dir;
  std::error_code ec;
  if (!fs::exists(root, ec)) return;
  fs::directory_iterator it(root, ec);
  if (ec) return;
  const fs::directory_iterator end;
  for (; it != end; it.increment(ec)) {
    if (ec) return;
    const fs::directory_entry& entry = *it;
    if (!entry.is_directory(ec)) continue;
    const std::string name = entry.path().filename().string();
    if (keep_version.empty() || name != keep_version) {
      std::error_code rm_ec;
      fs::remove_all(entry.path(), rm_ec);
      continue;
    }
    if (keep_asset_name.empty()) continue;
    std::error_code inner_ec;
    fs::directory_iterator inner(entry.path(), inner_ec);
    if (inner_ec) continue;
    const fs::directory_iterator inner_end;
    for (; inner != inner_end; inner.increment(inner_ec)) {
      if (inner_ec) break;
      if (!inner->is_regular_file(inner_ec)) continue;
      const std::string file = inner->path().filename().string();
      const std::string keep_part = keep_asset_name + ".part";
      const std::string keep_ready = keep_asset_name + ".ready";
      const std::string keep_final = keep_asset_name;
      if (file == keep_part || file == keep_ready || file == keep_final) continue;
      // 只清理本模块的中间产物，不碰用户放进缓存目录的其他文件。
      if (file.size() > 5 && (file.compare(file.size() - 5, 5, ".part") == 0 ||
                              file.compare(file.size() - 6, 6, ".ready") == 0)) {
        std::error_code rm_ec;
        fs::remove(inner->path(), rm_ec);
      }
    }
  }
}

}  // namespace fr::update
