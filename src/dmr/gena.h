#pragma once
// GENA 事件发送（DMR_SOAP_SPEC §4）：LastChange 组装 / 差量判重 / 一次性转义。
// SID / SEQ / 重试由 pupnp 协议栈内部处理；订阅初始全量 NOTIFY 由 dmr_device 触发。
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "app/event_bus.h"

namespace fr {

class GenaSender {
 public:
  // send(service_id, escaped_lastchange_xml)：dmr_device 提供 UpnpNotify 适配
  using SendFn = std::function<void(const char* service_id, const std::string& escaped_value)>;

  void setup(SendFn fn) { send_ = std::move(fn); }

  void send_avt_full(const PlaybackSnapshot& s);   // 订阅即推全量初始 NOTIFY（SPEC §4）
  void send_rc_full(const PlaybackSnapshot& s);
  void state_changed(const PlaybackSnapshot& s);   // 状态类立即发
  void media_changed(const PlaybackSnapshot& s);
  void volume_changed(const PlaybackSnapshot& s);
  void rc_display(const std::vector<std::pair<std::string, std::string>>& vars);
  void clear_cache();                              // 订阅全部消失时重置差量基线

 private:
  void send(const char* service_id, const std::string& raw_lastchange, const char* tag);
  static const char* kAvtSid;
  static const char* kRcsSid;
  SendFn send_;
  std::string last_state_, last_media_, last_rc_;
};

}  // namespace fr
