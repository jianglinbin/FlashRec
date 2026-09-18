// 网络接口枚举 —— 见同目录 net_if.h 的文件头说明（多网卡支持 v0.5.0 / d109）。
//
// 平台实现：Windows 用 GetAdaptersAddresses，Linux 用 getifaddrs。
// 平台宏只出现在本文件（AGENTS.md 规则 5）；本文件被追加进 pupnp 的
// upnp_static 目标一起编译，因此 pupnp 内部也能调用（规则 8：不改第三方源码）。

#include "platform/net_if.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

typedef struct FrNetIf {
  char ip[16];        // 点分十进制
  char mask[16];      // 点分十进制
  char name[128];     // 系统接口名（Win = FriendlyName，Linux = ifa_name）
  unsigned int index; // Win IfIndex / Linux if_nametoindex
  unsigned int addr;  // 网络序
  unsigned int nmask; // 网络序
} FrNetIf;

static FrNetIf g_ifs[FR_NET_IF_MAX];
static int g_count = 0;

// 线程局部存储：SSDP 主动广播会按网卡逐张发送，而 HTTP 请求由 miniserver 的
// 工作线程池并发处理，所以「强制本机地址」与「本次请求替换后的描述文档」都必须
// 是线程局部的，不能用普通全局变量。
#if defined(_MSC_VER)
#define FR_TLS __declspec(thread)
#else
#define FR_TLS __thread
#endif

// 「本机地址被强制指定」的上下文（ALIVE/BYEBYE 逐接口广播用）
static FR_TLS char g_forced[16];
static FR_TLS int g_forced_on = 0;

// 「本次请求替换后的描述文档」槽位（URLBase 按 Host 头生成用）
typedef struct FrDocSlot {
  char* buf;
  size_t len;
} FrDocSlot;
static FR_TLS FrDocSlot g_doc_slot;

static void doc_slot_clear(void) {
  if (g_doc_slot.buf) free(g_doc_slot.buf);
  g_doc_slot.buf = NULL;
  g_doc_slot.len = 0;
}

// ---------- 小工具 ----------

static void lower_copy(char* dst, size_t n, const char* src) {
  size_t i = 0;
  for (; src && src[i] && i + 1 < n; ++i) {
    char c = src[i];
    dst[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
  }
  dst[i] = '\0';
}

// 虚拟 / 隧道类接口名特征。**只用于排序**（把物理网卡排前面，让"主接口"
// 这个兜底概念落在真实网卡上），不用于剔除 —— 多网卡支持的语义是全都参与。
static int is_virtual_name(const char* name) {
  static const char* kKeys[] = {
      "vethernet", "hyper-v", "vmware",  "virtualbox", "virtual",
      "docker",    "veth",    "virbr",   "vmnet",      "tap",
      "tun",       "wireguard", "openvpn", "tunnel",   "bluetooth",
      "loopback",  "default switch",
  };
  char low[128];
  lower_copy(low, sizeof(low), name);
  for (size_t i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); ++i) {
    if (strstr(low, kKeys[i])) return 1;
  }
  return 0;
}

static int mask_from_prefix(int prefix, char* out, size_t n) {
  if (prefix < 0 || prefix > 32) return 0;
  unsigned int m = (prefix == 0) ? 0u : (0xFFFFFFFFu << (32 - prefix));
  struct in_addr a;
  a.s_addr = htonl(m);
  return inet_ntop(AF_INET, &a, out, (socklen_t)n) != NULL;
}

static void add_if(const char* ip, const char* mask, const char* name, unsigned int index) {
  if (!ip || g_count >= FR_NET_IF_MAX) return;
  FrNetIf* e = &g_ifs[g_count];
  snprintf(e->ip, sizeof(e->ip), "%s", ip);
  snprintf(e->mask, sizeof(e->mask), "%s", mask ? mask : "0.0.0.0");
  snprintf(e->name, sizeof(e->name), "%s", name ? name : "?");
  e->index = index;
  struct in_addr a, m;
  e->addr = (inet_pton(AF_INET, e->ip, &a) == 1) ? a.s_addr : 0u;
  e->nmask = (inet_pton(AF_INET, e->mask, &m) == 1) ? m.s_addr : 0u;
  ++g_count;
}

