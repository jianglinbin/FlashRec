#include "platform/window_glfw.h"

#include <chrono>   // d91：隐窗前等待在途交换的退避
#include <thread>   // d91

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#elif defined(__linux__)
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3native.h>   // 含 X11/Xlib.h（XQueryPointer 需要）
#undef None                     // Xlib.h 把 None 定义成 0L 宏，会破坏 WinEdge::None
#endif

#include "app/log.h"
#include "platform/paths.h"

// stb_image 声明模式（实现在 nanovg.c；必须放头文件包含区，且不带实现宏）
#include "stb_image.h"

namespace fr {

namespace {

// —— d90：Wayland 原生窗口移动桥接（定义在 glfw_wayland_move.c）——
// 该文件被追加进 glfw 目标编译（见 cmake/deps.cmake），因为它必须访问 GLFW 内部
// 状态（_glfw.wl / _GLFWwindow::wl），而内部头不对 flashrec 目标开放。
// 返回 0 = 已交给合成器；负数 = 未执行（-1 空句柄 -2 无 seat -3 无输入序列号
// -4 取不到 toplevel）。仅主线程可调。
#ifdef FLASHREC_HAVE_WL_MOVE
extern "C" int fr_wayland_start_window_move(GLFWwindow* handle, int unmaximize,
                                           int* out_path, unsigned int* out_serial);
// d91：连接健康探测（同一个 .c 文件里的第二个导出，故共用这个编译开关）。
// 返回 1 = 连接健康；0 = 无连接 / 连接已报错。
extern "C" int fr_wayland_display_alive(GLFWwindow* handle);
#endif

// GLFW 实际选中的后端名（诊断用）。Wayland 与 X11 的能力差异极大 —— 例如
// 「窗口不能移动」在 Wayland 后端是协议级限制（客户端无权定位窗口），
// 排查时必须先知道真实后端，否则会对着 X11 的代码猜 Wayland 的问题。
const char* glfw_platform_name() {
  switch (glfwGetPlatform()) {
    case GLFW_PLATFORM_WIN32:   return "win32";
    case GLFW_PLATFORM_COCOA:   return "cocoa";
    case GLFW_PLATFORM_WAYLAND: return "wayland";
    case GLFW_PLATFORM_X11:     return "x11";
    case GLFW_PLATFORM_NULL:    return "null";
    default:                    return "unknown";
  }
}

#ifdef _WIN32
// —— 无边框窗口的边缘缩放（两件事，缺一不可）——
// 前置条件：窗口必须带 WS_THICKFRAME。GLFW 的 DECORATED=false 建出的是纯 WS_POPUP，
//   没有 thick frame；此时即便 WM_NCHITTEST 正确返回 HTLEFT，DefWindowProc 在
//   WM_NCLBUTTONDOWN 阶段找不到可调整边框，不会进入系统缩放循环 → 表现为"命中判定
//   正确却完全拖不动"。故 create() 里补 WS_THICKFRAME | WS_MAXIMIZEBOX。
// 命中判定：WS_THICKFRAME 自带的非客户区只在有边框时有效，对无边框窗口无意义，
//   因此仍需子类化窗口过程，在 WM_NCHITTEST 里按屏幕坐标落在窗口边缘哪一带，
//   返回对应 HTLEFT / HTTOPRIGHT / … 主动把缩放意图告诉系统。
// 副作用处理：带 WS_THICKFRAME 后系统会为非客户区预留尺寸，故在 WM_NCCALCSIZE 里
//   声明客户区覆盖整个窗口矩形，消除边框厚度导致的四周缝隙。
// 平台相关代码只准在本文件（AGENTS.md 规则 5）。
constexpr int kResizeBand = 6;     // 直边命中带宽（物理像素）
// 角落 45° 双边拖拽区。系统默认角落判定 = 横带 ∩ 纵带（即 band×band 的小方块），
// 只有 6×6，实际极难命中 —— 用户反馈"很难进入 45 度角拖拽"。
// 这里把角落扩成 band×corner 的矩形：只要贴着任一条边、且距角不超过 corner，
// 就按对角方向缩放。这样整个圆角弧线覆盖的范围都是双边拖拽，符合直觉。
// 取 3×band：太小仍难命中，太大则贴边拖拽时会被角落"抢走"（想拉左边却变成拉左上）。
constexpr int kCornerBand = kResizeBand * 3;

// d117：外部正常退出协议。托盘语义（on_close_request 返回 true = 已处理）会同时
// 吞掉 WM_CLOSE 与 GLFW 对 WM_QUIT 的 close-request 转换（glfwSetWindowCloseCallback
// 的回调把 should_close 清回），二者均无法触发真退出 —— 外部工具（tools/quit_app.py）
// 只能经注册消息进入「托盘菜单退出」同款路径。RegisterWindowMessage 按消息名注册，
// 同名字符串在系统内返回同一消息号，跨进程对齐无需硬编码。
UINT quit_request_msg() {
  static const UINT msg = RegisterWindowMessageW(L"FlashRec.RequestQuit");
  return msg;
}

LRESULT CALLBACK resize_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  auto* self = reinterpret_cast<WindowGLFW*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  WNDPROC prev = self ? reinterpret_cast<WNDPROC>(self->prev_wndproc()) : nullptr;

