#pragma once
// DMR 设备侧：libupnp（pupnp）集成 —— SSDP / 设备与 SCPD 描述 / SOAP 分发 / GENA 订阅。
// 回调线程只投递 DmrCommand（规则 4）；LastChange 由 tick() 在主线程差量推送。
#include <map>
#include <string>

#include <upnp.h>

#include "app/event_bus.h"
#include "dmr/gena.h"
#include "dmr/soap_util.h"

namespace fr {

struct Config;

class DmrDevice {
 public:
  DmrDevice(EventBus& bus, const Config& cfg);
  ~DmrDevice();

  bool start();  // UpnpInit2 + 注册设备 + 广播；失败返回 false
  void stop();
  // 主线程每帧：消费 PlayerEvent → 差量 GENA NOTIFY（状态类立即，位置类不推）
  void tick();

 private:
  // —— pupnp 回调（静态跳板）——
  static int thunk(Upnp_EventType type, const void* event, void* cookie);
  int on_action(void* action_request);        // UPNP_CONTROL_ACTION_REQUEST
  int on_subscription(void* subscribe_req);   // UPNP_EVENT_SUBSCRIPTION_REQUEST
  void log_discovery(Upnp_EventType type, const void* event);

  // —— 虚拟目录：SCPD 静态文件（SCPDURL 可 GET，404=投屏失败）——
  // pupnp 1.14 新式回调：get_info 通过 request_cookie 把 body 指针带给 open/read/close
  static int vd_get_info(const char* filename, UpnpFileInfo* info, const void* cookie,
                         const void** request_cookie);
  static UpnpWebFileHandle vd_open(const char* filename, enum UpnpOpenFileMode mode,
                                   const void* cookie, const void* request_cookie);
  static int vd_read(UpnpWebFileHandle handle, char* buf, size_t buflen, const void* cookie,
                     const void* request_cookie);
  static int vd_close(UpnpWebFileHandle handle, const void* cookie, const void* request_cookie);
  const std::string* find_virtual(const std::string& path) const;

  void build_device_xml(const char* ip, int port);
  bool load_scpd_assets();
  static std::string make_udn();

  EventBus& bus_;
  const Config& cfg_;
  GenaSender gena_;
  RcDisplay rc_display_;
  UpnpDevice_Handle handle_ = 0;  // pupnp: typedef int
  std::string udn_;
  std::string desc_xml_;
  std::map<std::string, std::string> scpd_;  // "/fr/scpd/AVTransport.xml" -> body
  bool started_ = false;
};

}  // namespace fr
