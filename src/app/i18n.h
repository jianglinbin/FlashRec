#pragma once
// UI 界面文案多语言（v0.6.2 d362；UI_I18N_PLAN.md）。
// 范围 = 界面可见文案（OSD / 右键菜单 / 托盘菜单 / 待机引导 / 帮助面板）；
// 日志、协议报文（DMR/AirPlay）、设备名（dlna.friendly_name）不属于界面文案，不走本模块。
//
// 设计要点：
//   - 编译期内置语言表（zh-CN + en-US），不引运行时 JSON 加载 —— 文案量小、
//     改动走版本发布；外置社区翻译包留待 v2（规划 §5 拍板项 4）。
//   - StrId 枚举集中定义全部 key：漏译 = i18n_check.py 拦截（键位与枚举全集对齐）。
//   - 语言值存原子量：渲染线程 tr() 读、主线程 set_lang 写，无锁。
//   - 禁止拼接式翻译：整句/带占位（trf 的 "{}" 顺序替换）表达语序差异；
//     仅「前缀 + 数字」类双语语序一致的场合允许前缀拼接（如 "倍速 "/"Speed "）。
//   - 平台探测不在这里：system_ui_language() 收口在 platform/paths（规则 5），
//     main 启动时把 "auto" 解析成具体语言再进 init()，本模块保持无 OS 依赖。
#include <atomic>
#include <initializer_list>
#include <string>

namespace fr {
namespace i18n {

enum class Lang : int { ZhCn = 0, EnUs = 1 };

// 全部界面文案 key（新增文案 = 枚举加一项 + 两张表各加一行，i18n_check.py 校验）
enum class StrId : int {
  // —— OSD 反馈（chrome.cpp kind3 id 表）——
  OsdPlay,            // 播放
  OsdPause,           // 暂停
  OsdFullscreen,      // 全屏
  OsdExitFullscreen,  // 退出全屏
  OsdPip,             // 画中画
  OsdExitPip,         // 退出画中画
  OsdStop,            // 停止
  OsdTopOn,           // 窗口置顶 开
  OsdTopOff,          // 窗口置顶 关
  OsdDecodeAuto,      // 解码 · 自动
  OsdDecodeSw,        // 解码 · 软解
  OsdDecodeNvdec,     // 解码 · NVDEC
  OsdDecodeDxva2,     // 解码 · DXVA2
  OsdDecodePendingSuffix,  // d432：投屏中无法切换，下次生效（OSD id16–19 后缀）
  OsdLangZh,          // 语言：简体中文（id14）
  OsdLangEn,          // 语言：English（id15）
  OsdBootOn,          // d437：开机启动 开（id20）
  OsdBootOff,         // d437：开机启动 关（id21）
  OsdSpeedPrefix,     // 前缀："倍速 " / "Speed "（后接 format_speed）
  OsdWindowPrefix,    // 前缀："窗口 " / "Window "（后接 N%）
  OsdZoomPrefix,      // 前缀："画面 " / "Zoom "（后接 N%）
  OsdFitPrefix,       // d426：前缀："画面模式：" / "Fit mode: "（后接 FitMode* 模式名）

  // —— F1 帮助面板（chrome.cpp draw_help；d472 起标题由 软件名+版本-帮助面板 拼装）——
  HelpPlayPause,    // 播放 / 暂停
  HelpVolume,       // 音量 + / -
  HelpMute,         // 静音切换
  HelpSeek,         // 进度（按住拖动）
  HelpFullscreen,   // 全屏切换
  HelpExitFsPip,    // 退出全屏 / 画中画
  HelpAlwaysTop,    // 窗口置顶
  HelpWindowSize,   // 窗口大小 50–500%（七档）
  HelpZoom,         // 画面缩放（步进 10%）
  HelpPan,          // 画面平移（放大后拖看）
  HelpZoomReset,    // 画面缩放复位 100%
  HelpPanel,        // 帮助面板
  // 键位列里带中文词的条目（纯符号键位如 "Space"/"F1" 留字面量即可）
  HelpKeysWheelVol,  // ↑ / ↓ / 滚轮
  HelpKeysMute,      // 鼠标中键
  HelpKeysFs,        // 双击 / Ctrl+Enter
  HelpKeysTop,       // Ctrl+T / 右键菜单
  HelpKeysZoom,      // Ctrl+↑↓ / Ctrl+滚轮
  HelpKeysPan,       // Ctrl+左键拖拽
  HelpKeysReset,     // Ctrl+中键

