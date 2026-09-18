#pragma once
// 剪贴板采集（d74）：平台差异收口（AGENTS.md 规则 5，平台宏只在本实现文件）。
//
// Win = 系统事件监听（WM_CLIPBOARDUPDATE）：系统级生效——在浏览器里复制
// 链接即触发，不要求 FlashRec 在前台（这正是本功能的核心场景）；
// Linux/macOS 无「剪贴板变更」事件，返回 false，调用方以 ~1s 轮询
// get_text() 兜底（读 CLIPBOARD selection 无需焦点）。
//
// 线程约定：WM_CLIPBOARDUPDATE 事件在窗口消息线程（= 主线程）触发；
// get_text 必须在主线程调用（Win32 剪贴板句柄有线程所有性，GLFW X11 同样要求）。
#include <string>

struct GLFWwindow;

namespace fr {

namespace clipboard {

// 装 Win 事件监听（AddClipboardFormatListener）；返回 false = 平台无事件
// 模式（调用方轮询 get_text() 兜底）。变更事件由 window_glfw 的 wndproc
// 收到 WM_CLIPBOARDUPDATE 后触发 window.on_clipboard_update（main 接线）。
bool start_listener(GLFWwindow* win);

// 读剪贴板 UTF-8 文本（无文本 / 非文本 / 打不开 → 空串）。
std::string get_text(GLFWwindow* win);

}  // namespace clipboard

}  // namespace fr
