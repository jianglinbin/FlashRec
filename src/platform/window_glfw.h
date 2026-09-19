#pragma once
// GLFW 窗口封装（无边框、透明、GL 3.3 core）。平台相关归 platform（规则 5）。
#include <atomic>
#include <functional>
#include <string>

struct GLFWwindow;

namespace fr {

// R4（d146）：监视器快照 + 三步可用性判定的产物。
// ★指纹当身份（GLFW 源码级证据：Win32 名来自 EnumDisplayDevices 重插会变、
// Wayland 初始为空、X11 RandR 名尚可）——绝不用名字当唯一身份。
struct MonitorInfo {
  int idx = -1;              // glfwGetMonitors 枚举下标
  int x = 0, y = 0, w = 0, h = 0;       // 当前视频模式 + 位置
  int ax = 0, ay = 0, aw = 0, ah = 0;   // 工作区（避开任务栏）
  bool primary = false;
  char fp[160] = {};         // 指纹 "pos mode workarea"（记忆键）
};

// d186：显示器电源状态（DDC/CI 尽力而为）。Off/Unknown 的区别决定是否排除该屏。
enum class MonitorPower { On, Off, Unknown };

class WindowGLFW {
 public:
  ~WindowGLFW();
  bool create(int w, int h, const char* title);  // 必须主线程
  void destroy();

  bool should_close() const;
  void swap_buffers();
  void poll_events();

  // —— d91：隐窗 / 显窗的唯一入口（Wayland 竞态防护）——
  // 禁止再直接调 glfwHideWindow / glfwShowWindow：Wayland 下隐窗会销毁 xdg 角色，
  // 若渲染线程恰在 eglSwapBuffers 中（已越过后者的 wl.visible 守卫），它的
  // attach(buffer)+commit 会落在隐窗的 attach(NULL)+commit 之后 ⇒ 表面处于
  // 「有已提交缓冲 + 无角色」的非法状态 ⇒ 下次 show 的 get_xdg_surface 被 Mutter
  // 以 xdg_wm_base error 4 拒绝并掐断连接 ⇒ 进程连托盘一起退（2026-09-17 真机实证）。
  // 这两个包装负责在隐窗前关闭交换闸门并等在途交换退出临界区。仅主线程可调。
  void hide_window();
  void show_window();

  // —— 渲染线程化支撑 ——
  // GL 上下文跨线程迁移（GLFW 要求：先在旧线程 make null，再在新线程 make current）
  void release_context();                    // 主线程：glfwMakeContextCurrent(nullptr)
  void make_current() const;                 // 渲染线程：glfwMakeContextCurrent(win_)
  void set_swap_interval(int interval) const;
  // 唤醒（阻塞版）：主线程空闲时用；超时用于周期性巡检
  void wait_events_timeout(double seconds) const;
  // ⚠️ d94 已删除 post_empty_event()（原 glfwPostEmptyEvent）。它在 Wayland 下
  // 等价于 `wl_display_sync + flush`，而 GLFW 的事件等待（handleEvents）会
  // prepare_read + 读 fd + dispatch **默认队列** —— 默认队列正是主线程
  // wl_display_roundtrip（glfwShowWindow/HideWindow 内部）的接收队列：渲染线程
  // 若在主线程 roundtrip 期间读走并派发了回包，主线程就永久卡在 poll 里。
  // 渲染线程的唤醒一律走 RenderLoop::wake()（自有条件变量，不碰窗口系统）。

  // 当前是否处于"用户拖拽调整窗口大小"中（渲染可据此降级）。
  // Windows: WM_ENTERSIZEMOVE/EXITSIZEMOVE 区间；Linux/macOS: 恒 false
  // （X11/Wayland 无对应信号，且其事件循环本身不阻塞，无需降级）。
  bool interactive_resize() const { return resizing_; }
  // 窗口尺寸刚变化过（消费式：读取后清零）。渲染线程据此决定是否对齐 FBO。
  bool take_size_dirty() {
    const bool d = size_dirty_;
    size_dirty_ = false;
    return d;
  }

  // —— 统一窗口拖拽（d39：全平台一致，纯 GLFW API，无 OS 模态循环）——
  // 窗口边缘/角落命中（客户区坐标）。返回 None = 非边缘带。
  // 几何：直边 6px 带；贴角 18px 正方形（四角优先，角内部无死区）。
  // 调用方自行排除全屏/最大化/画中画。Move 由调用方按顶栏命中自行决定。
  enum class WinEdge { None, N, S, E, W, NE, NW, SE, SW };
  static WinEdge hit_window_edge(int client_w, int client_h, float mx, float my);
  // 按边缘带切换光标（GLFW 标准形状；None = 恢复默认箭头）。拖拽进行中不调用。
  void update_cursor_for_edge(WinEdge e);
  // 全局光标屏幕坐标（X11: XQueryPointer；Windows: GetCursorPos）。
  // 与窗口位置无关——统一拖拽锚定用（X11 上 glfwGetWindowPos 在异步 move
  // 期间读到旧值，客户区+窗口位置换算会漂移；全局光标完全免疫）。
  // 取不到（Wayland 等无全局指针概念的会话）返回 false，调用方回退锚定方案。
  static bool global_cursor(int* gx, int* gy);

