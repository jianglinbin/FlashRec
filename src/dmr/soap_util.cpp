#include "dmr/soap_util.h"

#include "app/config.h"
#include "app/str_util.h"

namespace fr {

int* RcDisplay::find(const std::string& name) {
  if (name == "Brightness") return &brightness;
  if (name == "Contrast") return &contrast;
  if (name == "Sharpness") return &sharpness;
  if (name == "ColorTemperature") return &color_temp;
  if (name == "RedVideoGain") return &red_gain;
  if (name == "GreenVideoGain") return &green_gain;
  if (name == "BlueVideoGain") return &blue_gain;
  if (name == "HorizontalKeystone") return &h_keystone;
  if (name == "VerticalKeystone") return &v_keystone;
  return nullptr;
}

// ---------------- AVTransport:1 ----------------

SoapResult handle_avtransport(const std::string& action, const SoapArgs& args,
                              const PlaybackSnapshot& snap, const Config& cfg, EventBus& bus,
                              int req_id) {
  SoapResult r;
  auto find = [&](const char* n) -> const std::string* {
    for (auto& [k, v] : args)
      if (k == n) return &v;
    return nullptr;
  };
  auto need = [&](std::initializer_list<const char*> names) -> bool {
    for (auto* n : names) {
      auto* v = find(n);
      if (!v) {
        r.ok = false;
        r.error_code = kErrInvalidArgs;
        r.error_msg = "Missing " + std::string(n);
        return false;
      }
    }
    return true;
  };
  auto publish = [&](DmrCommand::Type t) {
    DmrCommand c;
    c.type = t;
    c.req_id = req_id;
    if (auto* v = find("CurrentURI")) c.uri = *v;
    if (auto* v = find("CurrentURIMetaData"))
      c.metadata = xml_unescape(unwrap_cdata(*v));  // SPEC R1：反转义 CDATA/实体保留标题
    if (auto* v = find("NextURI")) c.uri = *v;
    if (auto* v = find("NextURIMetaData")) c.metadata = xml_unescape(unwrap_cdata(*v));
    if (auto* v = find("DesiredVolume")) c.volume = atoi(v->c_str());
    if (auto* v = find("DesiredMute")) c.mute = (*v == "1" || *v == "true" || *v == "yes");
    if (auto* v = find("PresetName")) c.preset = *v;
    if (auto* v = find("Target")) c.seek_to = parse_hms_seconds(*v);
    bus.publish_dmr(std::move(c));
  };

  using S = TransportState;
  if (action == "SetAVTransportURI") {
    if (!need({"InstanceID", "CurrentURI"})) return r;
    publish(DmrCommand::Type::SetUri);  // R1：任何状态无条件接受，默认自动开播
  } else if (action == "Play") {
    if (!need({"InstanceID"})) return r;
    publish(DmrCommand::Type::Play);
  } else if (action == "Pause") {
    if (!need({"InstanceID"})) return r;
    publish(DmrCommand::Type::Pause);
  } else if (action == "Stop") {
    if (!need({"InstanceID"})) return r;
    publish(DmrCommand::Type::Stop);  // Stop 触发进度快照（R5）
  } else if (action == "Seek") {
    if (!need({"InstanceID", "Unit", "Target"})) return r;
    // Unit 容错：REL_TIME / ABS_TIME 正常，TRACK_NR 按 REL_TIME 处理
    if (snap.is_live && !snap.seekable) {
      r.ok = false;
      r.error_code = kErrNoSuchItem;  // R6：非 DVR 直播禁 seek（强 seek 断流）
      r.error_msg = "Live stream is not seekable";
      return r;
    }
    if (snap.state != S::Playing && snap.state != S::PausedPlayback &&
        snap.state != S::Ready) {
      r.ok = false;
      r.error_code = kErrInvalidTransition;
      r.error_msg = "Transition not available";
      return r;
    }
    publish(DmrCommand::Type::Seek);
  } else if (action == "GetPositionInfo") {
    if (!need({"InstanceID"})) return r;
    bool has = snap.has_session;
    r.out = {
        {"Track", has ? "1" : "0"},
        {"TrackDuration", (has && !snap.is_live) ? format_hms_padded(snap.duration) : "00:00:00"},
        {"TrackMetaData", snap.metadata},
        {"TrackURI", uri_without_fragment(snap.uri)},
        {"RelTime", has ? format_hms_padded(snap.position) : "00:00:00"},
        {"AbsTime", has ? format_hms_padded(snap.position) : "00:00:00"},
        {"RelCount", "2147483647"},
        {"AbsCount", "2147483647"},
    };
  } else if (action == "GetTransportInfo") {
    if (!need({"InstanceID"})) return r;
    r.out = {
        {"CurrentTransportState", transport_state_upnp(snap.state)},
        {"CurrentTransportStatus", "OK"},  // R5：恒 OK（非 OK 会被部分 App 判设备异常）
        {"CurrentSpeed", "1"},
    };
  } else if (action == "GetMediaInfo") {
    // 兼容性关键：漏实现 = 部分 App 投屏直接失败（SPEC §1.1.8）
    if (!need({"InstanceID"})) return r;
    bool has = snap.has_session;
    r.out = {
        {"NrTracks", has ? "1" : "0"},
        {"MediaDuration", (has && !snap.is_live) ? format_hms_padded(snap.duration) : "00:00:00"},
        {"CurrentURI", uri_without_fragment(snap.uri)},
        {"CurrentURIMetaData", snap.metadata},
        {"NextURI", uri_without_fragment(snap.next_uri)},
        {"NextURIMetaData", snap.next_metadata},
        {"PlayMedium", "NETWORK"},
        {"RecordMedium", "NOT_IMPLEMENTED"},
        {"WriteStatus", "NOT_IMPLEMENTED"},
    };
  } else if (action == "GetDeviceCapabilities") {
    if (!need({"InstanceID"})) return r;
    r.out = {
        {"PlayMedia", "NETWORK"},           // 漏了部分 App 不显示拖动条
        {"RecMedia", "NOT_IMPLEMENTED"},
        {"RecQualityModes", "NOT_IMPLEMENTED"},
    };
  } else if (action == "GetTransportSettings") {
    if (!need({"InstanceID"})) return r;
    r.out = {{"PlayMode", "NORMAL"}, {"RecQualityMode", "NOT_IMPLEMENTED"}};
  } else if (action == "GetCurrentTransportActions") {
    // SPEC §1.13：按当前态回**真实支持集**，控制点据此决定控件可用性（缺了这个动作，
    // 控制点按 SCPD 建表时找不到，直接当作"设备没有传输控制能力"）。名单与真实行为严格对齐
    // （player_controller 的合法性判定 + 本文件 Seek 的前置条件）：
    //   IDLE→空（只能 SetURI）；TRANSITIONING→Play/Pause/Stop（三者入队，file-loaded 后按序执行；
    //   Seek 在 Transitioning 会被本层直接判 701，故不列）；STOPPED→Play（保持期内按当前位置重推）；
    //   READY→Play/Seek；PLAYING→Pause/Stop/Seek；PAUSED_PLAYBACK→Play/Stop/Seek。
    // 有下一集才追加 Next；Previous/Record 永不列入（前者明确回 710、后者未实现）。
    if (!need({"InstanceID"})) return r;
    std::string acts;
    auto add = [&acts](const char* a) {
      if (!acts.empty()) acts += ',';
      acts += a;
    };
    const bool can_seek = !(snap.is_live && !snap.seekable);
    switch (snap.state) {
      case S::Transitioning:
        add("Play");
        add("Pause");
        add("Stop");
        break;
      case S::Stopped:
        if (snap.has_session) add("Play");
        break;
      case S::Ready:
        add("Play");
        if (can_seek) add("Seek");
        break;
      case S::Playing:
        add("Pause");
        add("Stop");
        if (can_seek) add("Seek");
        break;
      case S::PausedPlayback:
        add("Play");
        add("Stop");
        if (can_seek) add("Seek");
        break;
      case S::Idle:
      default:
        break;  // 空串
    }
    if (!snap.next_uri.empty()) add("Next");
    r.out = {{"Actions", acts}};
  } else if (action == "Next") {
    if (!need({"InstanceID"})) return r;
    if (snap.next_uri.empty()) {
      r.ok = false;
      r.error_code = kErrNoSuchItem;  // 全局唯一允许 710 的场景之一
      r.error_msg = "No such item";
      return r;
    }
    DmrCommand c;
    c.type = DmrCommand::Type::SetUri;
    c.uri = snap.next_uri;
    c.metadata = snap.next_metadata;
    c.req_id = req_id;
    bus.publish_dmr(std::move(c));
  } else if (action == "Previous") {
    if (!need({"InstanceID"})) return r;
    r.ok = false;  // 无上一项：710（唯一允许场景）
    r.error_code = kErrNoSuchItem;
    r.error_msg = "No such item";
  } else if (action == "SetNextAVTransportURI") {
    if (!need({"InstanceID", "NextURI"})) return r;
    publish(DmrCommand::Type::SetNextUri);  // 存 next，EOF 自动消费（R2）
  } else {
    r.ok = false;
    r.error_code = kErrInvalidAction;
    r.error_msg = "Invalid action " + action;
  }
  return r;
}

// ---------------- RenderingControl:1 ----------------

SoapResult handle_rendering_control(const std::string& action, const SoapArgs& args,
                                    const PlaybackSnapshot& snap, const Config& cfg, EventBus& bus,
                                    RcDisplay& rc, int req_id) {
  SoapResult r;
  (void)cfg;
  auto find = [&](const char* n) -> const std::string* {
    for (auto& [k, v] : args)
      if (k == n) return &v;
    return nullptr;
  };
  auto need = [&](std::initializer_list<const char*> names) -> bool {
    for (auto* n : names) {
      auto* v = find(n);
      if (!v) {
        r.ok = false;
        r.error_code = kErrInvalidArgs;
        r.error_msg = "Missing " + std::string(n);
        return false;
      }
    }
    return true;
  };

  if (action == "ListPresets") {
    // 兼容性关键：漏了部分 App 音量面板异常（SPEC §1.2.1）
    // ★d134 对齐 SCPD/规范：出参名必须是 SCPD 宣告的 CurrentPresetNameList（原实现回
    // PresetNameList，按 SCPD 取值的控制点永远取不到）；值是状态变量的 CSV，官方预置名
    // 是复数 FactoryDefaults（RCS:1 表 2-17）。
    if (!need({"InstanceID"})) return r;
    r.out = {{"CurrentPresetNameList", "FactoryDefaults"}};
  } else if (action == "SelectPreset") {
    if (!need({"InstanceID", "PresetName"})) return r;
    DmrCommand c;
    c.type = DmrCommand::Type::SelectPreset;
    c.preset = *find("PresetName");
    c.req_id = req_id;
    bus.publish_dmr(std::move(c));
  } else if (action == "SetVolume") {
    if (!need({"InstanceID", "DesiredVolume"})) return r;
    DmrCommand c;
    c.type = DmrCommand::Type::SetVolume;
    c.volume = atoi(find("DesiredVolume")->c_str());
    c.req_id = req_id;
    bus.publish_dmr(std::move(c));  // 写 mpv 后由 GENA 回 LastChange（R4）
  } else if (action == "GetVolume") {
    if (!need({"InstanceID"})) return r;
    r.out = {{"CurrentVolume", std::to_string(snap.volume)}};
  } else if (action == "SetMute") {
    if (!need({"InstanceID", "DesiredMute"})) return r;
    DmrCommand c;
    c.type = DmrCommand::Type::SetMute;
    auto* v = find("DesiredMute");
    c.mute = (*v == "1" || *v == "true" || *v == "yes");
    c.req_id = req_id;
    bus.publish_dmr(std::move(c));
  } else if (action == "GetMute") {
    if (!need({"InstanceID"})) return r;
    r.out = {{"CurrentMute", snap.muted ? "1" : "0"}};
  } else if (action == "GetVolumeDB") {
    if (!need({"InstanceID"})) return r;
    int db = -6000 + 60 * snap.volume / 100;  // 0%→-6000dB，100%→0dB
    r.out = {{"CurrentVolume", std::to_string(db)}};
  } else if (action == "SetVolumeDB") {
    if (!need({"InstanceID", "DesiredVolume"})) return r;
    int db = atoi(find("DesiredVolume")->c_str());
    if (db < -6000) db = -6000;
    if (db > 0) db = 0;
    DmrCommand c;
    c.type = DmrCommand::Type::SetVolume;
    c.volume = (db + 6000) * 100 / 6000;  // 两套入口一套真值（R4）
    c.req_id = req_id;
    bus.publish_dmr(std::move(c));
  } else if (action == "GetVolumeDBRange") {
    if (!need({"InstanceID"})) return r;
    r.out = {{"MinValue", "-6000"}, {"MaxValue", "0"}};
  } else if (action.rfind("Get", 0) == 0) {
    // 显示类查询（Brightness/Contrast/Sharpness/ColorTemperature/Red·Green·BlueVideoGain/
    // Horizontal·VerticalKeystone）：能给默认值绝不回错误（SPEC §1.2.7）。
    // ★d134 修两处：① 原条件带 `find("Keystone") == npos` 排除 ⇒ GetHorizontal/VerticalKeystone
    // 永远掉进末尾 401 分支（与 SPEC §1.2.7 的"10 个显示类全部可用"直接矛盾，且与 Set 侧不对称）；
    // ② 原实现"未命中就什么都不填"⇒ 返回**成功但 0 出参**，控制点会把空值当真实状态，
    // 比直接回 401 更糟。改为命中即回值、未命中回标准 401。
    if (!need({"InstanceID"})) return r;
    auto* p = rc.find(action.substr(3));
    if (!p) {
      r.ok = false;
      r.error_code = kErrInvalidAction;
      r.error_msg = "Invalid action " + action;
      return r;
    }
    r.out = {{"Current" + action.substr(3), std::to_string(*p)}};
  } else if (action.rfind("Set", 0) == 0) {
    std::string desired = "Desired" + action.substr(3);
    if (!need({"InstanceID", desired.c_str()})) return r;
    auto* p = rc.find(action.substr(3));
    if (!p) {
      r.ok = false;
      r.error_code = kErrInvalidAction;
      r.error_msg = "Invalid action " + action;
      return r;
    }
    *p = atoi(find(desired.c_str())->c_str());
    r.notify_now = {{action.substr(3), std::to_string(*p)}};  // Set 存内存回 LastChange
  } else {
    r.ok = false;
    r.error_code = kErrInvalidAction;
    r.error_msg = "Invalid action " + action;
  }
  return r;
}

// ---------------- ConnectionManager:1 ----------------

SoapResult handle_connection_manager(const std::string& action, const SoapArgs& args,
                                     const PlaybackSnapshot& snap, const Config& cfg, EventBus& bus,
                                     int req_id) {
  SoapResult r;
  (void)snap;
  (void)bus;
  (void)req_id;
  if (action == "GetProtocolInfo") {
    // 兼容性关键：缺格式 → 网盘/直播类 App 直接灰投屏按钮（SPEC §1.3.1）
    r.out = {{"Source", ""}, {"Sink", cfg.sink_protocol_info()}};
  } else if (action == "GetCurrentConnectionIDs") {
    r.out = {{"ConnectionIDs", "0"}};
  } else if (action == "GetCurrentConnectionInfo") {
    r.out = {
        {"RcsID", "0"},
        {"AVTransportID", "0"},
        {"ProtocolInfo", ""},
        {"PeerConnectionManager", ""},
        {"PeerConnectionID", "-1"},
        {"Direction", "Input"},
        {"Status", "OK"},
    };
  } else {
    r.ok = false;
    r.error_code = kErrInvalidAction;
    r.error_msg = "Invalid action " + action;
  }
  return r;
}

// ---------------- GENA LastChange ----------------

std::string build_lastchange(
    const std::string& ns, const std::vector<std::pair<std::string, std::string>>& vars) {
  std::string body;
  for (auto& [k, v] : vars) body += "<" + k + " val=\"" + xml_escape(v) + "\"/>";
  return "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/" + ns + "\"><InstanceID val=\"0\">" +
         body + "</InstanceID></Event>";
}

std::string build_lastchange(
    const std::string& ns,
    const std::vector<std::tuple<std::string, std::string, std::string>>& vars) {
  std::string body;
  for (auto& [k, v, attrs] : vars) {
    body += "<" + k + (attrs.empty() ? "" : " " + attrs) + " val=\"" + xml_escape(v) + "\"/>";
  }
  return "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/" + ns + "\"><InstanceID val=\"0\">" +
         body + "</InstanceID></Event>";
}

std::string lastchange_avt_full(const PlaybackSnapshot& snap) {
  bool has = snap.has_session;
  std::string dur = (has && !snap.is_live) ? format_hms_padded(snap.duration) : "00:00:00";
  // CurrentTrackMetaData 双重转义：这里先转义一次，build_lastchange 的 val 属性转义是第二次
  // （错一字全事件解析失败，SPEC §4）
  std::string didl = xml_escape(snap.metadata);
  std::string next_didl = xml_escape(snap.next_metadata);
  std::vector<std::pair<std::string, std::string>> vars = {
      {"TransportState", transport_state_upnp(snap.state)},
      {"TransportStatus", "OK"},
      {"TransportPlaySpeed", "1"},
      {"NumberOfTracks", has ? "1" : "0"},
      {"CurrentTrack", has ? "1" : "0"},
      {"CurrentTrackDuration", dur},
      {"CurrentMediaDuration", dur},
      {"CurrentTrackURI", uri_without_fragment(snap.uri)},
      {"CurrentTrackMetaData", didl},
      {"AVTransportURI", uri_without_fragment(snap.uri)},
      {"NextAVTransportURI", uri_without_fragment(snap.next_uri)},
      {"NextAVTransportURIMetaData", next_didl},
      {"CurrentPlayMode", "NORMAL"},
  };
  return build_lastchange("AVT/", vars);
}

std::string lastchange_rc_full(const PlaybackSnapshot& snap) {
  // Volume/Mute 必须带 channel="Master"（RCS 1.0 §2.5.3）：订阅型控制点按 channel
  // 过滤事件变量，缺属性会被当作设备不支持调音量 → 音量条不显示、不发 SetVolume
  std::vector<std::tuple<std::string, std::string, std::string>> vars = {
      {"Volume", std::to_string(snap.volume), "channel=\"Master\""},
      {"Mute", snap.muted ? "1" : "0", "channel=\"Master\""},
      // 事件里的键是**状态变量名**（不是出参名）；值用官方复数拼写（RCS:1 表 2-17）
      {"PresetNameList", "FactoryDefaults", ""},
  };
  return build_lastchange("RCS/", vars);
}

}  // namespace fr
