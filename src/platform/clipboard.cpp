#include "platform/clipboard.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <windows.h>
#endif

#include <vector>

#include "app/log.h"

namespace fr {
namespace clipboard {

bool start_listener(GLFWwindow* win) {
#if defined(_WIN32)
  if (!win) return false;
  const HWND hwnd = glfwGetWin32Window(win);
  if (!hwnd || !AddClipboardFormatListener(hwnd)) {
    FR_LOG_WARN("[CLIP] AddClipboardFormatListener 失败，退化为轮询模式");
    return false;
  }
  return true;
#else
  (void)win;
  return false;  // Linux/macOS：无变更事件，调用方轮询 get_text()
#endif
}

std::string get_text(GLFWwindow* win) {
#if defined(_WIN32)
  (void)win;
  // CF_UNICODETEXT 只读；打不开（他人持有）或非文本一律回空串——静默降级。
  if (!OpenClipboard(nullptr)) return "";
  std::string out;
  HANDLE h = GetClipboardData(CF_UNICODETEXT);
  if (h) {
    if (const wchar_t* w = (const wchar_t*)GlobalLock(h)) {
      const int bytes = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
      if (bytes > 1) {
        std::vector<char> buf((size_t)bytes);
        WideCharToMultiByte(CP_UTF8, 0, w, -1, buf.data(), bytes, nullptr, nullptr);
        out.assign(buf.data(), (size_t)bytes - 1);  // 去掉结尾 NUL
      }
      GlobalUnlock(h);
    }
  }
  CloseClipboard();
  return out;
#else
  (void)win;
  if (!win) return "";
  const char* s = glfwGetClipboardString(win);
  return s ? std::string(s) : std::string();
#endif
}

}  // namespace clipboard
}  // namespace fr
