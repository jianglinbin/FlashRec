// Wayland 内部桥接（d90 移动 + d91 连接健康探测）
//
// 背景：Wayland 协议不允许客户端设置窗口位置 —— GLFW 的 Wayland 后端把
// glfwSetWindowPos 实现成空操作（wl_window.c: _glfwSetWindowPosWayland 只发一个
// GLFW_FEATURE_UNAVAILABLE 就返回），因此「拖顶栏移动窗口」在 Wayland 会话下
// 完全失效；而缩放/最大化/全屏走的是 xdg_toplevel 请求，一切正常。
//
// 正解：xdg_toplevel_move(toplevel, seat, serial) —— 由合成器接管一次交互式移动
// 抓取（GLFW 内部给自己画的装饰条用的就是这一条，见 wl_window.c:1563）。
//
// d91 追加第二个导出（同因：需要 GLFW 内部状态，故必须编进 glfw 目标）：
// fr_wayland_display_alive() —— 关闭请求回调里用它区分「用户真要关窗」与「连接已死」。
//
// 规则 8：third_party/ 只读。本文件不改 GLFW 一行，而是由 cmake/deps.cmake 把它
// 追加进 glfw 目标一起编译（同目录作用域 = 可直接使用 GLFW 内部头与 _glfw 全局）。
// 对外只暴露两个纯 C 函数，供 src/platform/window_glfw.cpp 声明后调用。
//
// ⚠️ 只能在主线程调用：_glfw.wl 是 GLFW 的主线程状态。

#include "internal.h"                    // → platform.h → wl_platform.h（_glfw / _GLFWwindow）
#include "xdg-shell-client-protocol.h"   // xdg_toplevel_move / unset_maximized
// 注意：不要显式 include <wayland-client.h>。wl_platform.h 已把 wl_display_* 等入口
// #define 成 _glfw.wl.client.* 的函数指针，再展开真实头文件会撞上宏替换。

// 取本窗口的 xdg_toplevel 代理。两条 shell 路径都要照顾 —— 只认一条就会静默失效：
//   ① libdecor 在场（GLFW 默认 GLFW_WAYLAND_PREFER_LIBDECOR，init.c:67）：
//      xdg_toplevel 由 libdecor 创建并持有，window->wl.xdg.toplevel 恒为 NULL
//      （全仓仅 createXdgShellObjects 里赋值一次）→ 必须用 libdecor_frame_get_xdg_toplevel()
//      反查。GLFW 已绑定该符号（wl_init.c:779），直接可用。
//   ② 无 libdecor（或已用 GLFW_WAYLAND_DISABLE_LIBDECOR 回裸 xdg-shell）：
//      由 createXdgShellObjects() 直接创建，直接取用。
static struct xdg_toplevel* fr_wl_toplevel(_GLFWwindow* window, int* out_path) {
    if (window->wl.libdecor.frame) {
        *out_path = 1;
        if (!_glfw.wl.libdecor.libdecor_frame_get_xdg_toplevel_)
            return NULL;
        return _glfw.wl.libdecor.libdecor_frame_get_xdg_toplevel_(window->wl.libdecor.frame);
    }

    *out_path = 2;
    return window->wl.xdg.toplevel;
}

// 请求一次由合成器接管的交互式窗口移动。
//
// 参数 unmaximize：非 0 时先发 xdg_toplevel_unset_maximized（最大化态拖标题栏
//   = 还原并跟手，对齐 GNOME 原生手感）；0 则直接拖。
// 出参 out_path：1 = libdecor 反查取得；2 = 裸 xdg.toplevel 取得；0 = 未取到。
// 出参 out_serial：本次使用的输入序列号（0 表示尚无有效序列号）。
//
// 返回 0 = 已交给合成器；负数 = 未执行：
//   -1 窗口句柄为空   -2 无 wl_seat   -3 无可用的输入序列号   -4 取不到 toplevel
int fr_wayland_start_window_move(GLFWwindow* handle, int unmaximize,
                                 int* out_path, unsigned int* out_serial) {
    if (out_path) *out_path = 0;
    if (out_serial) *out_serial = 0;

    _GLFWwindow* window = (_GLFWwindow*) handle;
    if (!window) return -1;
    if (!_glfw.wl.seat) return -2;

    // serial 必须是「发往本客户端的输入事件」的序列号，否则合成器直接丢弃这次
    // 抓取。GLFW 在 pointerHandleButton 里把它更新为按键按下的序列号
    // （wl_window.c:1543）；纯指针移动不会改它，所以按下→拖动期间它仍然有效。
    const uint32_t serial = _glfw.wl.serial;
    if (out_serial) *out_serial = serial;
    if (serial == 0) return -3;

    int path = 0;
    struct xdg_toplevel* toplevel = fr_wl_toplevel(window, &path);
    if (out_path) *out_path = path;
    if (!toplevel) return -4;

    if (unmaximize) xdg_toplevel_unset_maximized(toplevel);
    xdg_toplevel_move(toplevel, _glfw.wl.seat, serial);
    return 0;
}

// 查询 Wayland 连接是否仍然健康（d91）。
//
// 用途：GLFW 的 close 回调无法区分「用户真的要关窗」与「连接已断」—— 后者在
// wl_window.c:1235-1247 同样会被伪造成一次关闭请求（flushDisplay() 失败时 GLFW 对
// 全部窗口发一遍）。若不分青红皂白就把 should_close 清回 FALSE，连接已死的进程会变成
// 「再也显示不出来的僵尸窗口」。故清标记前先问这里。
//
// 返回 1 = 连接健康（可安全清除 should_close）；0 = 句柄为空 / 无连接 / 连接已报错。
//
// ⚠️ 为什么不能直接调 wl_display_get_error：
// GLFW 刻意**不静态链接** libwayland-client，而是运行时 dlopen（wl_init.c:514），
// 并把用到的入口 #define 成 _glfw.wl.client.* 函数指针（wl_platform.h:70-77）——
// 这样没装 wayland 的机器还能退到 X11。本工程同理不链它，所以直接用裸符号名会在
// 链接期报 "DSO missing from command line"（2026-09-17 实测踩到）。
// 而 GLFW 自己**没有**加载 get_error（不在它的清单里），于是我们按它的做法从同一个
// 模块句柄惰性取符号，缓存复用。
typedef int (*fr_pfn_wl_display_get_error)(struct wl_display* display);
static fr_pfn_wl_display_get_error fr_wl_get_error = NULL;

int fr_wayland_display_alive(GLFWwindow* handle) {
    if (!handle) return 0;
    if (!_glfw.wl.display) return 0;
    if (!fr_wl_get_error) {
        if (!_glfw.wl.client.handle) return 0;
        fr_wl_get_error = (fr_pfn_wl_display_get_error)
            _glfwPlatformGetModuleSymbol(_glfw.wl.client.handle, "wl_display_get_error");
        if (!fr_wl_get_error) return 0;   // 极端老旧 libwayland：按「健康」处理，交给去抖兜底
    }
    return fr_wl_get_error(_glfw.wl.display) == 0 ? 1 : 0;
}
