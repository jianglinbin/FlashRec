#include "platform/paths.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <vector>

namespace fr::paths {

namespace fs = std::filesystem;

static std::vector<std::string> g_assets;  // assets 搜索目录

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
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
#else
#include <unistd.h>
#include <limits.h>

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
#endif

std::string log_dir() { return app_data_dir() + "/logs"; }
std::string config_file() { return app_data_dir() + "/settings.json"; }

void set_env(const std::string& name, const std::string& value) {
#ifdef _WIN32
  _putenv_s(name.c_str(), value.c_str());
#else
  setenv(name.c_str(), value.c_str(), 1);
#endif
}

void init() {
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
