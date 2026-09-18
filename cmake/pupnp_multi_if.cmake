# pupnp 多网卡支持 · 构建期锚点补丁（v0.5.0，方案见根目录 MULTI_NIC_PLAN.md）
#
# 为什么这样做：
#   libupnp 1.14.31 是**单接口架构**（全局只有 gIF_IPV4 一个地址），HTTP 服务绑它、
#   SSDP 只加入它的组播组、M-SEARCH 响应的 LOCATION 与源地址都用它 ⇒ 机器上有
#   Hyper-V / VMware 虚拟网卡时会被选错，物理网段的控制点发现不了设备。
#   要做到「每个网段都能发现、且响应里给的是该网段的本机 IP」，只能改它的网络层。
#
# 遵守 AGENTS.md 规则 8（third_party/ 只读）：
#   不在源码树里提交改动，改为构建期做**字符串级锚点替换**。
#   · 幂等：已打过（检测到替换后特征）即跳过，重复 configure 安全
#   · 唯一性：锚点在文件里必须**恰好出现 1 次**，否则 FATAL_ERROR（拒绝误伤）
#   · 可逆：重新 tools/fetch_deps.sh 拉干净源码即还原
#   · 安全：锚点未命中直接 FATAL_ERROR —— libupnp 升级导致结构变化时**构建立刻失败**，
#           绝不静默漏补；错误信息里带文件、锚点与文档指针
#   · 逻辑主体在我方 src/platform/net_if.c，第三方源码里只留「调用我方函数」的钩子
#
# 锚点（13 处，FR_MULTI_IF=ON 时全部应用）：
#   A0  upnp/src/inc/upnpapi.h                    引入 platform/net_if.h（一个头覆盖全部锚点文件）
#   A1  upnp/src/genlib/miniserver/miniserver.c   HTTP 服务 bind 通配地址
#   A2  upnp/src/api/upnpapi.c                    接口选择完成后扫描全部候选接口
#   S1  upnp/src/ssdp/ssdp_server.c               逐候选接口加入 SSDP 组播组
#   S2  upnp/src/ssdp/ssdp_device.c               回复源地址按对端网段选
#   S3  upnp/src/ssdp/ssdp_device.c               LOCATION 主机段按对端网段重写（副本）
#   S4  upnp/src/ssdp/ssdp_device.c               主动广播（ALIVE/BYEBYE）逐张网卡各发一份
#   G1  upnp/src/gena/gena_device.c               订阅校验：命中任一网段即通过
#   V0  upnp/src/urlconfig/urlconfig.c            单独引我方头
#   V1  upnp/src/urlconfig/urlconfig.c            <URLBase> 主机段兜底（无 Host 时）
#   V2  upnp/src/urlconfig/urlconfig.c            同上（第二处站点）
#   W1  upnp/src/genlib/net/http/webserver.c      描述文档 URLBase 按本请求 Host 头生成
#   W2  upnp/src/genlib/net/http/webserver.c      发送替换后的文档（长度已同步 FileLength）
#
# 一句话规则：**谁问就按谁回** —— 请求响应按来源网段、主动广播逐张网卡各发一份、
# 描述文档按请求的 Host 头。不存在"挑一张网卡"的功能决策（V1/V2 只作无 Host 兜底）。
#
# 开关（双向）：
#   -DFR_MULTI_IF=OFF  →  把已打的补丁**还原**回原始源码，行为回到 v0.4.0（回归对照用）
#   再 -DFR_MULTI_IF=ON →  重新应用。两个方向都幂等，可反复切换，不需要重拉源码。

option(FR_MULTI_IF "多网卡支持：为 pupnp 应用构建期锚点补丁" ON)

# 本模块只负责「哪个文件、哪段代码、换成什么」；双向/幂等/唯一性判定统一由
# cmake/pupnp_patch_util.cmake 提供，这里把开关固定为 FR_MULTI_IF。
include(${CMAKE_CURRENT_LIST_DIR}/pupnp_patch_util.cmake)

