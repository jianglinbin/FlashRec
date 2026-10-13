// UI 文案语言表与查询（v0.6.2 d362；接口见 i18n.h）。
// 表列序 = Lang 枚举序（[0]=zh-CN，[1]=en-US）；行序必须与 StrId 枚举一一对应
//（错位不报错但显示错串 —— i18n_check.py 做「枚举成员 == 表行」的结构化对齐）。
#include "app/i18n.h"

#include <atomic>
#include <cstring>

namespace fr {
namespace i18n {

namespace {
std::atomic<int> g_lang{(int)Lang::EnUs};

// 行序对应 StrId 枚举声明序（含 Count 哨兵位，恒为 nullptr）。
const char* const kTable[(int)StrId::Count][2] = {
    /* OsdPlay          */ {"播放", "Play"},
    /* OsdPause         */ {"暂停", "Pause"},
    /* OsdFullscreen    */ {"全屏", "Fullscreen"},
    /* OsdExitFullscreen*/ {"退出全屏", "Exit Fullscreen"},
    /* OsdPip           */ {"画中画", "Picture-in-Picture"},
    /* OsdExitPip       */ {"退出画中画", "Exit Picture-in-Picture"},
    /* OsdStop          */ {"停止", "Stop"},
    /* OsdTopOn         */ {"窗口置顶 开", "Always on Top On"},
    /* OsdTopOff        */ {"窗口置顶 关", "Always on Top Off"},
    /* OsdDecodeAuto    */ {"解码 · 自动", "Decode · Auto"},
    /* OsdDecodeSw      */ {"解码 · 软解", "Decode · Software"},
    /* OsdDecodeNvdec   */ {"解码 · NVDEC", "Decode · NVDEC"},
    /* OsdDecodeDxva2   */ {"解码 · DXVA2", "Decode · DXVA2"},
    /* OsdLangZh        */ {"语言：简体中文", "Language: 简体中文"},
    /* OsdLangEn        */ {"语言：English", "Language: English"},
    /* OsdSpeedPrefix   */ {"倍速 ", "Speed "},
    /* OsdWindowPrefix  */ {"窗口 ", "Window "},
    /* OsdZoomPrefix    */ {"画面 ", "Zoom "},

    /* HelpTitle        */ {"快捷键（F1 关闭）", "Shortcuts (F1 to close)"},
    /* HelpPlayPause    */ {"播放 / 暂停", "Play / Pause"},
    /* HelpVolume       */ {"音量 + / -", "Volume + / -"},
    /* HelpMute         */ {"静音切换", "Toggle mute"},
    /* HelpSeek         */ {"进度（按住拖动）", "Seek (press and drag)"},
    /* HelpFullscreen   */ {"全屏切换", "Toggle fullscreen"},
    /* HelpExitFsPip    */ {"退出全屏 / 画中画", "Exit fullscreen / PiP"},
    /* HelpAlwaysTop    */ {"窗口置顶", "Always on top"},
    /* HelpWindowSize   */ {"窗口大小 50–500%（七档）", "Window size 50–500% (7 steps)"},
    /* HelpZoom         */ {"画面缩放（步进 10%）", "Video zoom (10% steps)"},
    /* HelpPan          */ {"画面平移（放大后拖看）", "Video pan (drag when zoomed)"},
    /* HelpZoomReset    */ {"画面缩放复位 100%", "Reset video zoom to 100%"},
    /* HelpPanel        */ {"帮助面板", "Help panel"},
    /* HelpKeysWheelVol */ {"↑ / ↓ / 滚轮", "↑ / ↓ / Wheel"},
    /* HelpKeysMute     */ {"鼠标中键", "Middle click"},
    /* HelpKeysFs       */ {"双击 / Ctrl+Enter", "Double-click / Ctrl+Enter"},
    /* HelpKeysTop      */ {"Ctrl+T / 右键菜单", "Ctrl+T / Context menu"},
    /* HelpKeysZoom     */ {"Ctrl+↑↓ / Ctrl+滚轮", "Ctrl+↑↓ / Ctrl+Wheel"},
    /* HelpKeysPan      */ {"Ctrl+左键拖拽", "Ctrl+Left drag"},
    /* HelpKeysReset    */ {"Ctrl+中键", "Ctrl+Middle click"},

    /* CtxPlayPause     */ {"播放 / 暂停", "Play / Pause"},
    /* CtxPrev          */ {"上一个", "Previous"},
    /* CtxNext          */ {"下一个", "Next"},
    /* CtxFullscreen    */ {"全屏", "Fullscreen"},
    /* CtxExitFullscreen*/ {"退出全屏", "Exit Fullscreen"},
    /* CtxPip           */ {"画中画", "Picture-in-Picture"},
    /* CtxSkin          */ {"皮肤", "Skins"},
    /* CtxMediaInfo     */ {"媒体信息（完整）", "Media info (full)"},
    /* CtxCastAutoFs    */ {"投屏自动全屏", "Auto fullscreen on cast"},
    /* CtxAlwaysTop     */ {"窗口置顶", "Always on top"},
    /* CtxDecode        */ {"解码", "Decoding"},
    /* CtxDecodeSw      */ {"软解", "Software"},
    /* CtxDecodeAuto    */ {"硬解 · 自动", "Hardware · Auto"},
    /* CtxDecodeNvdec   */ {"硬解 · NVDEC", "Hardware · NVDEC"},
    /* CtxDecodeDxva2   */ {"硬解 · DXVA2", "Hardware · DXVA2"},
    /* CtxWindowSize    */ {"窗口大小", "Window size"},
    /* CtxLanguage      */ {"语言", "Language"},
    /* CtxStop          */ {"停止", "Stop"},
    /* CtxExit          */ {"退出", "Exit"},

    /* LangZhName       */ {"简体中文", "简体中文"},
    /* LangEnName       */ {"English", "English"},

    /* TrayShow         */ {"显示主窗口", "Show main window"},
    /* TrayHide         */ {"隐藏主窗口", "Hide main window"},
    /* TrayPlay         */ {"播放", "Play"},
    /* TrayPause        */ {"暂停", "Pause"},
    /* TrayNext         */ {"下一集", "Next"},
    /* TrayAlwaysTop    */ {"窗口置顶", "Always on top"},
    /* TrayCastScreenFmt*/ {"投到屏幕 {}（{}×{}）{}", "Cast to screen {} ({}×{}){}"},
    /* TrayPrimary      */ {"· 主屏", "· Primary"},
    /* TrayReloadSkin   */ {"重载皮肤", "Reload skins"},
    /* TrayQuit         */ {"退出", "Exit"},
    /* TrayTooltip      */ {"FlashRec 投屏接收", "FlashRec Cast Receiver"},

    /* EmptyBadge       */ {"投屏接收端", "Cast receiver"},
    /* EmptyHint        */ {"等待投屏 · 在播放器的投屏设备中选择本机",
                          "Waiting for cast · Select this PC in your player's cast devices"},
    /* EmptyWaiting     */ {"等待投屏", "Waiting for cast"},

    /* BadgeAirplay     */ {"AirPlay 投屏", "AirPlay Cast"},
    /* BadgeDlna        */ {"DLNA 投屏", "DLNA Cast"},
    /* BadgeSwDecode    */ {"软解", "Software"},
    /* BadgeHwFmt       */ {"硬解 {}", "Hardware {}"},
    /* BadgeSwRender    */ {"软件渲染", "Software rendering"},
    /* BadgeVideoFpsFmt */ {"视频 {} fps", "Video {} fps"},
    /* BadgeVideoFpsNone*/ {"视频 --", "Video --"},

    /* InfoResolution   */ {"分辨率 ", "Resolution "},
    /* InfoVideoCodec   */ {" · 编码 ", " · Codec "},
    /* InfoBitrate      */ {" · 码率 ", " · Bitrate "},
    /* InfoBitrateNone  */ {" · 码率 -", " · Bitrate -"},
    /* InfoSwDecode     */ {" · 软解", " · Software"},
    /* InfoHwDecode     */ {" · 硬解 ", " · Hardware "},
    /* InfoAudio        */ {"音频编码 ", "Audio codec "},
    /* InfoSampleRate   */ {" · 采样率 ", " · Sample rate "},
    /* InfoChannels     */ {" · 声道数 ", " · Channels "},

    /* PromptDismiss    */ {"忽略", "Ignore"},
};

int col() { return g_lang.load(std::memory_order_relaxed); }

}  // namespace

void init(const std::string& cfg_value) {
  // 白名单映射；"auto" 不应到这里（main 已解析），仍兜底按未知值处理。
  if (cfg_value == "zh-CN")
    g_lang.store((int)Lang::ZhCn, std::memory_order_relaxed);
  else
    g_lang.store((int)Lang::EnUs, std::memory_order_relaxed);
}

Lang lang() { return (Lang)col(); }

void set_lang(Lang l) { g_lang.store((int)l, std::memory_order_relaxed); }

const char* tr(StrId id) {
  const int i = (int)id;
  if (i < 0 || i >= (int)StrId::Count) return "";
  return kTable[i][col()];
}

std::string trf(StrId id, std::initializer_list<const char*> args) {
  std::string out = tr(id);
  for (const char* a : args) {
    const size_t p = out.find("{}");
    if (p == std::string::npos) break;
    out.replace(p, 2, a ? a : "");
  }
  return out;
}

}  // namespace i18n
}  // namespace fr