  // —— d90：Wayland 原生窗口移动（合成器接管）——
  // Wayland 协议不允许客户端设置窗口位置（该后端把 set_pos 实现成空操作），唯一
  // 正路是请合成器接管一次交互式移动抓取（xdg_toplevel_move）。返回 true = 已交给
  // 合成器，调用方**必须跳过**后续 set_pos 本地跟手（那边在该后端本就无效）；
  // false = 后端不适用（X11/Win32 有真 set_pos，本地跟手更精确）或抓取条件不足
  // （无 seat / 无有效输入序列号 / 取不到 toplevel，均已记日志）。
  // unmaximize = true 时先请求还原：最大化态拖标题栏 = 还原并跟手。
  // ⚠️ 仅主线程可调（底层触碰 GLFW 的 Wayland 全局状态）。
  bool begin_system_move(bool unmaximize = false);

  // —— 窗口形态 ——
  bool maximized() const;          // 最大化中
  void toggle_maximized();         // 最大化 / 还原
  bool fullscreen() const;         // 全屏中（已绑显示器）
  void toggle_fullscreen();        // 全屏切换（保存/恢复原矩形）
  void set_floating(bool on);      // 置顶（画中画）
  void set_window_size(int w, int h);
  void set_pos(int x, int y);
  void pos_size(int* x, int* y, int* w, int* h) const;   // 窗口坐标/尺寸（屏幕坐标）
  void move_to_workarea_corner(int w, int h);            // 工作区（避开任务栏）右下角

  // 每帧坐标
  int width() const;    // framebuffer 物理像素（FBO/GL scissor 用）
  int height() const;   // framebuffer 物理像素
  // d210：UI 逻辑尺寸（屏幕坐标/DIP，glfwGetWindowSize）。绘制坐标一律用它，
  // nanovg 再按 pixel_ratio 放大到物理像素 → 高 DPI 下标题栏/字号自动跟随。
  int logical_width() const;
  int logical_height() const;
  // 物理 / 逻辑 比（= 设备像素比）；逻辑尺寸不可用时回落 content_scale()。
  float pixel_ratio() const;
  // 光标 → DIP 的换算系数 = logical_width / glfwGetWindowSize().width：
  //   · Windows：窗口尺寸==framebuffer（物理）⇒ 1/scale（光标是物理像素，要缩到 DIP）；
  //   · Wayland：窗口尺寸==逻辑（surface）⇒ 1（光标本就是逻辑坐标，不能再缩）；
  //   · X11：两者相等 ⇒ 1。避免"统一除 pixelRatio"在 Wayland 上把命中坐标减半。
  float dip_scale() const;
  // 窗口 DPI 缩放（glfwGetWindowContentScale 的 x 分量）
  float content_scale() const;

  // d68：任务栏图标（Win/X11；Wayland 走 app_id 匹配桌面项，见 create 注释）
  // —— R4（d146）：多屏信息与按屏全屏（仅主线程）——
  // 三步可用性判定（monitor_info）：① 在枚举列表（排除拔线/禁用/合盖）；
  // ② 视频模式非空且 w/h>0；③ 工作区非退化。任一步不过 = 不可用。
  // 已知边界：物理关机但信号保持的屏 GLFW 探测不到（仍在列表）——决策点 5 接受近似。
  static int monitor_count();
  static bool monitor_info(int idx, MonitorInfo* out);
  // d186：屏是否点亮（尽力而为）。Windows = DDC/CI VCP 0xD6；无法判定返回 Unknown。
  static MonitorPower monitor_power(int idx);
  int monitor_index_of_window() const;  // 窗口中心所在屏的枚举下标（-1 未知/不在任何屏）
  // 绑定指定屏全屏。已在该屏全屏 = no-op（不闪屏不重切，用户定值）；窗口态先保存
  // 还原矩形（同 toggle_fullscreen 语义，退出仍走 toggle_fullscreen）。
  void enter_fullscreen_on(int mon_idx);
  // 监视器接入/移除通知（glfwSetMonitorCallback 转发；event: GLFW_CONNECTED/DISCONNECTED）。
  // 仅主线程触发（poll_events 派发）。移除时上层自愈：当前窗口所在屏消失 → 重选+夹取。
  std::function<void(int)> on_monitors_changed;

