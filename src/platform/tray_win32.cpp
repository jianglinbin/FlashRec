// 托盘 Win32 实现（Shell_NotifyIcon，d46 双协议定稿）。
// 结构：隐藏消息窗口（主线程）承载 NOTIFYICONDATA 回调；glfwPollEvents
// 泵本线程全部消息（PeekMessage NULL 过滤），托盘消息与菜单模态循环都能
// 在主循环内自然运转 —— 不建独立线程，回调天然在主线程。
// 图标：资产 PNG 经 stb 解码（声明模式，实现复用 nanovg.c）→ 32bpp BGRA
// → CreateIconIndirect，与窗口图标同源，无需 .rc 资源管线。
// 关闭语义：explorer 重启（TaskbarCreated）后自动补挂图标。
#include "platform/tray.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <string>

// stb_image 声明模式（STB_IMAGE_IMPLEMENTATION 定义在 nanovg.c，C 链接符号共享）
#include "stb_image.h"

#include "app/log.h"

namespace fr {
namespace tray {

namespace {

constexpr UINT kIconCallbackMsg = WM_APP + 0x64;  // NOTIFYICONDATA 回调消息
constexpr int kQuitItemId = -1;                   // 「退出」约定 id（tray.h）

HWND g_hwnd = nullptr;
HICON g_icon = nullptr;
NOTIFYICONDATAW g_nid{};
UINT g_taskbar_created = 0;  // RegisterWindowMessage("TaskbarCreated")
TrayCallbacks g_cb;

std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
  return w;
}

// PNG → 32bpp HICON（RGBA 逐像素翻成 premultiplied BGRA + 全 0 掩码位图）
HICON icon_from_png(const char* path) {
  int w = 0, h = 0, comp = 0;
  stbi_uc* rgba = stbi_load(path, &w, &h, &comp, 4);
  if (!rgba) return nullptr;
  const int pixels = w * h;
  auto* bgra = (DWORD*)rgba;
  for (int i = 0; i < pixels; ++i) {
    const unsigned char a = (unsigned char)(bgra[i] >> 24);
    const unsigned char r = (unsigned char)(rgba[i * 4 + 0]);
    const unsigned char g = (unsigned char)(rgba[i * 4 + 1]);
    const unsigned char b = (unsigned char)(rgba[i * 4 + 2]);
    // premultiply（AlphaBlend/图标合成要求）
    bgra[i] = ((DWORD)(b * a / 255) << 16) | ((DWORD)(g * a / 255) << 8) |
              ((DWORD)(r * a / 255)) | ((DWORD)a << 24);
  }
  BITMAPV5HEADER bi{};
  bi.bV5Size = sizeof(bi);
  bi.bV5Width = w;
  bi.bV5Height = -h;  // top-down
  bi.bV5Planes = 1;
  bi.bV5BitCount = 32;
  bi.bV5Compression = BI_BITFIELDS;
  bi.bV5RedMask = 0x00FF0000;
  bi.bV5GreenMask = 0x0000FF00;
  bi.bV5BlueMask = 0x000000FF;
  bi.bV5AlphaMask = 0xFF000000;
  HDC dc = GetDC(nullptr);
  void* bits = nullptr;
  HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, dc);
  if (!color) {
    stbi_image_free(rgba);
    return nullptr;
  }
  memcpy(bits, rgba, (size_t)pixels * 4);
  stbi_image_free(rgba);
  HBITMAP mask = CreateBitmap(w, h, 1, 1, nullptr);  // 全 0 掩码：alpha 生效
  ICONINFO ii{};
  ii.fIcon = TRUE;
  ii.hbmColor = color;
  ii.hbmMask = mask;
  HICON icon = CreateIconIndirect(&ii);
  DeleteObject(color);
  DeleteObject(mask);
  return icon;
}

bool add_icon() {
  g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  g_nid.uCallbackMessage = kIconCallbackMsg;
  g_nid.hIcon = g_icon;
  g_nid.hWnd = g_hwnd;
  return Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
}

void show_native_menu() {
  if (!g_cb.menu_provider) return;
  const std::vector<TrayMenuItem> items = g_cb.menu_provider();
  HMENU menu = CreatePopupMenu();
  if (!menu) return;
  int order = 0;
  for (const auto& it : items) {
    if (it.sep) {
      InsertMenuW(menu, order, MF_SEPARATOR | MF_BYPOSITION, 0, nullptr);
    } else {
      UINT flags = MF_STRING | MF_BYPOSITION;
      if (!it.enabled) flags |= MF_GRAYED;
      if (it.checked) flags |= MF_CHECKED;
      InsertMenuW(menu, order, flags, (UINT_PTR)it.id, utf8_to_wide(it.label).c_str());
    }
    ++order;
  }
  // 托盘菜单经典三件套：前台权 + TPM_RETURNCMD + WM_NULL（否则点外部不收菜单）
  POINT pt;
  GetCursorPos(&pt);
  SetForegroundWindow(g_hwnd);
  const int cmd = (int)TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                      pt.x, pt.y, 0, g_hwnd, nullptr);
  PostMessageW(g_hwnd, WM_NULL, 0, 0);
  DestroyMenu(menu);
  if (cmd != 0 && g_cb.on_menu_action) g_cb.on_menu_action(cmd);
}

