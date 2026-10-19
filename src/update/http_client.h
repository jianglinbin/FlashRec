#pragma once
// 自动更新 HTTP 薄封装（libcurl 双平台）。只做 GET：重定向、超时、Range 续传、
// 响应体上限、进度与取消。无平台宏；每个请求独立 easy handle，可并发调用。
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fr::update {

enum class HttpError {
  None = 0,
  InvalidUrl,
  UnsupportedScheme,
  Init,
  Connect,
  Timeout,
  Tls,
  TooLarge,
  HttpStatus,   // 非 200/206；具体状态码见 metrics.response_code
  Cancelled,
  WriteFailed,
  Truncated,
  RangeMismatch,
  Other,
};

const char* http_error_name(HttpError error);

struct HttpRequest {
  std::string url;
  long connect_timeout_ms = 5000;
  long timeout_ms = 4000;                 // 0 = 不设总超时（下载用停滞检测）
  long stall_timeout_sec = 0;             // >0：低于 64 B/s 持续该秒数即中止
  std::size_t max_body_bytes = 4u * 1024u * 1024u;  // 0 = 不限（下载写盘）
  long long range_from = -1;              // >=0：Range: bytes=N-；服务器忽略时 resumed=false
  bool allow_http_loopback = false;       // 仅测试：允许 http://127.0.0.1 等本机地址
  std::vector<std::string> headers;       // 额外请求头（"Name: value"）
  std::string user_agent;                 // 空 = 默认 FlashRec/<ver> updater
};

struct HttpMetrics {
  long response_code = 0;
  long long body_bytes = 0;   // 本次实际收到的响应体字节（未请求压缩，即原始字节）
  long long total_ms = 0;     // 发起请求 → 传输结束（含 DNS/TLS/重定向）
  long long ttfb_ms = -1;     // 首字节耗时，仅诊断，不参与选路
  double bytes_per_sec = 0.0; // body_bytes ÷ 完整下载耗时
  long long range_start = -1; // Content-Range 起点；非 206 为 -1
  long long range_total = -1; // Content-Range 总长；未知为 -1
  bool resumed = false;       // 206 且起点与 range_from 一致
  bool truncated = false;     // 声明长度未收全
  std::string effective_url;
};

struct HttpResult {
  HttpError error = HttpError::None;
  std::string error_text;  // 诊断文本，不保证稳定
  HttpMetrics metrics;
  std::string body;        // on_body 为空时收集（≤ max_body_bytes）
};

struct HttpCallbacks {
  // 返回实际写入字节数；返回值 != size 视为写失败并中止。
  std::function<std::size_t(const char* data, std::size_t size)> on_body;
  // 返回 false 表示取消。dl_total 未知时为 0，dl_now 为本次已收字节。
  std::function<bool(long long dl_total, long long dl_now)> on_progress;
  // 首个响应体字节到达前回调（状态码与续传判定已就绪）；返回 false 中止。
  std::function<bool(long response_code, bool resumed)> on_response;
};

// cancel 可从任意线程置位；被取消时 error == Cancelled。
HttpResult http_get(const HttpRequest& request, const HttpCallbacks& callbacks = {},
                    const std::atomic<bool>* cancel = nullptr);

// 同一调用线程内用 curl_multi 并发执行多个 GET（更新模块仍只有一个工作线程）。
// concurrency 上限 8；结果顺序与 requests 一致，body 收集在各自 HttpResult::body。
std::vector<HttpResult> http_get_many(const std::vector<HttpRequest>& requests, int concurrency,
                                      const std::atomic<bool>* cancel = nullptr);

// 导出便于单测：仅 https；allow_http_loopback 时额外允许本机 http。
bool is_allowed_update_url(const std::string& url, bool allow_http_loopback);

}  // namespace fr::update
