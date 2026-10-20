#pragma once
// 运行形态识别（平台差异收口处）：决定更新时选择哪种资产与安装交接方式。
#include <string>

namespace fr::platform {

enum class InstallShape : int {
  Unknown = 0,     // 开发树/未知：只打开发布页
  WindowsSetup,    // Inno 安装版：弹出安装向导 + 重启
  WindowsPortable, // 便携 ZIP：打开下载目录手动覆盖
  LinuxAppImage,   // APPIMAGE：自替换 + 重启助手
  LinuxDeb,        // /usr 下的 deb 安装：交给软件中心/资源管理器
};

InstallShape detect_install_shape();
const char* install_shape_name(InstallShape shape);

// —— v0.6.4 d460：把 URL / 文件所在目录交给系统（主线程按钮动作，不阻塞等待）——
// 返回 false = 系统调用本身失败（无关联程序/无权限）；调用方只记日志。
bool open_url(const std::string& url);
bool open_containing_folder(const std::string& file_path);

// —— v0.6.4 d460：启动已校验安装包的安装/交接动作 ——
// WindowsSetup：ShellExecuteW 启动 Inno 交互式安装向导（/RELAUNCH）；
// LinuxAppImage：拉起重启助手，等待本进程退出后自替换并重启；
// WindowsPortable：★d495 自动换装（可写/助手/.tar 探测通过时）——拉起随包的
//   flashrec_swap.exe 分离进程，等本进程退出 → 备份 → 解压 → 起新版 → 探活，
//   失败自动还原；条件不满足 → 回退打开所在目录（人工交接）。
// LinuxDeb：★d496 polkit 提权自动安装（d473 决议落地）——detached shell 助手
//   （d461 同款，无独立二进制，平台分治 d495）：等本进程退出 → 复核哈希 →
//   pkexec apt install（授权框系统弹出，密码不经过我方进程）→ 成功拉起新版；
//   不支持/失败 → 打开所在目录。
// 返回 true = 已成功交接，调用方应尽快退出；false = 启动失败，留在应用内提示。
bool launch_update_installer(InstallShape shape, const std::string& asset_path,
                             const std::string& expected_sha256);

// —— v0.6.5 d495：Windows 便携版自动换装（d474 决议落地，独立进程实现）——
// 换装支持探测：exe 目录可写 + 随包助手存在 + 系统 tar.exe（三项齐备才支持）。
// 非 Windows 恒 false（Linux 无文件锁：AppImage=shell 助手 / deb=pkexec，平台分治）。
bool portable_swap_supported();
// 便携换装执行：分离进程拉起 flashrec_swap.exe。调用方（main）须已完成 zip 哈希
// 复核（main→update 合法方向）。返回 true = 助手已启动，调用方应退出应用。
bool launch_portable_swap(const std::string& asset_path);

// —— v0.6.5 d496：Linux deb polkit 提权自动安装（d473 决议落地）——
// 支持探测：pkexec 在位 + polkitd 运行中 + 图形会话（DISPLAY/WAYLAND_DISPLAY）。
// 三项缺一 = false（无桌面/SSH 会话回退「打开文件夹」人工路径）。非 Linux 恒 false。
bool deb_polkit_supported();

}  // namespace fr::platform