  // —— 右键菜单（render_loop.cpp 每帧重建）——
  CtxPlayPause,     // 播放 / 暂停
  CtxPrev,          // 上一个
  CtxNext,          // 下一个
  CtxFullscreen,    // 全屏
  CtxExitFullscreen,  // 退出全屏
  CtxPip,           // 画中画
  CtxSkin,          // 皮肤
  CtxMediaInfo,     // 媒体信息（完整）
  CtxCastAutoFs,    // 投屏自动全屏
  CtxAlwaysTop,     // 窗口置顶
  CtxDecode,        // 解码（子菜单父条目）
  CtxDecodeSw,      // 软解
  CtxDecodeAuto,    // 硬解 · 自动
  CtxDecodeNvdec,   // 硬解 · NVDEC
  CtxDecodeDxva2,   // 硬解 · DXVA2
  CtxDecodeNextCast,  // d432：（下次生效）—— AirPlay 会话挂起档后缀
  CtxWindowSize,    // 窗口大小（子菜单父条目；子条目为纯数字档位不翻译）
  CtxVideoZoom,     // 画面缩放（子菜单父条目；子条目为纯数字档位不翻译）
  CtxFitMode,       // d426：画面填充（子菜单父条目；子条目 = FitMode* 模式名）
  CtxLanguage,      // 语言（子菜单父条目）
  CtxStop,          // 停止
  CtxExit,          // 退出

  // —— 画面填充模式名（d426；右键「画面填充」子菜单条目 + OSD kind7 共用）——
  FitModeAuto,      // 自动
  FitModeCover,     // 铺满
  FitModeContain,   // 完整画面
  FitModeStretch,   // 拉伸

  // —— 语言名（语言子菜单/托盘两处共用；语言名按惯例恒用本名，不随界面语言变）——
  LangZhName,       // 简体中文
  LangEnName,       // English

  // —— 托盘菜单（main.cpp menu_provider；每次打开重建）——
  TrayShow,           // 显示主窗口
  TrayHide,           // 隐藏主窗口
  TrayAlwaysTop,      // 窗口置顶
  TrayStartOnBoot,    // d437：开机启动（勾选；真值 = 系统配置）
  TrayCastScreenFmt,  // "投到屏幕 {}（{}×{}）{}" / "Cast to screen {} ({}×{}){}"
  TrayPrimary,        // "· 主屏" / "· Primary"
  TrayReloadSkin,     // 重载皮肤
  TrayQuit,           // 退出
  TrayTooltip,        // 托盘悬停提示（FlashRec 投屏接收 / FlashRec Cast Receiver）

  // —— 自动更新 · 托盘入口（v0.6.4 d459；标签随 UpdateService 快照实时变化）——
  UpdTrayCheck,           // 检查更新
  UpdTrayChecking,        // 正在检查更新…（禁用）
  UpdTrayDownloadingFmt,  // "正在下载更新 {}%"（后接进度；禁用）
  UpdTrayDownloadFmt,     // "下载更新 v{}"（ManualOnly 待确认下载）
  UpdTrayReadyFmt,        // "安装更新 v{}"（下载校验完成）
  UpdTrayInstalling,      // 正在安装更新…（禁用）
  UpdTrayUpToDate,        // 已是最新版本（本次手动检查后短暂显示）
  UpdTopBtnLabel,         // v0.6.4 d464：顶栏更新按钮悬停文字（更新软件）
  HelpTopBtnLabel,        // v0.6.4 d471：顶栏帮助按钮悬停文字（帮助）

