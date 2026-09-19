#include "dmr/dmr_device.h"

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include <ixml.h>

#include "app/config.h"
#include "app/log.h"
#include "app/str_util.h"
#include "platform/paths.h"

namespace fr {

namespace {

// 虚拟目录打开句柄：body 指针 + 读游标（get_info → request_cookie 传递）
struct VirtualFile {
  const std::string* body = nullptr;
  size_t pos = 0;
};

constexpr const char* kDeviceType = "urn:schemas-upnp-org:device:MediaRenderer:1";
constexpr const char* kAvtType = "urn:schemas-upnp-org:service:AVTransport:1";
constexpr const char* kRcsType = "urn:schemas-upnp-org:service:RenderingControl:1";
constexpr const char* kCmnType = "urn:schemas-upnp-org:service:ConnectionManager:1";
constexpr const char* kAvtSid = "urn:upnp-org:serviceId:AVTransport";
constexpr const char* kRcsSid = "urn:upnp-org:serviceId:RenderingControl";
constexpr const char* kCmnSid = "urn:upnp-org:serviceId:ConnectionManager";
constexpr int kMaxAgeSec = 900;  // SPEC §5.4：ALIVE 按 max-age/2 自动重发（pupnp 内建）

// 服务类型 / ID → 简称（日志与 LastChange 用）
const char* service_short(const char* service_id) {
  if (!service_id) return "?";
  std::string s = service_id;
  if (s.find("AVTransport") != std::string::npos) return "AVT";
  if (s.find("RenderingControl") != std::string::npos) return "RCS";
  if (s.find("ConnectionManager") != std::string::npos) return "CM";
  return "?";
}

// sockaddr_storage → "ip" 文本（仅 IPv4 有意义，SSDP 场景足够）
void addr_to_ip(const sockaddr_storage* ss, char* out, size_t n) {
  out[0] = '\0';
  if (!ss || ss->ss_family != AF_INET) return;
  auto* in4 = reinterpret_cast<const sockaddr_in*>(ss);
  snprintf(out, n, "%s", inet_ntoa(in4->sin_addr));
}

}  // namespace

DmrDevice::DmrDevice(EventBus& bus, const Config& cfg) : bus_(bus), cfg_(cfg) {}

DmrDevice::~DmrDevice() { stop(); }

std::string DmrDevice::make_udn() {
  // 稳定 UDN：由主机名派生（同机不变、跨机不同）；设备身份不漂移
  char host[128] = {0};
#ifdef _WIN32
  WSADATA wsa{};
  WSAStartup(MAKEWORD(2, 2), &wsa);
  gethostname(host, sizeof(host) - 1);
  WSACleanup();
#else
  gethostname(host, sizeof(host) - 1);
#endif
  uint64_t h = 1469598103934665603ull;
  std::string seed = std::string(host) + "|FlashRec|DMR";
  for (char c : seed) {
    h ^= static_cast<unsigned char>(c);
    h *= 1099511628211ull;
  }
  char buf[48];
  snprintf(buf, sizeof(buf), "uuid:2f%04x-%04x-4%03x-8%03x-%012llx", (unsigned)(h & 0xffff),
           (unsigned)((h >> 16) & 0xffff), (unsigned)((h >> 32) & 0xfff),
           (unsigned)((h >> 44) & 0xfff), (unsigned long long)(h & 0xffffffffffffull));
  return buf;
}

bool DmrDevice::load_scpd_assets() {
  const char* files[] = {"AVTransport.xml", "RenderingControl.xml", "ConnectionManager.xml"};
  for (auto* f : files) {
    std::string path = paths::asset_file("scpd/" + std::string(f));
    if (path.empty()) {
      FR_LOG_ERROR("[DMR] 缺 SCPD 资源 scpd/{}", f);
      return false;
    }
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return false;
    std::string body;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) body.append(buf, n);
    fclose(fp);
    FR_LOG_INFO("[DMR] SCPD 装载 {} ({} B)", f, body.size());
    scpd_["/fr/scpd/" + std::string(f)] = std::move(body);
  }
  return true;
}

