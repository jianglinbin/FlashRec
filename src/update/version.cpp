#include "update/version.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>

namespace fr {

namespace {

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' ||
                        s.front() == '\n'))
    s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' ||
                        s.back() == '\n'))
    s.remove_suffix(1);
  return s;
}

bool parse_number(std::string_view s, int* out) {
  if (s.empty()) return false;
  if (s.size() > 1 && s.front() == '0') return false;  // semver：数字段禁止前导零
  uint64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + static_cast<uint64_t>(c - '0');
    if (v > static_cast<uint64_t>(std::numeric_limits<int>::max())) return false;
  }
  *out = static_cast<int>(v);
  return true;
}

bool is_numeric_id(std::string_view s) {
  if (s.empty()) return false;
  for (char c : s)
    if (c < '0' || c > '9') return false;
  return true;
}

bool valid_identifier(std::string_view s) {
  if (s.empty()) return false;
  for (char c : s) {
    const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                    (c >= 'A' && c <= 'Z') || c == '-';
    if (!ok) return false;
  }
  return true;
}

std::vector<std::string_view> split_dot(std::string_view s) {
  std::vector<std::string_view> out;
  size_t start = 0;
  for (;;) {
    const size_t dot = s.find('.', start);
    if (dot == std::string_view::npos) {
      out.push_back(s.substr(start));
      break;
    }
    out.push_back(s.substr(start, dot - start));
    start = dot + 1;
  }
  return out;
}

// semver §11：数字标识按数值比；数字 < 字母数字；字母数字按 ASCII 字典序。
int compare_identifier(std::string_view a, std::string_view b) {
  const bool an = is_numeric_id(a), bn = is_numeric_id(b);
  if (an && bn) {
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    const int c = a.compare(b);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
  }
  if (an != bn) return an ? -1 : 1;
  const int c = a.compare(b);
  return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

}  // namespace

bool parse_semver(std::string_view text, SemVer* out) {
  if (!out) return false;
  *out = SemVer{};
  text = trim(text);
  if (text.empty()) return false;
  if (text.front() == 'v' || text.front() == 'V') text.remove_prefix(1);

  const size_t plus = text.find('+');
  if (plus != std::string_view::npos) text = text.substr(0, plus);

  std::string_view pre;
  const size_t dash = text.find('-');
  if (dash != std::string_view::npos) {
    pre = text.substr(dash + 1);
    text = text.substr(0, dash);
    if (pre.empty()) return false;
    for (std::string_view id : split_dot(pre)) {
      if (!valid_identifier(id)) return false;
      if (is_numeric_id(id) && id.size() > 1 && id.front() == '0') return false;
      out->prerelease.emplace_back(id);
    }
  }

  const auto parts = split_dot(text);
  if (parts.size() != 3) return false;
  if (!parse_number(parts[0], &out->major) || !parse_number(parts[1], &out->minor) ||
      !parse_number(parts[2], &out->patch))
    return false;

  out->valid = true;
  return true;
}

int compare_semver(const SemVer& a, const SemVer& b) {
  if (!a.valid || !b.valid) return 0;
  if (a.major != b.major) return a.major < b.major ? -1 : 1;
  if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
  if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;

  // 正式版 > 预发布版
  if (a.prerelease.empty() && b.prerelease.empty()) return 0;
  if (a.prerelease.empty()) return 1;
  if (b.prerelease.empty()) return -1;

  const size_t n = std::min(a.prerelease.size(), b.prerelease.size());
  for (size_t i = 0; i < n; ++i) {
    const int c = compare_identifier(a.prerelease[i], b.prerelease[i]);
    if (c != 0) return c;
  }
  if (a.prerelease.size() == b.prerelease.size()) return 0;
  return a.prerelease.size() < b.prerelease.size() ? -1 : 1;
}

bool is_newer_version(std::string_view latest, std::string_view current) {
  SemVer l, c;
  if (!parse_semver(latest, &l) || !parse_semver(current, &c)) return false;
  return compare_semver(l, c) > 0;
}

std::string semver_to_string(const SemVer& v) {
  if (!v.valid) return {};
  std::string out = std::to_string(v.major) + "." + std::to_string(v.minor) + "." +
                    std::to_string(v.patch);
  for (size_t i = 0; i < v.prerelease.size(); ++i) {
    out += (i == 0) ? "-" : ".";
    out += v.prerelease[i];
  }
  return out;
}

}  // namespace fr