  // 客户区铺满整个窗口矩形：让 WS_THICKFRAME 不产生可见边框/缝隙（无边框窗口标准做法）
  if (msg == WM_NCCALCSIZE && wp == TRUE) {
    if (self) {
      if (self->fullscreen()) {
        // 全屏：客户区必须**严丝合缝等于显示器**。这里绝不能交回 DefWindowProc ——
        // 窗口带 WS_THICKFRAME（d14 为边缘缩放补的），DefWindowProc 会按边框宽高
        // 从外框里扣除，于是全屏客户区比显示器小了一圈（120 DPI 下 2560x1440 的外框
        // 只得到 2544x1424，四边各少 8px）→ 表现为"全屏尺寸不正确、四周有缝"。
        return 0;  // 客户区 = 整个窗口矩形（glfwSetWindowMonitor 已把窗口设为显示器尺寸）
      }
      // 最大化：直接 return 0（客户区 = 整个窗口矩形）。实测（tools/maximize_probe.py，
      // dpi=120）：WS_POPUP+THICKFRAME 无边框窗口最大化后**外框 = 恰好工作区大小，
      // 不向四边外扩**，旧代码在此按 frame 厚度收缩客户区反而制造出四周 9px 的缝
      // （d25 用户截图 + 探针实锤：客户区四周各差 9px）。即便个别系统外扩，多出的
      // 客户区也延伸到屏幕外，同样无可见缝隙。
      return 0;  // 客户区 = 整个窗口矩形
    }
  }

  // d39：边缘/拖动命中已统一为跨平台手动拖拽循环（main.cpp 状态机 + hit_window_edge）。
  // 全部命中交回客户区（HTCLIENT）——系统缩放/HTCAPTION 拖动退役；THICKFRAME 保留
  // （NCCALCSIZE 铺缝与 DWM 阴影依赖它）。代价：Aero Snap（拖到屏幕边半屏）随之失效。
  if (msg == WM_NCHITTEST) return HTCLIENT;

  // —— 交互式拖动调整大小的区间标记 ——
  // 拖动期间系统进入模态循环，主线程的消息泵被占住；渲染线程据此降级
  // （冻结 FBO 重建 + 暂停 mpv 重渲，用旧纹理拉伸），避免每帧做重活。
  if (self && msg == WM_ENTERSIZEMOVE) self->set_resizing(true);
  if (self && msg == WM_EXITSIZEMOVE) self->set_resizing(false);
  if (self && (msg == WM_SIZE || msg == WM_DPICHANGED)) self->mark_size_dirty();
  // d74 剪贴板变更通知（AddClipboardFormatListener 注册后生效；纯通知，
  // 文本读取在回调里做——回调由 clipboard 模块经 on_clipboard_update 登记）
  if (self && msg == WM_CLIPBOARDUPDATE) {
    if (self->on_clipboard_update) self->on_clipboard_update();
    return 0;
  }
  // d81 托盘：系统关闭请求（Alt+F4/关机广播）。回调可拒绝关闭（隐到托盘）。
  if (self && msg == WM_CLOSE && self->on_close_request) {
    if (self->on_close_request()) return 0;  // 已处理（如隐到托盘），不走默认关闭
  }
  // d117：外部正常退出协议（tools/quit_app.py 注册消息）→ on_quit_request
  // 执行托盘「退出」同款（隐窗 + 置 should_close），走完整收尾链后真退出。
  if (self && msg == quit_request_msg() && self->on_quit_request) {
    self->on_quit_request();
    return 0;
  }

  if (prev) return CallWindowProcW(prev, hwnd, msg, wp, lp);
  return DefWindowProcW(hwnd, msg, wp, lp);
}
#endif

// 找包含屏幕点 (cx, cy) 的显示器；找不到回退主屏
GLFWmonitor* monitor_containing(int cx, int cy) {
  int n = 0;
  GLFWmonitor** mons = glfwGetMonitors(&n);
  for (int i = 0; i < n; i++) {
    int mx, my;
    glfwGetMonitorPos(mons[i], &mx, &my);
    const GLFWvidmode* vm = glfwGetVideoMode(mons[i]);
    if (cx >= mx && cx < mx + vm->width && cy >= my && cy < my + vm->height) return mons[i];
  }
  return glfwGetPrimaryMonitor();
}

// —— d146 R4：监视器事件桥（GLFW 的 monitor 回调没有 user-pointer 通道，
// 且无窗口参数 —— 单窗口应用用静态弱引用桥到实例）——
WindowGLFW* g_active_window = nullptr;
}  // namespace

