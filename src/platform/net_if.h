// 网络接口枚举与「按对端选本机 IP」—— 多网卡支持（v0.5.0 / d109）
//
// 背景：libupnp 1.14.31 是单接口架构（全局只有 gIF_IPV4 一个地址），
// HTTP 服务 bind 该地址、SSDP 只加入该接口的组播组、LOCATION 在注册时
// 用该地址拼死 ⇒ 有 Hyper-V / VMware 虚拟网卡时会被选错，物理网段的
// 控制点收不到发现包、拿到的地址也连不上。
//
// 本模块提供五件事（供 pupnp 网络层在构建期补丁的锚点处调用）：
//   1. 枚举本机全部可用于 SSDP 的 IPv4 接口（IP / 掩码 / 名字 / 系统索引）
//   2. HTTP 服务的绑定地址（固定 0.0.0.0，让所有网卡可达）
//   3. 按「对端地址」匹配本机同网段 IP，供 SSDP 响应生成 LOCATION 与
//      选择回复源地址（不依赖 IP_PKTINFO：M-SEARCH 的客户端地址 + 本机
//      接口掩码即可判定它在哪个网段）
//   4. 主动广播（ALIVE/BYEBYE）没有对端可匹配 —— 由发送循环**逐张网卡各发一份**，
//      每份的本机地址在发包前临时指定（fr_net_if_force_* 上下文）
//   5. 描述文档内 URLBase 的主机段按**本请求的 Host 头**逐次生成（d117）：
//      客户端拉文档时带着的 Host 就是它用来找到我们的地址，等价于「按对端」
//
// 一句话规则：**谁问就按谁回**；不存在「挑一张网卡」的功能决策
// （fr_net_if_preferred_host 只作无 Host 时的兜底）。
//
// 归属 src/platform/ 是因为实现要平台宏（GetAdaptersAddresses / getifaddrs），
// 见 AGENTS.md 规则 5；编译上被追加进 pupnp 的 upnp_static 目标（规则 8：
// third_party 只读，改动一律走构建期步骤）。

#ifndef FR_PLATFORM_NET_IF_H
#define FR_PLATFORM_NET_IF_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 参与 SSDP 的接口上限（超出丢弃，仅计日志）
#define FR_NET_IF_MAX 32

// 枚举本机可用的 IPv4 接口。幂等：重复调用会重新扫描并覆盖上次结果。
// 返回收录的接口数量（0 表示一个都没有，调用方应回退到原有单接口行为）。
// 排序：物理网卡优先于虚拟网卡（见 net_if.c 的 is_virtual_name）。
int fr_net_if_scan(void);

// 上次扫描收录的接口数量。
int fr_net_if_count(void);

// 以下取值函数：idx 越界返回 NULL / 0。
const char* fr_net_if_ip(int idx);     // 点分十进制，如 "192.0.2.1"
const char* fr_net_if_mask(int idx);   // 子网掩码，如 "255.255.255.0"
const char* fr_net_if_name(int idx);   // 系统接口名（Win 为 FriendlyName）
unsigned int fr_net_if_index(int idx); // 系统接口索引（Win IfIndex / Linux if_nametoindex）

// HTTP 服务（pupnp miniserver）应绑定的地址：固定通配，让所有网卡可达。
const char* fr_net_if_http_bind(void);

// 按对端地址选本机出口 IP：
//   命中「与 peer 同网段」的接口 → 返回该接口 IP；
//   都不命中（跨网段 / 对端在别的子网） → 返回主接口 IP（列表首项）；
//   无任何接口 → 返回 NULL（调用方保持原行为）。
// 返回值指向模块内部静态存储，调用方不得释放；下次 scan 前有效。
const char* fr_net_if_local_for(const char* peer_ip);

// ---- 以下为 SSDP / GENA 侧调用（构建期补丁的锚点处使用）----

// 判定 ip_nbo（**网络序**的 32 位地址）是否落在「本机任一接口网段」内。
// no_if_fallback：接口列表为空（枚举失败）时的回退判定结果 —— 由调用方把
// 补丁前的原判定式传进来，这样接口枚举失败时行为与补丁前完全一致。
// 返回 1 = 落在某本机网段内；0 = 都不落。
int fr_net_if_subnet_match_nbo(unsigned int ip_nbo, int no_if_fallback);

