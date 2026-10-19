// v0.6.4 M1：更新核心（semver / JSON / 清单 / 配置）单测。
// 构建：cmake -DFLASHREC_BUILD_TESTS=ON ... && ctest --output-on-failure
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "app/config.h"
#include "platform/update_install.h"
#include "update/detector.h"
#include "update/downloader.h"
#include "update/endpoint_policy.h"
#include "update/http_client.h"
#include "update/json_lite.h"
#include "update/release_info.h"
#include "update/update_service.h"
#include "update/verify.h"
#include "update/version.h"

namespace {

int g_fail = 0;
int g_pass = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (cond) {                                                              \
      ++g_pass;                                                              \
    } else {                                                                 \
      ++g_fail;                                                              \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " << #cond \
                << "\n";                                                     \
    }                                                                        \
  } while (0)

#define CHECK_EQ(a, b)                                                       \
  do {                                                                       \
    const auto va = (a);                                                     \
    const auto vb = (b);                                                     \
    if (va == vb) {                                                          \
      ++g_pass;                                                              \
    } else {                                                                 \
      ++g_fail;                                                              \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " << #a      \
                << " == " << #b << "  (" << va << " vs " << vb << ")\n";     \
    }                                                                        \
  } while (0)

bool valid_utf8(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    size_t n = 0;
    if (c < 0x80)
      n = 1;
    else if ((c & 0xE0) == 0xC0)
      n = 2;
    else if ((c & 0xF0) == 0xE0)
      n = 3;
    else if ((c & 0xF8) == 0xF0)
      n = 4;
    else
      return false;
    if (i + n > s.size()) return false;
    for (size_t k = 1; k < n; ++k)
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
    i += n;
  }
  return true;
}

void test_semver() {
  fr::SemVer v;
  CHECK(fr::parse_semver("v0.6.5", &v));
  CHECK(v.valid);
  CHECK_EQ(v.major, 0);
  CHECK_EQ(v.minor, 6);
  CHECK_EQ(v.patch, 5);

  CHECK(fr::parse_semver("0.6.5-beta.1", &v));
  CHECK_EQ(fr::semver_to_string(v), std::string("0.6.5-beta.1"));
  CHECK(fr::parse_semver("0.6.5+build.7", &v));
  CHECK_EQ(fr::semver_to_string(v), std::string("0.6.5"));

  CHECK(fr::is_newer_version("0.6.10", "0.6.9"));
  CHECK(!fr::is_newer_version("0.6.9", "0.6.10"));
  CHECK(!fr::is_newer_version("v0.6.4", "0.6.4"));
  CHECK(fr::is_newer_version("0.6.5", "0.6.5-beta.1"));
  CHECK(!fr::is_newer_version("0.6.5-beta.1", "0.6.5"));
  CHECK(fr::is_newer_version("0.6.5-beta.10", "0.6.5-beta.2"));
  CHECK(fr::is_newer_version("0.6.5-beta.2", "0.6.5-beta.1"));
  CHECK(fr::is_newer_version("0.6.5-alpha.1", "0.6.5-1"));  // 数字 < 字母
  CHECK(fr::is_newer_version("0.6.5-alpha.1", "0.6.5-alpha"));  // 短者 < 长者
  CHECK(!fr::is_newer_version("0.6.5-alpha", "0.6.5-alpha.1"));

  CHECK(!fr::parse_semver("v0.6", &v));
  CHECK(!fr::parse_semver("0.6.5.1", &v));
  CHECK(!fr::parse_semver("0.6.5-", &v));
  CHECK(!fr::parse_semver("01.6.5", &v));
  CHECK(!fr::parse_semver("0.6.5-beta..1", &v));
  CHECK(!fr::parse_semver("0.6.5-beta.01", &v));
  CHECK(!fr::parse_semver("", &v));
  CHECK(!fr::is_newer_version("nope", "0.6.4"));
  CHECK(!fr::is_newer_version("0.6.5", "nope"));
}

void test_json() {
  const std::string text =
      "{\"a\":1,\"b\":true,\"c\":null,\"d\":\"x\\u4e2d\\ud83d\\ude00\","
      "\"nested\":{\"k\":[1,2,3]},\"dup\":1,\"dup\":2}";
  fr::json::Value v;
  std::string err;
  CHECK(fr::json::parse(text, &v, &err));
  CHECK(v.is_object());
  CHECK_EQ(v.get_number("a", 0), 1.0);
  CHECK(v.get_bool("b", false));
  CHECK(v.find("c") && v.find("c")->is_null());
  CHECK_EQ(v.get_string("d"), std::string("x中😀"));
  const fr::json::Value* nested = v.find("nested");
  CHECK(nested && nested->is_object());
  const fr::json::Value* arr = nested ? nested->find("k") : nullptr;
  CHECK(arr && arr->is_array() && arr->array.size() == 3);
  CHECK_EQ(v.get_number("dup", 0), 2.0);  // 重复键后者覆盖

  CHECK(!fr::json::parse("{\"a\":1,}", &v, &err));        // 尾逗号
  CHECK(!fr::json::parse("{\"a\":01}", &v, &err));        // 前导零
  CHECK(!fr::json::parse("{\"a\":1e999}", &v, &err));     // 溢出
  CHECK(!fr::json::parse("{\"a\":\"\\ud800\"}", &v, &err));  // 孤高代理
  CHECK(!fr::json::parse("{\"a\":\"x\ny\"}", &v, &err));  // 裸控制字符
  CHECK(!fr::json::parse("{\"a\":1} trailing", &v, &err));
  CHECK(!fr::json::parse("1234567890", &v, &err, 5));     // 体积上限

  std::string deep;
  for (int i = 0; i < 40; ++i) deep += "[";
  for (int i = 0; i < 40; ++i) deep += "]";
  CHECK(!fr::json::parse(deep, &v, &err));  // 深度上限
}

void test_assets() {
  CHECK(fr::is_safe_asset_name("FlashRec-0.6.5-win64.zip"));
  CHECK(!fr::is_safe_asset_name("../evil.zip"));
  CHECK(!fr::is_safe_asset_name("a/b.zip"));
  CHECK(!fr::is_safe_asset_name("a\\b.zip"));
  CHECK(!fr::is_safe_asset_name(".hidden"));
  CHECK(!fr::is_safe_asset_name(""));
  CHECK(!fr::is_safe_asset_name("a b.zip"));

  CHECK(fr::classify_asset("FlashRec-0.6.5-win64-setup.exe") == fr::AssetKind::WindowsSetup);
  CHECK(fr::classify_asset("FlashRec-0.6.5-win64.zip") == fr::AssetKind::WindowsZip);
  CHECK(fr::classify_asset("FlashRec-0.6.5-x86_64.AppImage") == fr::AssetKind::LinuxAppImage);
  CHECK(fr::classify_asset("flashrec_0.6.5_amd64.deb") == fr::AssetKind::LinuxDeb);
  CHECK(fr::classify_asset("something.bin") == fr::AssetKind::None);
  CHECK(fr::asset_name_matches("FlashRec-0.6.5-win64.zip", "0.6.5", fr::AssetKind::WindowsZip));
  CHECK(!fr::asset_name_matches("FlashRec-0.6.6-win64.zip", "0.6.5",
                                fr::AssetKind::WindowsZip));
}

std::string manifest_json(const std::string& version = "0.6.5") {
  const std::string sha(64, 'a');
  std::string asset_version = version;
  if (!asset_version.empty() && (asset_version.front() == 'v' || asset_version.front() == 'V'))
    asset_version.erase(asset_version.begin());
  std::string t = R"({
    "schema": 1,
    "version": "@VER@",
    "published_at": "2026-11-01T12:00:00Z",
    "release_url": "https://github.com/jianglinbin/FlashRec/releases/tag/@VER@",
    "notes": "fixes\nand improvements",
    "assets": {
      "windows_setup": {"name": "FlashRec-@AVER@-win64-setup.exe",
                        "size": 123, "sha256": "@SHA@"},
      "windows_zip":   {"name": "FlashRec-@AVER@-win64.zip",
                        "size": 456, "sha256": "@SHA@"},
      "linux_appimage":{"name": "FlashRec-@AVER@-x86_64.AppImage",
                        "size": 789, "sha256": "@SHA@"},
      "linux_deb":     {"name": "flashrec_@AVER@_amd64.deb",
                        "size": 321, "sha256": "@SHA@"}
    },
    "key_id": "update-2026",
    "signature": "deadbeef"
  })";
  for (size_t p = t.find("@VER@"); p != std::string::npos; p = t.find("@VER@", p + version.size()))
    t.replace(p, 5, version);
  for (size_t p = t.find("@AVER@"); p != std::string::npos;
       p = t.find("@AVER@", p + asset_version.size()))
    t.replace(p, 6, asset_version);
  for (size_t p = t.find("@SHA@"); p != std::string::npos; p = t.find("@SHA@", p + 64))
    t.replace(p, 5, sha);
  return t;
}

void test_manifest() {
  fr::ReleaseManifest m;
  std::string err;
  CHECK(fr::parse_manifest(manifest_json("v0.6.5"), &m, &err));
  CHECK(m.valid);
  CHECK_EQ(m.version, std::string("0.6.5"));
  CHECK(m.asset(fr::AssetKind::WindowsSetup) != nullptr);
  CHECK(m.asset(fr::AssetKind::WindowsZip) != nullptr);
  CHECK(m.asset(fr::AssetKind::LinuxAppImage) != nullptr);
  CHECK(m.asset(fr::AssetKind::LinuxDeb) != nullptr);
  CHECK_EQ(m.windows_zip.size, 456LL);
  CHECK_EQ(m.windows_zip.sha256, std::string(64, 'a'));
  CHECK_EQ(m.notes, std::string("fixes\nand improvements"));
  CHECK(m.has_signature());

  // 版本/资产名不一致 → 该资产不可用，清单本身仍可用
  std::string bad = manifest_json();
  const size_t p = bad.find("FlashRec-0.6.5-win64.zip");
  CHECK(p != std::string::npos);
  bad.replace(p, 24, "FlashRec-0.6.4-win64.zip");
  CHECK(fr::parse_manifest(bad, &m, &err));
  CHECK(m.asset(fr::AssetKind::WindowsZip) == nullptr);
  CHECK(m.asset(fr::AssetKind::WindowsSetup) != nullptr);

  // 非法版本 / schema → 整体失败
  CHECK(!fr::parse_manifest(manifest_json("not-a-version"), &m, &err));
  std::string schema2 = manifest_json();
  schema2.replace(schema2.find("\"schema\": 1"), 11, "\"schema\": 2");
  CHECK(!fr::parse_manifest(schema2, &m, &err));

  // notes 超长 → 600 字节内且 UTF-8 完整
  std::string big = manifest_json();
  const size_t n = big.find("fixes");
  std::string notes;
  for (int i = 0; i < 300; ++i) notes += "中";
  big.replace(n, 5, notes);
  CHECK(fr::parse_manifest(big, &m, &err));
  CHECK(m.notes.size() <= 600);
  CHECK(valid_utf8(m.notes));
}

void test_github_release() {
  fr::ReleaseInfo r;
  std::string err;
  CHECK(fr::parse_github_release(
      R"({"tag_name":"v0.6.5","html_url":"https://github.com/jianglinbin/FlashRec/releases/tag/v0.6.5",
          "published_at":"2026-11-01T12:00:00Z","prerelease":false,"draft":false,"body":"notes"})",
      &r, &err));
  CHECK(r.valid);
  CHECK_EQ(r.version, std::string("0.6.5"));
  CHECK_EQ(r.tag_name, std::string("v0.6.5"));
  CHECK_EQ(r.body, std::string("notes"));

  CHECK(fr::parse_github_release(R"({"tag_name":"nightly","prerelease":true})", &r, &err));
  CHECK(r.valid);
  CHECK(r.version.empty());  // 非法标签由调用方忽略
  CHECK(r.prerelease);

  CHECK(!fr::parse_github_release(R"({"html_url":"x"})", &r, &err));
  CHECK(!fr::parse_github_release("<html>rate limited</html>", &r, &err));

  // assets：digest 归一化为小写；非法资产名被丢弃；digest 可缺失
  const std::string json = R"({"tag_name":"v0.6.5","assets":[)"
                           R"({"name":"FlashRec-0.6.5-win64-setup.exe","size":100,)"
                           R"("digest":"sha256:)" + std::string(64, 'A') + R"("},)"
                           R"({"name":"../evil.exe","size":10,"digest":"sha256:)" +
                           std::string(64, 'b') + R"("},)"
                           R"({"name":"FlashRec-0.6.5-win64.zip","size":200}]})";
  CHECK(fr::parse_github_release(json, &r, &err));
  CHECK_EQ(r.assets.size(), std::size_t(2));
  if (r.assets.size() == 2) {
    CHECK_EQ(r.assets[0].name, std::string("FlashRec-0.6.5-win64-setup.exe"));
    CHECK_EQ(r.assets[0].size, 100LL);
    CHECK_EQ(r.assets[0].sha256, std::string(64, 'a'));
    CHECK_EQ(r.assets[1].name, std::string("FlashRec-0.6.5-win64.zip"));
    CHECK(r.assets[1].sha256.empty());
  }
}

