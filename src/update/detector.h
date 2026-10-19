#pragma once
// 检测链：API 赛跑 → 清单赛跑 → Atom 兜底。只做决策与解析，
// 网络通过 BatchFetch 注入（默认 curl_multi），便于单测完全离线复现。
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "app/config.h"
#include "update/endpoint_policy.h"
#include "update/http_client.h"
#include "update/release_info.h"

namespace fr::update {

enum class DetectionOutcome {
  Error = 0,
  UpToDate,
  Available,   // 有更新且清单有效，可静默下载
  NotifyOnly,  // 有更新但清单缺失/不可用，只能提示发布页
};

struct DetectionResult {
  DetectionOutcome outcome = DetectionOutcome::Error;
  std::string current_version;
  std::string latest_version;
  std::string release_url;
  std::string notes;
  ReleaseManifest manifest;
  std::vector<RankedEndpoint> download_endpoints;  // 与权威清单同版本，按吞吐降序
  bool atom = false;
  bool api_fallback = false;  // 清单缺失，改由 API 资产 + SHA256SUMS 合成
  std::string error;
};

using BatchFetch = std::function<std::vector<HttpResult>(
    const std::vector<HttpRequest>& requests, int concurrency, const std::atomic<bool>* cancel)>;

struct DetectionDeps {
  BatchFetch fetch;          // 空 = http_get_many
  long long now_sec = 0;     // 0 = steady_clock
  bool manual = false;       // 手动检查绕过冷却
  long long budget_ms = 45000;
};

DetectionResult detect_update(const UpdateConfig& cfg, const std::string& current_version,
                              EndpointPolicy* policy, const std::atomic<bool>* cancel,
                              const DetectionDeps& deps = {});

}  // namespace fr::update