// —— d146 R4：多屏信息 + 按屏全屏（三步可用性判定见头文件）——
int WindowGLFW::monitor_count() {
  int n = 0;
  glfwGetMonitors(&n);
  return n;
}

bool WindowGLFW::monitor_info(int idx, MonitorInfo* out) {
  int n = 0;
  GLFWmonitor** mons = glfwGetMonitors(&n);
  if (idx < 0 || idx >= n || !out) return false;
  GLFWmonitor* m = mons[idx];
  // 步骤 ①：在枚举列表里（能走到这里即在）
  int x = 0, y = 0;
  glfwGetMonitorPos(m, &x, &y);
  // 步骤 ②：视频模式非空且 w/h > 0（黑屏休眠但被驱动保留的输出此处仍通过——近似边界）
  const GLFWvidmode* vm = glfwGetVideoMode(m);
  if (!vm || vm->width <= 0 || vm->height <= 0) return false;
  // 步骤 ③：工作区非退化（排除"被系统禁用但残留枚举"的怪态）
  int ax = 0, ay = 0, aw = 0, ah = 0;
  glfwGetMonitorWorkarea(m, &ax, &ay, &aw, &ah);
  if (aw <= 0 || ah <= 0) return false;
  out->idx = idx;
  out->x = x; out->y = y; out->w = vm->width; out->h = vm->height;
  out->ax = ax; out->ay = ay; out->aw = aw; out->ah = ah;
  out->primary = glfwGetPrimaryMonitor() == m;
  snprintf(out->fp, sizeof(out->fp), "%d,%d %dx%d %d,%d %dx%d", x, y, vm->width, vm->height,
           ax, ay, aw, ah);
  return true;
}

int WindowGLFW::monitor_index_of_window() const {
  if (!win_) return -1;
  int wx = 0, wy = 0, ww = 0, wh = 0;
  pos_size(&wx, &wy, &ww, &wh);
  const int cx = wx + ww / 2, cy = wy + wh / 2;
  int n = 0;
  GLFWmonitor** mons = glfwGetMonitors(&n);
  for (int i = 0; i < n; i++) {
    int mx = 0, my = 0;
    glfwGetMonitorPos(mons[i], &mx, &my);
    const GLFWvidmode* vm = glfwGetVideoMode(mons[i]);
    if (!vm) continue;
    if (cx >= mx && cx < mx + vm->width && cy >= my && cy < my + vm->height) return i;
  }
  return -1;
}

void WindowGLFW::enter_fullscreen_on(int mon_idx) {
  if (!win_ || mon_idx < 0) return;
  int n = 0;
  GLFWmonitor** mons = glfwGetMonitors(&n);
  if (mon_idx >= n) return;
  GLFWmonitor* mon = mons[mon_idx];
  if (glfwGetWindowMonitor(win_) == mon) return;  // 已在该屏全屏：no-op（不闪屏不重切）
  if (!glfwGetWindowMonitor(win_)) {
    // 窗口态：先保存还原矩形（同 toggle_fullscreen 语义）
    glfwGetWindowPos(win_, &fs_sx_, &fs_sy_);
    glfwGetWindowSize(win_, &fs_sw_, &fs_sh_);
  }
  const GLFWvidmode* vm = glfwGetVideoMode(mon);
  if (!vm) return;
  glfwSetWindowMonitor(win_, mon, 0, 0, vm->width, vm->height, vm->refreshRate);
}

WindowGLFW::~WindowGLFW() { destroy(); }

// begin_drag 已退役（d39）：统一手动拖拽循环在 main.cpp，纯 GLFW API 实现。

// —— 统一窗口拖拽的边缘命中（d39；直边 6px 带、贴角 18px 正方形）——
// 角落必须用正方形（两条边距都在 kCorner 内）而非"贴边+距角扩展"的 L 形：
// L 形在角内部（距两直边各 7~17px）是死区 —— 用户实测"四角不触发、要找的小块
// 很难摸到"。18px 与视觉圆角半径 windowRadius=12 对齐，整段弧线覆盖在内。
WindowGLFW::WinEdge WindowGLFW::hit_window_edge(int cw, int ch, float mx, float my) {
  constexpr float kBand = 6.f, kCorner = 18.f;
  const float d_l = mx, d_r = cw - mx, d_t = my, d_b = ch - my;
  // 四角优先于四边（正方形判定，角落内部无死区）
  if (d_l < kCorner && d_t < kCorner) return WinEdge::NW;
  if (d_r < kCorner && d_t < kCorner) return WinEdge::NE;
  if (d_l < kCorner && d_b < kCorner) return WinEdge::SW;
  if (d_r < kCorner && d_b < kCorner) return WinEdge::SE;
  if (d_l < kBand) return WinEdge::W;
  if (d_r < kBand) return WinEdge::E;
  if (d_t < kBand) return WinEdge::N;
  if (d_b < kBand) return WinEdge::S;
  return WinEdge::None;
}

