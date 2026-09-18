# pupnp 请求入口「原样收件箱」补丁（d144）
#
# 目的：把**控制点发来的请求原文**（请求行 + 头 + 报文体，逐字节）在**这一层**直接落盘，
# 完全不经过我方任何内部路径 —— 不经过 spdlog 的级别过滤 / 异步队列 / 已知入参白名单，
# 也不经过 dmr_device 的 SOAP 分发、player_controller 的状态分支。
#
# 为什么需要它：d143 把 SOAP 入参明细打在 `on_action`（SOAP 层），但那仍然是"内部"。
# 只要下游有任何一条分支提前 `return`（例如 `handle_set_uri` 的"重复 URI → 幂等成功"），
# 那次请求的内容就永远不会出现在任何日志里 —— 排查"控制点二次推送被吞"时正是卡在这里。
# 本补丁挂在 `dispatch_request()` 的**第一条语句**上，位置比 pupnp 的分发、我方回调都早，
# 因此**不存在**"被谁过滤掉"的可能：进来的字节一定在这里。
#
# 与 pupnp_http_trace.cmake 的分工（两者互不依赖，可单独开关）：
#   · FR_HTTP_TRACE  → http_trace.log ：每请求**一行**摘要（方法/路径/对端/分发结果码），体积小
#   · FR_SOAP_INBOX  → soap_inbox.log ：每请求**一段原文**（含报文体全文），体积大，用于取证
#
# 开关：-DFR_SOAP_INBOX=OFF 可整体还原源码树（真正的还原，不是"不应用"）。
# 运行期由环境变量 FR_SOAP_INBOX_LOG 指定文件路径；未设/为空时零成本直接返回。
#
# 锚点：miniserver.c 的 `dispatch_request()` 函数签名（**唯一一处**，且不被其它补丁触碰）
#   替换后文本 = 「fr_soap_inbox() 定义」+ 原签名 + 函数体首行插入的一次调用

option(FR_SOAP_INBOX "pupnp 请求原文收件箱：为 pupnp 应用构建期锚点补丁" ON)

include(${CMAKE_CURRENT_LIST_DIR}/pupnp_patch_util.cmake)

function(fr_apply_pupnp_soap_inbox)
  if (NOT EXISTS "${FR_ROOT}/third_party/pupnp/CMakeLists.txt")
    message(WARNING
      "[FlashRec] pupnp 收件箱补丁：找不到本地 third_party/pupnp —— 请先跑 tools/fetch_deps.sh。")
    return()
  endif()
  if (FR_SOAP_INBOX)
    message(STATUS "[FlashRec] pupnp 收件箱补丁：FR_SOAP_INBOX=ON —— 应用补丁")
  else()
    message(STATUS "[FlashRec] pupnp 收件箱补丁：FR_SOAP_INBOX=OFF —— 还原源码树")
  endif()

  fr_pupnp_patch_toggle(FR_SOAP_INBOX
    "upnp/src/genlib/miniserver/miniserver.c"
    [[static int dispatch_request(
	/*! [in] Socket Information object. */
	SOCKINFO *info,
	/*! [in] HTTP parser object. */
	http_parser_t *parser)
{]]
    [[/*!
 * \brief FlashRec local patch (d144): verbatim request inbox dump.
 *
 * Appends the request **exactly as received** (request line, headers and
 * body, byte for byte) to the file named by the FR_SOAP_INBOX_LOG
 * environment variable. It is called as the **first statement** of
 * dispatch_request(), i.e. before pupnp dispatches the message and long
 * before any FlashRec SOAP handling -- so no later stage can filter,
 * rewrite or silently drop what the control point actually sent.
 *
 * The regular application log passes through level filters, an async queue
 * and a "known argument" whitelist; a payload that a later branch decides
 * to ignore never reaches it. This file has no such path.
 *
 * Disabled when the variable is unset or empty. Single file, no rotation,
 * no truncation below FR_SOAP_INBOX_MAX bytes.
 */
#define FR_SOAP_INBOX_MAX (256u * 1024u)

static void fr_soap_inbox(
	/*! [in] Socket Information object (peer address filled on incoming). */
	SOCKINFO *info,
	/*! [in] HTTP parser object holding the raw message. */
	http_parser_t *parser)
{
	static const char *cached_path = NULL;
	static unsigned long seq = 0;
	static ithread_mutex_t lock;
	static int lock_ready = 0;
	char ts[32];
	char peer[INET6_ADDRSTRLEN] = "-";
	struct tm tmv;
	time_t now;
	http_message_t *msg;
	http_header_t *hdr;
	memptr hdr_val;
	const char *method;
	const char *raw;
	size_t raw_len;
	FILE *f;
	unsigned long my_seq;

	if (!cached_path) {
		cached_path = getenv("FR_SOAP_INBOX_LOG");
		if (!cached_path) {
			cached_path = "";
		}
		if (cached_path[0] && !lock_ready) {
			ithread_mutex_init(&lock, NULL);
			lock_ready = 1;
		}
	}
	if (!cached_path[0]) {
		return;
	}

	msg = &parser->msg;
	raw = NULL;
	raw_len = 0;
	/* 优先用「整条原始报文」（含请求行与头部），退化时才用实体/报文体。 */
	if (msg->msg.buf && msg->msg.length > msg->amount_discarded) {
		raw = msg->msg.buf + msg->amount_discarded;
		raw_len = msg->msg.length - msg->amount_discarded;
	} else if (msg->entity.buf && msg->entity.length > 0) {
		raw = msg->entity.buf;
		raw_len = msg->entity.length;
	}
	if (!raw) {
		raw = "";
	}
	if (raw_len > (size_t)FR_SOAP_INBOX_MAX) {
		raw_len = (size_t)FR_SOAP_INBOX_MAX;
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
	method = method_to_str(msg->method);
	hdr_val.buf = NULL;
	hdr_val.length = 0;
	hdr = httpmsg_find_hdr(msg, HDR_SOAPACTION, &hdr_val);

	f = fopen(cached_path, "ab");
	if (!f) {
		return;
	}
	ithread_mutex_lock(&lock);
	my_seq = ++seq;
	fprintf(f,
		"\n===== #%lu %s | %s %.*s | <- %s | raw=%luB | body=%luB\n",
		my_seq,
		ts,
		method ? method : "?",
		msg->uri.pathquery.buff && msg->uri.pathquery.size
			? (int)msg->uri.pathquery.size
			: 1,
		msg->uri.pathquery.buff ? msg->uri.pathquery.buff : "-",
		peer,
		(unsigned long)raw_len,
		(unsigned long)msg->entity.length);
	if (hdr && hdr_val.buf) {
		fprintf(f,
			"SOAPACTION: %.*s\n",
			(int)hdr_val.length,
			hdr_val.buf);
	}
	fwrite(raw, 1, raw_len, f);
	fprintf(f, "\n===== end #%lu\n", my_seq);
	fflush(f);
	ithread_mutex_unlock(&lock);
	fclose(f);
}

static int dispatch_request(
	/*! [in] Socket Information object. */
	SOCKINFO *info,
	/*! [in] HTTP parser object. */
	http_parser_t *parser)
{
	fr_soap_inbox(info, parser);]])

  if (FR_SOAP_INBOX)
    message(STATUS "[FlashRec] pupnp 收件箱补丁：应用完成")
  else()
    message(STATUS "[FlashRec] pupnp 收件箱补丁：还原完成")
  endif()
endfunction()
