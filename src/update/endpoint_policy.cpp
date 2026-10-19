#include "update/endpoint_policy.h"

#include <algorithm>
#include <set>
#include <string_view>

#ifndef FR_UPDATE_REPO
#define FR_UPDATE_REPO "jianglinbin/FlashRec"
#endif

namespace fr::update {
namespace {

std::string percent_encode(std::string_view text) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(text.size() * 3);
  for (unsigned char c : text) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '.' || c == '_' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0f]);
    }
  }
  return out;
}

std::string version_without_v(std::string version) {
  if (!version.empty() && (version.front() == 'v' || version.front() == 'V'))
    version.erase(version.begin());
  return version;
}

constexpr long long kFailureThreshold = 3;
constexpr long long kFirstCooldownSec = 30 * 60;
constexpr long long kCooldownStepsSec[] = {5 * 60, 15 * 60, 60 * 60};

}  // namespace

std::vector<Endpoint> build_endpoints(const UpdateConfig& cfg) {
  std::vector<Endpoint> out;
  int order = 0;
  if (cfg.direct_enabled) {
    Endpoint direct;
    direct.id = "direct";
    direct.direct = true;
    direct.order = order++;
    out.push_back(std::move(direct));
  }
  std::set<std::string> used_ids;
  used_ids.insert("direct");
  for (const UpdateProxy& proxy : cfg.proxies) {
    if (!proxy.enabled) continue;
    if (proxy.prefix.empty() == proxy.url_template.empty()) continue;  // 必须二选一
    Endpoint ep;
    ep.id = proxy.name.empty() ? ("proxy" + std::to_string(order)) : proxy.name;
    if (used_ids.count(ep.id) != 0) {
      std::string base = ep.id;
      int suffix = 2;
      while (used_ids.count(base + "#" + std::to_string(suffix)) != 0) ++suffix;
      ep.id = base + "#" + std::to_string(suffix);
    }
    used_ids.insert(ep.id);
    ep.prefix = proxy.prefix;
    ep.url_template = proxy.url_template;
    ep.order = order++;
    out.push_back(std::move(ep));
  }
  return out;
}

std::string endpoint_url(const Endpoint& endpoint, const std::string& upstream_url) {
  if (endpoint.direct) return upstream_url;
  if (!endpoint.url_template.empty()) {
    std::string out = endpoint.url_template;
    const std::string needle = "{url}";
    const std::string encoded = percent_encode(upstream_url);
    for (size_t pos = out.find(needle); pos != std::string::npos; pos = out.find(needle, pos)) {
      out.replace(pos, needle.size(), encoded);
      pos += encoded.size();
    }
    return out;
  }
  if (endpoint.prefix.empty()) return upstream_url;
  std::string base = endpoint.prefix;
  if (base.back() != '/') base.push_back('/');
  std::string tail = upstream_url;
  while (!tail.empty() && tail.front() == '/') tail.erase(tail.begin());
  return base + tail;
}

const char* update_repo() { return FR_UPDATE_REPO; }

std::string latest_release_api_url(const std::string& api_base) {
  if (!api_base.empty()) return api_base;
  return std::string("https://api.github.com/repos/") + FR_UPDATE_REPO + "/releases/latest";
}

std::string manifest_url() {
  return std::string("https://github.com/") + FR_UPDATE_REPO +
         "/releases/latest/download/latest.json";
}

std::string atom_url() {
  return std::string("https://github.com/") + FR_UPDATE_REPO + "/releases.atom";
}

std::string asset_download_url(const std::string& version, const std::string& asset_name) {
  return std::string("https://github.com/") + FR_UPDATE_REPO + "/releases/download/v" +
         version_without_v(version) + "/" + asset_name;
}

std::vector<RankedEndpoint> rank_race(std::vector<RankedEndpoint> items) {
  std::stable_sort(items.begin(), items.end(), [](const RankedEndpoint& a,
                                                  const RankedEndpoint& b) {
    if (a.metrics.bytes_per_sec != b.metrics.bytes_per_sec)
      return a.metrics.bytes_per_sec > b.metrics.bytes_per_sec;
    return a.endpoint.order < b.endpoint.order;
  });
  return items;
}

std::size_t pick_with_sticky(const std::vector<RankedEndpoint>& ranked,
                             const std::string& sticky_id) {
  if (ranked.empty()) return kNoEndpoint;
  if (sticky_id.empty()) return 0;
  for (std::size_t i = 0; i < ranked.size(); ++i) {
    if (ranked[i].endpoint.id != sticky_id) continue;
    return ranked[i].metrics.bytes_per_sec >= ranked.front().metrics.bytes_per_sec / 1.15 ? i : 0;
  }
  return 0;
}

void EndpointHealth::note_success(const std::string& id) {
  auto it = entries_.find(id);
  if (it == entries_.end()) return;
  it->second = Entry{};
}

void EndpointHealth::note_failure(const std::string& id, long long now_sec) {
  Entry& e = entries_[id];
  ++e.failures;
  if (e.failures < kFailureThreshold || now_sec < e.until) return;
  long long duration = kFirstCooldownSec;
  if (e.cooldown_level > 0) {
    const int index = std::min<int>(e.cooldown_level - 1, 2);
    duration = kCooldownStepsSec[index];
  }
  e.until = now_sec + duration;
  ++e.cooldown_level;
}

bool EndpointHealth::available(const std::string& id, long long now_sec) const {
  auto it = entries_.find(id);
  return it == entries_.end() || now_sec >= it->second.until;
}

long long EndpointHealth::cooldown_until(const std::string& id) const {
  auto it = entries_.find(id);
  return it == entries_.end() ? 0 : it->second.until;
}

int EndpointHealth::consecutive_failures(const std::string& id) const {
  auto it = entries_.find(id);
  return it == entries_.end() ? 0 : it->second.failures;
}

void EndpointHealth::reset() { entries_.clear(); }

void EndpointPolicy::configure(const UpdateConfig& cfg) {
  endpoints_ = build_endpoints(cfg);
  health_.reset();
  api_sticky.clear();
  manifest_sticky.clear();
}

std::vector<Endpoint> EndpointPolicy::candidates(bool manual, long long now_sec) const {
  std::vector<Endpoint> out;
  for (const Endpoint& ep : endpoints_) {
    if (manual || health_.available(ep.id, now_sec)) out.push_back(ep);
  }
  return out;
}

}  // namespace fr::update