void test_atom() {
  std::string tag, url, err;
  CHECK(fr::parse_atom_latest(
      R"(<?xml version="1.0"?><feed><entry><title>v0.6.5</title>
         <link rel="alternate" type="text/html"
               href="https://github.com/jianglinbin/FlashRec/releases/tag/v0.6.5"/></entry></feed>)",
      &tag, &url, &err));
  CHECK_EQ(tag, std::string("v0.6.5"));
  CHECK_EQ(url, std::string("https://github.com/jianglinbin/FlashRec/releases/tag/v0.6.5"));

  CHECK(!fr::parse_atom_latest("<feed></feed>", &tag, &url, &err));
}

void test_config() {
  const auto dir = std::filesystem::temp_directory_path();
  const auto path = dir / "flashrec_update_tests_settings.json";
  {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << R"({
      "update": {
        "enabled": true,
        "auto_check": false,
        "check_interval_hours": 0,
        "startup_delay_sec": 9999,
        "direct_enabled": false,
        "api_base": "http://evil.example/",
        "probe": {"timeout_ms": 100, "concurrency": 99, "cache_sec": -5},
        "proxies": [
          {"name": "ok", "prefix": "https://gh-proxy.com", "enabled": true},
          {"name": "bad", "prefix": "http://example.com/", "enabled": true},
          {"name": "tmpl", "template": "https://moeyy.cn/gh-proxy/?url={url}", "enabled": false},
          {"name": "both", "prefix": "https://a/", "template": "https://b/?url={url}"}
        ]
      }
    })";
  }
  fr::Config c = fr::Config::load(path.string());
  CHECK(!c.update.auto_check);
  CHECK_EQ(c.update.check_interval_hours, 1);
  CHECK_EQ(c.update.startup_delay_sec, 600);
  CHECK(!c.update.direct_enabled);
  CHECK(c.update.api_base.empty());
  CHECK_EQ(c.update.probe.timeout_ms, 500);
  CHECK_EQ(c.update.probe.concurrency, 8);
  CHECK_EQ(c.update.probe.cache_sec, 0);
  CHECK_EQ(c.update.proxies.size(), size_t(2));
  if (c.update.proxies.size() == 2) {
    CHECK_EQ(c.update.proxies[0].name, std::string("ok"));
    CHECK_EQ(c.update.proxies[0].prefix, std::string("https://gh-proxy.com/"));
    CHECK_EQ(c.update.proxies[1].url_template,
             std::string("https://moeyy.cn/gh-proxy/?url={url}"));
    CHECK(!c.update.proxies[1].enabled);
  }

  CHECK(fr::Config::patch(path.string(),
                          {{"update", "skipped_version", "\"0.6.5\""},
                           {"update", "last_check_at", "12345"}}));
  fr::Config c2 = fr::Config::load(path.string());
  CHECK_EQ(c2.update.skipped_version, std::string("0.6.5"));
  CHECK_EQ(c2.update.last_check_at, 12345LL);

  std::error_code ec;
  std::filesystem::remove(path, ec);
}

void test_url_guard() {
  using fr::update::is_allowed_update_url;
  CHECK(is_allowed_update_url("https://github.com/jianglinbin/FlashRec", false));
  CHECK(is_allowed_update_url("https://gh-proxy.com/https://api.github.com/x", false));
  CHECK(!is_allowed_update_url("http://github.com/jianglinbin/FlashRec", false));
  CHECK(!is_allowed_update_url("http://github.com/jianglinbin/FlashRec", true));
  CHECK(is_allowed_update_url("http://127.0.0.1:8080/json", true));
  CHECK(is_allowed_update_url("http://localhost:9000/", true));
  CHECK(is_allowed_update_url("http://[::1]:49152/x", true));
  CHECK(!is_allowed_update_url("http://192.168.1.10/x", true));
  CHECK(!is_allowed_update_url("http://127.0.0.1@evil.example/x", true));
  CHECK(!is_allowed_update_url("https://user@github.com/x", false));
  CHECK(!is_allowed_update_url("https://github.com/a b", false));
  CHECK(!is_allowed_update_url("ftp://github.com/x", false));
  CHECK(!is_allowed_update_url("", false));
}

