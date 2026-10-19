#include "update/http_client.h"

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>

#ifndef FLASHREC_VERSION
#define FLASHREC_VERSION "0.0.0"
#endif

namespace fr::update {
namespace {

constexpr long kLowSpeedBytesPerSec = 64;
constexpr int kMaxConcurrency = 8;

std::once_flag g_curl_once;
CURLcode g_curl_init = CURLE_OK;

void init_curl_once() {
  std::call_once(g_curl_once, [] { g_curl_init = curl_global_init(CURL_GLOBAL_DEFAULT); });
}

// Windows Schannel 用系统证书库；OpenSSL 后端在 Linux 上按常见发行版路径兜底
// （AppImage 在 Fedora 等系统上可能没有构建机编译进去的默认 CA 路径）。
const char* ca_bundle_path() {
  static const std::string path = [] {
    const curl_version_info_data* vi = curl_version_info(CURLVERSION_NOW);
    if (vi == nullptr || vi->ssl_version == nullptr) return std::string();
    if (std::string_view(vi->ssl_version).find("Schannel") != std::string_view::npos)
      return std::string();
    static const char* kCandidates[] = {
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/etc/ssl/ca-bundle.pem",
        "/etc/ssl/cert.pem",
    };
    for (const char* candidate : kCandidates) {
      std::ifstream f(candidate, std::ios::binary);
      if (f) return std::string(candidate);
    }
    return std::string();
  }();
  return path.empty() ? nullptr : path.c_str();
}

struct HeaderState {
  long long range_start = -1;
  long long range_total = -1;
};

struct TransferState {
  const HttpRequest* request = nullptr;
  const HttpCallbacks* callbacks = nullptr;
  const std::atomic<bool>* cancel = nullptr;
  HttpResult* result = nullptr;
  HeaderState headers;
  long status_code = 0;
  bool response_notified = false;
  bool too_large = false;
  bool write_failed = false;
  bool user_cancelled = false;
};

struct EasyRequest {
  CURL* handle = nullptr;
  curl_slist* headers = nullptr;
  char error_buffer[CURL_ERROR_SIZE] = {0};
  HttpCallbacks callbacks;
  TransferState state;
  std::chrono::steady_clock::time_point started;
  bool added = false;
};

bool ascii_iequal_prefix(std::string_view text, std::string_view prefix) {
  if (text.size() < prefix.size()) return false;
  for (size_t i = 0; i < prefix.size(); ++i) {
    char a = text[i];
    char b = prefix[i];
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
    s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
    s.remove_suffix(1);
  return s;
}

bool parse_ll(std::string_view s, size_t* pos, long long* out) {
  size_t i = *pos;
  long long v = 0;
  const size_t begin = i;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
    if (v > (9223372036854775807LL - 9) / 10) return false;
    v = v * 10 + (s[i] - '0');
    ++i;
  }
  if (i == begin) return false;
  *pos = i;
  *out = v;
  return true;
}

// "Content-Range: bytes 100-199/200"（大小写不敏感）
void parse_content_range(std::string_view line, HeaderState* out) {
  constexpr std::string_view kName = "content-range:";
  if (!ascii_iequal_prefix(line, kName)) return;
  std::string_view v = trim(line.substr(kName.size()));
  constexpr std::string_view kUnit = "bytes ";
  if (!ascii_iequal_prefix(v, kUnit)) return;
  v.remove_prefix(kUnit.size());
  size_t pos = 0;
  long long start = 0;
  if (!parse_ll(v, &pos, &start) || pos >= v.size() || v[pos] != '-') return;
  ++pos;
  long long end = 0;
  if (!parse_ll(v, &pos, &end) || pos >= v.size() || v[pos] != '/') return;
  ++pos;
  long long total = -1;
  if (pos < v.size() && v[pos] == '*') {
    ++pos;
  } else if (!parse_ll(v, &pos, &total)) {
    return;
  }
  out->range_start = start;
  out->range_total = total;
}

// "HTTP/1.1 200 OK"
void parse_status_line(std::string_view line, TransferState* st) {
  st->headers = HeaderState{};
  st->response_notified = false;
  const size_t space = line.find(' ');
  if (space == std::string_view::npos) return;
  size_t i = space + 1;
  int code = 0;
  int digits = 0;
  while (i < line.size() && digits < 3 && line[i] >= '0' && line[i] <= '9') {
    code = code * 10 + (line[i] - '0');
    ++i;
    ++digits;
  }
  if (digits == 3) st->status_code = code;
}

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* st = static_cast<TransferState*>(userdata);
  const size_t n = size * nmemb;
  if (st->cancel != nullptr && st->cancel->load(std::memory_order_relaxed)) {
    st->user_cancelled = true;
    return 0;
  }
  // 3xx 是 FOLLOWLOCATION 的中间跳；其响应体属于跳转页，绝不能写入下载文件，
  // 也不能触发 on_response（那会让调用方误以为拿到了最终响应）。
  if (st->status_code >= 300 && st->status_code < 400) return n;
  // 流式写盘（on_body）只接收成功响应；错误页不得混进 .part 破坏续传。
  if (st->callbacks->on_body && st->status_code >= 400) return n;
  if (!st->response_notified && st->status_code >= 200 && st->status_code < 300 &&
      st->status_code != 204) {
    st->response_notified = true;
    if (st->callbacks->on_response) {
      const bool resumed = st->request->range_from >= 0 && st->status_code == 206 &&
                           st->headers.range_start == st->request->range_from;
      try {
        if (!st->callbacks->on_response(st->status_code, resumed)) {
          st->user_cancelled = true;
          return 0;
        }
      } catch (...) {
        st->user_cancelled = true;
        return 0;
      }
    }
  }
  const long long already = st->result->metrics.body_bytes;
  if (st->request->max_body_bytes != 0 &&
      already + static_cast<long long>(n) > static_cast<long long>(st->request->max_body_bytes)) {
    st->too_large = true;
    return 0;
  }
  try {
    if (st->callbacks->on_body) {
      if (st->callbacks->on_body(ptr, n) != n) {
        st->write_failed = true;
        return 0;
      }
    } else {
      st->result->body.append(ptr, n);
    }
  } catch (...) {
    st->write_failed = true;
    return 0;
  }
  st->result->metrics.body_bytes += static_cast<long long>(n);
  return n;
}

size_t header_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
  auto* st = static_cast<TransferState*>(userdata);
  const size_t n = size * nitems;
  std::string_view line(buffer, n);
  if (line.rfind("HTTP/", 0) == 0) {
    // 重定向/代理 CONNECT 会带来多段响应头：每段状态行重置区间信息。
    parse_status_line(line, st);
    return n;
  }
  parse_content_range(line, &st->headers);
  return n;
}

