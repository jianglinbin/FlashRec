#pragma once
// SOAP 语义层共享：出参容器 / 错误码 / 三大服务 action 分发 / GENA LastChange 构造。
// ixml 解析与拼装在 dmr_device（协议壳），本层只管"每个 action 该答什么"（DMR_SOAP_SPEC §1）。
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "app/event_bus.h"

namespace fr {

struct Config;

// UPnP 标准错误码。SPEC 全局铁律：唯一允许 710 的是 Next/Previous 无上/下项；
// 违规状态转移回 701；任何 action 禁止 500 裸错。
constexpr int kErrInvalidAction = 401;
constexpr int kErrInvalidArgs = 402;
constexpr int kErrActionFailed = 501;
constexpr int kErrInvalidTransition = 701;
constexpr int kErrNoSuchItem = 710;

using SoapArgs = std::vector<std::pair<std::string, std::string>>;  // (名, 值) 有序

struct SoapResult {
  bool ok = true;
  int error_code = 0;
  std::string error_msg;
  SoapArgs out;                                      // 有序出参
  SoapArgs notify_now;                               // 需要立即 NOTIFY 的 LastChange 变量（RC 显示类）
};

// RenderingControl 显示类变量的内存真值（Set 存内存回 LastChange，能答默认值绝不报错）
struct RcDisplay {
  int brightness = 100, contrast = 100, sharpness = 100;
  int color_temp = 0, red_gain = 0, green_gain = 0, blue_gain = 0;
  int h_keystone = 0, v_keystone = 0;
  int* find(const std::string& name);
};

// 三大服务 action 分发
SoapResult handle_avtransport(const std::string& action, const SoapArgs& args,
                              const PlaybackSnapshot& snap, const Config& cfg, EventBus& bus,
                              int req_id);
SoapResult handle_rendering_control(const std::string& action, const SoapArgs& args,
                                    const PlaybackSnapshot& snap, const Config& cfg, EventBus& bus,
                                    RcDisplay& rc, int req_id);
SoapResult handle_connection_manager(const std::string& action, const SoapArgs& args,
                                     const PlaybackSnapshot& snap, const Config& cfg, EventBus& bus,
                                     int req_id);

// GENA LastChange（SPEC §4）
std::string build_lastchange(
    const std::string& ns, const std::vector<std::pair<std::string, std::string>>& vars);
// 带额外属性的变体：Volume/Mute/VolumeDB 必须携带 channel="Master"（RCS 1.0 §2.5.3，
// 缺了订阅型控制点按 channel 过滤会认为设备不支持调音量），attrs 原样拼接如 `channel="Master"`
std::string build_lastchange(
    const std::string& ns,
    const std::vector<std::tuple<std::string, std::string, std::string>>& vars);
std::string lastchange_avt_full(const PlaybackSnapshot& snap);  // 订阅即推全量
std::string lastchange_rc_full(const PlaybackSnapshot& snap);

}  // namespace fr