void DmrDevice::build_device_xml(const char* ip, int port) {
  // SPEC §5.5：friendlyName 实体转义（原定 CDATA，但 pupnp ixml 的 CDATA 实现遇非 ASCII 解析失败
// rc=106，实测 2026-09-15；转义同等达成"防中文转义坑"）、X_DLNADOC DMR-1.50、URLBase 一致
  char url[256];
  snprintf(url, sizeof(url), "http://%s:%d/", ip, port);
  std::string name = cfg_.friendly_name;
  desc_xml_ =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
      "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">\r\n"
      "  <specVersion><major>1</major><minor>0</minor></specVersion>\r\n"
      "  <URLBase>" + std::string(url) + "</URLBase>\r\n"
      "  <device>\r\n"
      "    <deviceType>" + std::string(kDeviceType) + "</deviceType>\r\n"
      "    <friendlyName>" + xml_escape(name) + "</friendlyName>\r\n"
      "    <manufacturer>FlashRec</manufacturer>\r\n"
      "    <manufacturerURL>https://github.com/flashrec</manufacturerURL>\r\n"
      "    <modelDescription>FlashRec DLNA Media Renderer</modelDescription>\r\n"
      "    <modelName>FlashRec</modelName>\r\n"
      "    <modelNumber>2.0</modelNumber>\r\n"
      "    <UDN>" + udn_ + "</UDN>\r\n"
      "    <dlna:X_DLNADOC xmlns:dlna=\"urn:schemas-dlna-org:device-1-0\">DMR-1.50</dlna:X_DLNADOC>\r\n"
      "    <serviceList>\r\n"
      "      <service><serviceType>" + std::string(kAvtType) + "</serviceType><serviceId>" + kAvtSid +
      "</serviceId><SCPDURL>/fr/scpd/AVTransport.xml</SCPDURL>"
      "<controlURL>/fr/control/AVTransport</controlURL><eventSubURL>/fr/event/AVTransport</eventSubURL></service>\r\n"
      "      <service><serviceType>" + std::string(kRcsType) + "</serviceType><serviceId>" + kRcsSid +
      "</serviceId><SCPDURL>/fr/scpd/RenderingControl.xml</SCPDURL>"
      "<controlURL>/fr/control/RenderingControl</controlURL><eventSubURL>/fr/event/RenderingControl</eventSubURL></service>\r\n"
      "      <service><serviceType>" + std::string(kCmnType) + "</serviceType><serviceId>" + kCmnSid +
      "</serviceId><SCPDURL>/fr/scpd/ConnectionManager.xml</SCPDURL>"
      "<controlURL>/fr/control/ConnectionManager</controlURL><eventSubURL>/fr/event/ConnectionManager</eventSubURL></service>\r\n"
      "    </serviceList>\r\n"
      "  </device>\r\n"
      "</root>\r\n";
}

bool DmrDevice::start() {
  if (started_) return true;
  udn_ = make_udn();
  gena_.setup([this](const char* service_id, const std::string& escaped) {
    if (!handle_) return;
    const char* names[1] = {"LastChange"};
    const char* values[1] = {escaped.c_str()};
    UpnpNotify(handle_, udn_.c_str(), service_id, names, values, 1);
  });

  int rc = UpnpInit2(nullptr, cfg_.dlna_port == 0 ? 0 : cfg_.dlna_port);
  if (rc != UPNP_E_SUCCESS) {
    FR_LOG_ERROR("[DMR] UpnpInit2 失败 rc={}（检查防火墙 UDP 1900 / TCP {}）", rc, cfg_.dlna_port);
    return false;
  }
  const char* ip = UpnpGetServerIpAddress();
  int port = static_cast<int>(UpnpGetServerPort());
  FR_LOG_INFO("[SSDP] 协议栈就绪 ip={} port={}", ip ? ip : "?", port);

  if (!load_scpd_assets()) return false;
  build_device_xml(ip, port);
  // 描述别名兜底：部分控制点（如百度 DumediaDLNA）投屏中途会按拼出的路径
  // 重拉设备描述确认能力（实测 2026-09-15 GET /fr/device.xml 404 后放弃调 SetVolume）。
  // pupnp 内部 web 服务只应答 SSDP LOCATION 的正式路径，别名落到虚拟目录，这里主动喂 200。
  scpd_["/fr/device.xml"] = desc_xml_;
  scpd_["/fr/description.xml"] = desc_xml_;

  // pupnp 1.14：逐个注册虚拟目录回调（无 UpnpSetVirtualDirCallbacks）
  UpnpVirtualDir_set_GetInfoCallback(&DmrDevice::vd_get_info);
  UpnpVirtualDir_set_OpenCallback(&DmrDevice::vd_open);
  UpnpVirtualDir_set_ReadCallback(&DmrDevice::vd_read);
  UpnpVirtualDir_set_CloseCallback(&DmrDevice::vd_close);
  const void* oldcookie = nullptr;
  UpnpAddVirtualDir("fr", this, &oldcookie);

  rc = UpnpRegisterRootDevice2(UPNPREG_BUF_DESC, desc_xml_.c_str(),
                               static_cast<int>(desc_xml_.size()), 1, &DmrDevice::thunk, this,
                               &handle_);
  if (rc != UPNP_E_SUCCESS) {
    FR_LOG_ERROR("[DMR] UpnpRegisterRootDevice2 失败 rc={}", rc);
    return false;
  }
  rc = UpnpSendAdvertisement(handle_, kMaxAgeSec);
  FR_LOG_INFO("[SSDP] ALIVE 已广播（max-age={}s，自动重发）rc={} UDN={}", kMaxAgeSec, rc, udn_);
  started_ = true;
  return true;
}