int xferinfo_cb(void* userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
  auto* st = static_cast<TransferState*>(userdata);
  if (st->cancel != nullptr && st->cancel->load(std::memory_order_relaxed)) {
    st->user_cancelled = true;
    return 1;
  }
  if (st->callbacks->on_progress) {
    try {
      if (!st->callbacks->on_progress(static_cast<long long>(dltotal),
                                      static_cast<long long>(dlnow))) {
        st->user_cancelled = true;
        return 1;
      }
    } catch (...) {
      st->user_cancelled = true;
      return 1;
    }
  }
  return 0;
}

HttpError map_curl_error(CURLcode code, const TransferState& st) {
  if (st.user_cancelled) return HttpError::Cancelled;
  if (st.too_large) return HttpError::TooLarge;
  if (st.write_failed) return HttpError::WriteFailed;
  switch (code) {
    case CURLE_OK:
      return HttpError::None;
    case CURLE_UNSUPPORTED_PROTOCOL:
      return HttpError::UnsupportedScheme;
    case CURLE_URL_MALFORMAT:
      return HttpError::InvalidUrl;
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_COULDNT_CONNECT:
      return HttpError::Connect;
    case CURLE_OPERATION_TIMEDOUT:
      return HttpError::Timeout;
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CIPHER:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_ISSUER_ERROR:
      return HttpError::Tls;
    case CURLE_ABORTED_BY_CALLBACK:
      return HttpError::Cancelled;
    case CURLE_PARTIAL_FILE:
      return HttpError::Truncated;
    case CURLE_RANGE_ERROR:
      return HttpError::RangeMismatch;
    default:
      return HttpError::Other;
  }
}

bool has_forbidden_chars(std::string_view s) {
  for (unsigned char c : s) {
    if (c <= 0x20 || c == 0x7f) return true;
  }
  return false;
}

std::string_view url_authority(std::string_view url, size_t scheme_len) {
  size_t end = url.find_first_of("/?#", scheme_len);
  if (end == std::string_view::npos) end = url.size();
  return url.substr(scheme_len, end - scheme_len);
}

