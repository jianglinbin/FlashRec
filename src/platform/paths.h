#pragma once
// 路径与系统资源定位（平台差异，AGENTS.md 规则 5：只允许本目录出现平台宏）。
#include <string>
#include <vector>

namespace fr::paths {

// %APPDATA%\FlashRec / ~/.local/share/FlashRec
std::string app_data_dir();
std::string log_dir();       // app_data_dir()/logs（SPEC §6）
std::string config_file();   // app_data_dir()/settings.json
// v0.6.4 自动更新下载缓存：%LOCALAPPDATA%\FlashRec\updates 或 XDG_CACHE_HOME/FlashRec/updates
std::string update_cache_dir();
// 路径所在卷的可用字节数；探测失败返回 -1（更新模块按“未知”放宽处理）。
long long free_disk_bytes(const std::string& path);
// 可执行文件旁的 assets/；开发树回退到源码根 assets/
std::string asset_dir();
std::string asset_file(const std::string& rel);
// 系统中文字体（无则返回空串，UI 退化为默认字体）
std::string find_cjk_font();
// 系统界面语言（v0.6.2 d362 i18n auto 探测；返回 "zh-CN" / "en-US"——
// 非中文环境一律保守回落英文，与 UI_I18N_PLAN.md §2.2 一致）
std::string system_ui_language();
// 设置进程环境变量（平台差异收口，供三方库经 getenv 读取配置）
void set_env(const std::string& name, const std::string& value);
// 运行时一次探测所有资源路径
void init();
std::vector<std::string> search_dirs();  // assets 搜索路径（调试用）

}  // namespace fr::paths