// 稳定分区：物理网卡在前，虚拟/隧道的在后（各自保持原枚举顺序）
static void sort_physical_first(void) {
  FrNetIf tmp[FR_NET_IF_MAX];
  int n = 0;
  for (int i = 0; i < g_count; ++i) {
    if (!is_virtual_name(g_ifs[i].name)) tmp[n++] = g_ifs[i];
  }
  for (int i = 0; i < g_count; ++i) {
    if (is_virtual_name(g_ifs[i].name)) tmp[n++] = g_ifs[i];
  }
  memcpy(g_ifs, tmp, sizeof(FrNetIf) * (size_t)g_count);
}

// 参与网段判定的条目必须是「有地址 + 有有效掩码」的：掩码为 0 的条目
// （Linux 上 ifa_netmask 缺失时的兜底值）会匹配到任意地址，必须排除，
// 否则会把所有对端都判成"同网段"，选出错误的网卡。
static int valid_entry(const FrNetIf* e) {
  return e->addr != 0u && e->nmask != 0u;
}

// ---------- 平台扫描 ----------

#ifdef _WIN32
static int scan_impl(void) {
  ULONG size = 16 * 1024;
  IP_ADAPTER_ADDRESSES* adapts = NULL;
  ULONG ret = ERROR_BUFFER_OVERFLOW;

  for (int attempt = 0; attempt < 3 && ret == ERROR_BUFFER_OVERFLOW; ++attempt) {
    free(adapts);
    adapts = (IP_ADAPTER_ADDRESSES*)malloc(size);
    if (!adapts) return 0;
    ret = GetAdaptersAddresses(AF_INET,
                               GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                   GAA_FLAG_SKIP_DNS_SERVER,
                               NULL, adapts, &size);
  }
  if (ret != NO_ERROR) {
    free(adapts);
    return 0;
  }

  for (IP_ADAPTER_ADDRESSES* a = adapts; a != NULL; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp) continue;
    if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
    if (a->Flags & IP_ADAPTER_NO_MULTICAST) continue; // 不支持组播的接口对 SSDP 无意义
    for (IP_ADAPTER_UNICAST_ADDRESS* ua = a->FirstUnicastAddress; ua != NULL; ua = ua->Next) {
      if (!ua->Address.lpSockaddr || ua->Address.lpSockaddr->sa_family != AF_INET) continue;
      const struct sockaddr_in* sin = (const struct sockaddr_in*)ua->Address.lpSockaddr;
      char ip[16] = {0};
      char mask[16] = {0};
      if (!inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip))) continue;
      if (!mask_from_prefix((int)ua->OnLinkPrefixLength, mask, sizeof(mask))) continue;
      char name[128] = {0};
      WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, name, (int)sizeof(name) - 1, NULL, NULL);
      add_if(ip, mask, name, (unsigned int)a->IfIndex);
      break; // 每个适配器只取第一个 IPv4 地址
    }
  }
  free(adapts);
  return g_count;
}
#else
static int scan_impl(void) {
  struct ifaddrs* ifap = NULL;
  if (getifaddrs(&ifap) != 0) return 0;

  for (struct ifaddrs* ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
    if (!(ifa->ifa_flags & IFF_UP)) continue;
    if (ifa->ifa_flags & IFF_LOOPBACK) continue;
    if (!(ifa->ifa_flags & IFF_MULTICAST)) continue;
    const struct sockaddr_in* sin = (const struct sockaddr_in*)ifa->ifa_addr;
    const struct sockaddr_in* nm = (const struct sockaddr_in*)ifa->ifa_netmask;
    char ip[16] = {0};
    char mask[16] = {0};
    if (!inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip))) continue;
    if (!nm || !inet_ntop(AF_INET, &nm->sin_addr, mask, sizeof(mask))) {
      snprintf(mask, sizeof(mask), "0.0.0.0");
    }
    add_if(ip, mask, ifa->ifa_name, if_nametoindex(ifa->ifa_name));
  }
  freeifaddrs(ifap);
  return g_count;
}
#endif