void DmrDevice::stop() {
  if (!started_) return;
  if (handle_) {
    // UpnpNotify 是投递到 libupnp 线程池异步发送的；不留时间窗就直接 UpnpFinish
    // 会整栈拆除并丢弃队列，手机端收不到最终 NOTIFY（"投屏结束"）
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    UpnpUnRegisterRootDevice(handle_);
    handle_ = 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // BYEBYE 组播发出
  }
  UpnpFinish();
  started_ = false;
  FR_LOG_INFO("[DMR] 设备已下线");
}

void DmrDevice::tick() {
  for (auto& e : bus_.drain_events()) {
    switch (e.kind) {
      case PlayerEvent::Kind::StateChanged: gena_.state_changed(e.snap); break;
      case PlayerEvent::Kind::MediaInfo: gena_.media_changed(e.snap); break;
      case PlayerEvent::Kind::Volume: gena_.volume_changed(e.snap); break;
      // d169：倍速非 AVT 事件变量（SOAP 无对应状态），只更新快照供 UI 显示
      case PlayerEvent::Kind::Speed: break;
      case PlayerEvent::Kind::Position: break;  // 位置不进事件：App 只轮询 GetPositionInfo
    }
  }
}

// ---------------- pupnp 回调 ----------------

int DmrDevice::thunk(Upnp_EventType type, const void* event, void* cookie) {
  auto* self = static_cast<DmrDevice*>(cookie);
  if (!self || !event) return 0;
  switch (type) {
    case UPNP_CONTROL_ACTION_REQUEST:
      return self->on_action(const_cast<void*>(event));
    case UPNP_EVENT_SUBSCRIPTION_REQUEST:
      return self->on_subscription(const_cast<void*>(event));
    case UPNP_DISCOVERY_SEARCH_RESULT:
    case UPNP_DISCOVERY_ADVERTISEMENT_ALIVE:
    case UPNP_DISCOVERY_ADVERTISEMENT_BYEBYE:
      self->log_discovery(type, event);
      return 0;
    default:
      return 0;
  }
}

void DmrDevice::log_discovery(Upnp_EventType type, const void* event) {
  auto* d = static_cast<const UpnpDiscovery*>(event);
  if (!d) return;
  const char* tag = type == UPNP_DISCOVERY_SEARCH_RESULT
                        ? "M-SEARCH-RESP"
                        : type == UPNP_DISCOVERY_ADVERTISEMENT_ALIVE ? "ALIVE" : "BYEBYE";
  const char* loc = UpnpDiscovery_get_Location_cstr(d);
  // M-SEARCH 的 ST 在 ServiceType 字段；其余报文该字段为服务类型
  const char* st = UpnpDiscovery_get_ServiceType_cstr(d);
  char ip[64];
  addr_to_ip(UpnpDiscovery_get_DestAddr(d), ip, sizeof(ip));
  FR_LOG_DEBUG("[SSDP] {} ip={} st={} location={}", tag, ip, st && *st ? st : "-",
               loc ? loc : "?");
}

