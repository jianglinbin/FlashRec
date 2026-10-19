#include "platform/update_install.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iterator>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cwctype>
#include <windows.h>
#include <shellapi.h>

namespace fr::platform {
namespace {

constexpr wchar_t kUninstallKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
    L"{7A3E9C21-4B6D-4F58-9C2A-1E5D8B7F0A34}_is1";

std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
  return w;
}

bool shell_open(const std::wstring& target, const std::wstring& params) {
  const HINSTANCE r = ShellExecuteW(nullptr, L"open", target.c_str(),
                                    params.empty() ? nullptr : params.c_str(), nullptr,
                                    SW_SHOWNORMAL);
  return reinterpret_cast<INT_PTR>(r) > 32;
}

std::wstring normalize_path(std::wstring path) {
  std::replace(path.begin(), path.end(), L'/', L'\\');
  while (!path.empty() && path.back() == L'\\') path.pop_back();
  std::transform(path.begin(), path.end(), path.begin(),
                 [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  return path;
}

std::wstring exe_dir_wide() {
  wchar_t buf[MAX_PATH * 4] = {0};
  const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
  std::wstring path(buf, n);
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

bool read_install_location(HKEY root, REGSAM view, std::wstring* out) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(root, kUninstallKey, 0, KEY_READ | view, &key) != ERROR_SUCCESS) return false;
  wchar_t buf[1024] = {0};
  DWORD type = 0;
  DWORD bytes = static_cast<DWORD>(sizeof(buf));
  const LONG rc =
      RegQueryValueExW(key, L"InstallLocation", nullptr, &type, reinterpret_cast<LPBYTE>(buf),
                       &bytes);
  RegCloseKey(key);
  if (rc != ERROR_SUCCESS || type != REG_SZ) return false;
  *out = buf;
  return true;
}

}  // namespace

InstallShape detect_install_shape() {
  const std::wstring exe = normalize_path(exe_dir_wide());
  if (exe.empty()) return InstallShape::WindowsPortable;
  std::wstring location;
  if (read_install_location(HKEY_CURRENT_USER, 0, &location) ||
      read_install_location(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, &location) ||
      read_install_location(HKEY_LOCAL_MACHINE, 0, &location)) {
    const std::wstring install = normalize_path(location);
    if (!install.empty() &&
        (exe == install || exe.rfind(install + L"\\", 0) == 0)) {
      return InstallShape::WindowsSetup;
    }
  }
  return InstallShape::WindowsPortable;
}

bool open_url(const std::string& url) {
  // 链接来自远端清单/代理：只允许 HTTPS，绝不把 file:// 或自定义协议交给 ShellExecute。
  if (url.rfind("https://", 0) != 0) return false;
  return shell_open(utf8_to_wide(url), std::wstring());
}

bool open_containing_folder(const std::string& file_path) {
  if (file_path.empty()) return false;
  std::wstring w = utf8_to_wide(file_path);
  std::replace(w.begin(), w.end(), L'/', L'\\');
  const size_t slash = w.find_last_of(L'\\');
  const std::wstring dir = slash == std::wstring::npos ? w : w.substr(0, slash);
  if (GetFileAttributesW(w.c_str()) != INVALID_FILE_ATTRIBUTES)
    return shell_open(L"explorer.exe", L"/select,\"" + w + L"\"");
  return shell_open(dir, std::wstring());
}

bool launch_update_installer(InstallShape shape, const std::string& asset_path) {
  switch (shape) {
    case InstallShape::WindowsSetup: {
      if (asset_path.empty()) return false;
      wchar_t tmp[MAX_PATH] = {0};
      const DWORD n = GetTempPathW(static_cast<DWORD>(std::size(tmp)), tmp);
      std::wstring log = n > 0 ? std::wstring(tmp, n) : std::wstring();
      log += L"FlashRec-update.log";
      // /RELAUNCH=1 是安装脚本的自定义参数（{param:RELAUNCH} 读取，见
      // tools/installer_windows.iss）：安装完成后由 [Run] 拉起新版本。
      // 不加 /VERYSILENT：更新入口要求用户能看到安装程序已经启动并自行走完向导；
      // 静默安装会让"点击更新"看起来毫无反应（v0.6.3 实测 5s 装完，不可见）。
      const std::wstring args = L"/NORESTART /FORCECLOSEAPPLICATIONS /RELAUNCH=1 /LOG=\"" + log +
                                L"\"";
      return shell_open(utf8_to_wide(asset_path), args);
    }
    case InstallShape::WindowsPortable:
      return open_containing_folder(asset_path);
    case InstallShape::LinuxAppImage:
    case InstallShape::LinuxDeb:
    case InstallShape::Unknown:
      return false;
  }
  return false;
}

const char* install_shape_name(InstallShape shape) {
  switch (shape) {
    case InstallShape::WindowsSetup: return "windows-setup";
    case InstallShape::WindowsPortable: return "windows-portable";
    case InstallShape::LinuxAppImage: return "linux-appimage";
    case InstallShape::LinuxDeb: return "linux-deb";
    case InstallShape::Unknown: return "unknown";
  }
  return "unknown";
}

}  // namespace fr::platform