// ---------- 对外接口 ----------

int fr_net_if_scan(void) {
  g_count = 0;
  memset(g_ifs, 0, sizeof(g_ifs));
  if (scan_impl() > 0) sort_physical_first();
  return g_count;
}

int fr_net_if_count(void) { return g_count; }

const char* fr_net_if_ip(int idx) {
  return (idx >= 0 && idx < g_count) ? g_ifs[idx].ip : NULL;
}

const char* fr_net_if_mask(int idx) {
  return (idx >= 0 && idx < g_count) ? g_ifs[idx].mask : NULL;
}

const char* fr_net_if_name(int idx) {
  return (idx >= 0 && idx < g_count) ? g_ifs[idx].name : NULL;
}

unsigned int fr_net_if_index(int idx) {
  return (idx >= 0 && idx < g_count) ? g_ifs[idx].index : 0u;
}

const char* fr_net_if_http_bind(void) {
  // 通配地址：让 HTTP 服务在所有网卡上可达（多网卡的核心诉求之一）。
  return "0.0.0.0";
}

const char* fr_net_if_local_for(const char* peer_ip) {
  // 逐接口广播：本机地址由发送循环强制指定，优先于任何网段匹配
  // （组播目的地址 239.255.255.250 匹配不到任何本机网段，正好由这里接管）。
  if (g_forced_on) return g_forced;

  if (g_count == 0 || !peer_ip) return NULL;
  struct in_addr pa;
  if (inet_pton(AF_INET, peer_ip, &pa) != 1) return NULL;
  const unsigned int p = pa.s_addr;
  for (int i = 0; i < g_count; ++i) {
    if (!valid_entry(&g_ifs[i])) continue;
    if ((p & g_ifs[i].nmask) == (g_ifs[i].addr & g_ifs[i].nmask)) {
      return g_ifs[i].ip; // 命中同网段
    }
  }
  for (int i = 0; i < g_count; ++i) {
    if (valid_entry(&g_ifs[i])) return g_ifs[i].ip; // 跨网段：回退主接口
  }
  return NULL;
}

int fr_net_if_subnet_match_nbo(unsigned int ip_nbo, int no_if_fallback) {
  // 接口枚举失败 → 交给调用方（GENA 订阅校验）用它补丁前的原判定式。
  if (g_count == 0) return no_if_fallback;
  for (int i = 0; i < g_count; ++i) {
    if (!valid_entry(&g_ifs[i])) continue;
    if ((ip_nbo & g_ifs[i].nmask) == (g_ifs[i].addr & g_ifs[i].nmask)) return 1;
  }
  return 0;
}

const char* fr_net_if_preferred_host(const char* host_port_fallback) {
  // 注意入参是 "ip:端口"（libupnp 的 addrToString 产物），不是裸 IP ——
  // 只换主机段、**端口原样保留**。早先直接把整串换成裸 IP，导致 URLBase 变成
  // http://192.0.2.1/（端口丢失）⇒ 客户端按 80 端口发 SOAP ⇒ 连接被拒。
  static char buf[64];
  const char* ip = NULL;
  for (int i = 0; i < g_count; ++i) {
    if (valid_entry(&g_ifs[i])) { // 列表首项 = 物理网卡优先
      ip = g_ifs[i].ip;
      break;
    }
  }
  if (!ip || !host_port_fallback) return host_port_fallback;
  const char* colon = strrchr(host_port_fallback, ':'); // IPv4 "ip:port"
  if (colon) {
    snprintf(buf, sizeof(buf), "%s%s", ip, colon); // 优选 IP + ":端口"
  } else {
    snprintf(buf, sizeof(buf), "%s", ip);
  }
  return buf;
}

