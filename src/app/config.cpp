#include "app/config.h"

#include <charconv>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "app/log.h"

namespace fr {

// 默认 Sink 协议串（DMR_SOAP_SPEC §1.3：缺格式 → 网盘/直播类 App 直接灰投屏按钮）。
// 每条带 DLNA.ORG_OP=01（支持 Range 拖动）；CI=0（不做转码）。末尾通配兜底。
const std::vector<std::string>& Config::default_protocol_info() {
  static const std::vector<std::string> kList = {
      "http-get:*:video/mp4:DLNA.ORG_PN=AVC_MP4_BL_L31_MC_ID30;DLNA.ORG_OP=01;DLNA.ORG_CI=0",
      "http-get:*:video/mp4:DLNA.ORG_OP=01;DLNA.ORG_CI=0",
      "http-get:*:video/x-matroska:DLNA.ORG_OP=01;DLNA.ORG_CI=0",
      "http-get:*:video/x-msvideo:DLNA.ORG_OP=01;DLNA.ORG_CI=0",
      "http-get:*:video/x-flv:DLNA.ORG_OP=01;DLNA.ORG_CI=0",
      "http-get:*:video/mp2t:DLNA.ORG_OP=01;DLNA.ORG_CI=0",
      "http-get:*:video/webm:DLNA.ORG_OP=01;DLNA.ORG_CI=0",
      "http-get:*:video/avi:DLNA.ORG_OP=01;DLNA.ORG_CI=0",
      "http-get:*:video/quicktime:DLNA.ORG_OP=01;DLNA.ORG_CI=0",
      "http-get:*:application/vnd.apple.mpegurl:DLNA.ORG_OP=01;DLNA.ORG_CI=0",
      "http-get:*:application/x-mpegurl:DLNA.ORG_OP=01;DLNA.ORG_CI=0",
      "http-get:*:audio/mpeg:DLNA.ORG_PN=MP3;DLNA.ORG_OP=01;DLNA.ORG_CI=0",
      "http-get:*:audio/mp4:DLNA.ORG_OP=01;DLNA.ORG_CI=0",
      "http-get:*:audio/aac:DLNA.ORG_OP=01;DLNA.ORG_CI=0",
      "http-get:*:audio/flac:DLNA.ORG_OP=01;DLNA.ORG_CI=0",
      "http-get:*:audio/x-flac:DLNA.ORG_OP=01;DLNA.ORG_CI=0",
      "http-get:*:audio/x-ms-wma:DLNA.ORG_OP=01;DLNA.ORG_CI=0",
      "http-get:*:audio/wav:DLNA.ORG_OP=01;DLNA.ORG_CI=0",
      "http-get:*:image/jpeg:DLNA.ORG_PN=JPEG_SM;DLNA.ORG_OP=01;DLNA.ORG_CI=0",
      "http-get:*:image/jpeg:DLNA.ORG_PN=JPEG_MED;DLNA.ORG_OP=01;DLNA.ORG_CI=0",
      "http-get:*:image/jpeg:DLNA.ORG_PN=JPEG_LRG;DLNA.ORG_OP=01;DLNA.ORG_CI=0",
      "http-get:*:image/png:DLNA.ORG_OP=01;DLNA.ORG_CI=0",
      "http-get:*:image/gif:DLNA.ORG_OP=01;DLNA.ORG_CI=0",
      "http-get:*:image/bmp:DLNA.ORG_OP=01;DLNA.ORG_CI=0",
      "*:*:*:*",
  };
  return kList;
}

std::string Config::sink_protocol_info() const {
  const auto& list = protocol_info.empty() ? default_protocol_info() : protocol_info;
  std::string out;
  for (size_t i = 0; i < list.size(); ++i) {
    if (i) out += ",";
    out += list[i];
  }
  return out;
}

// ---- 极简 JSON 解析：扁平分组对象 + 字符串数组；坏值回默认，绝不抛异常 ----
namespace {

class MiniJson {
 public:
  explicit MiniJson(std::string text) : s_(std::move(text)) {}