void test_sha256() {
  using fr::update::Sha256;
  using fr::update::sha256_hex;
  CHECK_EQ(sha256_hex(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  CHECK_EQ(sha256_hex("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  CHECK_EQ(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
           std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  const std::string million(1000000, 'a');
  CHECK_EQ(sha256_hex(million),
           std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));

  Sha256 split;
  split.update("ab", 2);
  split.update("c", 1);
  CHECK_EQ(split.finish_hex(), sha256_hex("abc"));
  CHECK_EQ(split.finish_hex(), std::string(""));  // 二次 finish 不再产出

  std::string payload;
  for (int i = 0; i < 1000; ++i) payload += static_cast<char>('a' + i % 26);
  Sha256 chunked;
  for (std::size_t i = 0; i < payload.size(); i += 7)
    chunked.update(payload.data() + i, std::min<std::size_t>(7, payload.size() - i));
  CHECK_EQ(chunked.finish_hex(), sha256_hex(payload));

  const auto dir = std::filesystem::temp_directory_path();
  const auto path = dir / "flashrec_sha256_test.bin";
  {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << payload;
  }
  std::string file_hex;
  std::string err;
  CHECK(fr::update::sha256_file(path.string(), &file_hex, &err));
  CHECK_EQ(file_hex, sha256_hex(payload));
  std::error_code ec;
  std::filesystem::remove(path, ec);
  CHECK(!fr::update::sha256_file((dir / "flashrec_missing_sha.bin").string(), &file_hex, &err));
}

void test_endpoint_policy() {
  using namespace fr::update;
  fr::UpdateConfig cfg;
  cfg.direct_enabled = true;
  cfg.proxies.push_back({"", "https://p1.test/", "", true});
  cfg.proxies.push_back({"a", "https://p2.test", "", true});
  cfg.proxies.push_back({"a", "https://p3.test/", "", true});
  cfg.proxies.push_back({"t", "", "https://t.test/?url={url}", true});
  cfg.proxies.push_back({"bad", "", "", true});
  cfg.proxies.push_back({"off", "https://off.test/", "", false});

  const auto endpoints = build_endpoints(cfg);
  CHECK_EQ(endpoints.size(), std::size_t(5));
  if (endpoints.size() == 5) {
    CHECK_EQ(endpoints[0].id, std::string("direct"));
    CHECK(endpoints[0].direct);
    CHECK_EQ(endpoints[1].id, std::string("proxy1"));
    CHECK_EQ(endpoints[1].prefix, std::string("https://p1.test/"));
    CHECK_EQ(endpoints[2].id, std::string("a"));
    CHECK_EQ(endpoints[3].id, std::string("a#2"));
    CHECK_EQ(endpoints[4].id, std::string("t"));
    CHECK_EQ(endpoint_url(endpoints[0], "https://github.com/x"),
             std::string("https://github.com/x"));
    CHECK_EQ(endpoint_url(endpoints[1], "https://github.com/x"),
             std::string("https://p1.test/https://github.com/x"));
    CHECK_EQ(endpoint_url(endpoints[2], "https://github.com/x"),
             std::string("https://p2.test/https://github.com/x"));  // 补尾部 '/'
    CHECK_EQ(endpoint_url(endpoints[4], "https://github.com/a b"),
             std::string("https://t.test/?url=https%3A%2F%2Fgithub.com%2Fa%20b"));
  }

  cfg.direct_enabled = false;
  const auto no_direct = build_endpoints(cfg);
  CHECK_EQ(no_direct.size(), std::size_t(4));
  if (!no_direct.empty()) CHECK(!no_direct[0].direct);

  cfg.proxies.clear();
  cfg.direct_enabled = false;
  CHECK(build_endpoints(cfg).empty());

  RankedEndpoint direct;
  direct.endpoint.id = "direct";
  direct.endpoint.order = 0;
  direct.metrics.bytes_per_sec = 100.0;
  RankedEndpoint px1;
  px1.endpoint.id = "a";
  px1.endpoint.order = 1;
  px1.metrics.bytes_per_sec = 900.0;
  RankedEndpoint px2;
  px2.endpoint.id = "b";
  px2.endpoint.order = 2;
  px2.metrics.bytes_per_sec = 900.0;
  auto ranked = rank_race({direct, px1, px2});
  CHECK_EQ(ranked.size(), std::size_t(3));
  if (ranked.size() == 3) {
    CHECK_EQ(ranked[0].endpoint.id, std::string("a"));  // 同吞吐按配置顺序
    CHECK_EQ(ranked[1].endpoint.id, std::string("b"));
    CHECK_EQ(ranked[2].endpoint.id, std::string("direct"));
  }
  CHECK_EQ(pick_with_sticky(ranked, "a"), std::size_t(0));
  CHECK_EQ(pick_with_sticky(ranked, "b"), std::size_t(1));  // 900 ≥ 900/1.15
  CHECK_EQ(pick_with_sticky(ranked, "direct"), std::size_t(0));  // 100 < 782
  CHECK_EQ(pick_with_sticky(ranked, "missing"), std::size_t(0));
  CHECK_EQ(pick_with_sticky({}, "a"), kNoEndpoint);

  EndpointHealth health;
  health.note_failure("direct", 100);
  health.note_failure("direct", 101);
  CHECK(health.available("direct", 101));  // 阈值 3 次前不冷却
  health.note_failure("direct", 102);
  CHECK_EQ(health.consecutive_failures("direct"), 3);
  CHECK(!health.available("direct", 1901));
  CHECK(health.available("direct", 1902));  // 首次 30 分钟
  health.note_failure("direct", 1902);
  CHECK(!health.available("direct", 2201));
  CHECK(health.available("direct", 2202));  // 第二次 5 分钟
  health.note_failure("direct", 2202);
  CHECK(health.available("direct", 3102));  // 第三次 15 分钟
  health.note_failure("direct", 3102);
  CHECK(health.available("direct", 6702));  // 第四次 60 分钟
  health.note_success("direct");
  CHECK_EQ(health.consecutive_failures("direct"), 0);
  CHECK(health.available("direct", 6702));

  EndpointPolicy policy(cfg);
  fr::UpdateConfig none;
  none.direct_enabled = false;
  policy.configure(none);
  CHECK(policy.endpoints().empty());
  fr::UpdateConfig cfg2;
  cfg2.direct_enabled = true;
  cfg2.proxies.push_back({"px", "https://px.test/", "", true});
  policy.configure(cfg2);
  policy.note_failure("direct", 10);
  policy.note_failure("direct", 11);
  policy.note_failure("direct", 12);
  CHECK_EQ(policy.candidates(false, 20).size(), std::size_t(1));
  CHECK_EQ(policy.candidates(true, 20).size(), std::size_t(2));  // 手动绕过冷却
  policy.note_success("direct");
  CHECK_EQ(policy.candidates(false, 20).size(), std::size_t(2));
}

std::string release_json(const std::string& tag) {
  return std::string(R"({"tag_name":")") + tag +
         R"(","html_url":"https://github.com/jianglinbin/FlashRec/releases/tag/)" + tag +
         R"(","published_at":"2026-11-01T12:00:00Z","prerelease":false,"draft":false,"body":"notes"})";
}

// GitHub Releases API 带 assets 的响应（模拟真实 releases/latest）。
std::string release_assets_json(const std::string& version, const std::string& sha,
                                bool with_digest) {
  auto asset = [&](const std::string& name, long long size) {
    std::string s = R"({"name":")" + name + R"(","size":)" + std::to_string(size);
    if (with_digest) s += R"(,"digest":"sha256:)" + sha + R"(")";
    s += "}";
    return s;
  };
  return R"({"tag_name":"v)" + version +
         R"(","html_url":"https://github.com/jianglinbin/FlashRec/releases/tag/v)" + version +
         R"(","published_at":"2026-11-01T12:00:00Z","prerelease":false,"draft":false,)"
         R"("body":"notes","assets":[)" +
         asset("FlashRec-" + version + "-win64-setup.exe", 1000) + "," +
         asset("FlashRec-" + version + "-win64.zip", 2000) + "," +
         asset("FlashRec-" + version + "-x86_64.AppImage", 3000) + "," +
         asset("flashrec_" + version + "_amd64.deb", 4000) + "]}";
}

std::string sha256sums_text(const std::string& version, const std::string& sha) {
  return sha + "  FlashRec-" + version + "-win64-setup.exe\n" + sha + "  FlashRec-" + version +
         "-win64.zip\n" + sha + "  FlashRec-" + version + "-x86_64.AppImage\n" + sha +
         "  flashrec_" + version + "_amd64.deb\n";
}

void test_sha256sums() {
  fr::ReleaseInfo info;
  std::string err;
  CHECK(fr::parse_github_release(release_assets_json("0.6.5", "", false), &info, &err));
  CHECK_EQ(info.assets.size(), std::size_t(4));

  fr::ReleaseManifest m;
  CHECK(fr::build_manifest_from_release(info, &m));
  CHECK(m.valid);
  CHECK_EQ(m.version, std::string("0.6.5"));
  CHECK_EQ(m.windows_setup.name, std::string("FlashRec-0.6.5-win64-setup.exe"));
  CHECK_EQ(m.windows_setup.size, 1000LL);
  CHECK(!m.windows_setup.valid);  // 无 digest → 等 SHA256SUMS
  CHECK(fr::manifest_needs_sha256sums(m));

  const std::string sums = sha256sums_text("0.6.5", std::string(64, 'B'));
  CHECK(fr::apply_sha256sums(sums, &m));
  CHECK(m.windows_setup.valid);
  CHECK(m.linux_deb.valid);
  CHECK_EQ(m.windows_setup.sha256, std::string(64, 'b'));
  CHECK(!fr::manifest_needs_sha256sums(m));
  CHECK(!fr::apply_sha256sums("not a sums file\n", &m));

  // digest 与 SHA256SUMS 冲突：该资产不可下载（宁可只提示）
  fr::ReleaseInfo info2;
  CHECK(fr::parse_github_release(release_assets_json("0.6.5", std::string(64, 'a'), true),
                                 &info2, &err));
  fr::ReleaseManifest m2;
  CHECK(fr::build_manifest_from_release(info2, &m2));
  CHECK(m2.windows_setup.valid);
  CHECK(fr::apply_sha256sums(sha256sums_text("0.6.5", std::string(64, 'c')), &m2));
  CHECK(!m2.windows_setup.valid);
  CHECK(m2.windows_setup.sha256.empty());

  // 同一文件两个哈希 → 不可信
  fr::ReleaseManifest m3;
  CHECK(fr::build_manifest_from_release(info, &m3));
  CHECK(fr::apply_sha256sums(sums + std::string(64, 'd') +
                                 "  FlashRec-0.6.5-win64-setup.exe\n",
                             &m3));
  CHECK(!m3.windows_setup.valid);
}

fr::update::HttpResult mock_ok(std::string body, double bps, long long ttfb = 10) {
  fr::update::HttpResult r;
  r.metrics.response_code = 200;
  r.metrics.body_bytes = static_cast<long long>(body.size());
  r.metrics.bytes_per_sec = bps;
  r.metrics.ttfb_ms = ttfb;
  r.metrics.total_ms = bps > 0 ? static_cast<long long>(body.size() * 1000.0 / bps) : 1;
  r.body = std::move(body);
  return r;
}

fr::update::HttpResult mock_status(long code) {
  fr::update::HttpResult r;
  r.error = fr::update::HttpError::HttpStatus;
  r.error_text = "HTTP " + std::to_string(code);
  r.metrics.response_code = code;
  return r;
}

fr::update::HttpResult mock_error(fr::update::HttpError error) {
  fr::update::HttpResult r;
  r.error = error;
  r.error_text = fr::update::http_error_name(error);
  return r;
}

fr::update::BatchFetch mock_fetch(
    std::function<fr::update::HttpResult(const fr::update::HttpRequest&)> handler) {
  return [handler](const std::vector<fr::update::HttpRequest>& requests, int,
                   const std::atomic<bool>*) {
    std::vector<fr::update::HttpResult> out;
    out.reserve(requests.size());
    for (const auto& req : requests) out.push_back(handler(req));
    return out;
  };
}

bool uses_proxy(const fr::update::HttpRequest& req) {
  return req.url.rfind("https://proxy.test/", 0) == 0;
}