// 从 action request DOM 提取 (名, 值) 参数表
static SoapArgs extract_args(IXML_Document* doc, const char* action_name) {
  SoapArgs out;
  if (!doc || !action_name) return out;
  IXML_Node* root = ixmlNode_getFirstChild(reinterpret_cast<IXML_Node*>(doc));
  if (!root) return out;
  // 入参是 root 的子元素
  for (IXML_Node* n = ixmlNode_getFirstChild(root); n; n = ixmlNode_getNextSibling(n)) {
    const char* nm = ixmlNode_getNodeName(n);
    if (!nm) continue;
    std::string val;
    for (IXML_Node* t = ixmlNode_getFirstChild(n); t; t = ixmlNode_getNextSibling(t)) {
      const char* v = ixmlNode_getNodeValue(t);
      if (v) val += v;
      for (IXML_Node* t2 = ixmlNode_getFirstChild(t); t2; t2 = ixmlNode_getNextSibling(t2)) {
        const char* v2 = ixmlNode_getNodeValue(t2);
        if (v2) val += v2;
      }
    }
    out.emplace_back(nm, val);
  }
  return out;
}

int DmrDevice::on_action(void* req) {
  auto* er = static_cast<UpnpActionRequest*>(req);
  int req_id = next_req_id();
  auto t0 = std::chrono::steady_clock::now();

  const char* action = UpnpActionRequest_get_ActionName_cstr(er);
  const char* service_id = UpnpActionRequest_get_ServiceID_cstr(er);
  IXML_Document* req_doc = UpnpActionRequest_get_ActionRequest(er);
  std::string act = action ? action : "";
  const char* svc = service_short(service_id);
  SoapArgs args = extract_args(req_doc, act.c_str());

  // [HTTP] 层日志：来源 IP + User-Agent（pupnp 1.14 直接暴露 Os 字段）
  char ip[64];
  addr_to_ip(UpnpActionRequest_get_CtrlPtIPAddr(er), ip, sizeof(ip));
  const char* ua = UpnpActionRequest_get_Os_cstr(er);
  FR_LOG_INFO("[SOAP] [req#{}] {} {} ← {} ua=\"{}\" 参数 {} 项", req_id, svc, act, ip,
              ua ? ua : "", args.size());
  // ★d143：入参明细落盘。原先这段走 FR_LOG_TRACE，而全项目未定义 SPDLOG_ACTIVE_LEVEL
  // ⇒ 该宏**编译期就被抹成空语句**（实测 `log.level=trace` 但日志里 [trace] 行数 = 0），
  // 于是"控制点多传了什么参数"永远看不见 —— 这正是"投屏起始进度查不下去"的卡点。
  // 刻意**不改** SPDLOG_ACTIVE_LEVEL：那会让 mpv 每帧日志一并涌出，5MB×7 的轮转几分钟翻一圈，
  // 反而冲掉要找的行。策略：
  //   · 默认：只打**不在已知入参名单里**的参数（控制点塞的私货 —— 入参个数超出 SCPD 声明的
  //     部分，最可能是起始进度的载体），常规请求零噪音；
  //   · log.trace_http_body=true：全部打出且不截断（抓包模式）。
  static const char* kKnownArgs[] = {"InstanceID",   "CurrentURI",      "CurrentURIMetaData",
                                     "NextURI",      "NextURIMetaData", "DesiredVolume",
                                     "DesiredMute",  "PresetName",      "Target",
                                     "Unit"};
  const bool full_args = cfg_.log_trace_http_body;
  for (auto& [k, v] : args) {
    bool known = false;
    for (auto* n : kKnownArgs) {
      if (k == n) {
        known = true;
        break;
      }
    }
    if (known && !full_args) continue;
    std::string head = full_args ? v : v.substr(0, 1200);
    FR_LOG_INFO("[SOAP] [req#{}]   入参 {}{} = {}", req_id, known ? "" : "【扩展】", k, head);
  }

  PlaybackSnapshot snap = bus_.snapshot();
  SoapResult r;
  if (svc == std::string("AVT")) {
    r = handle_avtransport(act, args, snap, cfg_, bus_, req_id);
  } else if (svc == std::string("RCS")) {
    r = handle_rendering_control(act, args, snap, cfg_, bus_, rc_display_, req_id);
  } else if (svc == std::string("CM")) {
    r = handle_connection_manager(act, args, snap, cfg_, bus_, req_id);
  } else {
    r.ok = false;
    r.error_code = kErrInvalidAction;
    r.error_msg = "Unknown service";
  }

  // 立即 NOTIFY（RC 显示类 Set）
  if (!r.notify_now.empty()) gena_.rc_display(r.notify_now);

  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                  t0)
                .count();
  // soap_device.c 语义：回调返回值被忽略，只看 ErrCode；ErrCode != 0 → 标准 fault
  if (!r.ok) {
    FR_LOG_WARN("[SOAP] [req#{}] {} 失败 err={}（{}ms）{}", req_id, act, r.error_code, ms,
                r.error_msg);
    UpnpActionRequest_set_ActionResult(er, nullptr);
    UpnpActionRequest_set_ErrCode(er, r.error_code);
    // ★d131 必须同时给 ErrStr：soap_device.c 里 ErrCode != 0 时**只有 ErrStr 非空才原样透出**，
    // 空则无条件改写为 501 Action Failed ⇒ 401/402/701/710 全部退化成 501，控制点无法区分
    // "不支持/参数错/状态不允许/无此项"，会做错误兜底。error_msg 同时成为 fault 的 errorDescription。
    UpnpActionRequest_strcpy_ErrStr(er, r.error_msg.c_str());
    return UPNP_E_SUCCESS;
  }

  // 组装 <u:ActionNameResponse xmlns:u="serviceType">；文档所有权移交 pupnp
  IXML_Document* resp = ixmlDocument_createDocument();
  if (!resp) {
    UpnpActionRequest_set_ErrCode(er, kErrActionFailed);
    UpnpActionRequest_strcpy_ErrStr(er, "Cannot create response document");
    return UPNP_E_SUCCESS;
  }
  const char* svc_type = svc == std::string("AVT")   ? kAvtType
                         : svc == std::string("RCS") ? kRcsType
                                                     : kCmnType;
  // ★d130 元素名必须带 u: 前缀：只设 xmlns:u 而不用在元素名上，元素就落在**空命名空间**，
  // 只比标签名的解析器照常命中，但 namespace-aware 解析器（Go/.NET/JAXB/lxml）查
  // {serviceType}ActionResponse 必然取不到 ⇒ HTTP 200、值全对、控制点 UI 却零反馈。
  // 出参子元素保持不带前缀（UDA §3.1.1 示例即如此）。
  std::string root_name = "u:" + act + "Response";
  IXML_Element* root = ixmlDocument_createElement(resp, root_name.c_str());
  ixmlElement_setAttribute(root, "xmlns:u", svc_type);
  for (auto& [k, v] : r.out) {
    IXML_Element* el = ixmlDocument_createElement(resp, k.c_str());
    IXML_Node* text = ixmlDocument_createTextNode(resp, v.c_str());
    ixmlNode_appendChild(reinterpret_cast<IXML_Node*>(el), text);
    ixmlNode_appendChild(reinterpret_cast<IXML_Node*>(root), reinterpret_cast<IXML_Node*>(el));
  }
  ixmlNode_appendChild(reinterpret_cast<IXML_Node*>(resp), reinterpret_cast<IXML_Node*>(root));
  UpnpActionRequest_set_ActionResult(er, resp);
  UpnpActionRequest_set_ErrCode(er, UPNP_E_SUCCESS);
  FR_LOG_INFO("[SOAP] [req#{}] {} 成功（{}ms）出参 {} 项", req_id, act, ms, r.out.size());
  return UPNP_E_SUCCESS;
}