// 不分大小写地在 s 中找 needle 首次出现的位置（needle 视为小写）。
static const char* find_ci(const char* s, const char* needle) {
  size_t n = strlen(needle);
  for (; *s; ++s) {
    size_t i = 0;
    while (i < n) {
      char c = s[i];
      if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
      if (c != needle[i]) break;
      ++i;
    }
    if (i == n) return s;
  }
  return NULL;
}

char* fr_net_if_rewrite_location(const char* packet, const char* local_ip) {
  if (!packet) return NULL;

  const size_t plen = strlen(packet);
  const char* q = NULL;
  if (local_ip && local_ip[0]) {
    // 找 LOCATION 头（HTTP 头名不区分大小写；pupnp 输出为大写）
    const char* h = find_ci(packet, "location:");
    if (h) {
      q = h + 9;                                   // 跳过 "location:"
      while (*q == ' ' || *q == '\t') ++q;         // 跳过空白
      if (strncmp(q, "http://", 7) != 0) {
        q = NULL;                                  // 只重写 http URL
      } else if (q[7] == '[') {
        q = NULL;  // IPv6 字面量（http://[fe80::1]:1900/…）：本模块只管 IPv4
      }
    }
  }

  if (q) {
    const size_t hstart = (size_t)(q + 7 - packet); // 主机段起点
    size_t hend = hstart;
    while (packet[hend] && packet[hend] != '/' && packet[hend] != ':' &&
           packet[hend] != ' ' && packet[hend] != '\r' &&
           packet[hend] != '\n') {
      ++hend; // 主机段终点 = ':' / '/' 或行尾
    }
    const size_t llen = strlen(local_ip);
    if (hend > hstart && llen <= 63) { // 长度护栏：IPv4 文本最长 15
      char* out = (char*)malloc(plen - (hend - hstart) + llen + 1);
      if (!out) return NULL; // 分配失败 → 调用方回退发原包
      memcpy(out, packet, hstart);
      memcpy(out + hstart, local_ip, llen);
      memcpy(out + hstart + llen, packet + hend, plen - hend + 1); // 含 NUL
      return out;
    }
  }

  // 无需重写：也返回副本，好让调用方对所有包走同一条释放路径
  char* out = (char*)malloc(plen + 1);
  if (!out) return NULL;
  memcpy(out, packet, plen + 1);
  return out;
}

// ---------- 主动广播的「逐接口各发一份」上下文（d117）----------

int fr_net_if_addr_is_mcast_nbo(unsigned int ip_nbo) {
  struct in_addr m;
  if (inet_pton(AF_INET, "239.255.255.250", &m) != 1) return 0;
  return m.s_addr == ip_nbo ? 1 : 0;
}

void fr_net_if_force_begin(const char* local_ip) {
  struct in_addr a;
  if (!local_ip || inet_pton(AF_INET, local_ip, &a) != 1) {
    g_forced_on = 0; // 非法地址：不强制（调用方应只传枚举出来的地址）
    return;
  }
  snprintf(g_forced, sizeof(g_forced), "%s", local_ip);
  g_forced_on = 1;
}

void fr_net_if_force_end(void) { g_forced_on = 0; }

const char* fr_net_if_forced(void) { return g_forced_on ? g_forced : NULL; }

// ---------- 描述文档 URLBase 按本请求的 Host 头生成（d117）----------

// 有界、不分大小写地在 [s, s+n) 中找 needle（needle 视为小写）。
static const char* mem_find_ci(const char* s, size_t n, const char* needle) {
  size_t nl = strlen(needle);
  if (!s || nl == 0 || n < nl) return NULL;
  for (size_t i = 0; i + nl <= n; ++i) {
    size_t k = 0;
    while (k < nl) {
      char c = s[i + k];
      if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
      if (c != needle[k]) break;
      ++k;
    }
    if (k == nl) return s + i;
  }
  return NULL;
}