void test_detector_race() {
  using namespace fr::update;
  fr::UpdateConfig cfg;
  cfg.direct_enabled = true;
  cfg.proxies.push_back({"px", "https://proxy.test", "", true});
  cfg.probe.timeout_ms = 1000;
  cfg.probe.concurrency = 4;
  EndpointPolicy policy(cfg);

  int api_calls = 0;
  int manifest_calls = 0;
  int atom_calls = 0;
  DetectionDeps deps;
  deps.now_sec = 1000;
  deps.manual = true;
  deps.fetch = mock_fetch([&](const HttpRequest& req) {
    const bool proxy = uses_proxy(req);
    if (req.url.find("releases.atom") != std::string::npos) {
      ++atom_calls;
      return mock_error(HttpError::Connect);
    }
    if (req.url.find("latest.json") != std::string::npos) {
      ++manifest_calls;
      return mock_ok(manifest_json(proxy ? "v0.6.6" : "v0.6.5"), proxy ? 2000.0 : 10.0,
                     proxy ? 400 : 5);
    }
    ++api_calls;
    // 直连 TTFB 更快、但完整响应体吞吐更低：必须以吞吐选代理。
    return mock_ok(release_json(proxy ? "v0.6.6" : "v0.6.5"), proxy ? 900.0 : 100.0,
                   proxy ? 300 : 5);
  });

  const DetectionResult r = detect_update(cfg, "0.6.4", &policy, nullptr, deps);
  CHECK(r.outcome == DetectionOutcome::Available);
  CHECK_EQ(r.latest_version, std::string("0.6.6"));
  CHECK_EQ(r.manifest.version, std::string("0.6.6"));
  CHECK(r.release_url.find("v0.6.6") != std::string::npos);
  CHECK_EQ(api_calls, 2);
  CHECK_EQ(manifest_calls, 2);
  CHECK_EQ(atom_calls, 0);
  CHECK_EQ(policy.api_sticky, std::string("px"));
  CHECK_EQ(policy.manifest_sticky, std::string("px"));
  CHECK_EQ(r.download_endpoints.size(), std::size_t(1));
  if (!r.download_endpoints.empty()) {
    CHECK_EQ(r.download_endpoints[0].endpoint.id, std::string("px"));
    CHECK_EQ(r.download_endpoints[0].metrics.bytes_per_sec, 2000.0);
  }
}

void test_detector_up_to_date() {
  using namespace fr::update;
  fr::UpdateConfig cfg;
  cfg.direct_enabled = true;
  cfg.proxies.push_back({"px", "https://proxy.test", "", true});
  EndpointPolicy policy(cfg);
  int manifest_calls = 0;
  DetectionDeps deps;
  deps.now_sec = 100;
  deps.manual = true;
  deps.fetch = mock_fetch([&](const HttpRequest& req) {
    if (req.url.find("latest.json") != std::string::npos) {
      ++manifest_calls;
      return mock_ok(manifest_json("v0.6.6"), 100.0);
    }
    return mock_ok(release_json("v0.6.4"), 100.0);
  });
  const DetectionResult r = detect_update(cfg, "0.6.4", &policy, nullptr, deps);
  CHECK(r.outcome == DetectionOutcome::UpToDate);
  CHECK_EQ(manifest_calls, 0);  // 已是最新：不跑清单赛跑
  CHECK(r.download_endpoints.empty());
}

void test_detector_direct_disabled() {
  using namespace fr::update;
  fr::UpdateConfig cfg;
  cfg.direct_enabled = false;
  cfg.proxies.push_back({"px", "https://proxy.test", "", true});
  EndpointPolicy policy(cfg);
  int calls = 0;
  std::string url;
  DetectionDeps deps;
  deps.now_sec = 100;
  deps.manual = true;
  deps.fetch = mock_fetch([&](const HttpRequest& req) {
    ++calls;
    url = req.url;
    return mock_ok(release_json("v0.6.4"), 100.0);
  });
  const DetectionResult r = detect_update(cfg, "0.6.4", &policy, nullptr, deps);
  CHECK(r.outcome == DetectionOutcome::UpToDate);
  CHECK_EQ(calls, 1);
  CHECK_EQ(url.rfind("https://proxy.test/", 0), std::size_t(0));
}

void test_detector_manifest_only() {
  using namespace fr::update;
  fr::UpdateConfig cfg;
  cfg.direct_enabled = true;
  cfg.proxies.push_back({"px", "https://proxy.test", "", true});
  EndpointPolicy policy(cfg);
  DetectionDeps deps;
  deps.now_sec = 100;
  deps.manual = true;
  deps.fetch = mock_fetch([&](const HttpRequest& req) {
    const bool proxy = uses_proxy(req);
    if (req.url.find("latest.json") != std::string::npos)
      return mock_ok(manifest_json(proxy ? "v0.6.6" : "v0.6.5"), proxy ? 800.0 : 100.0);
    return proxy ? mock_status(404) : mock_error(HttpError::Connect);  // 代理 API 不支持
  });
  const DetectionResult r = detect_update(cfg, "0.6.4", &policy, nullptr, deps);
  CHECK(r.outcome == DetectionOutcome::Available);
  CHECK_EQ(r.latest_version, std::string("0.6.6"));
  CHECK(policy.api_sticky.empty());  // API 全军覆没，无胜者
  CHECK_EQ(r.download_endpoints.size(), std::size_t(1));
  if (!r.download_endpoints.empty())
    CHECK_EQ(r.download_endpoints[0].endpoint.id, std::string("px"));
}

void test_detector_notify_only() {
  using namespace fr::update;
  fr::UpdateConfig cfg;
  cfg.direct_enabled = true;
  cfg.proxies.push_back({"px", "https://proxy.test", "", true});
  EndpointPolicy policy(cfg);
  int atom_calls = 0;
  DetectionDeps deps;
  deps.now_sec = 100;
  deps.manual = true;
  deps.fetch = mock_fetch([&](const HttpRequest& req) {
    const bool proxy = uses_proxy(req);
    if (req.url.find("releases.atom") != std::string::npos) {
      ++atom_calls;
      return mock_error(HttpError::Connect);
    }
    if (req.url.find("latest.json") != std::string::npos) return mock_status(404);
    return mock_ok(release_json(proxy ? "v0.6.6" : "v0.6.5"), proxy ? 500.0 : 100.0);
  });
  const DetectionResult r = detect_update(cfg, "0.6.4", &policy, nullptr, deps);
  CHECK(r.outcome == DetectionOutcome::NotifyOnly);
  CHECK_EQ(r.latest_version, std::string("0.6.6"));
  CHECK(!r.atom);
  CHECK_EQ(atom_calls, 0);  // API 已判定有更新：清单缺失时不再退 Atom
  CHECK(r.download_endpoints.empty());
}

void test_detector_api_fallback_digest() {
  using namespace fr::update;
  fr::UpdateConfig cfg;
  cfg.direct_enabled = true;
  cfg.proxies.push_back({"px", "https://proxy.test", "", true});
  cfg.proxies.push_back({"px2", "https://proxy2.test", "", true});
  EndpointPolicy policy(cfg);
  const std::string sha(64, 'd');
  int sums_calls = 0;
  DetectionDeps deps;
  deps.now_sec = 100;
  deps.manual = true;
  deps.fetch = mock_fetch([&](const HttpRequest& req) {
    if (req.url.find("releases.atom") != std::string::npos)
      return mock_error(HttpError::Connect);
    if (req.url.find("latest.json") != std::string::npos) return mock_status(404);
    if (req.url.find("SHA256SUMS.txt") != std::string::npos) {
      ++sums_calls;
      return mock_ok(sha256sums_text("0.6.5", sha), 5000.0);
    }
    if (req.url.rfind("https://proxy2.test/", 0) == 0) return mock_status(404);
    return mock_ok(release_assets_json("0.6.5", sha, true),
                   uses_proxy(req) ? 400.0 : 100.0);
  });
  const DetectionResult r = detect_update(cfg, "0.6.4", &policy, nullptr, deps);
  CHECK(r.outcome == DetectionOutcome::Available);
  CHECK(r.api_fallback);
  CHECK(!r.atom);
  CHECK_EQ(r.latest_version, std::string("0.6.5"));
  CHECK(r.manifest.valid);
  CHECK(r.manifest.windows_setup.valid);
  CHECK(r.manifest.linux_deb.valid);
  CHECK_EQ(r.manifest.windows_setup.size, 1000LL);
  CHECK_EQ(r.manifest.windows_setup.sha256, sha);
  CHECK_EQ(sums_calls, 0);  // 四个资产都带 digest：无需 SHA256SUMS
  // API 赛跑按吞吐排序；API 不支持但可能能下资产的端点排在后面顺延。
  CHECK_EQ(r.download_endpoints.size(), std::size_t(3));
  if (r.download_endpoints.size() == 3) {
    CHECK_EQ(r.download_endpoints[0].endpoint.id, std::string("px"));
    CHECK_EQ(r.download_endpoints[1].endpoint.id, std::string("direct"));
    CHECK_EQ(r.download_endpoints[2].endpoint.id, std::string("px2"));
  }
}