// 把报文里的 `LOCATION: http://<host>[:port]/…` 的**主机段**换成 local_ip，
// 返回**新分配**的副本（调用方负责 free）。原 packet 指针不动、内容不改 ——
// SSDP 报文的所有权在调用方（AdvertiseAndReply 会释放原包）。
// 报文里没有 LOCATION、或不是 http:// 形式时，返回原样副本。
// malloc 失败返回 NULL（调用方应回退发送原包）。
char* fr_net_if_rewrite_location(const char* packet, const char* local_ip);

// 设备描述文档里 <URLBase> 的优选主机名。
//
// ⚠️ 这是**兜底**路径：正常路径已改为「按本请求的 Host 头逐次生成」（见
// fr_net_if_doc_localize），因为客户端拉描述文档时带着的 Host 头就是它用来找到
// 我们的地址 —— 那等价于"有对端就按对端选"。只有当请求里没有 Host（HTTP/1.0
// 老客户端）时才落回这里。
//
// 入参是 **"ip:端口"**（libupnp 的 addrToString 产物，不是裸 IP）：只换主机段，
// 端口原样保留 —— 早先整串换成裸 IP 会丢掉端口，客户端按 80 端口发 SOAP 直接失败。
// 返回值指向模块内静态存储（单线程注册期调用），调用方不得释放。
const char* fr_net_if_preferred_host(const char* host_port_fallback);

// ---- 以下为「主动广播逐接口各发一份」与「URLBase 按对端」新增（d117）----

// ip_nbo（**网络序**）是否等于 SSDP 组播地址 239.255.255.250。
// 供 pupnp 的发送函数区分「主动广播（组播目的地）」与「搜索响应（单播对端）」。
int fr_net_if_addr_is_mcast_nbo(unsigned int ip_nbo);

// 「本机地址被强制指定」的上下文：ALIVE/BYEBYE 要按每张网卡各发一份，此时没有
// 对端可匹配，由发送循环逐个接口指定本机地址；该地址会覆盖 fr_net_if_local_for()
// 的网段匹配结果（组播目的地址匹配不到任何本机网段，正好由这里接管）。
// 只在**同一线程内**生效（SSDP 发送可并发，故用线程局部存储）。
void fr_net_if_force_begin(const char* local_ip);
void fr_net_if_force_end(void);

// 当前生效的强制本机地址；未设置返回 NULL（发送循环内部据此避免递归扇出）。
const char* fr_net_if_forced(void);

// 把描述文档里 <URLBase> 的 **authority（主机+可选端口）** 换成本请求的 Host 头值。
//   · Host 自带端口 → 原样替换整个 authority；
//   · Host 不带端口 → 保留原 authority 里的端口（丢了它客户端会按 80 发 SOAP）。
// ★ 必须整体替换 authority：只换主机段会把 Host 里的端口留在后面，拼出
//   "ip:端口:端口"（d117 实测踩到，SOAP 一次都发不出去）。
//
//   · 返回替换后的新长度（>0）时，替换结果存在本线程槽位里，需由
//     fr_net_if_doc_take() 取走并 free；
//   · 返回 0 表示无需替换（无 URLBase / 是 IPv6 字面量 / Host 非法 /
//     与现值相同 / 分配失败）—— 调用方照原样发送即可，且槽位已清空。
// host 只接受 主机名|IPv4 与可选端口 的字符集，其余一律拒绝（它会被写进我们
// 自己的 XML，必须防注入）。
size_t fr_net_if_doc_localize(const char* doc,
                              size_t doc_len,
                              const char* host,
                              size_t host_len);

// 取出本线程最近一次 fr_net_if_doc_localize() 的结果并清空槽位；无则返回 NULL
// （此时 *out_len 为 0）。调用方负责 free 返回值。
char* fr_net_if_doc_take(size_t* out_len);

#ifdef __cplusplus
}
#endif

#endif  // FR_PLATFORM_NET_IF_H