// 按边缘带换光标（GLFW 3.4 标准形状含斜角双箭头；None = 恢复默认箭头）
void WindowGLFW::update_cursor_for_edge(WinEdge e) {
  static GLFWcursor* cur_w = nullptr;     // ↔（随进程存活，无需销毁）
  static GLFWcursor* cur_v = nullptr;     // ↕
  static GLFWcursor* cur_nwse = nullptr;  // ⤡ NW/SE
  static GLFWcursor* cur_nesw = nullptr;  // ⤢ NE/SW
  GLFWcursor* cur = nullptr;
  switch (e) {
    case WinEdge::E:
    case WinEdge::W:
      if (!cur_w) cur_w = glfwCreateStandardCursor(GLFW_HRESIZE_CURSOR);
      cur = cur_w;
      break;
    case WinEdge::N:
    case WinEdge::S:
      if (!cur_v) cur_v = glfwCreateStandardCursor(GLFW_VRESIZE_CURSOR);
      cur = cur_v;
      break;
    case WinEdge::NW:
    case WinEdge::SE:
      if (!cur_nwse) cur_nwse = glfwCreateStandardCursor(GLFW_RESIZE_NWSE_CURSOR);
      cur = cur_nwse;
      break;
    case WinEdge::NE:
    case WinEdge::SW:
      if (!cur_nesw) cur_nesw = glfwCreateStandardCursor(GLFW_RESIZE_NESW_CURSOR);
      cur = cur_nesw;
      break;
    case WinEdge::None:
      break;
  }
  glfwSetCursor(win_, cur);
}

// 全局光标屏幕坐标（d39：统一拖拽锚定。X11 上 glfwGetWindowPos 在异步 move
// 期间读到旧值，客户区+窗口位置换算会漂移；全局光标与窗口位置无关，完全免疫）
bool WindowGLFW::global_cursor(int* gx, int* gy) {
#ifdef _WIN32
  POINT p;
  if (!GetCursorPos(&p)) return false;   // 物理像素（app 为 per-monitor DPI aware）
  *gx = p.x; *gy = p.y;
  return true;
#elif defined(__linux__)
  Display* d = glfwGetX11Display();
  if (!d) return false;
  Window root = DefaultRootWindow(d);
  Window ret_root, ret_child;
  int rx, ry, wx, wy;
  unsigned int mask;
  if (!XQueryPointer(d, root, &ret_root, &ret_child, &rx, &ry, &wx, &wy, &mask))
    return false;                        // 光标不在本屏幕（多屏 XWayland 等）
  *gx = rx; *gy = ry;                    // X 根窗口坐标 == GLFW 窗口坐标空间
  return true;
#else
  return false;                          // macOS 后续接入（NSEvent mouseLocation）
#endif
}

void* WindowGLFW::proc_adapter(void* user, const char* name) {
  return reinterpret_cast<GetProcFn>(user)(name);
}

// —— d68 §6：程序图标装载（Win=任务栏/Alt-Tab；X11=_NET_WM_ICON；Wayland 自动忽略）——
// stb_image 只做声明（STB_IMAGE_IMPLEMENTATION 定义在 nanovg.c，C 链接符号共享，
// 再定义一份实现 = 重复链接符号）。资产缺失静默降级：图标失败不影响启动。
static void load_window_icon(GLFWwindow* win) {
  static const char* const kIcons[] = {"icons/app_16.png", "icons/app_32.png",
                                       "icons/app_48.png", "icons/app_256.png"};
  GLFWimage imgs[4];
  int n = 0;
  for (const char* rel : kIcons) {
    int w = 0, h = 0, comp = 0;
    stbi_uc* px = stbi_load(paths::asset_file(rel).c_str(), &w, &h, &comp, 4);
    if (!px) continue;
    imgs[n].width = w;
    imgs[n].height = h;
    imgs[n].pixels = px;
    ++n;
  }
  if (n > 0) {
    glfwSetWindowIcon(win, n, imgs);
    FR_LOG_INFO("[UI] 窗口图标已装载（{} 个尺寸）", n);
  } else {
    FR_LOG_WARN("[UI] 未找到窗口图标资产（assets/icons/app_*.png），任务栏用默认图标");
  }
  for (int i = 0; i < n; ++i) stbi_image_free(imgs[i].pixels);
}

#ifdef __linux__
// d93：Wayland app_id 常量（唯一真值）。改这里必须同步改
// assets/linux/<同名>.desktop.in 与 cmake/linux_desktop.cmake 的 FLASHREC_APP_ID ——
// 三处不一致 = 任务栏退回通用图标（GNOME 按 app_id ↔ 桌面项文件名匹配）。
constexpr const char* kWaylandAppId = "com.flashrec.FlashRec";
#endif

