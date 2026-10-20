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

#include "app/log.h"

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

// —— ★d495：Windows 便携版自动换装（d474 决议落地；用户定则：独立进程实现，
//    不用脚本）——主程序只负责拉起同目录的 flashrec_swap.exe（分离进程）并退出；
//    等待/备份/解压/探活/还原全在助手内（见 portable_swap_helper.c）。zip 哈希
//    复核在 main（main→update 合法方向），本层不再校验。平台分治：Windows 文件
//    锁语义必须独立进程；Linux 无此锁，AppImage=d461 shell 助手 / deb=d473 pkexec。

namespace {

constexpr wchar_t kSwapHelperName[] = L"flashrec_swap.exe";
constexpr wchar_t kSwapProbeName[] = L".fr_write_probe";

bool file_exists(const wchar_t *path) {
  const DWORD attr = GetFileAttributesW(path);
  return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

}  // namespace

bool portable_swap_supported() {
  const std::wstring dir = exe_dir_wide();
  if (dir.empty()) return false;
  // ① exe 目录可写（放进 Program Files 等只读位置 = 不支持，回退人工交接）
  const std::wstring probe = dir + L"\\" + kSwapProbeName;
  const HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_HIDDEN, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  CloseHandle(h);
  DeleteFileW(probe.c_str());
  // ② 助手随包（v0.6.4 及更早的包没有助手 = 不支持，装上本版后永久可用）
  if (!file_exists((dir + L"\\" + kSwapHelperName).c_str())) return false;
  // ③ 系统自带 tar.exe（解压依赖；Win10 1803+，缺失回退人工交接）
  wchar_t sys[MAX_PATH];
  const UINT syslen = GetSystemDirectoryW(sys, ARRAYSIZE(sys));
  if (syslen == 0 || syslen >= ARRAYSIZE(sys) - 8) return false;
  return file_exists((std::wstring(sys) + L"\\tar.exe").c_str());
}

bool launch_portable_swap(const std::string& asset_path) {
  if (asset_path.empty()) return false;
  const std::wstring dir = exe_dir_wide();
  const std::wstring helper = dir + L"\\" + kSwapHelperName;
  if (!portable_swap_supported() || !file_exists(helper.c_str())) return false;
  std::wstring zip = utf8_to_wide(asset_path);
  std::replace(zip.begin(), zip.end(), L'/', L'\\');

  // 分离进程：主进程退出后助手独立完成换装；句柄即取即关，不留等待义务。
  std::wstring cmd = L"\"" + helper + L"\" --pid " +
                     std::to_wstring(GetCurrentProcessId()) + L" --zip \"" + zip +
                     L"\" --dir \"" + dir + L"\"";
  if (cmd.size() >= 32767) return false;  // CreateProcessW 命令行上限

  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
    FR_LOG_WARN("[UPD] 便携换装助手启动失败（CreateProcess {}）", (int)GetLastError());
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  FR_LOG_INFO("[UPD] 便携换装助手已启动（分离进程）");
  return true;
}

// ★d496：deb polkit 自动安装仅 Linux 形态（Windows 走 Inno/换装助手）。
bool deb_polkit_supported() { return false; }

bool launch_update_installer(InstallShape shape, const std::string& asset_path,
                             const std::string& expected_sha256) {
  (void)expected_sha256;  // 哈希复核在 main 完成助手拉起前；Windows 各分支不用
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
      // ★d495：自动换装优先（可写探测通过 = 生成脚本分离进程交接）；
      // 不可写/启动失败回退打开所在目录（旧人工交接路径）。
      if (launch_portable_swap(asset_path)) return true;
      FR_LOG_WARN("[UPD] 便携自动换装不可用，回退打开所在目录");
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
#include <cstring>
#include <dirent.h>
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

// ★d495：便携自动换装仅 Windows 形态；Linux 便携等价物是 AppImage 助手（d461）。
bool portable_swap_supported() { return false; }
bool launch_portable_swap(const std::string&) { return false; }

bool open_containing_folder(const std::string& file_path) {
  if (file_path.empty()) return false;
  const size_t slash = file_path.find_last_of('/');
  const std::string dir =
      slash == std::string::npos ? std::string(".") : file_path.substr(0, slash);
  return spawn_detached({"xdg-open", dir.c_str()});
}

// ★d496：deb polkit 提权自动安装支持探测（d473 决议）。三项缺一 = 回退人工路径：
// ① pkexec 在位；② polkitd 运行中（扫 /proc/*/comm，不 spawn 子进程）；
// ③ 图形会话在位（授权框要弹在桌面上；SSH/无桌面会话恒回退）。
bool deb_polkit_supported() {
  if (access("/usr/bin/pkexec", X_OK) != 0) return false;
  bool polkitd = false;
  if (DIR *proc = opendir("/proc")) {
    while (const dirent *e = readdir(proc)) {
      if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
      char path[64], comm[64] = {0};
      std::snprintf(path, sizeof(path), "/proc/%.20s/comm", e->d_name);
      FILE *f = fopen(path, "r");
      if (f == nullptr) continue;
      const size_t n = fread(comm, 1, sizeof(comm) - 1, f);
      fclose(f);
      if (n > 0 && strncmp(comm, "polkitd", 7) == 0) {
        polkitd = true;
        break;
      }
    }
    closedir(proc);
  }
  if (!polkitd) return false;
  const char *disp = getenv("DISPLAY");
  const char *wayl = getenv("WAYLAND_DISPLAY");
  return (disp != nullptr && *disp != '\0') || (wayl != nullptr && *wayl != '\0');
}

bool launch_update_installer(InstallShape shape, const std::string& asset_path,
                             const std::string& expected_sha256) {
  if (shape == InstallShape::LinuxAppImage || asset_path.empty()) {
    if (shape != InstallShape::LinuxAppImage) return false;
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
  if (shape != InstallShape::LinuxDeb) return false;
  // ★d496：deb polkit 提权自动安装。detached shell 助手（d461 同款，平台分治
  // d495：Linux 不新增二进制）。脚本自身不联网；apt 用 `apt install ./deb`
  //（自带依赖解析，d473 定）而非裸 dpkg -i；pkexec 授权框系统弹出，密码不经过
  // 我方进程；安装前脚本内复核哈希（防 spawn→安装之间的文件窗口，d473 约束）。
  // 成功 → 拉起新版（/usr/bin/flashrec）；取消/授权失败/依赖缺失 → 打开所在目录，
  // 绝不留半残包数据库（apt 自身事务性兜底）。全程日志写 <deb>.install.log
  //（d497 演练教训：无日志的 pkexec 失败不可诊断——SSH/非会话拉起时 polkitd
  // 找不到认证代理，报 textual agent /dev/tty 错）。
  if (!deb_polkit_supported()) return open_containing_folder(asset_path);
  static constexpr char kDebScript[] =
      "pid=$1; deb=$2; hash=$3; "
      "exec >>\"$deb.install.log\" 2>&1; "
      "echo \"---- deb 助手启动 $(date +%T) pid=$pid\"; "
      "while kill -0 \"$pid\" 2>/dev/null; do sleep 0.2; done; "
      "actual=$(sha256sum \"$deb\" 2>/dev/null | cut -d' ' -f1); "
      "echo \"哈希 actual=$actual expect=$hash\"; "
      "[ \"$actual\" = \"$hash\" ] || { echo 哈希不符，回退; xdg-open \"$(dirname \"$deb\")\"; exit 1; }; "
      "echo 调用 pkexec apt install; "
      "if pkexec /usr/bin/apt install -y \"$deb\"; then "
      "  echo 安装成功，拉起新版; exec /usr/bin/flashrec; "
      "fi; "
      "echo \"pkexec 失败 rc=$?，回退\"; xdg-open \"$(dirname \"$deb\")\"; exit 1";
  char pid[32];
  std::snprintf(pid, sizeof(pid), "%ld", (long)getpid());
  return spawn_detached({"/bin/sh", "-c", kDebScript, "flashrec-deb-install", pid,
                         asset_path.c_str(), expected_sha256.c_str()});
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
bool launch_update_installer(InstallShape, const std::string&,
                             const std::string&) { return false; }
bool portable_swap_supported() { return false; }
bool launch_portable_swap(const std::string&) { return false; }
bool deb_polkit_supported() { return false; }

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