bool authority_is_loopback(std::string_view authority) {
  if (authority.find('@') != std::string_view::npos) return false;
  std::string_view host = authority;
  if (!host.empty() && host.front() == '[') {
    const size_t close = host.find(']');
    if (close == std::string_view::npos) return false;
    return host.substr(0, close + 1) == "[::1]";
  }
  const size_t colon = host.find(':');
  if (colon != std::string_view::npos) host = host.substr(0, colon);
  return host == "127.0.0.1" || host == "localhost";
}

bool prepare_easy(EasyRequest* item, const HttpRequest& request, const std::string& agent) {
  item->handle = curl_easy_init();
  if (item->handle == nullptr) return false;
  TransferState& st = item->state;
  st.request = &request;
  st.callbacks = &item->callbacks;

  CURL* h = item->handle;
  curl_easy_setopt(h, CURLOPT_URL, request.url.c_str());
  curl_easy_setopt(h, CURLOPT_ERRORBUFFER, item->error_buffer);
  curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(h, CURLOPT_HTTPGET, 1L);
  curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(h, CURLOPT_MAXREDIRS, 5L);
  // 初始请求允许本机 HTTP（仅测试），重定向永远只许 HTTPS，杜绝降级。
  curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR,
                   request.allow_http_loopback ? "http,https" : "https");
  curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS_STR, "https");
  curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
  if (const char* ca = ca_bundle_path()) curl_easy_setopt(h, CURLOPT_CAINFO, ca);
  curl_easy_setopt(h, CURLOPT_USERAGENT, agent.c_str());
  curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, request.connect_timeout_ms);
  curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, request.timeout_ms);
  curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(h, CURLOPT_DNS_CACHE_TIMEOUT, 120L);
  curl_easy_setopt(h, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
  curl_easy_setopt(h, CURLOPT_NETRC, CURL_NETRC_IGNORED);
  if (request.stall_timeout_sec > 0) {
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, kLowSpeedBytesPerSec);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, request.stall_timeout_sec);
  }
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &st);
  curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, header_cb);
  curl_easy_setopt(h, CURLOPT_HEADERDATA, &st);
  curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
  curl_easy_setopt(h, CURLOPT_XFERINFODATA, &st);

  for (const std::string& line : request.headers) {
    if (line.empty()) continue;
    struct curl_slist* next = curl_slist_append(item->headers, line.c_str());
    if (next == nullptr) return false;
    item->headers = next;
  }
  if (request.range_from >= 0) {
    char range[64];
    std::snprintf(range, sizeof(range), "Range: bytes=%lld-",
                  static_cast<long long>(request.range_from));
    struct curl_slist* next = curl_slist_append(item->headers, range);
    if (next == nullptr) return false;
    item->headers = next;
  }
  if (item->headers != nullptr) curl_easy_setopt(h, CURLOPT_HTTPHEADER, item->headers);
  return true;
}

void cleanup_easy(EasyRequest* item) {
  if (item->headers != nullptr) curl_slist_free_all(item->headers);
  if (item->handle != nullptr) curl_easy_cleanup(item->handle);
  item->headers = nullptr;
  item->handle = nullptr;
}

void finalize_easy(EasyRequest* item, CURLcode code, long long total_ms,
                   const HttpRequest& request, HttpResult* result) {
  CURL* h = item->handle;
  TransferState& st = item->state;
  curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &result->metrics.response_code);
  curl_off_t ttfb_us = -1;
  if (curl_easy_getinfo(h, CURLINFO_STARTTRANSFER_TIME_T, &ttfb_us) == CURLE_OK && ttfb_us >= 0)
    result->metrics.ttfb_ms = static_cast<long long>(ttfb_us / 1000);
  curl_off_t content_length = -1;
  if (curl_easy_getinfo(h, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &content_length) != CURLE_OK)
    content_length = -1;
  char* effective = nullptr;
  if (curl_easy_getinfo(h, CURLINFO_EFFECTIVE_URL, &effective) == CURLE_OK &&
      effective != nullptr)
    result->metrics.effective_url = effective;

  result->metrics.total_ms = total_ms;
  result->metrics.range_start = st.headers.range_start;
  result->metrics.range_total = st.headers.range_total;
  if (total_ms > 0) {
    result->metrics.bytes_per_sec =
        static_cast<double>(result->metrics.body_bytes) * 1000.0 / static_cast<double>(total_ms);
  }

  result->error = map_curl_error(code, st);
  if (result->error == HttpError::None) {
    if (result->metrics.response_code != 200 && result->metrics.response_code != 206) {
      result->error = HttpError::HttpStatus;
    } else if (request.range_from >= 0) {
      if (result->metrics.response_code == 206) {
        if (result->metrics.range_start == request.range_from) {
          result->metrics.resumed = true;
        } else {
          result->error = HttpError::RangeMismatch;
        }
      } else {
        // 服务器忽略了 Range，返回整文件：调用方需要把已有 .part 截断到 0。
        result->metrics.resumed = false;
      }
    } else if (result->metrics.response_code == 206) {
      result->error = HttpError::HttpStatus;
    }
  }
  if (result->error == HttpError::None && content_length >= 0 &&
      result->metrics.body_bytes < static_cast<long long>(content_length)) {
    result->metrics.truncated = true;
    result->error = HttpError::Truncated;
  }

  if (result->error != HttpError::None) {
    if (item->error_buffer[0] != '\0') {
      result->error_text = item->error_buffer;
    } else if (code != CURLE_OK) {
      result->error_text = curl_easy_strerror(code);
    } else {
      result->error_text = http_error_name(result->error);
    }
    if (result->error == HttpError::HttpStatus)
      result->error_text = "HTTP " + std::to_string(result->metrics.response_code);
  }
}