bool WindowGLFW::create(int w, int h, const char* title) {
#ifdef __linux__
  // d90：本应用无边框、标题栏自绘，不需要 libdecor 的装饰；而 GLFW 默认
  // GLFW_WAYLAND_PREFER_LIBDECOR（init.c:67）—— 一旦 libdecor 在场，窗口的
  // xdg_toplevel 就由 libdecor 持有，GLFW 侧 window->wl.xdg.toplevel 恒为 NULL，
  // 原生移动要多绕一层反查（见 glfw_wayland_move.c）。显式回裸 xdg-shell，
  // 让 xdg_toplevel 归 GLFW 直管。实测 Ubuntu 26.04 桌面默认装有
  // libdecor-0-0 + libdecor-0-plugin-1-gtk，不关掉必然走 libdecor 路径。
  // 必须在 glfwInit() 之前设置（init hint 只在初始化时读取一次）。
  glfwInitHint(GLFW_WAYLAND_LIBDECOR, GLFW_WAYLAND_DISABLE_LIBDECOR);
#endif
  if (!glfwInit()) {
    FR_LOG_ERROR("[UI] glfwInit 失败");
    return false;
  }
  // d90：把真实后端打进日志。历史教训 —— "Linux 不能移动窗口"排查时全靠推断后端，
  // 白烧了整轮；后端名一行日志就能定案。
  FR_LOG_INFO("[UI] GLFW 后端 = {}（编译期含 wayland + x11 时由平台探测决定）",
              glfw_platform_name());
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
  glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
  glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);        // 无边框
  glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);  // 透明（圆角合成）
  glfwWindowHint(GLFW_DOUBLEBUFFER, GLFW_TRUE);
  // d39：全屏失焦**不**自动最小化。glfwSetWindowMonitor 真全屏下 GLFW 默认
  // AUTO_ICONIFY=TRUE——双屏场景点到另一块屏，播放器全屏窗失焦即被最小化
  // （用户实测"另一个屏幕操作会导致播放器不显示"）。关掉它，全屏窗保持原样，
  // 电视节目照常播，另一块屏随便操作。
  glfwWindowHint(GLFW_AUTO_ICONIFY, GLFW_FALSE);
#ifdef __linux__
  // d93：Wayland app_id —— 桌面/任务栏的图标、名字、归组全由它决定。
  // 合成器拿 app_id 去匹配同名桌面项 <app_id>.desktop，图标只能从那里来；
  // 而 d68 §6 的 glfwSetWindowIcon 在 Wayland 后端是**空操作**（只有 Win32 任务栏
  // 和 X11 的 _NET_WM_ICON 生效）—— 这就是"图标设了却仍是通用图标"的原因。
  // 不设此 hint 时 GLFW 把 app_id 留成空串（wl_window.c:1041 strdup 默认 "",
  // 而 :938 只在非空指针时才发 xdg_toplevel_set_app_id）⇒ 匹配不到任何桌面项。
  // ⚠️ 必须与随包桌面项同名：assets/linux/com.flashrec.FlashRec.desktop.in
  glfwWindowHintString(GLFW_WAYLAND_APP_ID, kWaylandAppId);
  // 与 d90 的「后端名」同理：app_id 匹配失败是**静默**的（只表现为通用图标），
  // 把它打进日志，日后图标类问题一行定案。
  FR_LOG_INFO("[UI] Wayland app_id = {}（任务栏/坞图标按它匹配同名桌面项）",
              kWaylandAppId);
#endif

  win_ = glfwCreateWindow(w, h, title, nullptr, nullptr);
  if (!win_) {
    FR_LOG_ERROR("[UI] 窗口创建失败（透明帧缓冲可能不被桌面支持）");
    // 退化重试：无透明
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_FALSE);
    win_ = glfwCreateWindow(w, h, title, nullptr, nullptr);
    if (!win_) {
      glfwTerminate();
      return false;
    }
  }
  glfwMakeContextCurrent(win_);
  glfwSwapInterval(1);  // vsync

#ifdef _WIN32
  // 无边框窗口的边缘缩放：GLFW 不提供，只有子类化 WM_NCHITTEST 一条路
  if (HWND hwnd = glfwGetWin32Window(win_)) {
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    wndproc_old_ = reinterpret_cast<void*>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(resize_wndproc)));

    // 关键：WS_POPUP 没有 thick frame，DefWindowProc 不会进入缩放循环。
    // 补上 WS_THICKFRAME 才能让 WM_NCHITTEST 返回的 HTLEFT 等码真正生效；
    // WS_MAXIMIZEBOX 需要 WS_CAPTION 或 WS_THICKFRAME 之一才被系统承认。
    // 二者都不产生可见边框/标题栏（非客户区由 WM_NCCALCSIZE 归零、外观仍由自绘接管）。
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    SetWindowLongPtrW(hwnd, GWL_STYLE,
                      style | WS_THICKFRAME | WS_MAXIMIZEBOX);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
  }