  // —— 自动更新 · 弹窗（v0.6.4 d460；ui/views/update_dialog.cpp）——
  UpdDlgTitleAvailableFmt,  // "发现新版本 v{}" / "Update v{} available"
  UpdDlgTitleChecking,      // 正在检查更新
  UpdDlgTitleDownloading,   // 正在下载更新
  UpdDlgTitleUpToDate,      // 已是最新版本（弹窗标题）
  UpdDlgTitleSkipped,       // 已跳过此版本
  UpdDlgTitleFailed,        // 更新失败
  UpdDlgTitleInstalling,    // 正在安装更新
  UpdDlgVersionFmt,         // "{} → {}"（当前 → 新）
  UpdDlgSizeFmt,            // "安装包 {}"
  UpdDlgNotesTitle,         // 更新说明
  UpdDlgProgressFmt,        // "已下载 {}%"
  UpdDlgReadyHint,          // 下载完成，校验通过
  UpdDlgManualHint,         // 自动下载已关闭，可手动下载
  UpdDlgNoAssetHint,        // 当前形态无安装包，前往发布页
  UpdDlgPortableHint,       // 便携版：打开文件夹手动替换
  UpdDlgDebHint,            // 系统包：用软件中心/文件管理器安装
  UpdDlgDevHint,            // 开发构建不提供自动安装
  UpdDlgCastWarning,        // 更新会中断当前投屏
  UpdDlgInstallingHint,     // 正在退出并安装
  UpdDlgErrorFmt,           // "{}，请稍后重试。"（占位 = 错误类别）
  UpdBtnInstall,            // 立即安装
  UpdBtnInstallRestart,     // 立即更新并重启（AppImage）
  UpdBtnInstallAnyway,      // 仍要安装（投屏二次确认后）
  UpdBtnDownload,           // 下载更新
  UpdBtnLater,              // 稍后
  UpdBtnSkip,               // 跳过此版本
  UpdBtnRetry,              // 重试
  UpdBtnOpenFolder,         // 打开下载文件夹（便携 ZIP）
  UpdBtnOpenFolderDeb,      // 打开所在文件夹（deb）
  UpdBtnOpenPage,           // 打开发布页（开发构建/无资产）
  UpdBtnClose,              // 关闭
  UpdErrNetwork,            // 网络不可用
  UpdErrVerify,             // 安装包校验失败
  UpdErrDisk,               // 磁盘空间或写入失败
  UpdErrPermission,         // 没有写入权限
  UpdErrInstall,            // 安装程序启动失败
  UpdErrUnknown,            // 未知错误

  // —— 待机（empty_state.cpp）——
  EmptyBadge,    // 投屏接收端（顶栏徽标）
  EmptyHint,     // 等待投屏 · 在播放器的投屏设备中选择本机
  EmptyWaiting,  // 等待投屏（画中画小窗）

  // —— 徽章（player_view.cpp 顶栏常驻；i18n_check 扫描补全，d362）——
  BadgeAirplay,     // AirPlay 投屏
  BadgeDlna,        // DLNA 投屏
  BadgeSwDecode,    // 软解（徽章）
  BadgeHwFmt,       // "硬解 {}"（后接后端名，如 d3d11va）
  BadgeSwRender,    // 软件渲染（兜底提示徽章）
  BadgeVideoFpsFmt, // "视频 {} fps"（后接一位小数值）
  BadgeVideoFpsNone,// 视频 --

  // —— 媒体信息面板（render_loop.cpp；前缀拼接，双语语序一致）——
  InfoResolution,   // "分辨率 " / "Resolution "
  InfoVideoCodec,   // " · 编码 " / " · Codec "
  InfoBitrate,      // " · 码率 " / " · Bitrate "（视频/音频共用）
  InfoBitrateNone,  // " · 码率 -" / " · Bitrate -"
  InfoSwDecode,     // " · 软解" / " · Software"
  InfoHwDecode,     // " · 硬解 " / " · Hardware "（后接后端名）
  InfoAudio,        // "音频编码 " / "Audio codec "
  InfoSampleRate,   // " · 采样率 " / " · Sample rate "
  InfoChannels,     // " · 声道数 " / " · Channels "

  // —— 剪贴板提示条按钮（render_loop.cpp）——
  PromptDismiss,    // 忽略（播放按钮复用 OsdPlay 同文）

  Count,  // 哨兵：表行数 = 此值，勿在下方加项
};

// 启动初始化。cfg_value = settings.json 的 ui.language（"auto"|"zh-CN"|"en-US"）。
// 传入的 auto 应已由 main 经 paths::system_ui_language() 解析；此处仅做白名单
// 映射（未知值 → 英文保守回落，与规划 §2.2 一致）。
void init(const std::string& cfg_value);

Lang lang();
void set_lang(Lang l);

// 取文案（静态表内字符串，生命周期恒有效，无需释放）。
const char* tr(StrId id);

// 取文案并把按序出现的 "{}" 依次替换为 args（占位数不符时多余占位留原样）。
// 仅用于带占位的整句（如托盘「投到屏幕 {}（{}×{}）{}」）。
std::string trf(StrId id, std::initializer_list<const char*> args);

}  // namespace i18n
}  // namespace fr