  using GetProcFn = void* (*)(const char* name);
  // 供 mpv render API 的 get_proc 适配器（glMPGetProcAddress 风格）
  static void* proc_adapter(void* user, const char* name);

  // —— 交互回调（主线程内由 GLFW 投递）——
  std::function<void(double, double)> on_mouse_move;          // 窗口坐标
  std::function<void(int button, int action)> on_mouse_btn;   // button: 0=左 1=中 2=右（GLFW 枚举）
                                                              // action: 1=press 0=release
  std::function<void(int key, int action, int mods)> on_key;   // mods 含 Ctrl/Shift/Alt/Super
  std::function<void(double, double)> on_scroll;               // x/y：滚动增量（y>0=向上）
  std::function<void(bool)> on_mouse_enter;                    // true=进入窗口 false=离开（d36）
  std::function<void()> on_focus_lost;

  // d74：剪贴板变更通知（Win 事件模式，clipboard::start_listener 登记后生效；
  // 文本读取回调里自行调 clipboard::get_text）。仅窗口消息线程（主线程）触发。
  std::function<void()> on_clipboard_update;
  // d81 托盘：系统关闭请求（Win=WM_CLOSE，即 Alt+F4 与系统关机广播）。
  // 返回 true = 调用方已处理（如隐到托盘），不再走默认关闭；未设置/false =
  // 维持 GLFW 默认（置 should_close）。仅窗口消息线程（主线程）触发。
  std::function<bool()> on_close_request;
  // d117 外部正常退出协议：tools/quit_app.py 以注册消息 "FlashRec.RequestQuit"
  // 投递（跨进程按消息名对齐）。托盘语义下 on_close_request 返回 true 会清回
  // should_close —— WM_CLOSE 与 GLFW 对 WM_QUIT 的 close-request 转换都被吞，
  // 二者均无法真退出；本回调等价托盘菜单「退出」（隐窗 + 置 should_close），
  // 主循环退出后走完整收尾链。仅窗口消息线程（主线程）触发。
  std::function<void()> on_quit_request;
  // 系统请求重绘（模态循环内唯一出帧通道）；主线程收到后应唤醒渲染线程
  std::function<void()> on_refresh;
  // d92：窗口**已重新显示**（show_window 收尾处触发）。上层必须借此强制一次整窗
  // 重绘 —— Wayland 的 glfwShowWindow 只重建 xdg 角色、不提交任何缓冲，而按需
  // 渲染下的静默帧判为「无脏区」不出图 ⇒ 窗口会空等 2s 心跳才露面。
  // 所有显窗点都经 show_window()，故挂在这里不可能漏；仅主线程触发。
  std::function<void()> on_shown;

  // 光标显隐（d36：全屏+面板隐藏时藏光标）。GLFW_CURSOR_HIDDEN 仅视觉隐藏，
  // 移动/按键事件照常上报 —— 藏光标后仍可唤醒（防误判"永远无法唤醒"）。
  void set_cursor_visible(bool on);

  GLFWwindow* raw() const { return win_; }

  // 用户拖拽调整窗口大小中（d39 起由统一拖拽状态机驱动，Windows wndproc 同写此标记）；
  // 渲染线程据此冻结 FBO 重建、降级为旧纹理拉伸。
  void set_resizing(bool on) { resizing_ = on; }
  void mark_size_dirty() { size_dirty_ = true; }

#ifdef _WIN32
  // 子类化链：原窗口过程（resize_wndproc 回调需要）
  void* prev_wndproc() const { return wndproc_old_; }
#endif

 private:
  GLFWwindow* win_ = nullptr;
  int fs_sx_ = 0, fs_sy_ = 0, fs_sw_ = 0, fs_sh_ = 0;   // 全屏前的窗口矩形
  void* wndproc_old_ = nullptr;                          // 原窗口过程（子类化链）
  bool resizing_ = false;                                // 用户拖拽调整大小中
  bool size_dirty_ = true;                               // 尺寸待渲染线程对齐

  // —— d91：交换闸门（跨线程，故用原子）——
  // swap_gate_closed_：主线程写、渲染线程读。真 = 渲染线程禁止提交缓冲。
  // swap_in_flight_  ：渲染线程写、主线程读。真 = 正在 glfwSwapBuffers 临界区内，
  //                    主线程隐窗前必须等它归零（见 hide_window）。
  std::atomic<bool> swap_gate_closed_{false};
  std::atomic<bool> swap_in_flight_{false};
  // close 请求去抖：连接失效时 GLFW 会反复伪造关闭请求（每次 poll 一次），
  // 第一次按「隐到托盘」处理，未经重新显示就来的第二次按「连接已死」放行退出。
  // show_window() 里复位（＝重新显示即开启新一轮）。
  bool close_handled_ = false;
};

}  // namespace fr