#endif

  glfwSetWindowUserPointer(win_, this);
  load_window_icon(win_);  // d68 §6：任务栏 / Alt-Tab 图标（放在回调注册前后皆可）
  glfwSetCursorPosCallback(win_, [](GLFWwindow* w, double x, double y) {
    auto* self = static_cast<WindowGLFW*>(glfwGetWindowUserPointer(w));
    if (self && self->on_mouse_move) self->on_mouse_move(x, y);
  });
  glfwSetMouseButtonCallback(win_, [](GLFWwindow* w, int button, int action, int) {
    auto* self = static_cast<WindowGLFW*>(glfwGetWindowUserPointer(w));
    if (self && self->on_mouse_btn) self->on_mouse_btn(button, action);
  });
  glfwSetKeyCallback(win_, [](GLFWwindow* w, int key, int, int action, int mods) {
    auto* self = static_cast<WindowGLFW*>(glfwGetWindowUserPointer(w));
    if (self && self->on_key) self->on_key(key, action, mods);
  });
  glfwSetScrollCallback(win_, [](GLFWwindow* w, double x, double y) {
    auto* self = static_cast<WindowGLFW*>(glfwGetWindowUserPointer(w));
    if (self && self->on_scroll) self->on_scroll(x, y);
  });
  // 进入/离开窗口（d36：光标显隐前提之一"鼠标在窗口范围内"）
  glfwSetCursorEnterCallback(win_, [](GLFWwindow* w, int entered) {
    auto* self = static_cast<WindowGLFW*>(glfwGetWindowUserPointer(w));
    if (self && self->on_mouse_enter) self->on_mouse_enter(entered == GLFW_TRUE);
  });
  glfwSetWindowFocusCallback(win_, [](GLFWwindow* w, int focused) {
    auto* self = static_cast<WindowGLFW*>(glfwGetWindowUserPointer(w));
    if (self && !focused && self->on_focus_lost) self->on_focus_lost();
  });
  // 模态循环内部唯一能出帧的通道：系统在拖拽/缩放期间回调这里。
  // 主线程收到后唤醒渲染线程（自身不碰 GL）。
  glfwSetWindowRefreshCallback(win_, [](GLFWwindow* w) {
    auto* self = static_cast<WindowGLFW*>(glfwGetWindowUserPointer(w));
    if (self && self->on_refresh) self->on_refresh();
  });
  // d91：系统关闭请求（Alt+F4 / 桌面环境的「关闭·退出」/ 关机广播）。
  // Windows 上走的是我们子类化的 WM_CLOSE（见 resize_wndproc，返回值可直接拒绝关闭），
  // 但 Wayland/X11/macOS 没有 wndproc 这层 —— GLFW 在 _glfwInputWindowCloseRequest
  // （window.c:158）里**先无条件置 should_close、再回调**，且回调签名返回 void，
  // 「我已处理（隐到托盘）」根本无处表达 ⇒ d81 的「关窗 = 隐到托盘」在非 Windows 平台
  // 静默失效（任何系统关闭请求都会让进程连托盘一起退）。这里补上真正的 close 回调，
  // 处理成功时把标记清回去。
  glfwSetWindowCloseCallback(win_, [](GLFWwindow* w) {
    auto* self = static_cast<WindowGLFW*>(glfwGetWindowUserPointer(w));
    if (!self || !self->on_close_request) return;
    if (!self->on_close_request()) return;  // 未处理 → 维持 GLFW 默认（should_close 保持真）
#ifdef FLASHREC_HAVE_WL_MOVE
    // 连接已死时 GLFW 也会伪造关闭请求（flushDisplay 失败分支），且此后每次 poll
    // 都重发一次。两道判别，任一命中就放行 should_close 让进程正常退出：
    //   ① 连接已报错（协议错误必然记在 display 上）；
    //   ② 未经重新显示又来一次（真关闭请求在窗已隐后不可能连发，只有连接失效会）。
    if (!fr_wayland_display_alive(w)) {
      FR_LOG_ERROR("[UI] Wayland 连接不可用，关闭请求按退出处理（不稳窗）");
      return;
    }
    if (self->close_handled_) {
      FR_LOG_ERROR("[UI] 连续关闭请求且未重新显示 ⇒ 判定连接已失效，退出");
      return;
    }
#endif
    self->close_handled_ = true;
    glfwSetWindowShouldClose(w, GLFW_FALSE);  // 撤销 GLFW 的无条件置位
  });
  return true;
}