LRESULT CALLBACK tray_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == g_taskbar_created && g_taskbar_created) {
    // explorer 重启后托盘区重建：补挂图标（失败静默，下轮 TaskbarCreated 再试）
    add_icon();
    return 0;
  }
  if (msg == kIconCallbackMsg) {
    const UINT mouse = LOWORD(lp);
    if (mouse == WM_LBUTTONUP) {
      if (g_cb.on_toggle_window) g_cb.on_toggle_window();
    } else if (mouse == WM_RBUTTONUP || mouse == WM_CONTEXTMENU) {
      show_native_menu();
    }
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

bool create(GLFWwindow* win, const char* tooltip, const char* icon_png,
            const TrayCallbacks& cb) {
  (void)win;  // 消息窗口独立于 GLFW 窗口；GLFW 主循环泵本线程消息
  if (g_hwnd) return true;  // 已创建
  if (!tooltip || !icon_png || !cb.menu_provider || !cb.on_menu_action) return false;

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = tray_wndproc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"FlashRecTray";
  RegisterClassExW(&wc);
  g_hwnd = CreateWindowExW(0, L"FlashRecTray", L"FlashRec tray", 0, 0, 0, 0, 0,
                           HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
  if (!g_hwnd) return false;

  g_icon = icon_from_png(icon_png);
  if (!g_icon) {
    FR_LOG_WARN("[TRAY] 托盘图标解码失败 {}", icon_png);
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
    return false;
  }

  ZeroMemory(&g_nid, sizeof(g_nid));
  g_nid.cbSize = sizeof(g_nid);
  const std::wstring tip = utf8_to_wide(tooltip);
  wcsncpy(g_nid.szTip, tip.c_str(), 127);
  g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

  if (!add_icon()) {
    FR_LOG_WARN("[TRAY] Shell_NotifyIcon 失败（任务栏不可用？）");
    DestroyIcon(g_icon);
    g_icon = nullptr;
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
    return false;
  }
  g_cb = cb;
  FR_LOG_INFO("[TRAY] 托盘已挂载（Shell_NotifyIcon）");
  return true;
}

void poll() {
  // Win：GLFW 主循环 poll_events 泵本线程全部消息（含托盘消息窗口），
  // 无需额外处理 —— 空实现保证接口对称。
}

void remove() {
  if (g_hwnd) {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
  }
  if (g_icon) {
    DestroyIcon(g_icon);
    g_icon = nullptr;
  }
}

}  // namespace tray

}  // namespace fr