void test_detector_api_fallback_sums() {
  using namespace fr::update;
  fr::UpdateConfig cfg;
  cfg.direct_enabled = true;
  cfg.proxies.push_back({"px", "https://proxy.test", "", true});
  EndpointPolicy policy(cfg);
  const std::string sha(64, 'e');
  int sums_calls = 0;
  DetectionDeps deps;
  deps.now_sec = 100;
  deps.manual = true;
  deps.fetch = mock_fetch([&](const HttpRequest& req) {
    if (req.url.find("releases.atom") != std::string::npos)
      return mock_error(HttpError::Connect);
    if (req.url.find("latest.json") != std::string::npos) return mock_status(404);
    if (req.url.find("SHA256SUMS.txt") != std::string::npos) {
      ++sums_calls;
      return mock_ok(sha256sums_text("0.6.5", sha), uses_proxy(req) ? 8000.0 : 100.0);
    }
    // API 胜者是直连；下载端点应改按 SHA256SUMS 赛跑排序。
    return mock_ok(release_assets_json("0.6.5", "", false),
                   uses_proxy(req) ? 100.0 : 900.0);
  });
  const DetectionResult r = detect_update(cfg, "0.6.4", &policy, nullptr, deps);
  CHECK(r.outcome == DetectionOutcome::Available);
  CHECK(r.api_fallback);
  CHECK_EQ(r.latest_version, std::string("0.6.5"));
  CHECK(r.manifest.windows_setup.valid);
  CHECK_EQ(r.manifest.windows_setup.sha256, sha);
  CHECK_EQ(sums_calls, 2);
  CHECK_EQ(r.download_endpoints.size(), std::size_t(2));
  if (!r.download_endpoints.empty())
    CHECK_EQ(r.download_endpoints[0].endpoint.id, std::string("px"));
}

void test_detector_api_fallback_no_hash() {
  using namespace fr::update;
  fr::UpdateConfig cfg;
  cfg.direct_enabled = true;
  EndpointPolicy policy(cfg);
  DetectionDeps deps;
  deps.now_sec = 100;
  deps.manual = true;
  deps.fetch = mock_fetch([&](const HttpRequest& req) {
    if (req.url.find("latest.json") != std::string::npos) return mock_status(404);
    if (req.url.find("SHA256SUMS.txt") != std::string::npos) return mock_status(404);
    if (req.url.find("releases.atom") != std::string::npos)
      return mock_error(HttpError::Connect);
    return mock_ok(release_assets_json("0.6.5", "", false), 100.0);
  });
  const DetectionResult r = detect_update(cfg, "0.6.4", &policy, nullptr, deps);
  CHECK(r.outcome == DetectionOutcome::NotifyOnly);
  CHECK(!r.api_fallback);
  CHECK(!r.manifest.valid);
  CHECK(r.download_endpoints.empty());
}

void test_detector_atom_fallback() {
  using namespace fr::update;
  fr::UpdateConfig cfg;
  cfg.direct_enabled = true;
  cfg.proxies.push_back({"px", "https://proxy.test", "", true});
  EndpointPolicy policy(cfg);
  DetectionDeps deps;
  deps.now_sec = 100;
  deps.manual = true;
  deps.fetch = mock_fetch([&](const HttpRequest& req) {
    const bool proxy = uses_proxy(req);
    if (req.url.find("releases.atom") != std::string::npos) {
      if (!proxy) return mock_error(HttpError::Connect);
      return mock_ok(
          R"(<?xml version="1.0"?><feed><entry><title>v0.6.6</title><link rel="alternate" type="text/html" href="https://github.com/jianglinbin/FlashRec/releases/tag/v0.6.6"/></entry></feed>)",
          500.0);
    }
    if (req.url.find("latest.json") != std::string::npos) return mock_status(404);
    return mock_error(HttpError::Connect);
  });
  const DetectionResult r = detect_update(cfg, "0.6.4", &policy, nullptr, deps);
  CHECK(r.outcome == DetectionOutcome::NotifyOnly);
  CHECK(r.atom);
  CHECK_EQ(r.latest_version, std::string("0.6.6"));
  CHECK(r.release_url.find("v0.6.6") != std::string::npos);
  CHECK_EQ(r.download_endpoints.size(), std::size_t(0));
}

void test_detector_manifest_authority() {
  using namespace fr::update;
  fr::UpdateConfig cfg;
  cfg.direct_enabled = true;
  cfg.proxies.push_back({"px", "https://proxy.test", "", true});
  EndpointPolicy policy(cfg);
  DetectionDeps deps;
  deps.now_sec = 100;
  deps.manual = true;
  deps.fetch = mock_fetch([&](const HttpRequest& req) {
    const bool proxy = uses_proxy(req);
    if (req.url.find("latest.json") != std::string::npos)
      return mock_ok(manifest_json(proxy ? "v0.6.6" : "v0.6.5"), proxy ? 900.0 : 100.0);
    // API 胜出的是直连的 0.6.5；清单赛跑由代理的 0.6.6 胜出 → 以清单为准。
    return mock_ok(release_json(proxy ? "v0.6.6" : "v0.6.5"), proxy ? 100.0 : 900.0);
  });
  const DetectionResult r = detect_update(cfg, "0.6.4", &policy, nullptr, deps);
  CHECK(r.outcome == DetectionOutcome::Available);
  CHECK_EQ(policy.api_sticky, std::string("direct"));
  CHECK_EQ(r.latest_version, std::string("0.6.6"));
  CHECK_EQ(r.manifest.version, std::string("0.6.6"));
  CHECK(r.release_url.find("v0.6.6") != std::string::npos);
  CHECK_EQ(r.download_endpoints.size(), std::size_t(1));
  if (!r.download_endpoints.empty())
    CHECK_EQ(r.download_endpoints[0].endpoint.id, std::string("px"));
}

void test_detector_sticky() {
  using namespace fr::update;
  fr::UpdateConfig cfg;
  cfg.direct_enabled = true;
  cfg.proxies.push_back({"px", "https://proxy.test", "", true});
  EndpointPolicy policy(cfg);
  double direct_bps = 900.0;
  double proxy_bps = 1000.0;
  DetectionDeps deps;
  deps.now_sec = 100;
  deps.manual = true;
  deps.fetch = mock_fetch([&](const HttpRequest& req) {
    const bool proxy = uses_proxy(req);
    if (req.url.find("latest.json") != std::string::npos)
      return mock_ok(manifest_json("v0.6.6"), proxy ? proxy_bps : direct_bps);
    return mock_ok(release_json("v0.6.6"), proxy ? proxy_bps : direct_bps);
  });

  DetectionResult r = detect_update(cfg, "0.6.4", &policy, nullptr, deps);
  CHECK(r.outcome == DetectionOutcome::Available);
  CHECK_EQ(policy.api_sticky, std::string("px"));

  direct_bps = 1100.0;  // 直连略快但未超过 15%，继续粘滞代理
  deps.now_sec = 200;
  r = detect_update(cfg, "0.6.4", &policy, nullptr, deps);
  CHECK_EQ(policy.api_sticky, std::string("px"));

  direct_bps = 2000.0;  // 直连显著更快 → 切换
  proxy_bps = 500.0;
  deps.now_sec = 300;
  r = detect_update(cfg, "0.6.4", &policy, nullptr, deps);
  CHECK_EQ(policy.api_sticky, std::string("direct"));
}

fr::update::RankedEndpoint make_ranked(const std::string& id, const std::string& prefix) {
  fr::update::RankedEndpoint r;
  r.endpoint.id = id;
  r.endpoint.prefix = prefix;
  r.metrics.bytes_per_sec = 100.0;
  return r;
}

fr::ReleaseManifest make_manifest(const std::string& content) {
  fr::ReleaseManifest m;
  m.valid = true;
  m.version = "0.6.5";
  m.release_url = "https://github.com/jianglinbin/FlashRec/releases/tag/v0.6.5";
  m.windows_zip.name = "FlashRec-0.6.5-win64.zip";
  m.windows_zip.size = static_cast<long long>(content.size());
  m.windows_zip.sha256 = fr::update::sha256_hex(content);
  m.windows_zip.valid = true;
  return m;
}

std::string make_temp_dir(const std::string& tag) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto path = std::filesystem::temp_directory_path() /
                    ("flashrec_" + tag + "_" + std::to_string(stamp));
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
  std::filesystem::create_directories(path, ec);
  return path.string();
}