int DmrDevice::on_subscription(void* req) {
  auto* sr = static_cast<UpnpSubscriptionRequest*>(req);
  const char* service_id = UpnpSubscriptionRequest_get_ServiceId_cstr(sr);
  const char* sid = UpnpSubscriptionRequest_get_SID_cstr(sr);
  const char* short_name = service_short(service_id);
  FR_LOG_INFO("[GENA] SUBSCRIBE {} sid={}", short_name, sid ? sid : "-");
  // pupnp 1.14 对新订阅也回填新生成的 SID（gena_device.c respond_ok 之前
  // UpnpSubscriptionRequest_strcpy_SID 用的是新 sub 的 sid），无法用 SID 非空区分新/续订。
  // 实测 2026-09-15：新订阅被误判续订 → 初始全量 NOTIFY 从未发出。
  // 一律推初始全量：新订阅拿到状态转储（SPEC §4），续订收到 = 额外状态刷新，无害。
  PlaybackSnapshot snap = bus_.snapshot();
  // ★d136：按服务给**该服务自己**的初始值。ConnectionManager 的 evented 变量是
  // SourceProtocolInfo / SinkProtocolInfo / CurrentConnectionIDs（CM:1 事件模型不是 LastChange），
  // 原实现让 CM 也走 AVT 的 LastChange ⇒ 变量名对不上，AcceptSubscription 直接失败，
  // 订阅型控制点会认为"设备未连接"。
  if (short_name == std::string("CM")) {
    const std::string sink = cfg_.sink_protocol_info();
    const char* names[3] = {"SourceProtocolInfo", "SinkProtocolInfo", "CurrentConnectionIDs"};
    const char* values[3] = {"", sink.c_str(), "0"};
    int rc = UpnpAcceptSubscription(handle_, udn_.c_str(), service_id, names, values, 3, sid);
    FR_LOG_INFO("[GENA] 初始全量 NOTIFY CM sink_len={} rc={}", sink.size(), rc);
    if (rc != UPNP_E_SUCCESS) FR_LOG_WARN("[GENA] AcceptSubscription rc={} CM", rc);
    return UPNP_E_SUCCESS;
  }
  std::string initial = short_name == std::string("RCS") ? lastchange_rc_full(snap)
                                                         : lastchange_avt_full(snap);
  std::string escaped = xml_escape(initial);  // propertyset <val> 原样内嵌 → 转义一次
  const char* names[1] = {"LastChange"};
  const char* values[1] = {escaped.c_str()};
  int rc = UpnpAcceptSubscription(handle_, udn_.c_str(), service_id, names, values, 1, sid);
  FR_LOG_INFO("[GENA] 初始全量 NOTIFY {} len={} rc={}", short_name, initial.size(), rc);
  if (rc != UPNP_E_SUCCESS) FR_LOG_WARN("[GENA] AcceptSubscription rc={} {}", rc, short_name);
  return UPNP_E_SUCCESS;
}

