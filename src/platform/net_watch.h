#pragma once
// 网络变化被动监听（★d510）：接口/地址增删时置「有变化」标志，主循环消费。
//
// 背景：协议栈（libupnp UpnpInit2 / AirPlay mDNS）只在启动时碰一次运气 ——
// 启动时无网则永远起不来；运行中接口集变化（网卡上线/下线、IP 变更、
// 某网段后恢复）也不会自动跟随。本模块补上被动感知：
//   · Windows：NotifyIpInterfaceChange（系统回调线程，零自有线程）
//   · Linux：  netlink 路由 socket（LINK + IPv4/IPv6 IFADDR 组，独立线程）
//
// 线程纪律（规则 4）：监听侧只写一个原子标志，不回调业务、不碰任何状态；
// 消费只发生在主循环（net_watch_consume），防抖与协议栈重建决策都在主线程。
// 平台宏只出现在 net_watch.cpp（src/platform/，规则 5）。
#include <string>
#include <utility>
#include <vector>

namespace fr::platform {

// 启动监听。重复调用幂等。失败返回 false（主循环另有 60s 轮询兜底）。
bool net_watch_start();

// 停止并回收线程/句柄（幂等；退出序列调用）。
void net_watch_stop();

// 主循环每帧调：取走并清零「有变化」标志。true = 距上次消费发生过网络变化
//（变化可能是一串事件，消费方自行防抖合并）。
bool net_watch_consume();

// 无副作用接口枚举：返回 (ip, name) 列表。**不写** fr_net_if_scan 的共享全局
// 列表 —— 自愈线在主循环里重扫时，pupnp 的 SSDP/SOAP 线程可能正在读那份
// 列表（撕裂读风险），故本探测独立走系统 API。顺序不稳定，消费方自行排序。
// 仅收录「可用 IPv4 单播」接口（up 且非 loopback），与 net_if 的收录口径一致。
std::vector<std::pair<std::string, std::string>> net_watch_list_interfaces();

}  // namespace fr::platform