void test_downloader_resume() {
  using namespace fr::update;
  const std::string dir = make_temp_dir("dl_resume");
  const std::string content =
      "FlashRec updater payload: resume must preserve the first half, then append the rest.";
  const std::size_t half = content.size() / 2;
  const fr::ReleaseManifest manifest = make_manifest(content);
  const fr::ReleaseAsset asset = manifest.windows_zip;
  const std::vector<RankedEndpoint> endpoints = {make_ranked("px", "https://dl.test/")};

  int calls = 0;
  std::vector<HttpRequest> seen;
  int progress_reports = 0;
  DownloadProgress last_progress;
  DownloadDeps deps;
  deps.cache_dir = dir;
  deps.free_bytes = [](const std::string&) { return 1LL << 40; };
  deps.progress = [&](const DownloadProgress& p) {
    ++progress_reports;
    last_progress = p;
  };
  deps.fetch = [&](const HttpRequest& req, const HttpCallbacks& cb,
                   const std::atomic<bool>*) {
    seen.push_back(req);
    ++calls;
    if (req.range_from < 0) {
      cb.on_response(200, false);
      cb.on_body(content.data(), half);
      cb.on_progress(static_cast<long long>(content.size()), static_cast<long long>(half));
      HttpResult r = mock_error(HttpError::Truncated);
      r.metrics.response_code = 200;
      r.metrics.body_bytes = static_cast<long long>(half);
      return r;
    }
    cb.on_response(206, true);
    const std::size_t rest = content.size() - half;
    cb.on_body(content.data() + half, rest);
    cb.on_progress(static_cast<long long>(rest), static_cast<long long>(rest));
    HttpResult r = mock_ok("", 100.0);
    r.metrics.response_code = 206;
    r.metrics.range_start = static_cast<long long>(half);
    r.metrics.body_bytes = static_cast<long long>(rest);
    r.metrics.resumed = true;
    return r;
  };

  const DownloadResult r = download_asset(manifest, asset, endpoints, deps, nullptr);
  CHECK(r.status == DownloadStatus::Ok);
  CHECK_EQ(r.endpoint_id, std::string("px"));
  CHECK_EQ(calls, 2);
  CHECK_EQ(seen.size(), std::size_t(2));
  if (seen.size() == 2) {
    CHECK_EQ(seen[0].range_from, -1LL);
    CHECK_EQ(seen[1].range_from, static_cast<long long>(half));
  }
  CHECK_EQ(last_progress.downloaded, static_cast<long long>(content.size()));
  CHECK(progress_reports > 0);
  CHECK(r.file_path.find(".ready") == std::string::npos);
  CHECK_EQ(std::filesystem::path(r.file_path).filename().string(), asset.name);
  CHECK(std::filesystem::exists(dir + "/0.6.5/" + asset.name));
  CHECK(std::filesystem::exists(dir + "/0.6.5/" + asset.name + ".ready"));
  std::ifstream in(r.file_path, std::ios::binary);
  const std::string stored((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK_EQ(stored, content);
  CHECK(!std::filesystem::exists(dir + "/0.6.5/" + asset.name + ".part"));

  int reuse_calls = 0;
  DownloadDeps reuse_deps = deps;
  reuse_deps.fetch = [&](const HttpRequest&, const HttpCallbacks&, const std::atomic<bool>*) {
    ++reuse_calls;
    return mock_error(HttpError::Connect);
  };
  const DownloadResult again = download_asset(manifest, asset, endpoints, reuse_deps, nullptr);
  CHECK(again.status == DownloadStatus::Ok);
  CHECK(again.reused_ready);
  CHECK_EQ(reuse_calls, 0);

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

void test_downloader_range_ignored() {
  using namespace fr::update;
  const std::string dir = make_temp_dir("dl_range_ignored");
  const std::string content = "0123456789abcdefghijklmnopqrstuvwxyz";
  const std::size_t half = content.size() / 2;
  const fr::ReleaseManifest manifest = make_manifest(content);
  const fr::ReleaseAsset asset = manifest.windows_zip;
  const std::vector<RankedEndpoint> endpoints = {make_ranked("px", "https://dl.test/")};

  std::error_code ec;
  std::filesystem::create_directories(dir + "/0.6.5", ec);
  {
    std::ofstream f(dir + "/0.6.5/" + asset.name + ".part", std::ios::binary | std::ios::trunc);
    f.write(content.data(), static_cast<std::streamsize>(half));
  }

  int calls = 0;
  long long seen_range = -2;
  DownloadDeps deps;
  deps.cache_dir = dir;
  deps.free_bytes = [](const std::string&) { return 1LL << 40; };
  deps.fetch = [&](const HttpRequest& req, const HttpCallbacks& cb,
                   const std::atomic<bool>*) {
    ++calls;
    seen_range = req.range_from;
    // 服务器忽略 Range：回 200 + 全量响应体；下载器必须截断旧 .part。
    cb.on_response(200, false);
    cb.on_body(content.data(), content.size());
    return mock_ok("", 100.0);
  };
  const DownloadResult r = download_asset(manifest, asset, endpoints, deps, nullptr);
  CHECK(r.status == DownloadStatus::Ok);
  CHECK_EQ(calls, 1);
  CHECK_EQ(seen_range, static_cast<long long>(half));
  std::ifstream in(r.file_path, std::ios::binary);
  const std::string stored((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK_EQ(stored, content);
  std::filesystem::remove_all(dir, ec);
}

void test_downloader_416_restart() {
  using namespace fr::update;
  const std::string dir = make_temp_dir("dl_416");
  const std::string content = "0123456789abcdefghijklmnopqrstuvwxyz";
  const std::size_t half = content.size() / 2;
  const fr::ReleaseManifest manifest = make_manifest(content);
  const fr::ReleaseAsset asset = manifest.windows_zip;
  const std::vector<RankedEndpoint> endpoints = {make_ranked("px", "https://dl.test/")};

  std::error_code ec;
  std::filesystem::create_directories(dir + "/0.6.5", ec);
  {
    std::ofstream f(dir + "/0.6.5/" + asset.name + ".part", std::ios::binary | std::ios::trunc);
    f.write(content.data(), static_cast<std::streamsize>(half));
  }

  int calls = 0;
  long long second_range = -2;
  DownloadDeps deps;
  deps.cache_dir = dir;
  deps.free_bytes = [](const std::string&) { return 1LL << 40; };
  deps.fetch = [&](const HttpRequest& req, const HttpCallbacks& cb,
                   const std::atomic<bool>*) {
    ++calls;
    if (calls == 1) {
      CHECK_EQ(req.range_from, static_cast<long long>(half));
      return mock_status(416);  // 起点越界：删 .part 后从头重下
    }
    second_range = req.range_from;
    cb.on_response(200, false);
    cb.on_body(content.data(), content.size());
    return mock_ok("", 100.0);
  };
  const DownloadResult r = download_asset(manifest, asset, endpoints, deps, nullptr);
  CHECK(r.status == DownloadStatus::Ok);
  CHECK_EQ(calls, 2);
  CHECK_EQ(second_range, -1LL);
  std::filesystem::remove_all(dir, ec);
}

void test_downloader_hash_and_rotation() {
  using namespace fr::update;
  const std::string dir = make_temp_dir("dl_hash");
  const std::string good = "the real release payload";
  std::string bad = good;
  bad[0] = 'X';  // 与清单大小一致，仅哈希不符 → 必须走“换端点”而不是“字节数不足”
  const fr::ReleaseManifest manifest = make_manifest(good);
  const fr::ReleaseAsset asset = manifest.windows_zip;
  const std::vector<RankedEndpoint> endpoints = {
      make_ranked("bad", "https://bad.test/"),
      make_ranked("good", "https://good.test/"),
  };

  int calls = 0;
  DownloadDeps deps;
  deps.cache_dir = dir;
  deps.free_bytes = [](const std::string&) { return 1LL << 40; };
  deps.fetch = [&](const HttpRequest& req, const HttpCallbacks& cb,
                   const std::atomic<bool>*) {
    ++calls;
    const bool is_bad = req.url.rfind("https://bad.test/", 0) == 0;
    const std::string& body = is_bad ? bad : good;
    cb.on_response(200, false);
    cb.on_body(body.data(), body.size());
    return mock_ok("", 100.0);
  };
  const DownloadResult r = download_asset(manifest, asset, endpoints, deps, nullptr);
  CHECK(r.status == DownloadStatus::Ok);
  CHECK_EQ(r.endpoint_id, std::string("good"));
  CHECK_EQ(calls, 2);
  std::ifstream in(r.file_path, std::ios::binary);
  const std::string stored((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK_EQ(stored, good);

  // 两端点都校验失败 → 硬失败，不留下 .part/.ready
  const std::string dir2 = make_temp_dir("dl_hash_fail");
  DownloadDeps fail_deps = deps;
  fail_deps.cache_dir = dir2;
  fail_deps.fetch = [&](const HttpRequest&, const HttpCallbacks& cb, const std::atomic<bool>*) {
    cb.on_response(200, false);
    cb.on_body(bad.data(), bad.size());
    return mock_ok("", 100.0);
  };
  const DownloadResult failed = download_asset(manifest, asset, endpoints, fail_deps, nullptr);
  CHECK(failed.status == DownloadStatus::HashMismatch);
  CHECK(!std::filesystem::exists(dir2 + "/0.6.5/" + asset.name + ".part"));
  CHECK(!std::filesystem::exists(dir2 + "/0.6.5/" + asset.name + ".ready"));
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::remove_all(dir2, ec);
}

void test_downloader_disk_and_cancel() {
  using namespace fr::update;
  const std::string dir = make_temp_dir("dl_disk");
  const std::string content = "payload payload payload payload";
  const fr::ReleaseManifest manifest = make_manifest(content);
  const fr::ReleaseAsset asset = manifest.windows_zip;
  const std::vector<RankedEndpoint> endpoints = {make_ranked("px", "https://dl.test/")};

  int calls = 0;
  DownloadDeps tiny;
  tiny.cache_dir = dir;
  tiny.free_bytes = [](const std::string&) { return 1LL; };
  tiny.fetch = [&](const HttpRequest&, const HttpCallbacks&, const std::atomic<bool>*) {
    ++calls;
    return mock_ok("", 100.0);
  };
  const DownloadResult full = download_asset(manifest, asset, endpoints, tiny, nullptr);
  CHECK(full.status == DownloadStatus::DiskFull);
  CHECK_EQ(calls, 0);

  const std::string dir2 = make_temp_dir("dl_cancel");
  std::atomic<bool> cancel{false};
  const std::size_t half = content.size() / 2;
  DownloadDeps deps;
  deps.cache_dir = dir2;
  deps.free_bytes = [](const std::string&) { return 1LL << 40; };
  deps.fetch = [&](const HttpRequest&, const HttpCallbacks& cb, const std::atomic<bool>*) {
    cb.on_response(200, false);
    cb.on_body(content.data(), half);
    cancel.store(true);  // 模拟退出/取消
    HttpResult r = mock_error(HttpError::Cancelled);
    r.metrics.response_code = 200;
    r.metrics.body_bytes = static_cast<long long>(half);
    return r;
  };
  const DownloadResult cancelled = download_asset(manifest, asset, endpoints, deps, &cancel);
  CHECK(cancelled.status == DownloadStatus::Cancelled);
  const std::string part = dir2 + "/0.6.5/" + asset.name + ".part";
  CHECK(std::filesystem::exists(part));
  CHECK_EQ(static_cast<long long>(std::filesystem::file_size(part)),
           static_cast<long long>(half));
  CHECK(!std::filesystem::exists(dir2 + "/0.6.5/" + asset.name + ".ready"));

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::remove_all(dir2, ec);
}

void test_downloader_failure_preserves_part() {
  using namespace fr::update;
  const std::string dir = make_temp_dir("dl_fail");
  const std::string content = "partial download must survive network failures";
  const std::size_t half = content.size() / 2;
  const fr::ReleaseManifest manifest = make_manifest(content);
  const fr::ReleaseAsset asset = manifest.windows_zip;
  const std::vector<RankedEndpoint> endpoints = {make_ranked("px", "https://dl.test/")};

  int calls = 0;
  DownloadDeps deps;
  deps.cache_dir = dir;
  deps.free_bytes = [](const std::string&) { return 1LL << 40; };
  deps.fetch = [&](const HttpRequest&, const HttpCallbacks& cb, const std::atomic<bool>*) {
    ++calls;
    cb.on_response(200, false);
    cb.on_body(content.data(), half);
    HttpResult r = mock_error(HttpError::Truncated);
    r.metrics.response_code = 200;
    r.metrics.body_bytes = static_cast<long long>(half);
    return r;
  };
  const DownloadResult r = download_asset(manifest, asset, endpoints, deps, nullptr);
  CHECK(r.status == DownloadStatus::NetworkFailed);
  CHECK_EQ(calls, 2);  // 每端点最多 2 次
  CHECK(std::filesystem::exists(dir + "/0.6.5/" + asset.name + ".part"));
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

void test_cleanup_cache() {
  using namespace fr::update;
  const std::string dir = make_temp_dir("dl_cleanup");
  std::error_code ec;
  std::filesystem::create_directories(dir + "/0.6.4", ec);
  std::filesystem::create_directories(dir + "/0.6.5", ec);
  {
    std::ofstream f(dir + "/0.6.4/old.zip.ready");
    f << "old";
  }
  {
    std::ofstream f(dir + "/0.6.5/keep.zip.part");
    f << "part";
  }
  {
    std::ofstream f(dir + "/0.6.5/keep.zip.ready");
    f << "ready";
  }
  {
    std::ofstream f(dir + "/0.6.5/other.zip.part");
    f << "other";
  }
  {
    std::ofstream f(dir + "/0.6.5/user-note.txt");
    f << "keep me";
  }
  cleanup_update_cache("0.6.5", "keep.zip", dir);
  CHECK(!std::filesystem::exists(dir + "/0.6.4"));
  CHECK(std::filesystem::exists(dir + "/0.6.5/keep.zip.part"));
  CHECK(std::filesystem::exists(dir + "/0.6.5/keep.zip.ready"));
  CHECK(!std::filesystem::exists(dir + "/0.6.5/other.zip.part"));
  CHECK(std::filesystem::exists(dir + "/0.6.5/user-note.txt"));

  cleanup_update_cache("", "", dir);
  CHECK(std::filesystem::is_empty(dir));
  std::filesystem::remove_all(dir, ec);
}

bool wait_service(fr::update::UpdateService& svc,
                  const std::function<bool(const fr::update::UpdateSnapshot&)>& pred,
                  int timeout_ms = 3000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred(svc.snapshot())) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pred(svc.snapshot());
}

fr::UpdateConfig make_service_cfg() {
  fr::UpdateConfig cfg;
  cfg.enabled = true;
  cfg.auto_check = true;
  cfg.auto_download = true;
  cfg.check_interval_hours = 4;
  cfg.startup_delay_sec = 600;  // 测试不触发自动首查，全部由 request_check 驱动
  cfg.probe.timeout_ms = 1000;
  cfg.probe.concurrency = 4;
  cfg.probe.cache_sec = 0;
  cfg.direct_enabled = true;
  return cfg;
}

std::string make_config_path(const std::string& tag) {
  const std::string path = make_temp_dir(tag) + "/settings.json";
  std::ofstream f(path);
  f << "{}";
  return path;
}

fr::update::BatchFetch service_available_fetch(int* api_calls, int* manifest_calls) {
  return mock_fetch([=](const fr::update::HttpRequest& req) {
    if (req.url.find("latest.json") != std::string::npos) {
      if (manifest_calls != nullptr) ++*manifest_calls;
      return mock_ok(manifest_json("v0.6.5"), 800.0);
    }
    if (api_calls != nullptr) ++*api_calls;
    return mock_ok(release_json("v0.6.5"), 900.0);
  });
}

void test_service_up_to_date() {
  using namespace fr::update;
  fr::UpdateConfig cfg = make_service_cfg();
  int api_calls = 0;
  ServiceDeps deps;
  deps.detect_shape = [] { return fr::platform::InstallShape::WindowsPortable; };
  deps.fetch = mock_fetch([&](const HttpRequest& req) {
    if (req.url.find("latest.json") != std::string::npos)
      return mock_error(HttpError::Connect);
    ++api_calls;
    return mock_ok(release_json("v0.6.3"), 500.0);
  });
  UpdateService svc(deps);
  svc.start(cfg, "0.6.3", "");
  svc.request_check(true);
  CHECK(wait_service(svc,
                     [](const UpdateSnapshot& s) { return s.state == UpdateState::UpToDate; }));
  const UpdateSnapshot s = svc.snapshot();
  CHECK_EQ(std::string(s.latest_version), std::string("0.6.3"));
  CHECK(s.last_check_manual);
  CHECK_EQ(s.check_seq, 1LL);
  CHECK_EQ(api_calls, 1);
  CHECK(s.next_check_at > s.last_check_at);
  svc.stop();
}

void test_service_cache_reuse() {
  using namespace fr::update;
  fr::UpdateConfig cfg = make_service_cfg();
  cfg.probe.cache_sec = 600;
  int api_calls = 0;
  ServiceDeps deps;
  deps.detect_shape = [] { return fr::platform::InstallShape::WindowsPortable; };
  deps.fetch = mock_fetch([&](const HttpRequest& req) {
    if (req.url.find("latest.json") != std::string::npos)
      return mock_error(HttpError::Connect);
    ++api_calls;
    return mock_ok(release_json("v0.6.3"), 500.0);
  });
  UpdateService svc(deps);
  svc.start(cfg, "0.6.3", "");
  svc.request_check(true);
  CHECK(wait_service(svc,
                     [](const UpdateSnapshot& s) { return s.state == UpdateState::UpToDate; }));
  CHECK_EQ(api_calls, 1);

  svc.request_check(false);
  CHECK(wait_service(svc, [](const UpdateSnapshot& s) { return s.check_seq == 2; }));
  const UpdateSnapshot s = svc.snapshot();
  CHECK_EQ(api_calls, 1);  // 命中 probe.cache_sec，没有再发请求
  CHECK(!s.last_check_manual);
  CHECK(s.state == UpdateState::UpToDate);
  svc.stop();
}

void test_service_auto_download_ready() {
  using namespace fr::update;
  fr::UpdateConfig cfg = make_service_cfg();
  const std::string dir = make_temp_dir("svc_ready");
  int download_calls = 0;
  std::string got_asset;
  std::size_t got_endpoints = 0;
  ServiceDeps deps;
  deps.detect_shape = [] { return fr::platform::InstallShape::WindowsPortable; };
  deps.cache_dir = dir;
  deps.fetch = service_available_fetch(nullptr, nullptr);
  deps.download = [&](const fr::ReleaseManifest&, const fr::ReleaseAsset& a,
                      const std::vector<RankedEndpoint>& eps, const DownloadDeps& dd,
                      const std::atomic<bool>*) {
    ++download_calls;
    got_asset = a.name;
    got_endpoints = eps.size();
    if (dd.progress) dd.progress(DownloadProgress{a.size / 2, a.size, 123.0, "direct"});
    DownloadResult r;
    r.status = DownloadStatus::Ok;
    r.file_path = dir + "/" + a.name + ".ready";
    r.endpoint_id = "direct";
    r.sha256 = a.sha256;
    return r;
  };
  UpdateService svc(deps);
  svc.start(cfg, "0.6.4", "");
  svc.request_check(true);
  CHECK(wait_service(svc, [](const UpdateSnapshot& s) { return s.state == UpdateState::Ready; }));
  const UpdateSnapshot s = svc.snapshot();
  CHECK_EQ(download_calls, 1);
  CHECK_EQ(got_asset, std::string("FlashRec-0.6.5-win64.zip"));
  CHECK_EQ(got_endpoints, std::size_t(1));
  CHECK_EQ(std::string(s.latest_version), std::string("0.6.5"));
  CHECK_EQ(std::string(s.asset_name), std::string("FlashRec-0.6.5-win64.zip"));
  CHECK_EQ(s.asset_size, 456LL);
  CHECK_EQ(s.downloaded, 456LL);
  CHECK_EQ(s.progress_percent, 100);
  CHECK(!s.download_available);
  CHECK_EQ(std::string(s.release_url),
           std::string("https://github.com/jianglinbin/FlashRec/releases/tag/v0.6.5"));
  CHECK(s.next_check_at > s.last_check_at);
  svc.stop();
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

void test_service_manual_only_then_download() {
  using namespace fr::update;
  fr::UpdateConfig cfg = make_service_cfg();
  cfg.auto_download = false;
  const std::string dir = make_temp_dir("svc_manual");
  int download_calls = 0;
  ServiceDeps deps;
  deps.detect_shape = [] { return fr::platform::InstallShape::WindowsPortable; };
  deps.cache_dir = dir;
  deps.fetch = service_available_fetch(nullptr, nullptr);
  deps.download = [&](const fr::ReleaseManifest&, const fr::ReleaseAsset& a,
                      const std::vector<RankedEndpoint>&, const DownloadDeps&,
                      const std::atomic<bool>*) {
    ++download_calls;
    DownloadResult r;
    r.status = DownloadStatus::Ok;
    r.file_path = dir + "/" + a.name + ".ready";
    r.endpoint_id = "direct";
    return r;
  };
  UpdateService svc(deps);
  svc.start(cfg, "0.6.4", "");
  svc.request_check(true);
  CHECK(wait_service(svc,
                     [](const UpdateSnapshot& s) { return s.state == UpdateState::ManualOnly; }));
  UpdateSnapshot s = svc.snapshot();
  CHECK(s.download_available);
  CHECK_EQ(download_calls, 0);
  CHECK_EQ(std::string(s.asset_name), std::string("FlashRec-0.6.5-win64.zip"));

  svc.request_download();
  CHECK(wait_service(svc, [](const UpdateSnapshot& s) { return s.state == UpdateState::Ready; }));
  s = svc.snapshot();
  CHECK_EQ(download_calls, 1);
  CHECK(!s.download_available);
  CHECK_EQ(s.progress_percent, 100);
  svc.stop();
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

void test_service_skip_current_version() {
  using namespace fr::update;
  fr::UpdateConfig cfg = make_service_cfg();
  const std::string dir = make_temp_dir("svc_skip_api");
  const std::string config_path = make_config_path("svc_skip_api_cfg");
  {
    std::filesystem::create_directories(dir + "/0.6.5");
    std::ofstream f(dir + "/0.6.5/FlashRec-0.6.5-win64.zip.ready");
    f << "ready";
  }
  ServiceDeps deps;
  deps.detect_shape = [] { return fr::platform::InstallShape::WindowsPortable; };
  deps.cache_dir = dir;
  deps.fetch = service_available_fetch(nullptr, nullptr);
  deps.download = [&](const fr::ReleaseManifest&, const fr::ReleaseAsset&,
                      const std::vector<RankedEndpoint>&, const DownloadDeps&,
                      const std::atomic<bool>*) {
    DownloadResult r;
    r.status = DownloadStatus::Ok;
    r.file_path = dir + "/0.6.5/FlashRec-0.6.5-win64.zip.ready";
    r.endpoint_id = "direct";
    return r;
  };
  UpdateService svc(deps);
  svc.start(cfg, "0.6.4", config_path);
  svc.request_check(true);
  CHECK(wait_service(svc, [](const UpdateSnapshot& s) { return s.state == UpdateState::Ready; }));
  CHECK(std::filesystem::exists(dir + "/0.6.5/FlashRec-0.6.5-win64.zip.ready"));

  svc.skip_current_version();
  CHECK(wait_service(svc, [](const UpdateSnapshot& s) { return s.state == UpdateState::Skipped; }));
  const UpdateSnapshot s = svc.snapshot();
  CHECK_EQ(std::string(s.asset_path), std::string());
  CHECK(!s.download_available);
  CHECK_EQ(s.progress_percent, -1);
  CHECK(!std::filesystem::exists(dir + "/0.6.5"));  // 已下载包随跳过一并清理
  const fr::Config persisted = fr::Config::load(config_path);
  CHECK_EQ(persisted.update.skipped_version, std::string("0.6.5"));

  // 跳过落盘后再次检查：仍进 Skipped，且不会重新下载（自动检查避开手动 30s 去重）
  const long long seq0 = s.check_seq;
  svc.request_check(false);
  CHECK(wait_service(svc, [&](const UpdateSnapshot& x) { return x.check_seq > seq0; }));
  CHECK(svc.snapshot().state == UpdateState::Skipped);
  svc.stop();
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::remove_all(std::filesystem::path(config_path).parent_path(), ec);
}

void test_service_error_backoff_persist() {
  using namespace fr::update;
  fr::UpdateConfig cfg = make_service_cfg();
  const std::string config_path = make_config_path("svc_err");
  ServiceDeps deps;
  deps.detect_shape = [] { return fr::platform::InstallShape::WindowsPortable; };
  deps.fetch = mock_fetch([](const HttpRequest&) { return mock_error(HttpError::Connect); });
  UpdateService svc(deps);
  svc.start(cfg, "0.6.4", config_path);
  svc.request_check(true);
  CHECK(wait_service(svc,
                     [](const UpdateSnapshot& s) { return s.state == UpdateState::Backoff; }));
  const UpdateSnapshot s = svc.snapshot();
  CHECK(s.error == UpdateError::Network);
  CHECK_EQ(s.fail_count, 1);
  CHECK(s.next_check_at > s.last_check_at);
  CHECK_EQ(s.check_seq, 1LL);

  const fr::Config persisted = fr::Config::load(config_path);
  CHECK_EQ(persisted.update.fail_count, 1);
  CHECK(persisted.update.last_check_at > 0);
  CHECK_EQ(persisted.update.last_check_at, s.last_check_at);
  CHECK_EQ(persisted.update.next_retry_at, s.next_check_at);
  CHECK(persisted.update.next_retry_at - persisted.update.last_check_at >= 900);
  svc.stop();
  std::error_code ec;
  std::filesystem::remove_all(std::filesystem::path(config_path).parent_path(), ec);
}

void test_service_download_failure_backoff() {
  using namespace fr::update;
  fr::UpdateConfig cfg = make_service_cfg();
  ServiceDeps deps;
  deps.detect_shape = [] { return fr::platform::InstallShape::WindowsPortable; };
  deps.fetch = service_available_fetch(nullptr, nullptr);
  deps.download = [](const fr::ReleaseManifest&, const fr::ReleaseAsset&,
                     const std::vector<RankedEndpoint>&, const DownloadDeps&,
                     const std::atomic<bool>*) {
    DownloadResult r;
    r.status = DownloadStatus::HashMismatch;
    r.error = "sha256 mismatch";
    return r;
  };
  UpdateService svc(deps);
  svc.start(cfg, "0.6.4", "");
  svc.request_check(true);
  CHECK(wait_service(svc,
                     [](const UpdateSnapshot& s) { return s.state == UpdateState::Backoff; }));
  const UpdateSnapshot s = svc.snapshot();
  CHECK(s.error == UpdateError::Verify);
  CHECK_EQ(s.fail_count, 1);
  CHECK(s.download_available);  // 保留待下载包，允许用户重试
  CHECK(s.next_check_at > s.last_check_at);
  svc.stop();
}

void test_service_skip_and_unknown_shape() {
  using namespace fr::update;
  {
    fr::UpdateConfig cfg = make_service_cfg();
    cfg.skipped_version = "0.6.5";
    int download_calls = 0;
    ServiceDeps deps;
    deps.detect_shape = [] { return fr::platform::InstallShape::WindowsPortable; };
    deps.fetch = service_available_fetch(nullptr, nullptr);
    deps.download = [&](const fr::ReleaseManifest&, const fr::ReleaseAsset&,
                        const std::vector<RankedEndpoint>&, const DownloadDeps&,
                        const std::atomic<bool>*) {
      ++download_calls;
      DownloadResult r;
      r.status = DownloadStatus::Ok;
      return r;
    };
    UpdateService svc(deps);
    svc.start(cfg, "0.6.4", "");
    svc.request_check(true);
    CHECK(wait_service(svc,
                       [](const UpdateSnapshot& s) { return s.state == UpdateState::Skipped; }));
    const UpdateSnapshot s = svc.snapshot();
    CHECK_EQ(download_calls, 0);
    CHECK(!s.download_available);
    CHECK_EQ(std::string(s.latest_version), std::string("0.6.5"));
    svc.stop();
  }
  {
    fr::UpdateConfig cfg = make_service_cfg();
    ServiceDeps deps;
    deps.detect_shape = [] { return fr::platform::InstallShape::Unknown; };
    deps.fetch = service_available_fetch(nullptr, nullptr);
    UpdateService svc(deps);
    svc.start(cfg, "0.6.4", "");
    svc.request_check(true);
    CHECK(wait_service(svc,
                       [](const UpdateSnapshot& s) { return s.state == UpdateState::ManualOnly; }));
    const UpdateSnapshot s = svc.snapshot();
    CHECK(!s.download_available);
    CHECK_EQ(std::string(s.asset_name), std::string());
    svc.stop();
  }
}

void test_service_apply_config_skip() {
  using namespace fr::update;
  fr::UpdateConfig cfg = make_service_cfg();
  const std::string config_path = make_config_path("svc_apply");
  int download_calls = 0;
  ServiceDeps deps;
  deps.detect_shape = [] { return fr::platform::InstallShape::WindowsPortable; };
  deps.fetch = service_available_fetch(nullptr, nullptr);
  deps.download = [&](const fr::ReleaseManifest&, const fr::ReleaseAsset&,
                      const std::vector<RankedEndpoint>&, const DownloadDeps&,
                      const std::atomic<bool>*) {
    ++download_calls;
    DownloadResult r;
    r.status = DownloadStatus::Ok;
    return r;
  };
  UpdateService svc(deps);
  svc.start(cfg, "0.6.4", config_path);

  fr::UpdateConfig next = cfg;
  next.skipped_version = "0.6.5";
  next.check_interval_hours = 2;
  svc.apply_config(next);
  svc.request_check(true);
  CHECK(wait_service(svc,
                     [](const UpdateSnapshot& s) { return s.state == UpdateState::Skipped; }));
  CHECK_EQ(download_calls, 0);

  std::ifstream f(config_path);
  const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  CHECK(text.find("\"skipped_version\"") != std::string::npos);
  CHECK(text.find("0.6.5") != std::string::npos);
  svc.stop();
  std::error_code ec;
  std::filesystem::remove_all(std::filesystem::path(config_path).parent_path(), ec);
}

}  // namespace

int main() {
  test_semver();
  test_json();
  test_assets();
  test_manifest();
  test_github_release();
  test_sha256sums();
  test_atom();
  test_config();
  test_url_guard();
  test_sha256();
  test_endpoint_policy();
  test_detector_race();
  test_detector_up_to_date();
  test_detector_direct_disabled();
  test_detector_manifest_only();
  test_detector_notify_only();
  test_detector_api_fallback_digest();
  test_detector_api_fallback_sums();
  test_detector_api_fallback_no_hash();
  test_detector_atom_fallback();
  test_detector_manifest_authority();
  test_detector_sticky();
  test_downloader_resume();
  test_downloader_range_ignored();
  test_downloader_416_restart();
  test_downloader_hash_and_rotation();
  test_downloader_disk_and_cancel();
  test_downloader_failure_preserves_part();
  test_cleanup_cache();
  test_service_up_to_date();
  test_service_cache_reuse();
  test_service_auto_download_ready();
  test_service_manual_only_then_download();
  test_service_skip_current_version();
  test_service_error_backoff_persist();
  test_service_download_failure_backoff();
  test_service_skip_and_unknown_shape();
  test_service_apply_config_skip();
  std::cout << "update_tests: " << g_pass << " passed, " << g_fail << " failed\n";
  return g_fail == 0 ? 0 : 1;
}
