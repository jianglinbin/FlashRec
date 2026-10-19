#pragma once
// 更新端点：直连/代理候选、URL 拼装、吞吐排名、粘滞选择、健康冷却。
// 无网络 I/O，全部可在单测中注入验证。
#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "app/config.h"
#include "update/http_client.h"

namespace fr::update {

struct Endpoint {
  std::string id;            // "direct" 或代理名（重名时追加序号），健康与粘滞的键
  std::string prefix;        // 与 url_template 二选一
  std::string url_template;  // 含 {url}
  bool direct = false;
  int order = 0;             // 配置顺序，仅作同吞吐兜底
};

std::vector<Endpoint> build_endpoints(const UpdateConfig& cfg);

// prefix：<prefix><upstream>；template：{url} 替换为完整上游 URL 的百分号编码。
std::string endpoint_url(const Endpoint& endpoint, const std::string& upstream_url);

const char* update_repo();
std::string latest_release_api_url(const std::string& api_base);
std::string manifest_url();
std::string atom_url();
std::string asset_download_url(const std::string& version, const std::string& asset_name);

struct RankedEndpoint {
  Endpoint endpoint;
  HttpMetrics metrics;
};

// 按完整响应体吞吐降序；相同吞吐按配置顺序。
std::vector<RankedEndpoint> rank_race(std::vector<RankedEndpoint> items);

constexpr std::size_t kNoEndpoint = static_cast<std::size_t>(-1);

// 粘滞：sticky 吞吐 ≥ 最快项 ÷ 1.15 时继续用 sticky，否则用最快项。
std::size_t pick_with_sticky(const std::vector<RankedEndpoint>& ranked,
                             const std::string& sticky_id);

class EndpointHealth {
 public:
  void note_success(const std::string& id);
  // 连续 3 次失败进入冷却：首次 30 分钟，持续失败按 5m → 15m → 60m 递增。
  void note_failure(const std::string& id, long long now_sec);
  bool available(const std::string& id, long long now_sec) const;
  long long cooldown_until(const std::string& id) const;
  int consecutive_failures(const std::string& id) const;
  void reset();

 private:
  struct Entry {
    int failures = 0;
    int cooldown_level = 0;
    long long until = 0;
  };
  std::map<std::string, Entry> entries_;
};

class EndpointPolicy {
 public:
  EndpointPolicy() = default;
  explicit EndpointPolicy(const UpdateConfig& cfg) { configure(cfg); }
  void configure(const UpdateConfig& cfg);

  const std::vector<Endpoint>& endpoints() const { return endpoints_; }
  // 自动检查剔除冷却中的端点；手动检查绕过冷却。
  std::vector<Endpoint> candidates(bool manual, long long now_sec) const;
  void note_success(const std::string& id) { health_.note_success(id); }
  void note_failure(const std::string& id, long long now_sec) {
    health_.note_failure(id, now_sec);
  }
  bool cooling(const std::string& id, long long now_sec) const {
    return !health_.available(id, now_sec);
  }
  const EndpointHealth& health() const { return health_; }

  // 按阶段记录上次胜者（API / 清单）
  std::string api_sticky;
  std::string manifest_sticky;

 private:
  std::vector<Endpoint> endpoints_;
  EndpointHealth health_;
};

}  // namespace fr::update
