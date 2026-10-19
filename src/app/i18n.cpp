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
    /* OsdDecodePendingSuffix */
    {"（投屏中无法切换，下次生效）", " (can't switch while casting; next time)"},
    /* OsdLangZh        */ {"语言：简体中文", "Language: 简体中文"},
    /* OsdLangEn        */ {"语言：English", "Language: English"},
    /* OsdBootOn        */ {"开机启动 开", "Start on Boot On"},
    /* OsdBootOff       */ {"开机启动 关", "Start on Boot Off"},
    /* OsdSpeedPrefix   */ {"倍速 ", "Speed "},
    /* OsdWindowPrefix  */ {"窗口 ", "Window "},
    /* OsdZoomPrefix    */ {"画面 ", "Zoom "},
    /* OsdFitPrefix     */ {"画面模式：", "Fit mode: "},

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
    /* CtxDecodeNextCast */ {"（下次生效）", " (next cast)"},
    /* CtxWindowSize    */ {"窗口大小", "Window size"},
    /* CtxVideoZoom     */ {"画面缩放", "Video zoom"},
    /* CtxFitMode       */ {"画面填充", "Fit"},
    /* CtxLanguage      */ {"语言", "Language"},
    /* CtxStop          */ {"停止", "Stop"},
    /* CtxExit          */ {"退出", "Exit"},

    /* FitModeAuto      */ {"自动", "Auto"},
    /* FitModeCover     */ {"铺满", "Fill (crop)"},
    /* FitModeContain   */ {"完整画面", "Fit (letterbox)"},
    /* FitModeStretch   */ {"拉伸", "Stretch"},

    /* LangZhName       */ {"简体中文", "简体中文"},
    /* LangEnName       */ {"English", "English"},

    /* TrayShow         */ {"显示主窗口", "Show main window"},
    /* TrayHide         */ {"隐藏主窗口", "Hide main window"},
    /* TrayAlwaysTop    */ {"窗口置顶", "Always on top"},
    /* TrayStartOnBoot  */ {"开机启动", "Start on boot"},
    /* TrayCastScreenFmt*/ {"投到屏幕 {}（{}×{}）{}", "Cast to screen {} ({}×{}){}"},
    /* TrayPrimary      */ {"· 主屏", "· Primary"},
    /* TrayReloadSkin   */ {"重载皮肤", "Reload skins"},
    /* TrayQuit         */ {"退出", "Exit"},
    /* TrayTooltip      */ {"FlashRec 投屏接收", "FlashRec Cast Receiver"},

    /* UpdTrayCheck     */ {"检查更新", "Check for updates"},
    /* UpdTrayChecking  */ {"正在检查更新…", "Checking for updates…"},
    /* UpdTrayDownloadingFmt */ {"正在下载更新 {}%", "Downloading update {}%"},
    /* UpdTrayDownloadFmt */ {"下载更新 v{}", "Download update v{}"},
    /* UpdTrayReadyFmt  */ {"安装更新 v{}", "Install update v{}"},
    /* UpdTrayInstalling */ {"正在安装更新…", "Installing update…"},
    /* UpdTrayUpToDate  */ {"已是最新版本", "Up to date"},
    /* UpdTopBtnLabel   */ {"更新软件", "Update software"},
    /* HelpTopBtnLabel   */ {"帮助", "Help"},

    /* UpdDlgTitleAvailableFmt */ {"发现新版本 v{}", "Update v{} available"},
    /* UpdDlgTitleChecking */ {"正在检查更新", "Checking for updates"},
    /* UpdDlgTitleDownloading */ {"正在下载更新", "Downloading update"},
    /* UpdDlgTitleUpToDate */ {"已是最新版本", "You're up to date"},
    /* UpdDlgTitleSkipped */ {"已跳过此版本", "Version skipped"},
    /* UpdDlgTitleFailed */ {"更新失败", "Update failed"},
    /* UpdDlgTitleInstalling */ {"正在安装更新", "Installing update"},
    /* UpdDlgVersionFmt */ {"{} → {}", "Current {} → new {}"},
    /* UpdDlgSizeFmt    */ {"安装包 {}", "Installer {}"},
    /* UpdDlgNotesTitle */ {"更新说明", "What's new"},
    /* UpdDlgProgressFmt */ {"已下载 {}%", "{}% downloaded"},
    /* UpdDlgReadyHint  */ {"下载完成，校验通过", "Download complete and verified"},
    /* UpdDlgManualHint */ {"自动下载已关闭，可手动下载安装包。",
                           "Automatic download is off. Download the installer manually."},
    /* UpdDlgNoAssetHint */ {"当前运行形态没有可用安装包，请前往发布页。",
                            "No installer is available for this build. Open the release page."},
    /* UpdDlgPortableHint */ {"便携版：安装包已下载，打开文件夹后手动替换。",
                             "Portable build: open the folder to replace the installer."},
    /* UpdDlgDebHint    */ {"系统包：安装包已下载，请用软件中心或文件管理器安装。",
                           "System package: install the downloaded file via software center."},
    /* UpdDlgDevHint    */ {"开发构建不提供自动安装，请前往发布页。",
                           "Development builds cannot self-install. Open the release page."},
    /* UpdDlgCastWarning */ {"更新会中断当前投屏，请确认后再安装。",
                            "Updating interrupts the current cast. Confirm to continue."},
    /* UpdDlgInstallingHint */ {"正在退出并安装，请稍候…", "Exiting to install. Please wait…"},
    /* UpdDlgErrorFmt   */ {"{}，请稍后重试。", "{} Try again later."},
    /* UpdBtnInstall    */ {"立即安装", "Install now"},
    /* UpdBtnInstallRestart */ {"立即更新并重启", "Update and restart"},
    /* UpdBtnInstallAnyway */ {"仍要安装", "Install anyway"},
    /* UpdBtnDownload   */ {"下载更新", "Download update"},
    /* UpdBtnLater      */ {"稍后", "Later"},
    /* UpdBtnSkip       */ {"跳过此版本", "Skip this version"},
    /* UpdBtnRetry      */ {"重试", "Retry"},
    /* UpdBtnOpenFolder */ {"打开下载文件夹", "Open download folder"},
    /* UpdBtnOpenFolderDeb */ {"打开所在文件夹", "Open containing folder"},
    /* UpdBtnOpenPage   */ {"打开发布页", "Open release page"},
    /* UpdBtnClose      */ {"关闭", "Close"},
    /* UpdErrNetwork    */ {"网络不可用", "Network unavailable"},
    /* UpdErrVerify     */ {"安装包校验失败", "Installer verification failed"},
    /* UpdErrDisk       */ {"磁盘空间或写入失败", "Disk error"},
    /* UpdErrPermission */ {"没有写入权限", "Permission denied"},
    /* UpdErrInstall    */ {"安装程序启动失败", "Failed to start the installer"},
    /* UpdErrUnknown    */ {"未知错误", "Unknown error"},

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
