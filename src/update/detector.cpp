#include "update/detector.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <utility>

#include "app/log.h"
#include "update/json_lite.h"
#include "update/version.h"

namespace fr::update {
namespace {

struct RaceItem {
  Endpoint endpoint;
  HttpResult http;
};

long long steady_now_sec() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::vector<RaceItem> run_race(const std::vector<Endpoint>& endpoints,
                               const std::string& upstream_url,
                               const std::vector<std::string>& headers, long long timeout_ms,
                               int concurrency, const BatchFetch& fetch,
                               const std::atomic<bool>* cancel) {
  std::vector<RaceItem> out;
  if (endpoints.empty()) return out;
  out.reserve(endpoints.size());
  std::vector<HttpRequest> requests;
  requests.reserve(endpoints.size());
  const bool proxy_http = upstream_url.rfind("http://", 0) == 0;
  for (const Endpoint& ep : endpoints) {
    HttpRequest req;
    req.url = endpoint_url(ep, upstream_url);
    req.connect_timeout_ms = static_cast<long>(std::min<long long>(5000, timeout_ms));
    req.timeout_ms = static_cast<long>(timeout_ms);
    req.max_body_bytes = json::kDefaultMaxBytes;
    req.headers = headers;
    // 放行只认拼装后的实际 URL：本机 http 代理（mock/联调）可用，远端仍强制 HTTPS。
    req.allow_http_loopback = proxy_http || req.url.rfind("http://", 0) == 0;
    requests.push_back(std::move(req));
  }
  std::vector<HttpResult> results = fetch(requests, concurrency, cancel);
  if (results.size() < requests.size()) {
    const size_t missing = requests.size() - results.size();
    results.resize(requests.size());
    for (size_t i = requests.size() - missing; i < requests.size(); ++i) {
      results[i].error = HttpError::Other;
      results[i].error_text = "fetch returned no result";
    }
  } else if (results.size() > requests.size()) {
    results.resize(requests.size());
  }
  for (size_t i = 0; i < endpoints.size(); ++i)
    out.push_back(RaceItem{endpoints[i], std::move(results[i])});
  return out;
}

void note_health(EndpointPolicy* policy, const RaceItem& item, long long now_sec) {
  if (policy == nullptr) return;
  // HTTP 错误码说明端点可达（例如代理不支持 API 返回 404），不算健康失败。
  if (item.http.error == HttpError::None || item.http.error == HttpError::HttpStatus)
    policy->note_success(item.endpoint.id);
  else
    policy->note_failure(item.endpoint.id, now_sec);
}

long long remaining_ms(std::chrono::steady_clock::time_point deadline) {
  const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now());
  return left.count() > 0 ? left.count() : 0;
}

DetectionResult error_result(const DetectionResult& base, std::string error) {
  DetectionResult out = base;
  out.outcome = DetectionOutcome::Error;
  out.error = std::move(error);
  return out;
}

bool has_downloadable_asset(const ReleaseManifest& manifest) {
  return manifest.windows_setup.valid || manifest.windows_zip.valid ||
         manifest.linux_appimage.valid || manifest.linux_deb.valid;
}

void append_unique_endpoints(std::vector<RankedEndpoint>* out,
                             const std::vector<RankedEndpoint>& extra) {
  for (const RankedEndpoint& item : extra) {
    bool seen = false;
    for (const RankedEndpoint& existing : *out) {
      if (existing.endpoint.id == item.endpoint.id) {
        seen = true;
        break;
      }
    }
    if (!seen) out->push_back(item);
  }
}

const char* format_kb_s(double bytes_per_sec, char* buf, std::size_t size) {
  std::snprintf(buf, size, "%.1f KB/s", bytes_per_sec / 1024.0);
  return buf;
}

// 记录每个端点的完整响应体吞吐（B/s）与落选原因，便于现场判断代理能力。
void log_probe_result(const char* stage, const std::vector<RaceItem>& items,
                      const std::vector<RankedEndpoint>& ranked) {
  std::string line;
  for (const RankedEndpoint& item : ranked) {
    char speed[32];
    char entry[192];
    std::snprintf(entry, sizeof(entry), "%s%s %s (%lld B / %lld ms)",
                  line.empty() ? "" : "；", item.endpoint.id.c_str(),
                  format_kb_s(item.metrics.bytes_per_sec, speed, sizeof(speed)),
                  item.metrics.body_bytes, item.metrics.total_ms);
    line += entry;
  }
  if (line.empty()) line = "无有效响应";
  FR_LOG_INFO("[UPD] {}测速排名：{}", stage, line);

  std::string failed;
  for (const RaceItem& item : items) {
    bool accepted = false;
    for (const RankedEndpoint& item_ranked : ranked) {
      if (item_ranked.endpoint.id == item.endpoint.id) {
        accepted = true;
        break;
      }
    }
    if (accepted) continue;
    char entry[192];
    if (item.http.error == HttpError::None) {
      std::snprintf(entry, sizeof(entry), "%s%s HTTP %ld 响应不可用",
                    failed.empty() ? "" : "；", item.endpoint.id.c_str(),
                    item.http.metrics.response_code);
    } else {
      std::snprintf(entry, sizeof(entry), "%s%s %s", failed.empty() ? "" : "；",
                    item.endpoint.id.c_str(),
                    item.http.error_text.empty() ? http_error_name(item.http.error)
                                                  : item.http.error_text.c_str());
    }
    failed += entry;
  }
  if (!failed.empty()) FR_LOG_INFO("[UPD] {}未采用：{}", stage, failed);
}

// 下载失败时的顺延项：API 阶段没成功的端点（manifest-only）也可能能下资产。
void append_unique_endpoints(std::vector<RankedEndpoint>* out,
                             const std::vector<Endpoint>& extra) {
  for (const Endpoint& endpoint : extra) {
    bool seen = false;
    for (const RankedEndpoint& existing : *out) {
      if (existing.endpoint.id == endpoint.id) {
        seen = true;
        break;
      }
    }
    if (!seen) out->push_back(RankedEndpoint{endpoint, HttpMetrics{}});
  }
}

}  // namespace