std::string default_agent() {
  return std::string("FlashRec/") + FLASHREC_VERSION + " updater";
}

}  // namespace

const char* http_error_name(HttpError error) {
  switch (error) {
    case HttpError::None: return "none";
    case HttpError::InvalidUrl: return "invalid-url";
    case HttpError::UnsupportedScheme: return "unsupported-scheme";
    case HttpError::Init: return "init-failed";
    case HttpError::Connect: return "connect";
    case HttpError::Timeout: return "timeout";
    case HttpError::Tls: return "tls";
    case HttpError::TooLarge: return "too-large";
    case HttpError::HttpStatus: return "http-status";
    case HttpError::Cancelled: return "cancelled";
    case HttpError::WriteFailed: return "write-failed";
    case HttpError::Truncated: return "truncated";
    case HttpError::RangeMismatch: return "range-mismatch";
    case HttpError::Other: return "other";
  }
  return "unknown";
}

bool is_allowed_update_url(const std::string& url, bool allow_http_loopback) {
  if (has_forbidden_chars(url)) return false;
  constexpr std::string_view kHttps = "https://";
  constexpr std::string_view kHttp = "http://";
  if (url.rfind(kHttps, 0) == 0) {
    const std::string_view authority = url_authority(url, kHttps.size());
    return !authority.empty() && authority.find('@') == std::string_view::npos;
  }
  if (!allow_http_loopback || url.rfind(kHttp, 0) != 0) return false;
  const std::string_view authority = url_authority(url, kHttp.size());
  return !authority.empty() && authority_is_loopback(authority);
}

HttpResult http_get(const HttpRequest& request, const HttpCallbacks& callbacks,
                    const std::atomic<bool>* cancel) {
  HttpResult result;
  if (!is_allowed_update_url(request.url, request.allow_http_loopback)) {
    result.error = request.url.rfind("http://", 0) == 0 ? HttpError::UnsupportedScheme
                                                        : HttpError::InvalidUrl;
    result.error_text = "URL 不在允许范围（仅 HTTPS，测试可放行本机 HTTP）";
    return result;
  }

  init_curl_once();
  if (g_curl_init != CURLE_OK) {
    result.error = HttpError::Init;
    result.error_text = curl_easy_strerror(g_curl_init);
    return result;
  }

  EasyRequest item;
  item.callbacks = callbacks;
  item.state.cancel = cancel;
  item.state.result = &result;
  const std::string agent = request.user_agent.empty() ? default_agent() : request.user_agent;
  if (!prepare_easy(&item, request, agent)) {
    cleanup_easy(&item);
    result.error = HttpError::Init;
    result.error_text = "curl_easy_init / curl_slist_append failed";
    return result;
  }

  const auto started = std::chrono::steady_clock::now();
  const CURLcode code = curl_easy_perform(item.handle);
  const auto finished = std::chrono::steady_clock::now();
  const long long total_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(finished - started).count();

  finalize_easy(&item, code, total_ms, request, &result);
  cleanup_easy(&item);
  return result;
}