char* fr_net_if_doc_take(size_t* out_len) {
  char* b = g_doc_slot.buf;
  if (out_len) *out_len = g_doc_slot.len;
  g_doc_slot.buf = NULL;
  g_doc_slot.len = 0;
  return b;
}

size_t fr_net_if_doc_localize(const char* doc,
                              size_t doc_len,
                              const char* host,
                              size_t host_len) {
  doc_slot_clear(); // 先清上一次（可能因错误路径没被取走）

  if (!doc || doc_len == 0 || !host || host_len == 0 || host_len > 64) return 0;

  // Host 只允许 主机名 / IPv4 / 可选端口 的字符集。它会被写进我们自己的 XML，
  // 必须挡掉 '<' '>' '&' 等注入字符（Host 是客户端可控的）。
  for (size_t i = 0; i < host_len; ++i) {
    char c = host[i];
    int ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
             (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == ':';
    if (!ok) return 0;
  }

  // 定位 <URLBase> 的值区间
  const char* ub = mem_find_ci(doc, doc_len, "<urlbase");
  if (!ub) return 0; // 文档里没有 URLBase → 客户端按 LOCATION 解析，本就正确
  size_t i = (size_t)(ub - doc);
  while (i < doc_len && doc[i] != '>') ++i;
  if (i >= doc_len) return 0;
  const size_t v_start = i + 1;
  size_t v_end = v_start;
  while (v_end < doc_len && doc[v_end] != '<') ++v_end;
  if (v_end <= v_start) return 0;

  // 值形如 http://<authority>/path —— 要替换的是**整个 authority**（主机+可选端口），
  // 不是只有主机：只换主机会把 Host 里的端口留在后面，变成 "ip:49152:49152"
  //（本版实测踩到）。因此这里取到第一个 '/' 为止。
  static const char kScheme[] = "http://";
  const size_t kSchemeLen = sizeof(kScheme) - 1;
  if (v_end - v_start <= kSchemeLen) return 0;
  for (size_t k = 0; k < kSchemeLen; ++k) {
    char c = doc[v_start + k];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (c != kScheme[k]) return 0;
  }

  const size_t a_start = v_start + kSchemeLen;
  if (doc[a_start] == '[') return 0; // IPv6 字面量：本模块只管 IPv4
  size_t a_end = a_start;
  while (a_end < v_end && doc[a_end] != '/') ++a_end;
  if (a_end <= a_start) return 0;

  // 组装替换文本：
  //   Host 带端口        → 原样用（"ip:端口"）
  //   Host 不带端口      → 保留原 authority 里的端口（d112 教训：丢了端口客户端
  //                        会按 80 发 SOAP，连接被拒）
  char repl[96];
  size_t repl_len;
  const char* cpos = (const char*)memchr(doc + a_start, ':', a_end - a_start);
  if (memchr(host, ':', host_len) != NULL || cpos == NULL) {
    if (host_len >= sizeof(repl)) return 0;
    memcpy(repl, host, host_len);
    repl_len = host_len;
  } else {
    const size_t port_len = a_end - (size_t)(cpos - doc); // 含 ':' 本体
    if (host_len + port_len >= sizeof(repl)) return 0;
    memcpy(repl, host, host_len);
    memcpy(repl + host_len, cpos, port_len);
    repl_len = host_len + port_len;
  }

  // 与现值相同 → 不必替换（客户端本就在我们需要的那张网卡上）
  if (a_end - a_start == repl_len && memcmp(doc + a_start, repl, repl_len) == 0) {
    return 0;
  }

  const size_t out_len = doc_len - (a_end - a_start) + repl_len;
  char* out = (char*)malloc(out_len + 1);
  if (!out) return 0;
  memcpy(out, doc, a_start);
  memcpy(out + a_start, repl, repl_len);
  memcpy(out + a_start + repl_len, doc + a_end, doc_len - a_end);
  out[out_len] = '\0';

  g_doc_slot.buf = out;
  g_doc_slot.len = out_len;
  return out_len;
}
