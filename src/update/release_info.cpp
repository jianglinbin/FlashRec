#include "update/release_info.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

#include "update/json_lite.h"
#include "update/version.h"

namespace fr {

namespace {

constexpr size_t kMaxAssetName = 160;
constexpr size_t kMaxNotesBytes = 600;
constexpr long long kMaxAssetSize = 8LL * 1024 * 1024 * 1024;  // 8GB 防御上限

bool starts_with(std::string_view s, std::string_view p) {
  return s.size() >= p.size() && s.substr(0, p.size()) == p;
}
bool ends_with(std::string_view s, std::string_view p) {
  return s.size() >= p.size() && s.substr(s.size() - p.size()) == p;
}

std::string_view trim_view(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' ||
                        s.front() == '\n'))
    s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' ||
                        s.back() == '\n'))
    s.remove_suffix(1);
  return s;
}

bool is_hex64(std::string_view s) {
  if (s.size() != 64) return false;
  for (char c : s) {
    const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                    (c >= 'A' && c <= 'F');
    if (!ok) return false;
  }
  return true;
}

void sanitize_text(std::string* s) {
  std::string out;
  out.reserve(s->size());
  for (char c : *s) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 && c != '\n' && c != '\t') continue;
    out.push_back(c);
  }
  *s = std::move(out);
}

// latest.json 的单个资产条目 → ReleaseAsset（任何不一致都只让该资产不可用）
void parse_asset(const json::Value& assets, const char* key, std::string_view version,
                 AssetKind kind, ReleaseAsset* out) {
  *out = ReleaseAsset{};
  const json::Value* v = assets.find(key);
  if (!v || !v->is_object()) return;

  const json::Value* name = v->find("name");
  const json::Value* size = v->find("size");
  const json::Value* sha = v->find("sha256");
  if (!name || !name->is_string() || !size || !size->is_number() || !sha || !sha->is_string())
    return;

  if (!is_safe_asset_name(name->string)) return;
  if (!asset_name_matches(name->string, version, kind)) return;
  if (size->number <= 0 || size->number > static_cast<double>(kMaxAssetSize)) return;
  if (std::floor(size->number) != size->number) return;
  if (!is_hex64(sha->string)) return;

  out->name = name->string;
  out->size = static_cast<long long>(size->number);
  out->sha256 = sha->string;
  std::transform(out->sha256.begin(), out->sha256.end(), out->sha256.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  out->valid = true;
}

std::string xml_unescape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '&') {
      out.push_back(s[i]);
      continue;
    }
    const size_t semi = s.find(';', i + 1);
    if (semi == std::string_view::npos || semi - i > 10) {
      out.push_back(s[i]);
      continue;
    }
    const std::string_view ent = s.substr(i + 1, semi - i - 1);
    if (ent == "amp")
      out.push_back('&');
    else if (ent == "lt")
      out.push_back('<');
    else if (ent == "gt")
      out.push_back('>');
    else if (ent == "quot")
      out.push_back('"');
    else if (ent == "apos")
      out.push_back('\'');
    else if (ent == "#39")
      out.push_back('\'');
    else {
      out.append(s.substr(i, semi - i + 1));
    }
    i = semi;
  }
  return out;
}

std::string_view attr_value(std::string_view tag, std::string_view name) {
  const std::string pat = std::string(name) + "=\"";
  const size_t p = tag.find(pat);
  if (p == std::string_view::npos) return {};
  const size_t start = p + pat.size();
  const size_t end = tag.find('"', start);
  if (end == std::string_view::npos) return {};
  return tag.substr(start, end - start);
}

// GitHub 资产 digest："sha256:<64hex>"（前缀大小写不敏感，也接受裸 hex）。
std::string parse_api_digest(std::string_view digest) {
  constexpr std::string_view kPrefix = "sha256:";
  if (digest.size() >= kPrefix.size()) {
    bool prefix = true;
    for (size_t i = 0; i < kPrefix.size(); ++i) {
      if (std::tolower(static_cast<unsigned char>(digest[i])) != kPrefix[i]) {
        prefix = false;
        break;
      }
    }
    if (prefix) digest.remove_prefix(kPrefix.size());
  }
  if (!is_hex64(digest)) return {};
  std::string out(digest);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

}  // namespace

