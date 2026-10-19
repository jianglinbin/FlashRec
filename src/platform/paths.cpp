#include "platform/paths.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <vector>

namespace fr::paths {

namespace fs = std::filesystem;

static std::vector<std::string> g_assets;  // assets 搜索目录

#ifdef _WIN32
// CMake 已全局 /D 这两个宏；加卫语句避免 C4005 宏重定义警告（tray_win32 同款）。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

static std::string exe_dir() {
  char buf[MAX_PATH] = {0};
  GetModuleFileNameA(nullptr, buf, MAX_PATH);
  std::string p = buf;
  size_t pos = p.find_last_of("\\/");
  return pos == std::string::npos ? "." : p.substr(0, pos);
}

std::string app_data_dir() {
  char* env = nullptr;
  size_t len = 0;
  _dupenv_s(&env, &len, "APPDATA");
  std::string base = env ? std::string(env) : std::string(".");
  free(env);
  std::replace(base.begin(), base.end(), '\\', '/');
  return base + "/FlashRec";
}

std::string find_cjk_font() {
  char* windir = nullptr;
  size_t len = 0;
  _dupenv_s(&windir, &len, "WINDIR");
  std::string w = windir ? windir : "C:\\Windows";
  free(windir);
  const char* candidates[] = {
      "\\Fonts\\msyh.ttc",  "\\Fonts\\msyh.ttf",  "\\Fonts\\msyhbd.ttc",
      "\\Fonts\\simhei.ttf", "\\Fonts\\simsun.ttc", "\\Fonts\\simhei.ttf",
  };
  for (auto* rel : candidates) {
    std::string p = w + rel;
    if (fs::exists(p)) return p;
  }
  return {};
}

std::string system_ui_language() {
  // v0.6.2 d362：i18n「auto」探测 —— 跟随系统 UI 语言（≠ 区域格式设置）。
  // 主语言码判定：中文（含繁体/各变体）→ zh-CN（UI 文案本就只有简中一张表），
  // 其余一律 en-US 保守回落。i18n::init 只认这两个值。
  LANGID lid = GetUserDefaultUILanguage();
  return PRIMARYLANGID(lid) == LANG_CHINESE ? "zh-CN" : "en-US";
}
#else
#include <unistd.h>
#include <limits.h>
#include <sys/statvfs.h>

static std::string exe_dir() {
  char buf[PATH_MAX] = {0};
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return ".";
  buf[n] = 0;
  std::string p = buf;
  size_t pos = p.find_last_of('/');
  return pos == std::string::npos ? "." : p.substr(0, pos);
}

std::string app_data_dir() {
  const char* xdg = getenv("XDG_DATA_HOME");
  if (xdg && *xdg) return std::string(xdg) + "/FlashRec";
  const char* home = getenv("HOME");
  return std::string(home && *home ? home : ".") + "/.local/share/FlashRec";
}

std::string find_cjk_font() {
  const char* candidates[] = {
      "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
      "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
      "/usr/share/fonts/noto-cjk/NotoSansCJKsc-Regular.otf",
      "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
      "/usr/share/fonts/wenquanyi-microhei/wqy-microhei.ttc",
  };
  for (auto* p : candidates)
    if (fs::exists(p)) return p;
  return {};
}

std::string system_ui_language() {
  // v0.6.2 d362：i18n「auto」探测 —— LC_ALL > LC_MESSAGES > LANG 依次取，
  // "zh" 前缀 → zh-CN，其余（含探测不到）→ en-US 保守回落。
  for (const char* var : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
    const char* v = getenv(var);
    if (v && *v) return std::string(v).rfind("zh", 0) == 0 ? "zh-CN" : "en-US";
  }
  return "en-US";
}
#endif

std::string log_dir() { return app_data_dir() + "/logs"; }
std::string config_file() { return app_data_dir() + "/settings.json"; }

std::string update_cache_dir() {
#ifdef _WIN32
  char* env = nullptr;
  size_t len = 0;
  _dupenv_s(&env, &len, "LOCALAPPDATA");
  const bool has_local = env != nullptr && env[0] != '\0';
  std::string base = has_local ? std::string(env) : app_data_dir();
  free(env);
  std::replace(base.begin(), base.end(), '\\', '/');
  return has_local ? base + "/FlashRec/updates" : base + "/updates";
#else
  const char* cache = getenv("XDG_CACHE_HOME");
  if (cache && *cache) return std::string(cache) + "/FlashRec/updates";
  const char* home = getenv("HOME");
  return std::string(home && *home ? home : ".") + "/.cache/FlashRec/updates";
#endif
}

long long free_disk_bytes(const std::string& path) {
#ifdef _WIN32
  ULARGE_INTEGER free_bytes;
  if (!GetDiskFreeSpaceExA(path.c_str(), &free_bytes, nullptr, nullptr)) return -1;
  return static_cast<long long>(free_bytes.QuadPart);
#else
  struct statvfs st {};
  if (statvfs(path.c_str(), &st) != 0) return -1;
  return static_cast<long long>(st.f_bavail) * static_cast<long long>(st.f_frsize);
#endif
}

void set_env(const std::string& name, const std::string& value) {
#ifdef _WIN32
  _putenv_s(name.c_str(), value.c_str());
#else
  setenv(name.c_str(), value.c_str(), 1);
#endif
}void init() {
  g_assets.clear();
  std::string ed = exe_dir();
  std::replace(ed.begin(), ed.end(), '\\', '/');
  g_assets.push_back(ed + "/assets");          // 便携：exe 旁
  g_assets.push_back(ed + "/../assets");       // build/bin 回源码树
  g_assets.push_back(ed + "/../../assets");    // build/ 回源码树
#ifndef _WIN32
  // FHS 安装布局（d95）：<prefix>/bin/flashrec → <prefix>/share/flashrec/assets。
  // 覆盖 /usr 下装（deb）、$HOME/.local 下装、以及 AppImage 内 AppDir/usr 三种情形。
  g_assets.push_back(ed + "/../share/flashrec/assets");
#endif
#ifdef FLASHREC_ASSETS_DIR
  g_assets.push_back(FLASHREC_ASSETS_DIR);
#endif
}

std::vector<std::string> search_dirs() { return g_assets; }

std::string asset_dir() {
  for (auto& d : g_assets)
    if (fs::exists(d)) return d;
  return g_assets.empty() ? "assets" : g_assets.front();
}

std::string asset_file(const std::string& rel) {
  std::string reln = rel;
  std::replace(reln.begin(), reln.end(), '\\', '/');
  for (auto& d : g_assets) {
    std::string p = d + "/" + reln;
    if (fs::exists(p)) return p;
  }
  return {};
}

}  // namespace fr::paths
