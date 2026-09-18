#pragma once
// 轻量工具（app 基础设施，任何层可 include；不含第三方依赖）。
#include <cstdint>
#include <string>

namespace fr {

// "H:MM:SS" / "MM:SS" / 秒数 → 秒（DMR Seek 目标与 #t= fragment 共用，容错解析）
double parse_hms_seconds(const std::string& s);

// 秒 → "H:MM:SS"（**UI 专用**，小时位不补零，保持界面观感）
std::string format_hms_seconds(double sec);

// 秒 → "HH:MM:SS"（**协议专用**：GetPositionInfo/GetMediaInfo 的时长与位置、GENA LastChange）。
// DLNA 对 DMR 要求两位小时；iOS 侧惯用 "HH:mm:ss" 严格解析 ⇒ "0:03:49" 会解析失败，
// "00:03:49" 两种解析器都能吃（SPEC §3-R3，d135）。
std::string format_hms_padded(double sec);

// 去掉 URI 的 #t=... 片段（重复 URI 判定按 base 比较）
std::string uri_without_fragment(const std::string& uri);

// 提取 #t=xx / #t=hh:mm:ss 片段秒数；无片段返回 -1
double uri_start_fragment(const std::string& uri);

// 从 DIDL-Lite 提取 dc:title（找不到返回空串）
std::string extract_dc_title(const std::string& didl);

// XML 基本反转义（&amp; &lt; &gt; &quot; &apos;）——SOAP 入参侧
std::string xml_unescape(const std::string& s);
// XML 基本转义（SOAP 出参侧）
std::string xml_escape(const std::string& s);
// 去掉 CDATA 包裹
std::string unwrap_cdata(const std::string& s);

}  // namespace fr