#elif defined(__linux__)
#include <limits.h>
#include <unistd.h>
#include <cstdio>
#include <initializer_list>
#include <vector>

namespace fr::platform {
namespace {

bool spawn_detached(std::initializer_list<const char*> args) {
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (const char* a : args) argv.push_back(const_cast<char*>(a));
  argv.push_back(nullptr);
  const pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    setsid();
    execvp(argv[0], argv.data());
    _exit(127);
  }
  return true;
}

}  // namespace

InstallShape detect_install_shape() {
  const char* appimage = getenv("APPIMAGE");
  if (appimage != nullptr && *appimage != '\0') return InstallShape::LinuxAppImage;
  char buf[PATH_MAX] = {0};
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n > 0) {
    buf[n] = '\0';
    const std::string path(buf);
    if (path.rfind("/usr/", 0) == 0) return InstallShape::LinuxDeb;
  }
  return InstallShape::Unknown;
}

bool open_url(const std::string& url) {
  // 与 Windows 侧同规：远端链接只接受 HTTPS。
  if (url.rfind("https://", 0) != 0) return false;
  return spawn_detached({"xdg-open", url.c_str()});
}

bool open_containing_folder(const std::string& file_path) {
  if (file_path.empty()) return false;
  const size_t slash = file_path.find_last_of('/');
  const std::string dir =
      slash == std::string::npos ? std::string(".") : file_path.substr(0, slash);
  return spawn_detached({"xdg-open", dir.c_str()});
}

bool launch_update_installer(InstallShape shape, const std::string& asset_path) {
  if (shape == InstallShape::LinuxDeb) return open_containing_folder(asset_path);
  if (shape != InstallShape::LinuxAppImage || asset_path.empty()) return false;
  const char* appimage = getenv("APPIMAGE");
  if (appimage == nullptr || *appimage == '\0') return false;
  static constexpr char kScript[] =
      "pid=$1; src=$2; dst=$3; "
      "while kill -0 \"$pid\" 2>/dev/null; do sleep 0.2; done; "
      "dir=$(dirname \"$dst\"); "
      "cp -f \"$src\" \"$dst.new\" && chmod +x \"$dst.new\" || { xdg-open \"$dir\"; exit 1; }; "
      "rm -f \"$dst.old\"; mv -f \"$dst\" \"$dst.old\" || { xdg-open \"$dir\"; exit 1; }; "
      "mv -f \"$dst.new\" \"$dst\" || { xdg-open \"$dir\"; exit 1; }; exec \"$dst\"";
  char pid[32];
  std::snprintf(pid, sizeof(pid), "%ld", (long)getpid());
  return spawn_detached({"/bin/sh", "-c", kScript, "flashrec-update", pid, asset_path.c_str(),
                         appimage});
}

const char* install_shape_name(InstallShape shape) {
  switch (shape) {
    case InstallShape::WindowsSetup: return "windows-setup";
    case InstallShape::WindowsPortable: return "windows-portable";
    case InstallShape::LinuxAppImage: return "linux-appimage";
    case InstallShape::LinuxDeb: return "linux-deb";
    case InstallShape::Unknown: return "unknown";
  }
  return "unknown";
}

}  // namespace fr::platform

#else
namespace fr::platform {

InstallShape detect_install_shape() { return InstallShape::Unknown; }

bool open_url(const std::string&) { return false; }
bool open_containing_folder(const std::string&) { return false; }
bool launch_update_installer(InstallShape, const std::string&) { return false; }

const char* install_shape_name(InstallShape shape) {
  switch (shape) {
    case InstallShape::WindowsSetup: return "windows-setup";
    case InstallShape::WindowsPortable: return "windows-portable";
    case InstallShape::LinuxAppImage: return "linux-appimage";
    case InstallShape::LinuxDeb: return "linux-deb";
    case InstallShape::Unknown: return "unknown";
  }
  return "unknown";
}

}  // namespace fr::platform
#endif