void WindowGLFW::set_cursor_visible(bool on) {
  if (!win_) return;
  // HIDDEN 只是视觉隐藏：光标移动/点击事件照常投递（唤醒链路不受影响）
  glfwSetInputMode(win_, GLFW_CURSOR, on ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_HIDDEN);
}

void WindowGLFW::release_context() {
  if (win_) glfwMakeContextCurrent(nullptr);
}

void WindowGLFW::make_current() const {
  if (win_) glfwMakeContextCurrent(win_);
}

void WindowGLFW::set_swap_interval(int interval) const {
  if (win_) glfwSwapInterval(interval);
}

void WindowGLFW::wait_events_timeout(double seconds) const {
  glfwWaitEventsTimeout(seconds);
}

void WindowGLFW::destroy() {
  if (win_) {
    if (g_active_window == this) g_active_window = nullptr;  // d146：撤监视器事件桥
#ifdef _WIN32
    // 还原原窗口过程再销毁（顺序反了会让 GLFW 的清理走到已失效的指针上）
    if (wndproc_old_) {
      if (HWND hwnd = glfwGetWin32Window(win_))
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(wndproc_old_));
      wndproc_old_ = nullptr;
    }
#endif
    glfwDestroyWindow(win_);
    win_ = nullptr;
    glfwTerminate();
  }
}

bool WindowGLFW::should_close() const { return win_ && glfwWindowShouldClose(win_); }

void WindowGLFW::swap_buffers() {
  if (!win_) return;
  // d91：隐窗期间禁止交换。见头文件 hide_window 的长注释 —— 一次落在隐窗之后的
  // 缓冲提交就会让表面变成「有已提交缓冲 + 无 xdg 角色」，下次显示窗口即被合成器
  // 以协议错误 4 掐断连接。注意：这里早退与 GLFW 内部「隐藏窗不交换」的守卫
  // （egl_context.c: swapBuffersEGL）意图相同，但闸门是主线程在下达隐窗**之前**
  // 就关上的，因此能封住「已越过后者的判断、正在 eglSwapBuffers 里」的在途交换。
  if (swap_gate_closed_.load(std::memory_order_acquire)) return;
  swap_in_flight_.store(true, std::memory_order_release);
  glfwSwapBuffers(win_);
  swap_in_flight_.store(false, std::memory_order_release);
}

// d91：隐窗 —— 关闸 → 等在途交换退出临界区 → glfwHideWindow。闸门保持关闭到 show_window()。
void WindowGLFW::hide_window() {
  if (!win_) return;
  swap_gate_closed_.store(true, std::memory_order_release);
  // 退避等待（上限 100ms）。vsync 下 eglSwapBuffers 本身可能阻塞到下一个垂直回扫
  // （最长 ~16.7ms @60Hz），故不能零等待；上限则保证「万一渲染线程卡在别处」时
  // 主线程只多等 100ms 就照常隐窗，不会挂住 UI。
  for (int i = 0; i < 1000 && swap_in_flight_.load(std::memory_order_acquire); ++i)
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  if (swap_in_flight_.load(std::memory_order_acquire))
    FR_LOG_WARN("[UI] 隐窗前等待在途交换超时（100ms），仍继续隐窗");
  glfwHideWindow(win_);
}

// d91：显窗 —— 先开窗（角色在 glfwShowWindow → createShellObjects 里重建），再放行渲染。
// 顺序不能反：开闸后再 show 的话，渲染线程可能在角色重建之前就提交缓冲，又回到同一个坑。
void WindowGLFW::show_window() {
  if (!win_) return;
  glfwShowWindow(win_);
  close_handled_ = false;      // 重新显示 = 新一轮关闭请求去抖的起点
  swap_gate_closed_.store(false, std::memory_order_release);
  // d92：显窗必须强制一次整窗重绘。_glfwShowWindowWayland（wl_window.c:2423-2430）
  // 只重建 xdg 角色、**不提交任何缓冲** —— 合成器手里没内容就画不出窗口；而渲染
  // 线程在按需渲染下把静默帧判为「无脏区」直接 return（render_loop.cpp:720），
  // 唯一的自愈通道是 2s 心跳，且 `last_keepalive_ = now` 在出帧路径（同文件:1234）
  // —— 隐窗期间被闸门丢弃的那些白画帧照样把它归零。净效果：显窗后随机等 0~2s
  // 窗口才露面（播放中有 mpv 帧、鼠标恰好在窗口区时才会秒出）。
  // 这里交给上层置 force_dirty（归因表 forced ⇒ add_all）并唤醒渲染线程，
  // 延迟约一帧；闸门已在上一行打开，故这一帧真能提交。所有显窗点都走本函数。
  if (on_shown) on_shown();
}

void WindowGLFW::poll_events() { glfwPollEvents(); }

int WindowGLFW::width() const {
  int w = 0, h = 0;
  if (win_) glfwGetFramebufferSize(win_, &w, &h);
  return w;
}

