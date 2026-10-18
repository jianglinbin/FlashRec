// 开机启动实现（AGENTS.md 规则 5：平台宏只出现在 src/platform/）。
// 只做系统配置读写，不碰 UI / 日志 —— 成败由调用方（main 托盘动作）决定提示。
#include "platform/autostart.h"

#ifdef _WIN32

// CMake 已全局 /D 这两个宏；加卫语句避免 C4005 宏重定义警告（tray_win32 同款）。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <iterator>
#include <string>

namespace fr::autostart {
namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kValueName[] = L"FlashRec";

std::wstring exe_path() {
  wchar_t buf[MAX_PATH * 4];
  const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
  if (n == 0 || n >= std::size(buf)) return {};
  return std::wstring(buf, n);
}

std::wstring unquoted(const std::wstring& s) {
  if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') return s.substr(1, s.size() - 2);
  return s;
}

bool read_value(std::wstring* out) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
    return false;
  wchar_t buf[4096];
  DWORD bytes = sizeof(buf);
  DWORD type = 0;
  const LONG rc = RegQueryValueExW(key, kValueName, nullptr, &type,
                                   reinterpret_cast<LPBYTE>(buf), &bytes);
  RegCloseKey(key);
  if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return false;
  out->assign(buf, bytes / sizeof(wchar_t));
  while (!out->empty() && out->back() == L'\0') out->pop_back();
  return true;
}

}  // namespace

bool enabled() {
  std::wstring stored;
  if (!read_value(&stored)) return false;
  const std::wstring exe = exe_path();
  if (exe.empty()) return true;  // 读不到自身路径时按"已配置"保守处理，不误删用户入口
  // 便携包挪目录：入口仍在但指向旧路径 → 就地修正，保证下次开机真的能起来。
  if (CompareStringOrdinal(unquoted(stored).c_str(), -1, exe.c_str(), -1, TRUE) != CSTR_EQUAL) {
    return set_enabled(true);
  }
  return true;
}

bool set_enabled(bool on) {
  if (!on) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
      return true;  // 键不存在 = 本就没有入口，目标态已达成
    const LONG rc = RegDeleteValueW(key, kValueName);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
  }
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                      nullptr) != ERROR_SUCCESS) {
    return false;
  }
  const std::wstring exe = exe_path();
  if (exe.empty()) {
    RegCloseKey(key);
    return false;
  }
  const std::wstring cmd = L"\"" + exe + L"\"";
  const LONG rc =
      RegSetValueExW(key, kValueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                     static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
  return rc == ERROR_SUCCESS;
}

}  // namespace fr::autostart

#else  // Linux（其余平台编译占位：无系统自启动约定，菜单项据此不可用）

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fr::autostart {
namespace {

namespace fs = std::filesystem;

std::string exe_path() {
  // AppImage：/proc/self/exe 指向临时挂载点（每次启动都变），必须用运行时给的
  // 原始 .AppImage 路径，否则开机启动指向一个早已卸载的路径。
  if (const char* appimage = std::getenv("APPIMAGE"); appimage && *appimage) return appimage;
  std::error_code ec;
  const fs::path p = fs::read_symlink("/proc/self/exe", ec);
  return ec ? std::string() : p.string();
}

std::string config_dir() {
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) return xdg;
  if (const char* home = std::getenv("HOME"); home && *home) return std::string(home) + "/.config";
  return {};
}

std::string desktop_file() {
  const std::string dir = config_dir();
  return dir.empty() ? std::string() : dir + "/autostart/flashrec.desktop";
}

// Desktop Entry Exec 引号（spec：双引号内 \ " ` $ 需反斜杠转义；% 是字段码，字面量写 %%）。
std::string exec_quote(const std::string& path) {
  std::string out = "\"";
  for (const char c : path) {
    if (c == '\\' || c == '"' || c == '`' || c == '$') out += '\\';
    if (c == '%') {
      out += "%%";
      continue;
    }
    out += c;
  }
  out += '"';
  return out;
}

std::string read_all(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool write_all(const std::string& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  out.flush();
  return out.good();
}

bool line_is(const std::string& text, const std::string& line) {
  const std::string needle = line + "\n";
  return text.rfind(needle, 0) == 0 || text.find("\n" + needle) != std::string::npos;
}

}  // namespace

bool enabled() {
  const std::string path = desktop_file();
  if (path.empty()) return false;  // HOME/XDG 均缺失
  const std::string text = read_all(path);
  if (text.empty()) return false;
  if (line_is(text, "Hidden=true") || line_is(text, "X-GNOME-Autostart-enabled=false"))
    return false;
  const std::string exe = exe_path();
  if (exe.empty()) return true;
  if (text.find("Exec=" + exec_quote(exe) + "\n") != std::string::npos) return true;
  // 入口存在但指向旧路径（AppImage/二进制挪位）→ 就地修正。
  return set_enabled(true);
}

bool set_enabled(bool on) {
  const std::string path = desktop_file();
  if (path.empty()) return false;
  if (!on) {
    std::error_code ec;
    fs::remove(path, ec);
    return !ec;
  }
  const std::string exe = exe_path();
  if (exe.empty()) return false;
  std::error_code ec;
  fs::create_directories(fs::path(path).parent_path(), ec);
  if (ec) return false;
  const std::string content = "[Desktop Entry]\nType=Application\nName=FlashRec\nExec=" +
                              exec_quote(exe) +
                              "\nTerminal=false\nX-GNOME-Autostart-enabled=true\n";
  // d373 既有约定：同目录 .tmp → rename 原子替换，桌面会话不会读到半个文件。
  const std::string tmp = path + ".tmp";
  if (!write_all(tmp, content)) return false;
  std::error_code ec2;
  fs::rename(tmp, path, ec2);
  if (ec2) {
    std::error_code ignored;
    fs::remove(tmp, ignored);
    return false;
  }
  return true;
}

}  // namespace fr::autostart

#endif
