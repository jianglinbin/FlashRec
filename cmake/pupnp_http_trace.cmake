# pupnp HTTP 入口全量日志（原 d22，本版纳入锚点机制 —— 见 MULTI_NIC_PLAN.md §8 / d118）
#
# 背景：`fr_http_trace` 原先是我方**手工直接改 third_party/pupnp** 落地的，仓库里
# 没有任何自动应用步骤 ⇒ tools/fetch_deps.sh 重拉 pupnp 之后该补丁**静默消失**，
# 而 src/app/config.h 的 `log_http_entry` 开关仍在 —— 于是它变成一个**哑开关**：
# 用户打开它什么都不会发生，也不会报错。这属于"依赖被刷新后行为悄悄改变"，必须消除。
#
# 现在它和 c++ 侧的多网卡补丁一样走**构建期锚点替换**：
#   · 重拉依赖后 configure 会**自动重新应用**，不会再静默丢失；
#   · 锚点未命中 → 构建立刻 FATAL_ERROR，绝不静默漏补；
#   · 开关独立（-DFR_HTTP_TRACE=OFF 可整体还原源码树），与 FR_MULTI_IF 解耦。
#
# 行为：miniserver 每条 HTTP 请求（SOAP / GENA / GET，包括 pupnp 平时会静默拒掉的
# 未知方法或路径）都会往 FR_HTTP_TRACE_LOG 指定的文件追加一行
# `[时间] 方法 路径 <- 对端 => 状态码`。环境变量未设/为空时零成本直接返回
# （读一次并缓存，应用侧在 UpnpInit 之前设好，之后不变）。
#
# 两个锚点：
#   H1  upnp/src/genlib/miniserver/miniserver.c   插入 fr_http_trace() 定义
#   H2  upnp/src/genlib/miniserver/miniserver.c   在 ExitFunction 处调用它（覆盖全部出口）

option(FR_HTTP_TRACE "HTTP 入口全量日志：为 pupnp 应用构建期锚点补丁" ON)

include(${CMAKE_CURRENT_LIST_DIR}/pupnp_patch_util.cmake)

function(fr_apply_pupnp_http_trace)
  if (NOT EXISTS "${FR_ROOT}/third_party/pupnp/CMakeLists.txt")
    message(WARNING
      "[FlashRec] pupnp HTTP 日志补丁：找不到本地 third_party/pupnp —— "
      "log_http_entry 将不生效。请先跑 tools/fetch_deps.sh。")
    return()
  endif()
  if (FR_HTTP_TRACE)
    message(STATUS "[FlashRec] pupnp HTTP 日志补丁：FR_HTTP_TRACE=ON —— 应用补丁")
  else()
    message(STATUS "[FlashRec] pupnp HTTP 日志补丁：FR_HTTP_TRACE=OFF —— 还原源码树")
  endif()

  # H1 · 插入 fr_http_trace() 定义（锚点是紧随其后的 dispatch_request 文档注释）
  fr_pupnp_patch_toggle(FR_HTTP_TRACE
    "upnp/src/genlib/miniserver/miniserver.c"
    [[/*!
 * \brief Based on the type pf message, appropriate callback is issued.
 *
 * \return 0 on Success or HTTP_INTERNAL_SERVER_ERROR if Callback is NULL.
 */]]
    [[/*!
 * \brief FlashRec local patch (d22): full HTTP request entry trace.
 *
 * Logs every incoming request (method, path, peer address, dispatch result)
 * to the file named by the FR_HTTP_TRACE_LOG environment variable, which the
 * app sets to its log directory before UpnpInit. Disabled when the variable
 * is unset or empty. Covers SOAP / GENA / HTTP GET and any unknown method or
 * path that pupnp would otherwise reject silently (404/500 with no log).
 */
static void fr_http_trace(
	/*! [in] Socket Information object (peer address filled on incoming). */
	SOCKINFO *info,
	/*! [in] HTTP parser object. */
	http_parser_t *parser,
	/*! [in] Dispatch result code (HTTP status). */
	int rc)
{
	char ts[32];
	char peer[INET6_ADDRSTRLEN] = "-";
	struct tm tmv;
	time_t now;
	FILE *f;
	http_message_t *msg = &parser->msg;
	const char *method = method_to_str(parser->msg.method);
	/* Read the env var once: the app sets it before UpnpInit and never
	 * changes it afterwards. Empty/unset = tracing disabled (zero cost). */
	static const char *cached_path = NULL;

	if (!cached_path) {
		cached_path = getenv("FR_HTTP_TRACE_LOG");
		if (!cached_path) {
			cached_path = "";
		}
	}
	if (!cached_path[0]) {
		return;
	}
	f = fopen(cached_path, "a");
	if (!f) {
		return;
	}
	time(&now);
#ifdef _WIN32
	localtime_s(&tmv, &now);
#else
	localtime_r(&now, &tmv);
#endif
	strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);
	if (info->foreign_sockaddr.ss_family == AF_INET) {
		inet_ntop(AF_INET,
			&((struct sockaddr_in *)&info->foreign_sockaddr)
				 ->sin_addr,
			peer,
			sizeof peer);
	} else if (info->foreign_sockaddr.ss_family == AF_INET6) {
		inet_ntop(AF_INET6,
			&((struct sockaddr_in6 *)&info->foreign_sockaddr)
				 ->sin6_addr,
			peer,
			sizeof peer);
	}
	fprintf(f,
		"[%s] %s %.*s <- %s => %d\n",
		ts,
		method ? method : "?",
		msg->uri.pathquery.buff && msg->uri.pathquery.size
			? (int)msg->uri.pathquery.size
			: 1,
		msg->uri.pathquery.buff ? msg->uri.pathquery.buff : "-",
		peer,
		rc);
	fclose(f);
}

/*!
 * \brief Based on the type pf message, appropriate callback is issued.
 *
 * \return 0 on Success or HTTP_INTERNAL_SERVER_ERROR if Callback is NULL.
 */]])

  # H2 · 在 ExitFunction 处调用（它是该函数的唯一出口，故覆盖全部路径）
  fr_pupnp_patch_toggle(FR_HTTP_TRACE
    "upnp/src/genlib/miniserver/miniserver.c"
    [[	callback(parser, request, info);

ExitFunction:
	return rc;
}]]
    [[	callback(parser, request, info);

ExitFunction:
	fr_http_trace(info, parser, rc);
	return rc;
}]])

  if (FR_HTTP_TRACE)
    message(STATUS "[FlashRec] pupnp HTTP 日志补丁：应用完成")
  else()
    message(STATUS "[FlashRec] pupnp HTTP 日志补丁：还原完成")
  endif()
endfunction()