int WindowGLFW::height() const {
  int w = 0, h = 0;
  if (win_) glfwGetFramebufferSize(win_, &w, &h);
  return h;
}

float WindowGLFW::content_scale() const {
  // 当前 UI 的坐标系 = 未缩放 framebuffer 物理像素（无 DPI 缩放渲染），交互阈值
  // 与 GetSystemMetrics 同口径、无需折算 —— 本访问器暂为预留（d68）。
  float xs = 1.f, ys = 1.f;
  if (win_) glfwGetWindowContentScale(win_, &xs, &ys);
  return xs > 0.f ? xs : 1.f;
}

bool WindowGLFW::maximized() const {
  return win_ && glfwGetWindowAttrib(win_, GLFW_MAXIMIZED) == GLFW_TRUE;
}

void WindowGLFW::toggle_maximized() {
  if (!win_) return;
  if (glfwGetWindowAttrib(win_, GLFW_MAXIMIZED) == GLFW_TRUE)
    glfwRestoreWindow(win_);
  else
    glfwMaximizeWindow(win_);
}

bool WindowGLFW::fullscreen() const {
  return win_ && glfwGetWindowMonitor(win_) != nullptr;
}

void WindowGLFW::toggle_fullscreen() {
  if (!win_) return;
  if (glfwGetWindowMonitor(win_)) {
    glfwSetWindowMonitor(win_, nullptr, fs_sx_, fs_sy_, fs_sw_, fs_sh_, 0);
  } else {
    glfwGetWindowPos(win_, &fs_sx_, &fs_sy_);
    glfwGetWindowSize(win_, &fs_sw_, &fs_sh_);
    GLFWmonitor* mon = monitor_containing(fs_sx_ + fs_sw_ / 2, fs_sy_ + fs_sh_ / 2);
    const GLFWvidmode* vm = glfwGetVideoMode(mon);
    glfwSetWindowMonitor(win_, mon, 0, 0, vm->width, vm->height, vm->refreshRate);
  }
}

void WindowGLFW::set_floating(bool on) {
  if (win_) glfwSetWindowAttrib(win_, GLFW_FLOATING, on ? GLFW_TRUE : GLFW_FALSE);
}

void WindowGLFW::set_window_size(int w, int h) {
  if (win_) glfwSetWindowSize(win_, w, h);
}

void WindowGLFW::set_pos(int x, int y) {
  if (win_) glfwSetWindowPos(win_, x, y);
}

// d90：把「移动窗口」这件事交给合成器（Wayland 唯一可行路径）。
// 见头文件说明；调用成功后调用方必须跳过本地 set_pos 跟手。
bool WindowGLFW::begin_system_move(bool unmaximize) {
  if (!win_) return false;
#ifdef FLASHREC_HAVE_WL_MOVE
  // 只对 Wayland 后端有意义：X11/Win32 有真正的 glfwSetWindowPos，本地跟手更精确
  // 且不受合成器抓取时序影响。同时这也是一道安全闸 —— 底层的 _glfw 全局只在
  // Wayland 后端下才是 wl_* 结构，后端不符时读它会取到另一套平台的字段。
  if (glfwGetPlatform() != GLFW_PLATFORM_WAYLAND) return false;

  int path = 0;
  unsigned int serial = 0;
  const int rc = fr_wayland_start_window_move(win_, unmaximize ? 1 : 0, &path, &serial);
  if (rc == 0) {
    FR_LOG_INFO("[UI] 窗口移动已交给合成器（toplevel 取自 {}，serial={}{}）",
                path == 1 ? "libdecor 反查" : "xdg.toplevel", serial,
                unmaximize ? "，先请求还原" : "");
    return true;
  }
  FR_LOG_WARN("[UI] Wayland 原生移动未执行 rc={} serial={} → 回退本地跟手"
              "（-1 空句柄 -2 无 seat -3 无输入序列号 -4 取不到 toplevel）",
              rc, serial);
  return false;
#else
  (void)unmaximize;   // 非 Wayland 构建（Windows / macOS）：无此能力
  return false;
#endif
}

void WindowGLFW::pos_size(int* x, int* y, int* w, int* h) const {
  if (!win_) return;
  glfwGetWindowPos(win_, x, y);
  glfwGetWindowSize(win_, w, h);
}

void WindowGLFW::move_to_workarea_corner(int w, int h) {
  if (!win_) return;
  int wx, wy, ww, wh;
  glfwGetWindowPos(win_, &wx, &wy);
  glfwGetWindowSize(win_, &ww, &wh);
  GLFWmonitor* mon = monitor_containing(wx + ww / 2, wy + wh / 2);
  int ax, ay, aw, ah;
  glfwGetMonitorWorkarea(mon, &ax, &ay, &aw, &ah);
  glfwSetWindowPos(win_, ax + aw - w, ay + ah - h);
}

}  // namespace fr