const ReleaseAsset* ReleaseManifest::asset(AssetKind kind) const {
  const ReleaseAsset* a = nullptr;
  switch (kind) {
    case AssetKind::WindowsSetup: a = &windows_setup; break;
    case AssetKind::WindowsZip: a = &windows_zip; break;
    case AssetKind::LinuxAppImage: a = &linux_appimage; break;
    case AssetKind::LinuxDeb: a = &linux_deb; break;
    default: return nullptr;
  }
  return a->valid ? a : nullptr;
}

bool is_safe_asset_name(std::string_view name) {
  if (name.empty() || name.size() > kMaxAssetName) return false;
  if (name.front() == '.') return false;
  if (name.find("..") != std::string_view::npos) return false;
  for (char c : name) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F) return false;
    const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                    (c >= 'A' && c <= 'Z') || c == '.' || c == '_' || c == '-' || c == '+';
    if (!ok) return false;
  }
  return true;
}

AssetKind classify_asset(std::string_view name) {
  if (!is_safe_asset_name(name)) return AssetKind::None;
  if (ends_with(name, "-win64-setup.exe")) return AssetKind::WindowsSetup;
  if (ends_with(name, "-win64.zip")) return AssetKind::WindowsZip;
  if (ends_with(name, "-x86_64.AppImage")) return AssetKind::LinuxAppImage;
  if (starts_with(name, "flashrec_") && ends_with(name, "_amd64.deb"))
    return AssetKind::LinuxDeb;
  return AssetKind::None;
}

bool asset_name_matches(std::string_view name, std::string_view version, AssetKind kind) {
  if (version.empty()) return false;
  std::string expected;
  switch (kind) {
    case AssetKind::WindowsSetup:
      expected = "FlashRec-" + std::string(version) + "-win64-setup.exe";
      break;
    case AssetKind::WindowsZip:
      expected = "FlashRec-" + std::string(version) + "-win64.zip";
      break;
    case AssetKind::LinuxAppImage:
      expected = "FlashRec-" + std::string(version) + "-x86_64.AppImage";
      break;
    case AssetKind::LinuxDeb:
      expected = "flashrec_" + std::string(version) + "_amd64.deb";
      break;
    default:
      return false;
  }
  return name == expected;
}

const char* asset_kind_key(AssetKind kind) {
  switch (kind) {
    case AssetKind::WindowsSetup: return "windows_setup";
    case AssetKind::WindowsZip: return "windows_zip";
    case AssetKind::LinuxAppImage: return "linux_appimage";
    case AssetKind::LinuxDeb: return "linux_deb";
    default: return "";
  }
}

void truncate_utf8(std::string* s, size_t max_bytes) {
  if (!s || s->size() <= max_bytes) return;
  size_t n = max_bytes;
  while (n > 0 && (static_cast<unsigned char>((*s)[n]) & 0xC0) == 0x80) --n;
  s->resize(n);
}

bool parse_github_release(std::string_view json, ReleaseInfo* out, std::string* err) {
  if (!out) return false;
  *out = ReleaseInfo{};
  json::Value root;
  if (!json::parse(json, &root, err)) return false;
  if (!root.is_object()) {
    if (err) *err = "release: root is not an object";
    return false;
  }
  const json::Value* tag = root.find("tag_name");
  if (!tag || !tag->is_string() || tag->string.empty()) {
    if (err) *err = "release: missing tag_name";
    return false;
  }
  out->tag_name = tag->string;
  out->html_url = root.get_string("html_url");
  out->published_at = root.get_string("published_at");
  out->body = root.get_string("body");
  sanitize_text(&out->body);
  truncate_utf8(&out->body, 8192);
  out->prerelease = root.get_bool("prerelease", false);
  out->draft = root.get_bool("draft", false);

  const json::Value* assets = root.find("assets");
  if (assets && assets->is_array()) {
    constexpr size_t kMaxAssets = 64;
    for (const json::Value& item : assets->array) {
      if (out->assets.size() >= kMaxAssets) break;
      if (!item.is_object()) continue;
      const json::Value* name = item.find("name");
      const json::Value* size = item.find("size");
      if (!name || !name->is_string() || !is_safe_asset_name(name->string)) continue;
      if (!size || !size->is_number() || size->number <= 0 ||
          size->number > static_cast<double>(kMaxAssetSize))
        continue;
      if (std::floor(size->number) != size->number) continue;
      ReleaseApiAsset asset;
      asset.name = name->string;
      asset.size = static_cast<long long>(size->number);
      const json::Value* digest = item.find("digest");
      if (digest && digest->is_string()) asset.sha256 = parse_api_digest(digest->string);
      out->assets.push_back(std::move(asset));
    }
  }

  SemVer v;
  if (parse_semver(out->tag_name, &v)) out->version = semver_to_string(v);
  out->valid = true;
  return true;
}