DetectionResult detect_update(const UpdateConfig& cfg, const std::string& current_version,
                              EndpointPolicy* policy, const std::atomic<bool>* cancel,
                              const DetectionDeps& deps) {
  DetectionResult result;
  result.current_version = current_version;

  SemVer current;
  if (!parse_semver(current_version, &current)) return error_result(result, "invalid current version");
  if (policy == nullptr) return error_result(result, "no endpoint policy");

  const BatchFetch fetch = deps.fetch ? deps.fetch : BatchFetch(http_get_many);
  const long long now_sec = deps.now_sec > 0 ? deps.now_sec : steady_now_sec();
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max<long long>(0, deps.budget_ms));

  std::vector<Endpoint> candidates = policy->candidates(deps.manual, now_sec);
  if (candidates.empty()) return error_result(result, "no update endpoints available");

  const long long api_budget = remaining_ms(deadline);
  if (api_budget <= 0) return error_result(result, "detection budget exhausted");
  const long long api_timeout = std::min<long long>(cfg.probe.timeout_ms, api_budget);

  const std::vector<std::string> api_headers = {"Accept: application/vnd.github+json"};
  std::vector<RaceItem> api_items =
      run_race(candidates, latest_release_api_url(cfg.api_base), api_headers, api_timeout,
               cfg.probe.concurrency, fetch, cancel);

  std::vector<RankedEndpoint> api_ranked;
  std::map<std::string, ReleaseInfo> api_releases;
  for (const RaceItem& item : api_items) {
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) break;
    note_health(policy, item, now_sec);
    if (item.http.error != HttpError::None) continue;
    ReleaseInfo info;
    if (!parse_github_release(item.http.body, &info, nullptr)) continue;
    if (!info.valid || info.version.empty()) continue;
    api_ranked.push_back(RankedEndpoint{item.endpoint, item.http.metrics});
    api_releases[item.endpoint.id] = std::move(info);
  }
  api_ranked = rank_race(std::move(api_ranked));
  log_probe_result("API", api_items, api_ranked);

  bool api_says_newer = false;
  const ReleaseInfo* api_info = nullptr;
  if (!api_ranked.empty()) {
    const std::size_t winner = pick_with_sticky(api_ranked, policy->api_sticky);
    const RankedEndpoint& picked = api_ranked[winner];
    policy->api_sticky = picked.endpoint.id;
    char speed[32];
    FR_LOG_INFO("[UPD] API 选择端点：{}（{}）", picked.endpoint.id,
                format_kb_s(picked.metrics.bytes_per_sec, speed, sizeof(speed)));
    const ReleaseInfo& info = api_releases[picked.endpoint.id];
    api_info = &info;
    result.latest_version = info.version;
    result.release_url = info.html_url;
    api_says_newer = is_newer_version(info.version, current_version);
    if (!api_says_newer) {
      result.outcome = DetectionOutcome::UpToDate;
      return result;
    }
  }

  // 清单赛跑：API 判定有更新，或 API 全军覆没时触发。
  const long long manifest_budget = remaining_ms(deadline);
  if (manifest_budget <= 0) {
    if (api_says_newer) {
      result.outcome = DetectionOutcome::NotifyOnly;
      return result;
    }
    return error_result(result, "detection budget exhausted");
  }
  const long long manifest_timeout = std::min<long long>(cfg.probe.timeout_ms, manifest_budget);
  std::vector<RaceItem> manifest_items =
      run_race(candidates, manifest_url(), {}, manifest_timeout, cfg.probe.concurrency, fetch, cancel);

  std::vector<RankedEndpoint> manifest_ranked;
  std::map<std::string, ReleaseManifest> manifests;
  for (const RaceItem& item : manifest_items) {
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) break;
    note_health(policy, item, now_sec);
    if (item.http.error != HttpError::None) continue;
    ReleaseManifest manifest;
    if (!parse_manifest(item.http.body, &manifest, nullptr)) continue;
    if (!manifest.valid) continue;
    manifest_ranked.push_back(RankedEndpoint{item.endpoint, item.http.metrics});
    manifests[item.endpoint.id] = std::move(manifest);
  }
  manifest_ranked = rank_race(std::move(manifest_ranked));
  log_probe_result("清单", manifest_items, manifest_ranked);

  if (manifest_ranked.empty()) {
    if (api_says_newer) {
      // 兼容未携带 latest.json 的旧 Release（如 v0.6.3）：资产名/大小取 API，
      // SHA-256 取 API digest；digest 缺失时补齐同版本 SHA256SUMS.txt。
      ReleaseManifest fallback;
      const bool built = api_info != nullptr && build_manifest_from_release(*api_info, &fallback);
      std::vector<RankedEndpoint> fallback_endpoints = api_ranked;
      if (built && manifest_needs_sha256sums(fallback)) {
        const long long sums_budget = remaining_ms(deadline);
        if (sums_budget > 0) {
          const std::string sums_url = asset_download_url(fallback.version, "SHA256SUMS.txt");
          std::vector<RaceItem> sums_items =
              run_race(candidates, sums_url, {},
                       std::min<long long>(cfg.probe.timeout_ms, sums_budget),
                       cfg.probe.concurrency, fetch, cancel);
          struct SumsHit {
            ReleaseManifest manifest;
            RankedEndpoint ranked;
          };
          std::vector<SumsHit> hits;
          for (const RaceItem& item : sums_items) {
            if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) break;
            note_health(policy, item, now_sec);
            if (item.http.error != HttpError::None) continue;
            ReleaseManifest trial = fallback;
            if (!apply_sha256sums(item.http.body, &trial)) continue;
            if (!has_downloadable_asset(trial)) continue;
            hits.push_back(
                SumsHit{std::move(trial), RankedEndpoint{item.endpoint, item.http.metrics}});
          }
          std::vector<RankedEndpoint> sums_ranked;
          for (const SumsHit& hit : hits) sums_ranked.push_back(hit.ranked);
          sums_ranked = rank_race(std::move(sums_ranked));
          log_probe_result("SHA256SUMS", sums_items, sums_ranked);
          if (!sums_ranked.empty()) {
            const std::string winner_id = sums_ranked.front().endpoint.id;
            char speed[32];
            FR_LOG_INFO("[UPD] SHA256SUMS 选择端点：{}（{}）", winner_id,
                        format_kb_s(sums_ranked.front().metrics.bytes_per_sec, speed,
                                    sizeof(speed)));
            for (SumsHit& hit : hits) {
              if (hit.ranked.endpoint.id == winner_id) {
                fallback = std::move(hit.manifest);
                break;
              }
            }
            // SHA256SUMS 与实际资产同属 Release 下载路径，测速结果更贴近大文件；
            // 其余 API 可用端点继续排在后面作为下载失败时的顺延项。
            fallback_endpoints = std::move(sums_ranked);
            append_unique_endpoints(&fallback_endpoints, api_ranked);
          }
        }
      }
      if (built) append_unique_endpoints(&fallback_endpoints, candidates);
      if (built && has_downloadable_asset(fallback)) {
        result.latest_version = fallback.version;
        result.notes = fallback.notes;
        if (!fallback.release_url.empty()) result.release_url = fallback.release_url;
        result.manifest = std::move(fallback);
        result.download_endpoints = std::move(fallback_endpoints);
        result.api_fallback = true;
        result.outcome = DetectionOutcome::Available;
        return result;
      }
      result.outcome = DetectionOutcome::NotifyOnly;
      result.notes.clear();
      return result;
    }
    // Atom 兜底：只取版本号，不猜下载地址、不自动下载。
    const long long atom_budget = remaining_ms(deadline);
    if (atom_budget <= 0) return error_result(result, "detection budget exhausted");
    std::vector<RaceItem> atom_items =
        run_race(candidates, atom_url(), {}, std::min<long long>(cfg.probe.timeout_ms, atom_budget),
                 cfg.probe.concurrency, fetch, cancel);
    std::vector<RankedEndpoint> atom_ranked;
    struct AtomHit {
      std::string tag;
      std::string url;
    };
    std::map<std::string, AtomHit> hits;
    for (const RaceItem& item : atom_items) {
      if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) break;
      note_health(policy, item, now_sec);
      if (item.http.error != HttpError::None) continue;
      std::string tag;
      std::string url;
      if (!parse_atom_latest(item.http.body, &tag, &url, nullptr)) continue;
      SemVer parsed;
      if (!parse_semver(tag, &parsed)) continue;
      atom_ranked.push_back(RankedEndpoint{item.endpoint, item.http.metrics});
      hits[item.endpoint.id] = AtomHit{tag, url};
    }
    atom_ranked = rank_race(std::move(atom_ranked));
    log_probe_result("Atom", atom_items, atom_ranked);
    if (atom_ranked.empty()) return error_result(result, "all endpoints failed");
    const std::size_t winner = pick_with_sticky(atom_ranked, policy->manifest_sticky);
    policy->manifest_sticky = atom_ranked[winner].endpoint.id;
    char speed[32];
    FR_LOG_INFO("[UPD] Atom 选择端点：{}（{}）", atom_ranked[winner].endpoint.id,
                format_kb_s(atom_ranked[winner].metrics.bytes_per_sec, speed, sizeof(speed)));
    const AtomHit& hit = hits[atom_ranked[winner].endpoint.id];
    SemVer atom_version;
    parse_semver(hit.tag, &atom_version);
    result.latest_version = semver_to_string(atom_version);
    result.release_url = hit.url;
    if (result.release_url.empty())
      result.release_url =
          std::string("https://github.com/") + update_repo() + "/releases/latest";
    result.atom = true;
    result.outcome = is_newer_version(hit.tag, current_version) ? DetectionOutcome::NotifyOnly
                                                                : DetectionOutcome::UpToDate;
    return result;
  }

  // 清单为权威：先取最快有效清单的版本，再在同版本清单内应用粘滞，避免旧缓存胜出。
  const std::string authoritative_version = manifests[manifest_ranked.front().endpoint.id].version;
  std::vector<RankedEndpoint> same_version;
  for (const RankedEndpoint& ranked : manifest_ranked) {
    if (manifests[ranked.endpoint.id].version == authoritative_version)
      same_version.push_back(ranked);
  }
  if (same_version.empty()) same_version = manifest_ranked;
  const std::size_t winner = pick_with_sticky(same_version, policy->manifest_sticky);
  const RankedEndpoint& picked = same_version[winner];
  policy->manifest_sticky = picked.endpoint.id;
  char speed[32];
  FR_LOG_INFO("[UPD] 清单选择端点：{}（{}）", picked.endpoint.id,
              format_kb_s(picked.metrics.bytes_per_sec, speed, sizeof(speed)));

  result.latest_version = authoritative_version;
  result.manifest = manifests[picked.endpoint.id];
  result.notes = result.manifest.notes;
  // 清单是权威：它的 release_url 与版本一致，优先于 API 胜者的链接。
  if (!result.manifest.release_url.empty()) result.release_url = result.manifest.release_url;

  if (!is_newer_version(authoritative_version, current_version)) {
    result.outcome = DetectionOutcome::UpToDate;
    return result;
  }

  result.download_endpoints.reserve(same_version.size());
  result.download_endpoints.push_back(picked);
  for (const RankedEndpoint& ranked : same_version) {
    if (ranked.endpoint.id != picked.endpoint.id) result.download_endpoints.push_back(ranked);
  }
  result.outcome = DetectionOutcome::Available;
  return result;
}

}  // namespace fr::update
