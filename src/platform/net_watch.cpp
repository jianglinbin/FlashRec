// net_watch.cpp —— 网络变化被动监听实现（★d510，net_watch.h 注释为规格）。
//
// 两平台统一语义：变化 → g_dirty=true。不区分事件类型（消费方拿不准差别，
// 反正决策依据是「重新扫描后的接口快照对比」——见 main.cpp net_fingerprint）。
#include "platform/net_watch.h"

#include <atomic>

namespace fr::platform {
namespace {
std::atomic<bool> g_dirty{false};
std::atomic<bool> g_running{false};
}  // namespace

#ifdef _WIN32

// 顺序铁律：winsock2 → ws2tcpip → windows → iphlpapi（iphlpapi 抢先会拉起
// 没有基础类型的 nldef.h，C2059 满屏）。
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>

namespace {

HANDLE g_handle = nullptr;

// 系统回调线程：只置标志，绝不触碰业务（注册时 InitialNotification=TRUE
// 会先回调一次，正好覆盖「启动后立刻插网线」的最小窗口）。
VOID CALLBACK on_iface_change(PVOID, PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE) {
  g_dirty.store(true, std::memory_order_relaxed);
}

}  // namespace

bool net_watch_start() {
  if (g_running.exchange(true)) return true;  // 幂等
  g_dirty.store(false, std::memory_order_relaxed);
  if (NotifyIpInterfaceChange(AF_UNSPEC, on_iface_change, nullptr, TRUE, &g_handle) != NO_ERROR) {
    g_handle = nullptr;
    g_running.store(false, std::memory_order_relaxed);
    return false;
  }
  return true;
}

void net_watch_stop() {
  if (!g_running.exchange(false, std::memory_order_relaxed)) return;
  if (g_handle) {
    CancelMibChangeNotify2(g_handle);
    g_handle = nullptr;
  }
}

std::vector<std::pair<std::string, std::string>> net_watch_list_interfaces() {
  std::vector<std::pair<std::string, std::string>> out;
  ULONG sz = 16 * 1024;
  std::vector<unsigned char> buf(sz);
  auto* aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
  ULONG rc = ERROR_NO_DATA;
  for (int attempt = 0; attempt < 3; ++attempt) {  // 系统提示缓冲不够 → 重取
    rc = GetAdaptersAddresses(AF_UNSPEC,
                              GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                  GAA_FLAG_SKIP_DNS_SERVER,
                              nullptr, aa, &sz);
    if (rc != ERROR_BUFFER_OVERFLOW) break;
    buf.resize(sz);
    aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
  }
  if (rc != NO_ERROR) return out;
  for (auto* a = aa; a; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp) continue;
    if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
    char name[128] = {0};
    WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, name, sizeof(name), nullptr, nullptr);
    for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
      if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
      const auto* sin = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
      char ip[46] = {0};
      if (!inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip))) continue;
      out.emplace_back(ip, name);
    }
  }
  return out;
}

#else  // __linux__

#include <cerrno>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <poll.h>
#include <thread>
#include <unistd.h>

#include <linux/netlink.h>
#include <sys/socket.h>

namespace {

std::thread g_thread;
int g_fd = -1;                 // netlink 路由 socket
int g_stop_pipe[2] = {-1, -1}; // 停止信号（写 1 字节 → poll 唤醒 → 线程退出）

}  // namespace

bool net_watch_start() {
  if (g_running.exchange(true)) return true;  // 幂等
  g_dirty.store(false, std::memory_order_relaxed);

  const int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
  if (fd < 0) {
    g_running.store(false, std::memory_order_relaxed);
    return false;
  }
  sockaddr_nl addr{};
  addr.nl_family = AF_NETLINK;
  // LINK = 网卡起停；IFADDR = 地址增删。IPv6 也订阅：v6 地址变化常伴随
  // v4 接口状态翻转，宁可多触发（消费方有快照对比兜着，不会误动作）。
  addr.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR;
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
      pipe(g_stop_pipe) < 0) {
    close(fd);
    g_running.store(false, std::memory_order_relaxed);
    return false;
  }
  g_fd = fd;
  g_thread = std::thread([fd] {
    pollfd pfd[2];
    while (g_running.load(std::memory_order_relaxed)) {
      pfd[0] = {fd, POLLIN, 0};
      pfd[1] = {g_stop_pipe[0], POLLIN, 0};
      const int rc = poll(pfd, 2, -1);
      if (rc < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if (rc == 0) continue;
      if (pfd[1].revents & POLLIN) break;  // 停止信号
      if (pfd[0].revents & POLLIN) {
        // 一次变化常是连串 netlink 报文；逐条读但标志幂等，防抖在消费方。
        char buf[4096];
        while (recv(fd, buf, sizeof(buf), 0) > 0) {
          g_dirty.store(true, std::memory_order_relaxed);
        }
      }
    }
  });
  return true;
}

void net_watch_stop() {
  if (!g_running.exchange(false, std::memory_order_relaxed)) return;
  if (g_stop_pipe[1] >= 0) {
    const char c = 'x';
    ssize_t ignored = write(g_stop_pipe[1], &c, 1);
    (void)ignored;
  }
  if (g_thread.joinable()) g_thread.join();
  if (g_fd >= 0) {
    close(g_fd);
    g_fd = -1;
  }
  for (int& e : g_stop_pipe) {
    if (e >= 0) {
      close(e);
      e = -1;
    }
  }
}

std::vector<std::pair<std::string, std::string>> net_watch_list_interfaces() {
  std::vector<std::pair<std::string, std::string>> out;
  ifaddrs* ifap = nullptr;
  if (getifaddrs(&ifap) != 0) return out;
  for (ifaddrs* ifa = ifap; ifa; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
    if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;
    char ip[64] = {0};
    if (getnameinfo(ifa->ifa_addr, sizeof(sockaddr_in), ip, sizeof(ip), nullptr, 0,
                    NI_NUMERICHOST) != 0)
      continue;
    out.emplace_back(ip, ifa->ifa_name ? ifa->ifa_name : "?");
  }
  freeifaddrs(ifap);
  return out;
}

#endif  // 平台开关

bool net_watch_consume() { return g_dirty.exchange(false, std::memory_order_relaxed); }

}  // namespace fr::platform
