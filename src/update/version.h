#pragma once
// 自动更新版本比较（AUTO_UPDATE_PLAN §6）：严格 semver，标签可带 v 前缀。
// 只做解析/比较，不涉及网络与配置，便于单测。
#include <string>
#include <string_view>
#include <vector>

namespace fr {

struct SemVer {
  int major = 0;
  int minor = 0;
  int patch = 0;
  std::vector<std::string> prerelease;  // 空 = 正式版
  bool valid = false;
};

// "v0.6.5" / "0.6.5" / "0.6.5-beta.1" → SemVer；构建元数据（+xxx）忽略。
// 非法标签返回 false，out->valid = false（调用方必须把非法标签当"忽略该 Release"，
// 不能当成"已是最新"）。
bool parse_semver(std::string_view text, SemVer* out);

// 负数/0/正数 = a < b / a == b / a > b。要求两者 valid。
int compare_semver(const SemVer& a, const SemVer& b);

// latest 严格新于 current 才返回 true；任一非法返回 false（不误报更新）。
bool is_newer_version(std::string_view latest, std::string_view current);

// 规范化：去 v 前缀与构建元数据，输出 major.minor.patch[-prerelease]。
// 非法输入返回空串。
std::string semver_to_string(const SemVer& v);

}  // namespace fr
