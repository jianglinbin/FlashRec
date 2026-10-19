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
// 其余形态：打开所在目录（便携/deb 的人工交接）。
// 返回 true = 已成功交接，调用方应尽快退出；false = 启动失败，留在应用内提示。
bool launch_update_installer(InstallShape shape, const std::string& asset_path);

}  // namespace fr::platform
