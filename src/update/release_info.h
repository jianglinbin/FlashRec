#pragma once
// Release 报文模型（AUTO_UPDATE_PLAN §4）：
//   - GitHub Releases API（releases/latest）
//   - 自维护清单 latest.json（版本/资产/哈希/备注的唯一权威）
//   - releases.atom（只在 API 与清单都不可用时取版本号）
// 解析层不做网络、不读配置；非法输入返回 false 或把对应字段标记为不可用。
#include <string>
#include <string_view>
#include <vector>

namespace fr {

enum class AssetKind { None = 0, WindowsSetup, WindowsZip, LinuxAppImage, LinuxDeb };

struct ReleaseAsset {
  std::string name;
  long long size = 0;
  std::string sha256;
  bool valid = false;
};

struct ReleaseManifest {
  int schema = 0;
  std::string version;  // 规范化（无 v 前缀、无构建元数据）
  std::string published_at;
  std::string release_url;
  std::string notes;    // 更新说明·中文（纯文本，≤2048 字节，UTF-8 边界安全截断）
  std::string notes_en; // 更新说明·英文（d519；缺省空 = 清单未携带，显示层回退中文）
  std::string key_id;
  std::string signature;
  ReleaseAsset windows_setup;
  ReleaseAsset windows_zip;
  ReleaseAsset linux_appimage;
  ReleaseAsset linux_deb;
  bool valid = false;

  const ReleaseAsset* asset(AssetKind kind) const;
  bool has_signature() const { return !signature.empty(); }
};

struct ReleaseApiAsset {
  std::string name;
  long long size = 0;
  std::string sha256;  // GitHub API digest（sha256:<hex>）；旧资产可能为空
};

struct ReleaseInfo {
  std::string tag_name;  // 原始标签（可能带 v）
  std::string version;   // 规范化版本；标签非法时为空
  std::string html_url;
  std::string published_at;
  std::string body;
  std::vector<ReleaseApiAsset> assets;
  bool prerelease = false;
  bool draft = false;
  bool valid = false;
};

bool parse_github_release(std::string_view json, ReleaseInfo* out, std::string* err);
bool parse_manifest(std::string_view json, ReleaseManifest* out, std::string* err);

// 兼容未携带 latest.json 的旧 Release（如 v0.6.3）：资产名/大小取自 API，
// SHA-256 优先取 API digest；缺哈希的资产留给 apply_sha256sums 补齐。
bool build_manifest_from_release(const ReleaseInfo& info, ReleaseManifest* out);
// 是否存在“资产名/大小有效但缺 SHA-256”的平台资产（需要补 SHA256SUMS.txt）。
bool manifest_needs_sha256sums(const ReleaseManifest& manifest);
// SHA256SUMS.txt（"<64hex>  <name>" 每行）：补齐缺失哈希；与 API digest 冲突则
// 该资产标记为不可用（宁可只提示也不下载来历不明的包）。
bool apply_sha256sums(std::string_view text, ReleaseManifest* manifest);
bool parse_atom_latest(std::string_view xml, std::string* tag, std::string* url,
                       std::string* err);

// 资产名安全白名单：拒绝路径分隔符、前导点、控制字符、超长与非白名单字符。
bool is_safe_asset_name(std::string_view name);
AssetKind classify_asset(std::string_view name);
// 资产名必须与版本、平台、架构完全匹配（AUTO_UPDATE_PLAN §4.2）。
bool asset_name_matches(std::string_view name, std::string_view version, AssetKind kind);
const char* asset_kind_key(AssetKind kind);  // latest.json 里的键名

// UTF-8 安全截断（不切断多字节序列）。
void truncate_utf8(std::string* s, size_t max_bytes);

// 双语 Release 正文切分（d519）：按「# 开头且含 (English) 的行」分界，前段=中文、
// 后段=英文（分界标题行本身不进任何一段）；无英文段时 en 为空、zh=全文。
// 两段各自剥轻量 markdown（行首 "#"+ 空格、行内 "**"）成纯文本。
void split_bilingual_notes(std::string_view body, std::string* zh, std::string* en);

}  // namespace fr