// ---------------- 虚拟目录（SCPD 静态服务）----------------

const std::string* DmrDevice::find_virtual(const std::string& path) const {
  auto it = scpd_.find(path);
  return it == scpd_.end() ? nullptr : &it->second;
}

int DmrDevice::vd_get_info(const char* filename, UpnpFileInfo* info, const void* cookie,
                           const void** request_cookie) {
  auto* self = static_cast<const DmrDevice*>(cookie);
  if (!self || !filename || !info) return UPNP_E_INVALID_PARAM;
  auto* body = self->find_virtual(filename);
  if (!body) {
    FR_LOG_WARN("[HTTP] GET {} 404", filename);
    return UPNP_E_FILE_NOT_FOUND;
  }
  UpnpFileInfo_set_FileLength(info, static_cast<off_t>(body->size()));
  UpnpFileInfo_set_LastModified(info, time(nullptr));
  UpnpFileInfo_set_IsDirectory(info, 0);
  UpnpFileInfo_set_IsReadable(info, 1);
  UpnpFileInfo_set_ContentType(info, const_cast<DOMString>("text/xml; charset=\"utf-8\""));
  if (request_cookie) *request_cookie = body;  // body 指针随 request 传给 open/read/close
  FR_LOG_INFO("[HTTP] GET {} 200 ({} B)", filename, body->size());
  return UPNP_E_SUCCESS;
}

UpnpWebFileHandle DmrDevice::vd_open(const char* filename, enum UpnpOpenFileMode mode,
                                     const void* cookie, const void* request_cookie) {
  if (mode != UPNP_READ || !request_cookie) return nullptr;
  auto* body = static_cast<const std::string*>(request_cookie);
  FR_LOG_DEBUG("[HTTP] OPEN {} ({} B)", filename ? filename : "?", body->size());
  return new VirtualFile{body, 0};
}

int DmrDevice::vd_read(UpnpWebFileHandle handle, char* buf, size_t buflen, const void* cookie,
                       const void* request_cookie) {
  (void)cookie;
  (void)request_cookie;
  auto* vf = static_cast<VirtualFile*>(handle);
  if (!vf || !vf->body || !buf) return -1;
  size_t remain = vf->body->size() - vf->pos;
  size_t n = remain < buflen ? remain : buflen;
  if (n > 0) memcpy(buf, vf->body->data() + vf->pos, n);
  vf->pos += n;
  return static_cast<int>(n);
}

int DmrDevice::vd_close(UpnpWebFileHandle handle, const void* cookie, const void* request_cookie) {
  (void)cookie;
  (void)request_cookie;
  delete static_cast<VirtualFile*>(handle);
  return 0;
}

}  // namespace fr
