// 本机回环流服务端 —— 见同目录 stream_local.h 的文件头说明（AirPlay 数据面）。
//
// 平台实现：Win32 用 ioctlsocket FIONBIO 非阻塞 + select 等待（accept 前置
// WSAStartup 引用计数）；POSIX 用 fcntl O_NONBLOCK + poll。平台宏只出现在
// 本文件（AGENTS.md 规则 5），做法照 net_if.c:13-25。
//
// API 返回 int fd：Win32 下是 SOCKET 与 int 的相互收窄（WinSock 句柄实践上
// 恒为小的非负整数，INVALID_SOCKET 单独判定，不会走到 -1），对调用方不透明。

#include "platform/stream_local.h"

#include <string.h>

#ifdef _WIN32
/* CMake 已全局 /D 该宏；加卫语句避免 C4005 宏重定义警告（tray_win32 同款）。 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#endif

// ---------- 平台小工具 ----------

#ifdef _WIN32

static int sock_to_fd(SOCKET s) { return (int)(intptr_t)s; }
static SOCKET fd_to_sock(int fd) { return (SOCKET)(intptr_t)fd; }

// WSAStartup 引用计数：本模块 socket 调用的前置条件。listen fd 由 video/audio
// 两个流服务懒创建、可能并发，用 InitOnce 保证只初始化一次且并发方等到结果；
// 进程生命周期内不配对 WSACleanup（与 mpv/libupnp 等其他 winsock 用户惯例一致）。
static BOOL CALLBACK wsa_init_once(PINIT_ONCE once, PVOID param, PVOID* ctx) {
  (void)once;
  (void)param;
  (void)ctx;
  WSADATA d;
  return WSAStartup(MAKEWORD(2, 2), &d) == 0 ? TRUE : FALSE;
}

static int wsa_ensure(void) {
  static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
  if (!InitOnceExecuteOnce(&once, wsa_init_once, NULL, NULL)) return -1;
  return 0;
}

static void set_nonblocking(SOCKET s, int on) {
  u_long nb = on ? 1u : 0u;
  ioctlsocket(s, FIONBIO, &nb);
}

#else

// 单调毫秒时钟：accept 超时只在这个窗口内有意义，取 CLOCK_MONOTONIC
// 防系统时间被 NTP 拨动时误判截止。
static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void set_nonblocking(int s, int on) {
  const int fl = fcntl(s, F_GETFL, 0);
  if (fl < 0) return;
  fcntl(s, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}

#endif

// conn fd 的硬性契约（见 stream_local.h）：阻塞模式 + 写超时 + TCP_NODELAY。
// 任一失败都按整体失败处理（契约宁可显式失败，不留静默降级）。
// 失败时不在这里 close，由调用方统一回收。
//
// ★d282：**写超时恒有界**，与 accept 等待解耦。旧实现把两者绑在同一个
//   timeout_ms 上，而 accept 传的是 0（"立返回，不为等连接阻塞数据线程"）⇒
//   SO_SNDTIMEO 也被设成 0 = 无限阻塞。后果是真机两起 AppHang：mpv 暂停后
//   不再读回环流 → 发送缓冲 ~85ms 填满（750KB/s）→ mux 线程永久阻塞在 send()
//   且持着 Channel::mu_ → 主线程 5s 汇总取同一把锁 → 消息泵停摆。
//   读端本模块从不使用，故 RCVTIMEO 保持原语义（0 = 不设）。
static int conn_setup(int fd, int timeout_ms) {
  const int tmo = (timeout_ms > 0) ? timeout_ms : 0;  // accept 等待：0 = 不等待
  // ★写超时是**常量**，与 accept 等待彻底无关。二者语义本就正交（"等 mpv 连上多久"
  //   vs "单次 send 最多阻塞多久"），派生只会让"想多等一会儿连接"顺带把写也放宽 ——
  //   而写越宽，mux 线程攥着 Channel::mu_ 的时间越长，正是 AppHang 的致病量。
  const int wtmo = FR_STREAM_WRITE_TIMEOUT_MS;
#ifdef _WIN32
  const SOCKET s = fd_to_sock(fd);
  set_nonblocking(s, 0); // 阻塞模式（Windows 下 accept 不保证继承 listener 模式）
  {
    const DWORD rms = (DWORD)tmo;
    if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rms, sizeof(rms)) != 0) return -1;
    const DWORD wms = (DWORD)wtmo;
    if (setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&wms, sizeof(wms)) != 0) return -1;
  }
  {
    const BOOL nd = TRUE;
    if (setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nd, sizeof(nd)) != 0) return -1;
  }
  // ★d313：发送缓冲加大到 1MiB —— 吸收 iPhone ~60s GOP 的 IDR 码率浪（真机实测
  //   单 5s 窗口峰值 16.8MB ≈ 26.8Mbps，而 mpv 读取窗口只有 readahead=0.1s 的
  //   d279 压延迟调优 ⇒ 浪头直接打满缓冲 → 250ms 写超时 → 丢片 → mpv 播到内容
  //   空洞处冻结 2-3s（周期性延迟尖峰的来源）。★代价口径：实时流的缓冲水位
  //   不随浪过而回落 ⇒ 稳态延迟最多 +~1s（1MiB / ~1MB/s 均值码率），换冻结
  //   消失/减短；「零缓冲低延迟」与「无冻结」物理上不可兼得，取舍已定。
  {
    const int sndbuf = 1024 * 1024;
    if (setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&sndbuf, sizeof(sndbuf)) != 0)
      return -1;
  }
#else
  set_nonblocking(fd, 0); // Linux/BSD 的 conn 会继承 listener 的 O_NONBLOCK，显式改回
  {
    struct timeval rtv;
    memset(&rtv, 0, sizeof(rtv));
    if (tmo > 0) {
      rtv.tv_sec = tmo / 1000;
      rtv.tv_usec = (suseconds_t)(tmo % 1000) * 1000;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv)) != 0) return -1;
    struct timeval wtv;
    wtv.tv_sec = wtmo / 1000;
    wtv.tv_usec = (suseconds_t)(wtmo % 1000) * 1000;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &wtv, sizeof(wtv)) != 0) return -1;
  }
  {
    const int nd = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd)) != 0) return -1;
  }
  // ★d313：同 Win32 分支 —— 吸收 IDR 码率浪，代价口径见上。
  {
    const int sndbuf = 1024 * 1024;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) != 0) return -1;
  }
#endif
  return 0;
}

// ---------- 对外接口 ----------

int fr_stream_listen(unsigned short* port) {
  if (!port) return -1;
#ifdef _WIN32
  if (wsa_ensure() != 0) return -1;
#endif

#ifdef _WIN32
  const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return -1;
#else
  const int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s < 0) return -1;
#endif

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  // 安全边界（方案 §11.4）：只绑回环地址，回环流绝不暴露到外网接口。
  // 走 inet_pton 而非 INADDR_LOOPBACK 宏，免掉 Win32 头文件里的定义差异。
  if (inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr) != 1) goto fail;
  sa.sin_port = htons(*port);

  // 地址复用策略分平台（后果不对称，不能共用一个开关）：
  //   · Win32：不设 SO_REUSEADDR —— 它允许其他进程抢绑同端口，改设
  //     SO_EXCLUSIVEADDRUSE 独占；端口被占/TIME_WAIT 时 bind 立即失败，
  //     默认 *port=0 由系统分配新端口，不受影响。
  //   · POSIX：SO_REUSEADDR 惯例开启 —— 允许 TIME_WAIT 期重绑，且不存在
  //     Win32 那种被第三方抢绑同端口的安全问题。
#ifdef _WIN32
  {
    const BOOL excl = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&excl, sizeof(excl));
  }
#else
  {
    const int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  }
#endif

  if (bind(s, (struct sockaddr*)&sa, sizeof(sa)) != 0) goto fail;
  if (listen(s, 1) != 0) goto fail; // backlog=1：预期只有 mpv 一个客户端

  {
#ifdef _WIN32
    int glen = (int)sizeof(struct sockaddr_in);
#else
    socklen_t glen = (socklen_t)sizeof(struct sockaddr_in);
#endif
    struct sockaddr_in got;
    memset(&got, 0, sizeof(got));
    if (getsockname(s, (struct sockaddr*)&got, &glen) != 0) goto fail;
    *port = ntohs(got.sin_port);
  }

  // listen fd 置非阻塞：fr_stream_accept 的超时等待依赖它（poll/select +
  // 非阻塞 accept 是防「poll 返回后对端恰好断开导致 accept 永久阻塞」的
  // 经典组合）。conn 侧的阻塞模式在 conn_setup 里显式恢复。
  set_nonblocking(s, 1);

#ifdef _WIN32
  return sock_to_fd(s);
fail:
  closesocket(s);
#else
  return s;
fail:
  close(s);
#endif
  return -1;
}

int fr_stream_accept(int lfd, int timeout_ms) {
  if (lfd < 0) return -1;
#ifdef _WIN32
  const SOCKET ls = fd_to_sock(lfd);
#else
  const int ls = lfd;
#endif

  // 截止时刻：poll/select 可能被信号等提前唤醒，或 accept 撞上竞态
  // （对端在完成三次握手前断开），统一循环等到截止时间。
#ifdef _WIN32
  const ULONGLONG deadline = GetTickCount64() + (ULONGLONG)(timeout_ms > 0 ? timeout_ms : 0);
#else
  const long long deadline = now_ms() + (timeout_ms > 0 ? timeout_ms : 0);
#endif

  for (;;) {
    if (timeout_ms > 0) {
      // 等待 listen fd 可读（有已完成连接）
#ifdef _WIN32
      const ULONGLONG now = GetTickCount64();
      if (now >= deadline) return -1;
      const DWORD remain = (DWORD)(deadline - now);
      fd_set rset;
      FD_ZERO(&rset);
      FD_SET(ls, &rset);
      struct timeval tv;
      tv.tv_sec = (long)(remain / 1000);
      tv.tv_usec = (long)(remain % 1000) * 1000;
      const int rv = select(0, &rset, NULL, NULL, &tv); // Win32 首参被忽略
      if (rv == 0) return -1; // 超时：lfd 仍有效
      if (rv < 0) {
        if (WSAGetLastError() == WSAEINTR) continue;
        return -1;
      }
#else
      const long long now = now_ms();
      if (now >= deadline) return -1;
      struct pollfd pfd;
      pfd.fd = ls;
      pfd.events = POLLIN;
      pfd.revents = 0;
      const int rv = poll(&pfd, 1, (int)(deadline - now));
      if (rv == 0) return -1; // 超时：lfd 仍有效
      if (rv < 0) {
        if (errno == EINTR) continue;
        return -1;
      }
#endif
    }

#ifdef _WIN32
    const SOCKET cs = accept(ls, NULL, NULL);
    if (cs != INVALID_SOCKET) {
      if (conn_setup(sock_to_fd(cs), timeout_ms) == 0) return sock_to_fd(cs);
      closesocket(cs);
      return -1;
    }
    const int err = (int)WSAGetLastError();
    if (err == WSAEWOULDBLOCK || err == WSAECONNABORTED) {
      // 非阻塞竞态 / 对端握手前断开：还有剩余时间就继续等
      if (timeout_ms <= 0 || GetTickCount64() >= deadline) return -1;
      continue;
    }
    return -1;
#else
    const int cs = accept(ls, NULL, NULL);
    if (cs >= 0) {
      if (conn_setup(cs, timeout_ms) == 0) return cs;
      close(cs);
      return -1;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED ||
        errno == EINTR) {
      // 非阻塞竞态 / 对端握手前断开：还有剩余时间就继续等
      if (timeout_ms <= 0 || now_ms() >= deadline) return -1;
      continue;
    }
    return -1;
#endif
  }
}

// 三态返回（见 stream_local.h）：OK / STALL（超时，对端活着不读）/ ERROR（硬错误）。
// ★d282：以前只有 0/-1 两态，超时与"对端关闭"被迫混为一谈 —— 而两者处置完全相反
//   （超时该丢片续跑，关闭该收尾）。真机 AppHang 复盘后拆开。
int fr_stream_write(int fd, const void* buf, int len) {
  if (fd < 0 || !buf || len < 0) return FR_STREAM_WRITE_ERROR;
  if (len == 0) return FR_STREAM_WRITE_OK;

  const char* p = (const char*)buf;
  int left = len;
#ifdef _WIN32
  const SOCKET s = fd_to_sock(fd);
  while (left > 0) {
    const int n = send(s, p, left, 0);
    if (n == SOCKET_ERROR) {
      const int e = WSAGetLastError();
      if (e == WSAEINTR) continue;
      // 超时（SO_SNDTIMEO 生效）/ 暂不可写 = 对端不读，不是断链
      if (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK) return FR_STREAM_WRITE_STALL;
      // WSAECONNRESET（对端关闭）/ WSAESHUTDOWN（己方 shutdown 唤醒）/ 其他：硬错误
      return FR_STREAM_WRITE_ERROR;
    }
    p += n;
    left -= n;
  }
#else
  while (left > 0) {
    // MSG_NOSIGNAL：对端关闭时 send 返回 EPIPE 而不是让进程吃 SIGPIPE
    const ssize_t n = send(fd, p, (size_t)left, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT)
        return FR_STREAM_WRITE_STALL;  // SO_SNDTIMEO 到期
      return FR_STREAM_WRITE_ERROR;    // EPIPE（对端关闭）/ 其他
    }
    if (n == 0) return FR_STREAM_WRITE_ERROR;  // 阻塞 TCP send 不会出现，防御兜底
    p += n;
    left -= (int)n;
  }
#endif
  return FR_STREAM_WRITE_OK;
}

int fr_stream_shutdown(int fd) {
  if (fd < 0) return -1;
#ifdef _WIN32
  if (shutdown(fd_to_sock(fd), SD_BOTH) != 0) return -1;
#else
  if (shutdown(fd, SHUT_RDWR) != 0) return -1;
#endif
  return 0;
}

void fr_stream_close(int fd) {
  if (fd < 0) return;
#ifdef _WIN32
  closesocket(fd_to_sock(fd));
#else
  close(fd);
#endif
}
