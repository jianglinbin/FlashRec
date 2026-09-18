#include "dmr/gena.h"

#include "app/log.h"
#include "app/str_util.h"
#include "dmr/soap_util.h"

namespace fr {

const char* GenaSender::kAvtSid = "urn:upnp-org:serviceId:AVTransport";
const char* GenaSender::kRcsSid = "urn:upnp-org:serviceId:RenderingControl";

void GenaSender::send(const char* service_id, const std::string& raw, const char* tag) {
  if (!send_) return;
  // GENA propertyset 的 <val> 是原样内嵌（pupnp 不转义）→ LastChange 必须 XML 转义一次
  send_(service_id, xml_escape(raw));
  FR_LOG_DEBUG("[GENA] NOTIFY {} seq-refresh len={}", tag, raw.size());
}

void GenaSender::send_avt_full(const PlaybackSnapshot& s) {
  send(kAvtSid, lastchange_avt_full(s), "AVT-full");
  last_state_ = transport_state_upnp(s.state);
  last_media_ = uri_without_fragment(s.uri) + "|" + std::to_string(s.duration) + "|" + s.title;
  last_rc_ = std::to_string(s.volume) + "/" + (s.muted ? "1" : "0");
}

void GenaSender::send_rc_full(const PlaybackSnapshot& s) {
  send(kRcsSid, lastchange_rc_full(s), "RCS-full");
  last_rc_ = std::to_string(s.volume) + "/" + (s.muted ? "1" : "0");
}

void GenaSender::state_changed(const PlaybackSnapshot& s) {
  const char* st = transport_state_upnp(s.state);
  if (st == last_state_) return;  // 差量判重：状态类立即发，但不重发相同值
  last_state_ = st;
  send(kAvtSid, build_lastchange("AVT/", {{"TransportState", st}}), "AVT-state");
}

void GenaSender::media_changed(const PlaybackSnapshot& s) {
  std::string key = uri_without_fragment(s.uri) + "|" + std::to_string(s.duration) + "|" +
                    s.title + "|" + uri_without_fragment(s.next_uri);
  if (key == last_media_) return;
  last_media_ = key;
  std::string dur = (s.has_session && !s.is_live) ? format_hms_padded(s.duration) : "00:00:00";
  send(kAvtSid, build_lastchange("AVT/",
                                 {
                                     {"NumberOfTracks", s.has_session ? "1" : "0"},
                                     {"CurrentTrack", s.has_session ? "1" : "0"},
                                     {"CurrentTrackDuration", dur},
                                     {"CurrentMediaDuration", dur},
                                     {"CurrentTrackURI", uri_without_fragment(s.uri)},
                                     {"AVTransportURI", uri_without_fragment(s.uri)},
                                     {"CurrentTrackMetaData", xml_escape(s.metadata)},  // LastChange 内层单转义（此处外层由 send 再转义一次 = 双重）
                                     {"NextAVTransportURI", uri_without_fragment(s.next_uri)},
                                 }),
       "AVT-media");
}

void GenaSender::volume_changed(const PlaybackSnapshot& s) {
  std::string key = std::to_string(s.volume) + "/" + (s.muted ? "1" : "0");
  if (key == last_rc_) return;
  last_rc_ = key;
  // Volume/Mute 必须带 channel="Master"（与 lastchange_rc_full 同因，RCS 1.0 §2.5.3）
  send(kRcsSid, build_lastchange("RCS/",
                                 {
                                     {"Volume", std::to_string(s.volume), "channel=\"Master\""},
                                     {"Mute", s.muted ? "1" : "0", "channel=\"Master\""},
                                 }),
       "RCS-vol");
}

void GenaSender::rc_display(const std::vector<std::pair<std::string, std::string>>& vars) {
  send(kRcsSid, build_lastchange("RCS/", vars), "RCS-display");
}

void GenaSender::clear_cache() {
  last_state_.clear();
  last_media_.clear();
  last_rc_.clear();
}

}  // namespace fr