  bool parse() {
    i_ = 0;
    ws();
    object("");
    return true;
  }

  std::map<std::string, std::string> kv;       // "dlna.port" -> 值（字符串形式）
  std::vector<std::string> protocol_info;      // dlna.protocol_info 数组
  // d146 R4：ui.window.monitors[] 数组（元素为对象，逐字段收集后由 load() 转换）
  struct MonRaw {
    std::map<std::string, std::string> f;
  };
  std::vector<MonRaw> monitors;

 private:
  void ws() {
    while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\r' || s_[i_] == '\n'))
      ++i_;
  }
  char peek() { ws(); return i_ < s_.size() ? s_[i_] : '\0'; }
  bool eat(char c) {
    if (peek() == c) { ++i_; return true; }
    return false;
  }
  std::string str() {
    std::string out;
    if (!eat('"')) return out;
    while (i_ < s_.size() && s_[i_] != '"') {
      char c = s_[i_++];
      if (c == '\\' && i_ < s_.size()) {
        char e = s_[i_++];
        switch (e) {
          case 'n': out += '\n'; break;
          case 't': out += '\t'; break;
          case 'r': out += '\r'; break;
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case '/': out += '/'; break;
          case 'u': {
            if (i_ + 4 <= s_.size()) {
              int cp = 0;
              auto from_hex = [](char h) -> int {
                if (h >= '0' && h <= '9') return h - '0';
                if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                return 0;
              };
              for (int k = 0; k < 4; ++k) cp = cp * 16 + from_hex(s_[i_ + k]);
              i_ += 4;
              if (cp < 0x80) out += static_cast<char>(cp);
              else if (cp < 0x800) {
                out += static_cast<char>(0xC0 | (cp >> 6));
                out += static_cast<char>(0x80 | (cp & 0x3F));
              } else {
                out += static_cast<char>(0xE0 | (cp >> 12));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
              }
            }
            break;
          }
          default: out += e; break;
        }
      } else {
        out += c;
      }
    }
    if (i_ < s_.size()) ++i_;  // 收尾引号
    return out;
  }
  std::string scalar() {  // 字符串 / 数字 / true false / null
    if (peek() == '"') return str();
    std::string out;
    while (i_ < s_.size() && s_[i_] != ',' && s_[i_] != '}' && s_[i_] != ']' &&
           !isspace(static_cast<unsigned char>(s_[i_]))) {
      out += s_[i_++];
    }
    return out;
  }
  void object(const std::string& prefix) {
    if (!eat('{')) return;
    if (eat('}')) return;
    for (;;) {
      std::string key = str();
      if (key.empty() && peek() != ':') break;  // 容错：坏结构直接放弃
      if (!eat(':')) break;
      std::string path = prefix.empty() ? key : prefix + "." + key;
      if (peek() == '{') {
        object(path);
      } else if (peek() == '[') {
        // d146：数组三路——已知数组解析；其余未知数组整体跳过（嵌套对象/数组
        // 落进 scalar() 会把后续键全部吞掉，必须配对跳过）
        if (path == "dlna.protocol_info")
          parse_protocol_info();
        else if (path == "ui.window.monitors")
          parse_monitors();
        else
          skip_value();
      } else {
        kv[path] = scalar();
      }
      if (eat(',')) { ws(); if (peek() == '}') break; continue; }
      eat('}');
      break;
    }
  }
  void parse_protocol_info() {
    if (!eat('[')) return;
    if (eat(']')) return;
    for (;;) {
      if (peek() == '"') {
        std::string v = str();
        if (!v.empty()) protocol_info.push_back(std::move(v));
      } else {
        scalar();
      }
      if (eat(',')) continue;
      eat(']');
      break;
    }
  }
  // d146 R4：ui.window.monitors[] —— 对象数组逐字段收集（未知字段跳过，坏结构容错）
  void parse_monitors() {
    if (!eat('[')) return;
    if (eat(']')) return;
    for (;;) {
      if (peek() == '{') {
        MonRaw m;
        if (!eat('{')) break;
        if (!eat('}')) {
          for (;;) {
            std::string key = str();
            if (!eat(':')) break;
            if (!key.empty()) m.f[key] = scalar();
            if (eat(',')) { ws(); if (peek() == '}') break; continue; }
            eat('}');
            break;
          }
        }
        if (!m.f.empty()) monitors.push_back(std::move(m));
      } else {
        scalar();
      }
      if (eat(',')) { ws(); if (peek() == ']') break; continue; }
      eat(']');
      break;
    }
  }
  // d146：未知值（数组/嵌套对象）配对跳过；字符串感知（值内括号不计数）
  void skip_value() {
    char c = peek();
    if (c != '[' && c != '{') { scalar(); return; }
    int depth = 0;
    while (i_ < s_.size()) {
      char k = s_[i_++];
      if (k == '"') {  // 字符串整体跳过（转义感知）
        while (i_ < s_.size()) {
          char q = s_[i_++];
          if (q == '\\') { if (i_ < s_.size()) ++i_; continue; }
          if (q == '"') break;
        }
        continue;
      }
      if (k == '[' || k == '{') ++depth;
      else if (k == ']' || k == '}') {
        if (--depth == 0) return;
      }
    }
  }
  std::string s_;
  size_t i_ = 0;
};

}  // namespace

Config Config::load(const std::string& path) {
  Config cfg;
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    FR_LOG_INFO("[APP] 配置不存在，用默认值：{}", path);
    return cfg;
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  MiniJson j(ss.str());
  if (!j.parse()) {
    FR_LOG_WARN("[APP] 配置解析失败，全部回默认：{}", path);
    return cfg;
  }
  auto get = [&](const char* k) -> const std::string* {
    auto it = j.kv.find(k);
    return it == j.kv.end() ? nullptr : &it->second;
  };
  auto get_bool = [&](const char* k, bool dft) {
    auto* v = get(k);
    return v ? (*v == "true" || *v == "1") : dft;
  };
  auto get_int = [&](const char* k, int dft) {
    auto* v = get(k);
    if (!v || v->empty()) return dft;
    int out = dft;
    auto [p, ec] = std::from_chars(v->data(), v->data() + v->size(), out);
    return ec == std::errc() ? out : dft;
  };
  auto get_double = [&](const char* k, double dft) {
    auto* v = get(k);
    if (!v || v->empty()) return dft;
    try {
      size_t pos = 0;
      double out = std::stod(*v, &pos);
      return pos > 0 ? out : dft;
    } catch (...) {
      return dft;  // 坏值回默认，绝不让配置文件把程序搞挂
    }
  };
  if (auto* lv = get("log.level"); lv && !lv->empty()) cfg.log_level = *lv;
  cfg.log_trace_http_body = get_bool("log.trace_http_body", cfg.log_trace_http_body);
  cfg.log_http_entry = get_bool("log.http_entry", cfg.log_http_entry);
  cfg.log_soap_inbox = get_bool("log.soap_inbox", cfg.log_soap_inbox);
  cfg.dlna_port = get_int("dlna.port", cfg.dlna_port);
  cfg.dlna_auto_play_on_set_uri =
      get_bool("dlna.auto_play_on_set_uri", cfg.dlna_auto_play_on_set_uri);
  cfg.dlna_progress_retention_sec =
      get_int("dlna.progress_retention_sec", cfg.dlna_progress_retention_sec);
  cfg.friendly_name =
      get("dlna.friendly_name") ? *get("dlna.friendly_name") : cfg.friendly_name;
  cfg.skin = get("ui.skin") ? *get("ui.skin") : cfg.skin;
  cfg.ui_show_fps = get_bool("ui.show_fps", cfg.ui_show_fps);
  cfg.ui_show_video_fps = get_bool("ui.show_video_fps", cfg.ui_show_video_fps);
  cfg.ui_show_info = get_bool("ui.show_info", cfg.ui_show_info);
  // d146 R2/R4：投屏自动全屏 + 多屏记忆
  cfg.ui_cast_auto_fullscreen = get_bool("ui.cast_auto_fullscreen", cfg.ui_cast_auto_fullscreen);
  if (auto* pv = get("ui.window.prefer"); pv && !pv->empty()) cfg.ui_window_prefer = *pv;
  for (const auto& raw : j.monitors) {
    MonitorMemory m;
    auto fs = [&](const char* k) -> const std::string* {
      auto it = raw.f.find(k);
      return it == raw.f.end() ? nullptr : &it->second;
    };
    auto fi = [&](const char* k, int dft) {
      auto* v = fs(k);
      if (!v || v->empty()) return dft;
      int out = dft;
      auto [p, ec] = std::from_chars(v->data(), v->data() + v->size(), out);
      return ec == std::errc() ? out : dft;
    };
    if (auto* v = fs("fp")) m.fp = *v;
    m.x = fi("x", 0); m.y = fi("y", 0); m.w = fi("w", 0); m.h = fi("h", 0);
    m.maximized = fs("maximized") && *fs("maximized") == "true";
    m.fullscreen = fs("fullscreen") && *fs("fullscreen") == "true";
    if (auto* v = fs("last_used")) {
      long long out = 0;
      auto [p, ec] = std::from_chars(v->data(), v->data() + v->size(), out);
      if (ec == std::errc()) m.last_used = out;
    }
    if (!m.fp.empty() && cfg.ui_window_monitors.size() < 16)
      cfg.ui_window_monitors.push_back(std::move(m));
  }
  cfg.live_reconnect_max = get_int("live.reconnect_max", cfg.live_reconnect_max);
  cfg.clipboard_play = get_bool("clipboard.play", cfg.clipboard_play);  // d74
  // d163：音量/静音持久化（player.volume / player.muted；越界夹回 0..100）
  cfg.player_volume = get_int("player.volume", cfg.player_volume);
  if (cfg.player_volume < 0) cfg.player_volume = 0;
  if (cfg.player_volume > 100) cfg.player_volume = 100;
  cfg.player_muted = get_bool("player.muted", cfg.player_muted);
  // d169：倍速持久化（player.speed，夹到 0.25..4.0）
  cfg.player_speed = get_double("player.speed", cfg.player_speed);
  if (cfg.player_speed < 0.25) cfg.player_speed = 0.25;
  if (cfg.player_speed > 4.0) cfg.player_speed = 4.0;
  if (!j.protocol_info.empty()) cfg.protocol_info = j.protocol_info;
  FR_LOG_INFO("[APP] 配置加载完成 {}：port={} auto_play={} retention={}s reconnect={}", path,
              cfg.dlna_port, cfg.dlna_auto_play_on_set_uri, cfg.dlna_progress_retention_sec,
              cfg.live_reconnect_max);
  return cfg;
}

// ---- d146 R1：文本级修补 settings.json（不引 JSON 库）----
// 设计：settings.json 由本程序生成、结构扁平简单（两组嵌套以内、值无花括号/引号键样式），
// 因此按「"组名" → 配对花括号 → "键名" → 冒号后值」的文本锚点修补是可靠的。
// 值扫描引号/括号感知（monitors 数组整体替换用）；原子写 .tmp → 替换。
namespace {

// 键位置 → 键后第一个 '{' 与其配对 '}' 的位置（组体）。找不到返回 false。
bool group_body(const std::string& t, size_t key_pos, size_t* lbrace, size_t* rbrace) {
  size_t lb = t.find('{', key_pos);
  if (lb == std::string::npos) return false;
  int depth = 0;
  for (size_t i = lb; i < t.size(); ++i) {
    char c = t[i];
    if (c == '"') {  // 字符串整体跳过（转义感知）
      ++i;
      while (i < t.size()) {
        char q = t[i++];
        if (q == '\\') { if (i < t.size()) ++i; else return false; continue; }
        if (q == '"') break;
      }
      if (i >= t.size()) return false;
      continue;
    }
    if (c == '{' || c == '[') ++depth;
    else if (c == '}' || c == ']') {
      if (--depth == 0) { *lbrace = lb; *rbrace = i; return true; }
    }
  }
  return false;
}

// 在 [from, to) 内找 "\"key\""（首次出现；本文件键名不在字符串值中复现，接受该假设）
size_t find_key(const std::string& t, size_t from, size_t to, const std::string& key) {
  const std::string pat = "\"" + key + "\"";
  const size_t p = t.find(pat, from);
  return (p == std::string::npos || p >= to) ? std::string::npos : p;
}

// 值区扫描：从冒号后第一个非空白起，引号/括号感知推进到值尾（顶层 ',' 前或值末）
bool value_span(const std::string& t, size_t colon, size_t limit, size_t* v0, size_t* v1) {
  size_t i = colon + 1;
  while (i < limit && isspace(static_cast<unsigned char>(t[i]))) ++i;
  if (i >= limit) return false;
  *v0 = i;
  int depth = 0;
  size_t end = i;
  while (i < limit) {
    char c = t[i];
    if (c == '"') {  // 字符串整体跳过
      ++i;
      while (i < limit) {
        char q = t[i++];
        if (q == '\\') { if (i < limit) ++i; continue; }
        if (q == '"') break;
      }
      end = i;
      continue;
    }
    if (c == '{' || c == '[') ++depth;
    else if (c == '}' || c == ']') --depth;
    else if (c == ',' && depth == 0) break;
    ++i;
    if (depth == 0 && c != ' ' && c != '\t' && c != '\r' && c != '\n') end = i;
  }
  *v1 = end;
  return *v1 > *v0;
}

// 组路径 "ui.window" 逐层下钻，返回最内层组键位置并输出其组体范围；中途缺失 npos。
size_t drill_group(const std::string& t, const std::string& group_path, size_t* lbrace,
                   size_t* rbrace) {
  size_t from = 0, to = t.size();
  size_t seg_start = 0;
  for (size_t i = 0; i <= group_path.size(); ++i) {
    if (i == group_path.size() || group_path[i] == '.') {
      const std::string seg = group_path.substr(seg_start, i - seg_start);
      seg_start = i + 1;
      const size_t kp = find_key(t, from, to, seg);
      if (kp == std::string::npos) return std::string::npos;
      size_t lb = 0, rb = 0;
      if (!group_body(t, kp, &lb, &rb)) return std::string::npos;
      from = lb + 1;
      to = rb;
      if (i == group_path.size()) {
        *lbrace = lb;
        *rbrace = rb;
        return kp;
      }
    }
  }
  return std::string::npos;
}

}  // namespace

bool Config::patch(const std::string& path, const std::vector<PatchKV>& kvs) {
  std::string text;
  {
    std::ifstream f(path, std::ios::binary);
    if (f) {
      std::ostringstream ss;
      ss << f.rdbuf();
      text = ss.str();
    } else {
      text = "{}\n";
    }
  }
  bool changed = false;
  for (const auto& kv : kvs) {
    size_t lb = 0, rb = 0;
    size_t gp = drill_group(text, kv.group, &lb, &rb);
    if (gp == std::string::npos) {
      // 组路径部分缺失：找到最深的已存在层，把剩余层+键作为链挂进去；
      // 连第一层组都没有 → 整条链追加到顶层 '}' 前。
      std::vector<std::string> segs;
      {
        std::string cur;
        for (char c : kv.group) {
          if (c == '.') { segs.push_back(cur); cur.clear(); } else cur += c;
        }
        segs.push_back(cur);
      }
      size_t used = 0, plb = 0, prb = 0;
      for (size_t i = 0; i < segs.size(); ++i) {
        std::string partial = segs[0];
        for (size_t k = 1; k <= i; ++k) partial += "." + segs[k];
        size_t l2 = 0, r2 = 0;
        if (drill_group(text, partial, &l2, &r2) == std::string::npos) break;
        used = i + 1;
        plb = l2;
        prb = r2;
      }
      // 剩余层（含键）从内向外包成一条链
      std::string rest = "\"" + kv.key + "\": " + kv.raw_value;
      for (size_t i = segs.size(); i-- > used;) rest = "\"" + segs[i] + "\": { " + rest + " }";
      std::string ins = "\n    " + rest;
      if (used > 0) {
        size_t p = prb;
        while (p > plb + 1 && isspace(static_cast<unsigned char>(text[p - 1]))) --p;
        if (p > plb + 1 && text[p - 1] != '{' && text[p - 1] != ',') ins = "," + ins;
        text.insert(p, ins);
      } else {
        const size_t top = text.rfind('}');
        const size_t at = top == std::string::npos ? text.size() : top;
        std::string pre;
        size_t p = at;
        while (p > 0 && isspace(static_cast<unsigned char>(text[p - 1]))) --p;
        if (p > 0 && text[p - 1] != '{') pre = ",";
        text.insert(at, pre + "\n  " + rest);
      }
      changed = true;
      continue;
    }
    // 组存在：组内找键
    const size_t kp = find_key(text, lb + 1, rb, kv.key);
    if (kp != std::string::npos) {
      const size_t colon = text.find(':', kp + kv.key.size() + 2);
      size_t v0 = 0, v1 = 0;
      if (colon != std::string::npos && value_span(text, colon, rb, &v0, &v1)) {
        if (text.compare(v0, v1 - v0, kv.raw_value) != 0) {
          text.replace(v0, v1 - v0, kv.raw_value);
          changed = true;
        }
      }
    } else {
      // 键缺失：追加到组尾（'}' 前最后一个非空白后，缺逗号补逗号）
      size_t p = rb;
      while (p > lb + 1 && isspace(static_cast<unsigned char>(text[p - 1]))) --p;
      std::string ins;
      if (p > lb + 1 && text[p - 1] != '{' && text[p - 1] != ',') ins += ",";
      ins += "\n    \"" + kv.key + "\": " + kv.raw_value;
      text.insert(p, ins);
      changed = true;
    }
  }
  if (!changed) return true;
  // 原子写：.tmp → 替换（Windows 上 rename 不能覆盖已存在目标，先 remove）
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) {
      FR_LOG_WARN("[APP] 配置写盘失败（tmp 打不开）：{}", tmp);
      return false;
    }
    f << text;
  }
  std::error_code ec;
  std::filesystem::remove(path, ec);
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    FR_LOG_WARN("[APP] 配置写盘失败（rename）：{} ({})", path, ec.message());
    return false;
  }
  return true;
}

bool Config::patch_monitors(const std::string& path,
                            const std::vector<MonitorMemory>& mons) {
  // 序列化数组（fp 是纯 ASCII 指纹，无转义需求；仍走保守检查）
  std::string arr = "[";
  for (size_t i = 0; i < mons.size(); ++i) {
    const auto& m = mons[i];
    std::string fp;
    for (char c : m.fp)
      if (c != '"' && c != '\\' && static_cast<unsigned char>(c) >= 0x20) fp += c;
    char buf[192];
    snprintf(buf, sizeof(buf),
             "\n        { \"fp\": \"%s\", \"x\": %d, \"y\": %d, \"w\": %d, \"h\": %d, "
             "\"maximized\": %s, \"fullscreen\": %s, \"last_used\": %lld }%s",
             fp.c_str(), m.x, m.y, m.w, m.h, m.maximized ? "true" : "false",
             m.fullscreen ? "true" : "false", m.last_used, i + 1 < mons.size() ? "," : "");
    arr += buf;
  }
  arr += mons.empty() ? "]" : "\n      ]";
  return patch(path, {{"ui.window", "monitors", arr}});
}

}  // namespace fr