# fr_pupnp_toggle(<相对 third_party/pupnp 的路径> <锚点原文> <替换后文本>)
#   依据 FR_MULTI_IF：ON → 把「锚点」换成「替换后文本」；OFF → 换回去
#   （是真的还原源码树，不是不应用，否则会因残留 fr_net_if_* 调用而链接失败）。
#   两个方向都幂等：目标状态已达成即跳过；锚点与替换都不在 → FATAL_ERROR。
function(fr_pupnp_toggle rel anchor replacement)
  fr_pupnp_patch_toggle(FR_MULTI_IF "${rel}" "${anchor}" "${replacement}")
endfunction()

function(fr_apply_pupnp_multi_if)
  if (NOT EXISTS "${FR_ROOT}/third_party/pupnp/CMakeLists.txt")
    message(WARNING
      "[FlashRec] pupnp 多网卡补丁：找不到本地 third_party/pupnp —— "
      "多网卡支持**不会生效**（行为回到 v0.4.0）。请先跑 tools/fetch_deps.sh。")
    return()
  endif()
  if (FR_MULTI_IF)
    message(STATUS "[FlashRec] pupnp 多网卡补丁：FR_MULTI_IF=ON —— 应用补丁")
  else()
    message(STATUS "[FlashRec] pupnp 多网卡补丁：FR_MULTI_IF=OFF —— 还原源码树（回到 v0.4.0 行为）")
  endif()

  # A0 · 一处 include 覆盖全部锚点文件
  #      upnpapi.c / ssdp_server.c / ssdp_device.c / gena_device.c / miniserver.c
  #      都 include 了 upnpapi.h，所以在它里面声明即可（避免逐个 .c 加 include）
  fr_pupnp_toggle("upnp/src/inc/upnpapi.h"
    [[#include "service_table.h"]]
    [[#include "service_table.h"
/* FlashRec 多网卡支持：接口枚举 / 按对端选本机 IP（我方模块，构建期注入，见 MULTI_NIC_PLAN.md） */
#include "platform/net_if.h"]])

  # A1 · HTTP 服务：由「只绑被选中的那块网卡」改为「绑通配地址」⇒ 所有网卡可达
  #      （三处 init_socket_stuff 里只动 IPv4 那处；IPv6 保持原样）
  fr_pupnp_toggle("upnp/src/genlib/miniserver/miniserver.c"
    "\terr_init_4 = init_socket_stuff(&ss4, gIF_IPV4, 4);"
    "\terr_init_4 = init_socket_stuff(&ss4, fr_net_if_http_bind(), 4);")

  # A2 · 接口选择完成后扫描全部候选接口
  #      （供 S1 逐接口加入组播、S2/S3 按来源网段生成 LOCATION，均由后续补丁接入）
  fr_pupnp_toggle("upnp/src/api/upnpapi.c"
    "\tretVal = UpnpGetIfInfo(IfName);"
    "\tretVal = UpnpGetIfInfo(IfName);\n\tfr_net_if_scan();")

  # S1 · SSDP 组播：主接口按原逻辑加入（失败仍报错退出），其余候选接口补加，
  #      失败只记日志 ⇒ 每个网段都能收到 M-SEARCH / ALIVE。
  fr_pupnp_toggle("upnp/src/ssdp/ssdp_server.c"
    "\t/* Set multicast interface. */"
    [[	/* FlashRec 多网卡支持：主接口 join 成功后，把其余候选接口也加入同一组播组，
	 * 让每个网段都能收到 M-SEARCH。主接口失败已在上面按原逻辑报错退出；
	 * 这里某个接口失败只记日志，不影响其余接口。见 MULTI_NIC_PLAN.md §3 S1。 */
	{
		int fr_i;
		int fr_n = fr_net_if_count();
		for (fr_i = 0; fr_i < fr_n; fr_i++) {
			const char *fr_ip = fr_net_if_ip(fr_i);
			int fr_rc;
			if (fr_ip == NULL || strcmp(fr_ip, gIF_IPV4) == 0) {
				continue; /* 主接口已在上面加入 */
			}
			memset((void *)&ssdpMcastAddr, 0, sizeof ssdpMcastAddr);
			inet_pton(AF_INET, fr_ip, &ssdpMcastAddr.imr_interface);
			inet_pton(AF_INET, SSDP_IP, &ssdpMcastAddr.imr_multiaddr);
			fr_rc = setsockopt(*ssdpSock,
				IPPROTO_IP,
				IP_ADD_MEMBERSHIP,
				(OPTION_VALUE_CAST)&ssdpMcastAddr,
				sizeof(struct ip_mreq));
			UpnpPrintf(UPNP_INFO,
				SSDP,
				__FILE__,
				__LINE__,
				"FlashRec: SSDP multicast join %s -> %s\n",
				fr_ip,
				(fr_rc == -1) ? "FAILED (ignored)" : "ok");
		}
	}
	/* Set multicast interface. */]])

  # S2 · 回复源地址（IP_MULTICAST_IF / 单播源）按「对端所在网段」选本机网卡 IP。
  #      单播对端 = M-SEARCH 回复 → 命中同网段的那块网卡；
  #      组播目的地（ssdp:alive / byebye）→ fr_net_if_local_for() 回退主接口（物理网卡优先）；
  #      接口枚举失败 → 原 gIF_IPV4 行为。
  fr_pupnp_toggle("upnp/src/ssdp/ssdp_device.c"
    "\tif (strlen(gIF_IPV4) > (size_t)0 &&\n\t\t!inet_pton(AF_INET, gIF_IPV4, &replyAddr)) {\n\t\treturn UPNP_E_INVALID_PARAM;\n\t}"
    [[	{
		/* FlashRec 多网卡支持：SSDP 回复源地址按「对端所在网段」的本机网卡选。
		 * 见 MULTI_NIC_PLAN.md §3 S2。 */
		int fr_done = 0;
		if (DestAddr != NULL && DestAddr->sa_family == AF_INET) {
			char fr_peer[INET_ADDRSTRLEN];
			if (inet_ntop(AF_INET,
				    &((struct sockaddr_in *)DestAddr)->sin_addr,
				    fr_peer,
				    sizeof(fr_peer)) != NULL) {
				const char *fr_lip =
					fr_net_if_local_for(fr_peer);
				if (fr_lip != NULL &&
				    inet_pton(AF_INET,
					    fr_lip,
					    &replyAddr) == 1) {
					fr_done = 1;
					UpnpPrintf(UPNP_INFO,
						SSDP,
						__FILE__,
						__LINE__,
						"FlashRec: SSDP peer %s -> "
						"reply source %s\n",
						fr_peer,
						fr_lip);
				}
			}
		}
		if (!fr_done && strlen(gIF_IPV4) > (size_t)0 &&
		    !inet_pton(AF_INET, gIF_IPV4, &replyAddr)) {
			return UPNP_E_INVALID_PARAM;
		}
	}]])

  # S3 · LOCATION 主机段按「对端所在网段」重写为对应本机 IP。
  #      只在**新分配的副本**上改：原包指针与内容不动（所有权在调用方 AdvertiseAndReply），
  #      副本发完即 free；分配失败 → 原样发送（退化为补丁前行为，不崩）。
  fr_pupnp_toggle("upnp/src/ssdp/ssdp_device.c"
    "\tfor (res = result; res != NULL; res = res->ai_next) {\n\t\tif (SendToCaller(\n\t\t\t    res, DestAddr, NumPacket, RqPacket, &replyAddr) ==\n\t\t\tUPNP_E_SUCCESS)\n\t\t\tret = UPNP_E_SUCCESS; // one successful send makes\n\t\t\t\t\t      // response successful\n\t}\n\tfreeaddrinfo(result);"
    [[	{
		/* FlashRec 多网卡支持：LOCATION 的主机段按「对端所在网段」重写为该网段
		 * 的本机 IP（HTTP 已绑 0.0.0.0，因此每个本机 IP 都真的连得上）。
		 * 见 MULTI_NIC_PLAN.md §3 S3。 */
		char fr_peer[INET_ADDRSTRLEN] = {0};
		const char *fr_lip = NULL;
		char **fr_pkts = RqPacket;
		char *fr_copy[8] = {0};
		int fr_made = 0;
		int fr_i;

		if (DestAddr != NULL && DestAddr->sa_family == AF_INET &&
		    inet_ntop(AF_INET,
			    &((struct sockaddr_in *)DestAddr)->sin_addr,
			    fr_peer,
			    sizeof(fr_peer)) != NULL) {
			fr_lip = fr_net_if_local_for(fr_peer);
		}

		if (fr_lip != NULL && NumPacket > 0 && NumPacket <= 8) {
			for (fr_i = 0; fr_i < NumPacket; fr_i++) {
				fr_copy[fr_i] = fr_net_if_rewrite_location(
					RqPacket[fr_i], fr_lip);
				if (fr_copy[fr_i] == NULL) {
					break; /* 分配失败：整批回退发原包 */
				}
				fr_made++;
			}
			if (fr_made == NumPacket) {
				fr_pkts = fr_copy; /* 全部副本就绪才启用 */
			}
		}

		for (res = result; res != NULL; res = res->ai_next) {
			if (SendToCaller(res,
				    DestAddr,
				    NumPacket,
				    fr_pkts,
				    &replyAddr) == UPNP_E_SUCCESS)
				ret = UPNP_E_SUCCESS; // one successful send makes
						      // response successful
		}

		for (fr_i = 0; fr_i < fr_made; fr_i++) {
			free(fr_copy[fr_i]);
		}
	}
	freeaddrinfo(result);]])

  # S4 · 主动广播（ALIVE / BYEBYE）**逐张网卡各发一份**。
  #      组播广播没有对端可匹配，所以不能"挑一张网卡"：不在某个接口上发，那一段上的
  #      控制点就**永远收不到**（不是晚点收到）。判据是"目的地 = SSDP 组播地址" ——
  #      M-SEARCH 的回复走单播对端地址，不会进这个分支（那条路由 S2/S3 按对端处理）。
  #      每张网卡：LOCATION 主机段换成该网卡 IP，组播出口也换成它（S2 读 fr_net_if_local_for
  #      时会先看到 fr_net_if_force_begin 设定的强制地址）。
  fr_pupnp_toggle("upnp/src/ssdp/ssdp_device.c"
    "\tint ret = UPNP_E_SOCKET_ERROR;"
    [[	int ret = UPNP_E_SOCKET_ERROR;

	{
		/* FlashRec 多网卡支持：主动广播（ALIVE / BYEBYE）逐张网卡各发一份。
		 * 每一份的 LOCATION 主机段与组播出口都换成该网卡自己的本机地址
		 * （HTTP 已绑 0.0.0.0，所以每个地址都真的连得上）。见 §3 S4。 */
		const char *fr_forced = fr_net_if_forced();
		if (fr_forced == NULL && DestAddr != NULL &&
		    DestAddr->sa_family == AF_INET && NumPacket > 0 &&
		    NumPacket <= 8 && fr_net_if_count() > 0 &&
		    fr_net_if_addr_is_mcast_nbo(
			    ((struct sockaddr_in *)DestAddr)->sin_addr.s_addr)) {
			int fr_n = fr_net_if_count();
			int fr_any = 0;
			int fr_i;

			for (fr_i = 0; fr_i < fr_n; fr_i++) {
				const char *fr_ip = fr_net_if_ip(fr_i);
				char *fr_copy[8] = {0};
				int fr_made = 0;
				int fr_k;

				if (fr_ip == NULL) {
					continue;
				}
				for (fr_k = 0; fr_k < NumPacket; fr_k++) {
					fr_copy[fr_k] = fr_net_if_rewrite_location(
						RqPacket[fr_k], fr_ip);
					if (fr_copy[fr_k] == NULL) {
						break;
					}
					fr_made++;
				}
				if (fr_made != NumPacket) {
					/* 副本不齐 → 这张网卡跳过：宁可少发一份，
					 * 也不发一份 LOCATION 指向别处的公告。 */
					for (fr_k = 0; fr_k < fr_made; fr_k++) {
						free(fr_copy[fr_k]);
					}
					UpnpPrintf(UPNP_INFO,
						SSDP,
						__FILE__,
						__LINE__,
						"FlashRec: announce skip %s (alloc)\n",
						fr_ip);
					continue;
				}
				/* 强制本机地址：S2 取它做组播出口、S3 取它改写
				 * LOCATION。递归回本函数时 fr_forced 非 NULL，
				 * 因此不会再次扇出。 */
				fr_net_if_force_begin(fr_ip);
				rc = NewRequestHandler(
					DestAddr, NumPacket, fr_copy);
				fr_net_if_force_end();
				for (fr_k = 0; fr_k < fr_made; fr_k++) {
					free(fr_copy[fr_k]);
				}
				if (rc == UPNP_E_SUCCESS) {
					fr_any = 1;
				}
				UpnpPrintf(UPNP_INFO,
					SSDP,
					__FILE__,
					__LINE__,
					"FlashRec: announce via %s -> %s\n",
					fr_ip,
					(rc == UPNP_E_SUCCESS) ? "ok"
							       : "FAILED (ignored)");
			}
			return fr_any ? UPNP_E_SUCCESS
				      : UPNP_E_SOCKET_ERROR;
		}
	}]])

  # G1 · GENA 订阅校验：原来「不在 gIF_IPV4 那一个网段就拒绝」，多网卡下会把
  #      同网段的其余网卡客户端误拒 ⇒ 改为命中本机任一接口网段即通过。
  #      第二形参是「接口枚举失败时的回退判定」= 补丁前的原判定式，保证零行为差。
  fr_pupnp_toggle("upnp/src/gena/gena_device.c"
    "\t\t\tif ((deliveryAddr4->sin_addr.s_addr &\n\t\t\t\t    genaNetmask.s_addr) !=\n\t\t\t\t(genaAddr4.s_addr & genaNetmask.s_addr)) {"
    [[			/* FlashRec 多网卡支持：命中本机任一接口网段即通过；接口枚举失败
			 * 时回退为补丁前的原判定（见第二形参）。见 MULTI_NIC_PLAN.md §3 G1。 */
			if (!fr_net_if_subnet_match_nbo(
				    deliveryAddr4->sin_addr.s_addr,
				    (int)((deliveryAddr4->sin_addr.s_addr &
					   genaNetmask.s_addr) ==
					  (genaAddr4.s_addr &
					   genaNetmask.s_addr)))) {]])

# V0 · urlconfig.c 不走 upnpapi.h，单独引我方头
fr_pupnp_toggle("upnp/src/urlconfig/urlconfig.c"
    [[#include "webserver.h"]]
    [[#include "webserver.h"
/* FlashRec 多网卡支持：URLBase 优选主机名（我方模块，构建期注入） */
#include "platform/net_if.h"]])

# V1/V2 · 设备描述文档 <URLBase> 的主机段
#   libupnp 在 config_description_doc() 里**总是**用 gIF_IPV4 重写 URLBase
#   （缺失就新建、存在就只换主机段），所以从我们自己的 XML 里删掉 URLBase 没用。
#   而 URLBase 是文档**体内**的绝对地址，发出去就固定了，没法像 SSDP 响应那样按
#   对端逐次生成 ⇒ 只能选「各网段最可能都可达」的那块网卡（物理网卡优先）。
#   不修它的症状：控制点拿到正确 LOCATION 取到描述后，按 URLBase 解析相对的
#   controlURL/eventSubURL ⇒ SOAP 全发到虚拟网卡 IP，局域网客户端够不到（投屏失败）。
fr_pupnp_toggle("upnp/src/urlconfig/urlconfig.c"
    [[		if (membuffer_append_str(&url_str, "http://") != 0 ||
			membuffer_append_str(&url_str, ip_str) != 0 ||]]
    [[		if (membuffer_append_str(&url_str, "http://") != 0 ||
			membuffer_append_str(&url_str,
				fr_net_if_preferred_host(ip_str)) != 0 ||]])

fr_pupnp_toggle("upnp/src/urlconfig/urlconfig.c"
    [[			membuffer_append_str(&url_str, "://") != 0 ||
			membuffer_append_str(&url_str, ip_str) != 0) {]]
    [[			membuffer_append_str(&url_str, "://") != 0 ||
			membuffer_append_str(&url_str,
				fr_net_if_preferred_host(ip_str)) != 0) {]])

  # W1 · 描述文档 <URLBase> 的主机段按**本请求的 Host 头**生成。
  #      URLBase 在文档体内、只能生成一次，但客户端是**带着一条 HTTP 连接**来拉它的 ——
  #      请求里的 Host 就是它用来找到我们的地址，所以"按 Host"等价于"按对端"。
  #      这里为每个请求各替换一份，并把新长度写回 FileLength：下游就是用它算
  #      Content-Length 的，所以长度必须同步（各网卡 IP 字符串长度并不相同）。
  #      没有 Host 的老客户端（HTTP/1.0）落回 V1/V2 的兜底路径。
  fr_pupnp_toggle("upnp/src/genlib/net/http/webserver.c"
    [[			using_alias = get_alias(request_doc, alias, finfo);
			if (using_alias == 1) {]]
    [[			using_alias = get_alias(request_doc, alias, finfo);
			if (using_alias == 1) {
				/* FlashRec 多网卡支持：URLBase 主机段按本请求的 Host 头
				 * 生成 —— 客户端拉文档时带着的 Host 就是它用来找到我们的
				 * 地址，等价于「有对端就按对端选」。长度同步给 FileLength
				 * （下游据此写 Content-Length）。见 §5 P4b。 */
				{
					memptr fr_host;
					size_t fr_newlen;

					fr_host.buf = NULL;
					fr_host.length = 0;
					if (httpmsg_find_hdr(
						    req, HDR_HOST, &fr_host) != NULL &&
					    fr_host.buf != NULL &&
					    fr_host.length > 0) {
						fr_newlen = fr_net_if_doc_localize(
							alias->doc.buf,
							alias->doc.length,
							fr_host.buf,
							fr_host.length);
						if (fr_newlen > 0) {
							UpnpFileInfo_set_FileLength(
								finfo,
								(off_t)fr_newlen);
							UpnpPrintf(UPNP_INFO,
								HTTP,
								__FILE__,
								__LINE__,
								"FlashRec: URLBase -> Host %.*s\n",
								(int)fr_host.length,
								fr_host.buf);
						}
					}
				}]])
  # 状态判定：W1 的锚点是其替换文本的前缀，两个方向都靠"替换后文本"判定（同 A0/A2/A1/S1/S4）。

  # W2 · 发送替换后的文档（长度来自 fr_net_if_doc_take）。
  #      取不到替换版（无 Host / 与现值相同 / 分配失败）就发原文档，行为与补丁前一致。
  fr_pupnp_toggle("upnp/src/genlib/net/http/webserver.c"
    [[		case RESP_XMLDOC:
			http_SendMessage(info,
				&timeout,
				"Ibb",
				&RespInstr,
				headers.buf,
				headers.length,
				xmldoc.doc.buf,
				xmldoc.doc.length);
			alias_release(&xmldoc);
			break;]]
    [[		case RESP_XMLDOC: {
			/* FlashRec 多网卡支持：本请求若命中按 Host 头替换的文档，
			 * 就发替换版 —— 长度已同步给 FileLength，故 Content-Length
			 * 已经是对的。见 §5 P4b。 */
			size_t fr_len = 0;
			char *fr_doc = fr_net_if_doc_take(&fr_len);

			http_SendMessage(info,
				&timeout,
				"Ibb",
				&RespInstr,
				headers.buf,
				headers.length,
				(fr_doc != NULL) ? fr_doc : xmldoc.doc.buf,
				(fr_doc != NULL) ? fr_len : xmldoc.doc.length);
			free(fr_doc);
			alias_release(&xmldoc);
			break;
		}]])

  if (FR_MULTI_IF)
    message(STATUS "[FlashRec] pupnp 多网卡补丁：应用完成（FR_MULTI_IF=ON）")
  else()
    message(STATUS "[FlashRec] pupnp 多网卡补丁：还原完成（FR_MULTI_IF=OFF，等同 v0.4.0）")
  endif()
endfunction()
