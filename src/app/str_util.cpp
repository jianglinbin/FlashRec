#include "app/str_util.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fr {

double parse_hms_seconds(const std::string& s) {
  // 容错：纯秒数 / "MM:SS" / "H:MM:SS" / "H:MM:SS.mmm"；负数或乱码返回 -1
  // 修复：旧实现内层 sscanf("%d:%lf") 无条件重写 m/sec，两冒号时落回外层
  // return 用的是残值 → "0:02:58" 解析成 2s（实测实锤，H:MM:SS 全错）。
  // 改为按冒号个数分流，两段格式互不污染。
  if (s.empty()) return -1.0;
  bool all_num = true;
  for (char c : s)
    if (!(isdigit(static_cast<unsigned char>(c)) || c == '.')) all_num = false;
  if (all_num) return atof(s.c_str());
  const size_t c1 = s.find(':');
  if (c1 == std::string::npos) return -1.0;
  if (s.find(':', c1 + 1) == std::string::npos) {
    // "MM:SS"
    int m = 0;
    double sec = 0;
    if (sscanf(s.c_str(), "%d:%lf", &m, &sec) == 2) return m * 60.0 + sec;
    return -1.0;
  }
  // "H:MM:SS(.mmm)"
  int h = 0, m = 0;
  double sec = 0;
  if (sscanf(s.c_str(), "%d:%d:%lf", &h, &m, &sec) >= 2)
    return h * 3600.0 + m * 60.0 + sec;
  return -1.0;
}

std::string format_hms_seconds(double sec) {
  if (sec < 0) sec = 0;
  long long total = static_cast<long long>(sec + 0.5);
  int h = static_cast<int>(total / 3600);
  int m = static_cast<int>((total % 3600) / 60);
  int s = static_cast<int>(total % 60);
  char buf[32];
  snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
  return buf;
}

std::string format_hms_padded(double sec) {
  if (sec < 0) sec = 0;
  long long total = static_cast<long long>(sec + 0.5);
  int h = static_cast<int>(total / 3600);
  int m = static_cast<int>((total % 3600) / 60);
  int s = static_cast<int>(total % 60);
  char buf[32];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
  return buf;
}

std::string format_speed(double s) {
  char buf[16];
  // 档位都是 0.25 的整数倍：百分位为 0 → 一位小数（1.0/1.5/2.0），否则两位（0.75/1.25）
  const long long hundredths = llround(s * 100.0);
  if (hundredths % 10 == 0)
    snprintf(buf, sizeof(buf), "%.1fx", (double)hundredths / 100.0);
  else
    snprintf(buf, sizeof(buf), "%.2fx", (double)hundredths / 100.0);
  return buf;
}

std::string uri_without_fragment(const std::string& uri) {
  size_t pos = uri.find("#t=");
  if (pos == std::string::npos) return uri;
  return uri.substr(0, pos);
}

double uri_start_fragment(const std::string& uri) {
  size_t pos = uri.find("#t=");
  if (pos == std::string::npos) return -1.0;
  std::string frag = uri.substr(pos + 3);
  size_t amp = frag.find('&');
  if (amp != std::string::npos) frag = frag.substr(0, amp);
  return parse_hms_seconds(frag);
}

std::string unwrap_cdata(const std::string& s) {
  auto trim = [](std::string v) {
    size_t b = v.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    size_t e = v.find_last_not_of(" \t\r\n");
    return v.substr(b, e - b + 1);
  };
  std::string t = trim(s);
  if (t.rfind("<![CDATA[", 0) == 0 && t.size() > 12 && t.compare(t.size() - 3, 3, "]]>") == 0)
    return trim(t.substr(9, t.size() - 12));
  return t;
}

std::string xml_unescape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '&') {
      if (s.compare(i, 5, "&amp;") == 0) { out += '&'; i += 4; }
      else if (s.compare(i, 4, "&lt;") == 0) { out += '<'; i += 3; }
      else if (s.compare(i, 4, "&gt;") == 0) { out += '>'; i += 3; }
      else if (s.compare(i, 6, "&quot;") == 0) { out += '"'; i += 5; }
      else if (s.compare(i, 6, "&apos;") == 0) { out += '\''; i += 5; }
      else out += '&';
    } else {
      out += s[i];
    }
  }
  return out;
}

std::string xml_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 16);
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: out += c;
    }
  }
  return out;
}

std::string extract_dc_title(const std::string& didl) {
  if (didl.empty()) return {};
  // 容错：&lt;dc:title&gt; 可能是转义或原文两种形态
  for (int pass = 0; pass < 2; ++pass) {
    std::string t = pass == 0 ? didl : xml_unescape(didl);
    size_t p = t.find("<dc:title");
    if (p == std::string::npos) continue;
    size_t gt = t.find('>', p);
    size_t end = t.find("</dc:title>", gt);
    if (gt == std::string::npos || end == std::string::npos) continue;
    std::string inner = t.substr(gt + 1, end - gt - 1);
    return unwrap_cdata(xml_unescape(inner));
  }
  return {};
}

}  // namespace fr