bool parse_manifest(std::string_view json, ReleaseManifest* out, std::string* err) {
  if (!out) return false;
  *out = ReleaseManifest{};
  json::Value root;
  if (!json::parse(json, &root, err)) return false;
  if (!root.is_object()) {
    if (err) *err = "manifest: root is not an object";
    return false;
  }
  const json::Value* schema = root.find("schema");
  if (!schema || !schema->is_number()) {
    if (err) *err = "manifest: missing schema";
    return false;
  }
  const int sch = static_cast<int>(schema->number);
  if (schema->number != static_cast<double>(sch) || sch != 1) {
    if (err) *err = "manifest: unsupported schema";
    return false;
  }
  out->schema = sch;

  const json::Value* ver = root.find("version");
  if (!ver || !ver->is_string()) {
    if (err) *err = "manifest: missing version";
    return false;
  }
  SemVer v;
  if (!parse_semver(ver->string, &v)) {
    if (err) *err = "manifest: invalid version";
    return false;
  }
  out->version = semver_to_string(v);

  out->published_at = root.get_string("published_at");
  out->release_url = root.get_string("release_url");
  if (!out->release_url.empty() && !starts_with(out->release_url, "https://"))
    out->release_url.clear();
  out->notes = root.get_string("notes");
  sanitize_text(&out->notes);
  truncate_utf8(&out->notes, kMaxNotesBytes);
  out->key_id = root.get_string("key_id");
  out->signature = root.get_string("signature");

  const json::Value* assets = root.find("assets");
  if (assets && assets->is_object()) {
    parse_asset(*assets, "windows_setup", out->version, AssetKind::WindowsSetup,
                &out->windows_setup);
    parse_asset(*assets, "windows_zip", out->version, AssetKind::WindowsZip,
                &out->windows_zip);
    parse_asset(*assets, "linux_appimage", out->version, AssetKind::LinuxAppImage,
                &out->linux_appimage);
    parse_asset(*assets, "linux_deb", out->version, AssetKind::LinuxDeb, &out->linux_deb);
  }
  out->valid = true;
  return true;
}

bool build_manifest_from_release(const ReleaseInfo& info, ReleaseManifest* out) {
  if (!out) return false;
  *out = ReleaseManifest{};
  if (!info.valid || info.version.empty()) return false;

  out->schema = 1;
  out->version = info.version;
  out->published_at = info.published_at;
  if (starts_with(info.html_url, "https://")) out->release_url = info.html_url;
  out->notes = info.body;
  sanitize_text(&out->notes);
  truncate_utf8(&out->notes, kMaxNotesBytes);

  for (const ReleaseApiAsset& src : info.assets) {
    const AssetKind kind = classify_asset(src.name);
    if (kind == AssetKind::None) continue;
    if (!asset_name_matches(src.name, out->version, kind)) continue;
    if (src.size <= 0 || src.size > kMaxAssetSize) continue;
    ReleaseAsset* dst = nullptr;
    switch (kind) {
      case AssetKind::WindowsSetup: dst = &out->windows_setup; break;
      case AssetKind::WindowsZip: dst = &out->windows_zip; break;
      case AssetKind::LinuxAppImage: dst = &out->linux_appimage; break;
      case AssetKind::LinuxDeb: dst = &out->linux_deb; break;
      default: continue;
    }
    if (!dst->name.empty()) continue;
    dst->name = src.name;
    dst->size = src.size;
    dst->sha256 = src.sha256;
    dst->valid = !dst->sha256.empty();  // 无哈希的资产留给 SHA256SUMS 补齐
  }
  out->valid = true;
  return true;
}

