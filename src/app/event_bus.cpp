#include "app/event_bus.h"

namespace fr {

const char* transport_state_name(TransportState s) {
  switch (s) {
    case TransportState::Idle: return "IDLE";
    case TransportState::Ready: return "READY";
    case TransportState::Transitioning: return "TRANSITIONING";
    case TransportState::Playing: return "PLAYING";
    case TransportState::PausedPlayback: return "PAUSED_PLAYBACK";
    case TransportState::Stopped: return "STOPPED";
  }
  return "IDLE";
}

const char* transport_state_upnp(TransportState s) {
  // GetTransportInfo 的合法值（错值会让 App 按钮卡死，SPEC §1.1.7）
  switch (s) {
    case TransportState::Idle: return "NO_MEDIA_PRESENT";
    case TransportState::Ready: return "STOPPED";
    case TransportState::Transitioning: return "TRANSITIONING";
    case TransportState::Playing: return "PLAYING";
    case TransportState::PausedPlayback: return "PAUSED_PLAYBACK";
    case TransportState::Stopped: return "STOPPED";
  }
  return "NO_MEDIA_PRESENT";
}

}  // namespace fr