std::vector<HttpResult> http_get_many(const std::vector<HttpRequest>& requests, int concurrency,
                                      const std::atomic<bool>* cancel) {
  const size_t count = requests.size();
  std::vector<HttpResult> results(count);
  if (count == 0) return results;

  init_curl_once();
  if (g_curl_init != CURLE_OK) {
    for (HttpResult& r : results) {
      r.error = HttpError::Init;
      r.error_text = curl_easy_strerror(g_curl_init);
    }
    return results;
  }

  const int limit =
      std::max(1, std::min(concurrency <= 0 ? 1 : concurrency, kMaxConcurrency));
  const std::string agent = default_agent();

  std::vector<std::unique_ptr<EasyRequest>> items(count);
  std::vector<size_t> queued;
  queued.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    if (!is_allowed_update_url(requests[i].url, requests[i].allow_http_loopback)) {
      results[i].error = requests[i].url.rfind("http://", 0) == 0
                             ? HttpError::UnsupportedScheme
                             : HttpError::InvalidUrl;
      results[i].error_text = "URL 不在允许范围（仅 HTTPS，测试可放行本机 HTTP）";
      continue;
    }
    auto item = std::make_unique<EasyRequest>();
    item->state.cancel = cancel;
    item->state.result = &results[i];
    if (!prepare_easy(item.get(), requests[i], agent)) {
      cleanup_easy(item.get());
      results[i].error = HttpError::Init;
      results[i].error_text = "curl_easy_init / curl_slist_append failed";
      continue;
    }
    items[i] = std::move(item);
    queued.push_back(i);
  }
  if (queued.empty()) return results;

  CURLM* multi = curl_multi_init();
  if (multi == nullptr) {
    for (size_t i : queued) {
      cleanup_easy(items[i].get());
      results[i].error = HttpError::Init;
      results[i].error_text = "curl_multi_init failed";
    }
    return results;
  }

  std::map<CURL*, size_t> active;
  size_t next = 0;
  size_t done = 0;
  auto add_item = [&](size_t index) {
    EasyRequest& item = *items[index];
    item.started = std::chrono::steady_clock::now();
    if (curl_multi_add_handle(multi, item.handle) != CURLM_OK) {
      results[index].error = HttpError::Other;
      results[index].error_text = "curl_multi_add_handle failed";
      ++done;
      return;
    }
    item.added = true;
    active[item.handle] = index;
  };
  for (size_t k = 0; k < queued.size() && k < static_cast<size_t>(limit); ++k)
    add_item(queued[next++]);

  while (done < queued.size()) {
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
      const auto now = std::chrono::steady_clock::now();
      for (auto& [handle, index] : active) {
        EasyRequest& item = *items[index];
        item.state.user_cancelled = true;
        const long long ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - item.started).count();
        curl_multi_remove_handle(multi, handle);
        finalize_easy(&item, CURLE_ABORTED_BY_CALLBACK, ms, requests[index], &results[index]);
        item.added = false;
        ++done;
      }
      active.clear();
      for (size_t k = next; k < queued.size(); ++k) {
        results[queued[k]].error = HttpError::Cancelled;
        results[queued[k]].error_text = "cancelled";
        ++done;
      }
      break;
    }

    int running = 0;
    if (curl_multi_perform(multi, &running) != CURLM_OK) {
      const auto now = std::chrono::steady_clock::now();
      for (auto& [handle, index] : active) {
        EasyRequest& item = *items[index];
        const long long ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - item.started).count();
        curl_multi_remove_handle(multi, handle);
        finalize_easy(&item, CURLE_ABORTED_BY_CALLBACK, ms, requests[index], &results[index]);
        results[index].error = HttpError::Other;
        results[index].error_text = "curl_multi_perform failed";
        item.added = false;
        ++done;
      }
      active.clear();
      for (size_t k = next; k < queued.size(); ++k) {
        results[queued[k]].error = HttpError::Other;
        results[queued[k]].error_text = "curl_multi_perform failed";
        ++done;
      }
      break;
    }

    CURLMsg* message = nullptr;
    int pending = 0;
    while ((message = curl_multi_info_read(multi, &pending)) != nullptr) {
      if (message->msg != CURLMSG_DONE) continue;
      const auto found = active.find(message->easy_handle);
      if (found == active.end()) continue;
      const size_t index = found->second;
      active.erase(found);
      EasyRequest& item = *items[index];
      const auto now = std::chrono::steady_clock::now();
      const long long ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - item.started).count();
      curl_multi_remove_handle(multi, message->easy_handle);
      item.added = false;
      finalize_easy(&item, message->data.result, ms, requests[index], &results[index]);
      ++done;
      if (next < queued.size()) add_item(queued[next++]);
    }

    if (done < queued.size()) curl_multi_poll(multi, nullptr, 0, 100, nullptr);
  }

  curl_multi_cleanup(multi);
  for (size_t i : queued) cleanup_easy(items[i].get());
  return results;
}

}  // namespace fr::update