bool manifest_needs_sha256sums(const ReleaseManifest& manifest) {
  const ReleaseAsset* all[] = {&manifest.windows_setup, &manifest.windows_zip,
                               &manifest.linux_appimage, &manifest.linux_deb};
  for (const ReleaseAsset* a : all) {
    if (!a->name.empty() && a->size > 0 && a->sha256.empty()) return true;
  }
  return false;
}

bool apply_sha256sums(std::string_view text, ReleaseManifest* manifest) {
  if (!manifest) return false;
  std::map<std::string, std::string> sums;
  size_t pos = 0;
  int parsed = 0;
  while (pos < text.size()) {
    const size_t eol = text.find('\n', pos);
    std::string_view line =
        text.substr(pos, eol == std::string_view::npos ? std::string_view::npos : eol - pos);
    pos = eol == std::string_view::npos ? text.size() : eol + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    line = trim_view(line);
    if (line.empty() || line.front() == '#') continue;

    size_t sep = 0;
    while (sep < line.size() && line[sep] != ' ' && line[sep] != '\t') ++sep;
    if (sep == line.size()) continue;
    const std::string_view hash = line.substr(0, sep);
    std::string_view name = trim_view(line.substr(sep));
    if (!name.empty() && name.front() == '*') name.remove_prefix(1);
    if (!is_hex64(hash) || !is_safe_asset_name(name)) continue;

    std::string value(hash);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::string key(name);
    auto it = sums.find(key);
    if (it == sums.end())
      sums.emplace(key, std::move(value));
    else if (it->second != value)
      it->second.clear();  // 同一文件出现两个哈希：视为不可信
    ++parsed;
  }
  if (parsed == 0) return false;

  ReleaseAsset* all[] = {&manifest->windows_setup, &manifest->windows_zip,
                         &manifest->linux_appimage, &manifest->linux_deb};
  for (ReleaseAsset* a : all) {
    if (a->name.empty()) continue;
    const auto it = sums.find(a->name);
    if (it == sums.end()) continue;
    if (it->second.empty()) {
      a->valid = false;
      a->sha256.clear();
      continue;
    }
    if (!a->sha256.empty()) {
      if (a->sha256 != it->second) {
        a->valid = false;
        a->sha256.clear();
      }
      continue;
    }
    a->sha256 = it->second;
    a->valid = a->size > 0;
  }
  return true;
}

bool parse_atom_latest(std::string_view xml, std::string* tag, std::string* url,
                       std::string* err) {
  if (tag) tag->clear();
  if (url) url->clear();
  const size_t entry = xml.find("<entry");
  if (entry == std::string_view::npos) {
    if (err) *err = "atom: no entry";
    return false;
  }
  const size_t entry_end = xml.find("</entry>", entry);
  const std::string_view e =
      xml.substr(entry, entry_end == std::string_view::npos ? std::string_view::npos
                                                            : entry_end - entry);

  const size_t t0 = e.find("<title");
  if (t0 == std::string_view::npos) {
    if (err) *err = "atom: no title";
    return false;
  }
  const size_t t_open = e.find('>', t0);
  const size_t t_close = e.find("</title>", t_open == std::string_view::npos ? t0 : t_open);
  if (t_open == std::string_view::npos || t_close == std::string_view::npos) {
    if (err) *err = "atom: malformed title";
    return false;
  }
  const std::string title =
      xml_unescape(trim_view(e.substr(t_open + 1, t_close - t_open - 1)));
  if (title.empty()) {
    if (err) *err = "atom: empty title";
    return false;
  }

  std::string link;
  const size_t l0 = e.find("<link");
  if (l0 != std::string_view::npos) {
    const size_t l_end = e.find('>', l0);
    if (l_end != std::string_view::npos) {
      const std::string_view tag_text = e.substr(l0, l_end - l0 + 1);
      const std::string_view href = attr_value(tag_text, "href");
      link = xml_unescape(href);
    }
  }

  if (tag) *tag = title;
  if (url) *url = std::move(link);
  return true;
}

}  // namespace fr
