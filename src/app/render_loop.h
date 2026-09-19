#pragma once
// 渲染线程：独占 GL 上下文，跑完整的「mpv → FBO → nanovg → swap」渲染链。
//
// 为什么需要它（详见 ledger d15）：Windows 在用户拖拽窗口边缘时进入系统模态循环，
// 该循环由 DefWindowProc 内部的消息泵驱动，**glfwPollEvents/glfwWaitEvents 不会返回**
// 直到拖拽结束（GLFW 官方 FAQ 3.5 明确承认这是 Windows 设计使然、GLFW 无法改变）。
// 单线程模型下主循环因此整体停摆，渲染一帧都出不去，屏幕上是 DWM 的兜底内容
// （把客户区左上角对齐粘贴 + 底边/右边像素拉伸填满），表现为「缩小正常、放大跟不上」。
// 解法即 GLFW 官方 FAQ 的建议："you should render from a secondary thread"
// （Chromium 走同一路线）。线程拆开后模态循环只能阻塞消息线程，碰不到渲染线程。
//
// 线程职责（AGENTS.md 规则 5：本文件零平台宏）：
//   - 持有并独占 GL 上下文 + nanovg ctx + FBO
//   - 每帧读输入快照 → 绘制 → 提交意图
//   - **不做**窗口操作、不碰 DMR/播放状态机（那都是主线程的事）
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "app/event_bus.h"
#include "app/view_state.h"
#include "ui/damage.h"
#include "ui/framebuffer.h"
#include "ui/nanovg_gl_impl.h"
#include "ui/regions.h"  // d59：UiRegions（归因差集基准）

struct NVGcontext;

// —— d146 R5：徽章内容量化（归因条目与绘制段共用，禁单改一处）——
// 禁 memcmp（d43）：逐字段显式比较。字符串不进 Q（比较贵 + 拷贝禁令）——
// 编码名等以 FNV-1a 短指纹入 Q，文本粒度 = 量化粒度（文本由量化值派生）。
struct BadgeQ {
  int lines = 0;      // 行/条数（0..3；行数变化 = 包络高度变化，归因必须新旧并集）
  bool tio = false;   // true = 三合一模式（d149 起同样每行一条，仅供量化比较分流）
  int fps_q = -1;     // UI 帧率（0.1fps 定点；整数档 ×10+1；-1 = 不显示）
  int vfps_q = -1;    // 视频帧率（0.1fps 定点；-1 = "--"/不显示）
  int wh = -1;        // 分辨率 w<<16|h（-1 = 无视频行）
  int vcodec_q = -1;  // 视频编码名 FNV 短指纹
  int vbr_q = -1;     // 视频码率 0.1Mbps 定点
  int hw_q = 0;       // 0=无数据 1=软解 ≥2=硬解（2+FNV(后端名)）
  int acodec_q = -1;  // 音频编码名 FNV 短指纹（音频行整体缺席 = -1）
  int asr_q = -1;     // 采样率 0.1kHz 定点
  int ach_q = -1;     // 声道数
  int abr_q = -1;     // 音频码率 kbps 定点
  bool eq(const BadgeQ& o) const {
    return lines == o.lines && tio == o.tio && fps_q == o.fps_q && vfps_q == o.vfps_q &&
           wh == o.wh && vcodec_q == o.vcodec_q && vbr_q == o.vbr_q && hw_q == o.hw_q &&
           acodec_q == o.acodec_q && asr_q == o.asr_q && ach_q == o.ach_q &&
           abr_q == o.abr_q;
  }
};

// d146 R5：徽章媒体数据缓存（render_loop.cpp 2Hz 轮询 player_.video_stats() 填充；
// 不引 player 头——字段与 VideoStats 一一对应，字符串原样保存供绘制直取）。
struct BadgeMedia {
  double vfps = -1;
  int w = -1, h = -1;
  long long vbr = -1, abr = -1;  // 视频/音频码率 bps（视频码率 video-br 优先退总码率）
  int asr = -1, ach = -1;        // 采样率 Hz / 声道数
  std::string vcodec, acodec;    // 编码短名（空 = 无数据，不猜）
  std::string hwname;            // hwdec-current 原值（""=取不到，"no"=软解）
};

namespace fr {

class PlayerController;
class ThumbPreview;
struct Theme;
class WindowGLFW;

class RenderLoop {
 public:
  // 依赖均为外部持有、生命周期覆盖本对象（借用指针，不接管）
  RenderLoop(WindowGLFW& window, EventBus& bus, PlayerController& player,
             ViewStateChannel& channel, const Theme& theme);
  ~RenderLoop();

  // 启动渲染线程。调用前主线程须已 release_context()（GL 上下文交出来）。
  // mpv 的 render context 在本函数内、渲染线程中初始化（它需要 current 的 GL 上下文）。
  bool start();

  // 请求停止并 join。必须在销毁窗口之前调用（否则 GL 调用会打到已销毁的窗口）。
  void stop();

  // 任意线程可调：唤醒渲染线程立即出一帧（输入变化/系统请求重绘时）。
  // d94：**不再经由 GLFW 事件循环**（旧实现 = glfwPostEmptyEvent，见 wait_wake 注释）。
  void wake();

  // 主线程可调：请求**全窗**重绘（v0.4.0 d56，on_refresh 绑定）。系统要求重绘
  //（模态循环、DPI 变化、遮挡恢复等）意味着窗口表面可能已失效，局部补画无据可依
  // → 归因表按"整窗"处理（damage add_all），与 set_pip 等强制置位同路。
  void request_full_repaint();

  // mpv 渲染是否就绪（供主线程判断投屏能否出画面）
  bool render_ready() const { return render_ready_.load(std::memory_order_acquire); }

  // 画中画态：由主线程设置（渲染线程读，决定画什么）
  void set_pip(bool on) { pip_mode_ = on; force_dirty_.store(true, std::memory_order_release); }
  // 设备友好名（待机态显示用）
  void set_friendly_name(const char* n) {
    friendly_name_ = n ? n : "";
    force_dirty_.store(true, std::memory_order_release);
  }
  // 缩略图预览源（主线程持有其生命周期；可为 nullptr = 无预览）
  void set_thumb(ThumbPreview* t) { thumb_ = t; }
  // —— d44 常驻帧率显示（诊断，主线程启动时设一次）——
  // d46：徽章与跳帧共存（d44 的旁路已撤销），徽章如实显示真实出帧率——
  // 静默时 ~1fps（2s 心跳兜底）、操作时 60fps。
  void set_show_fps(bool on) { show_fps_ = on; }
  void set_show_video_fps(bool on) { show_video_fps_ = on; }
  void set_show_info(bool on) { show_info_ = on; }
  // d146 R2：投屏自动全屏勾选初值（主线程启动设一次；之后勾选切换在渲染线程
  // 本地置位并回投 UiPrefChanged，主线程落盘 + 管线读取走 cfg）
  void set_cast_auto_fullscreen(bool on) { cast_auto_fs_ = on; }
  bool cast_auto_fs_ = false;  // 右键菜单「投屏自动全屏」勾选态（渲染线程自有）
  // d45：主线程请求关闭右键菜单（ESC）。菜单开着 → 关；没开 → 渲染线程回投
  // Close 意图（保持 ESC 原本的关窗行为）。菜单状态在渲染线程手里，主线程
  // 不知情，故只能走这个单向请求标记。d47：附带唤醒 —— 渲染线程已改按需睡眠
  //（不被主循环踢醒），不 post 的话关菜单要等它下一次超时醒来才生效。
  void request_menu_dismiss() {
    menu_dismiss_.store(true, std::memory_order_release);
    wake();
  }

 private:
  void run();                          // 渲染线程主体
  bool init_gl_resources();            // 在渲染线程内初始化 GL 相关资源
  void frame(double now);              // 单帧渲染
  // —— d94：渲染线程的等待（自有条件变量，绝不触碰 GLFW/Wayland）——
  // 为什么不能用 glfwWaitEventsTimeout：GLFW 在 Wayland 下的等待实现（wl_window.c
  // handleEvents）会 prepare_read + 读 fd + **dispatch 默认队列**，而默认队列正是
  // 主线程 wl_display_roundtrip 的接收队列（glfwShowWindow/HideWindow、窗口创建、
  // EGL 初始化都在其中做 roundtrip）。渲染线程一旦在那些时刻把回包读走并派发掉，
  // 主线程就永久卡在 roundtrip 的 poll 里（回包已被消费，没人再唤醒它的轮询）——
  // 2026-09-17 真机栈实证：主线程 wl_display_roundtrip_queue 卡死、渲染线程同时
  // 停在 handleEvents，表现为「点托盘窗口不再出现、连托盘菜单一起失去响应」。
  // 条件变量与窗口系统无关，从根上消除这处跨线程竞争。
  void wait_wake(double seconds);

  WindowGLFW& window_;
  EventBus& bus_;
  PlayerController& player_;
  ViewStateChannel& channel_;
  const Theme& theme_;

  std::thread thread_;
  std::atomic<bool> quit_{false};
  std::atomic<bool> render_ready_{false};

  // —— 主线程设置、渲染线程读（简单原子/单写者，够用）——
  std::atomic<bool> pip_mode_{false};
  std::string friendly_name_;
  ThumbPreview* thumb_ = nullptr;  // 主线程持有，渲染线程只读（borrow）

  // —— 仅渲染线程访问（无需同步）——
  NanoVgGL nvg_;
  Framebuffer fbo_;
  // d60：窗口内容唯一真值（合成缓冲）。所有 UI 绘制（full/区域两路）画进本
  // FBO，帧末整体 blit 到默认帧缓冲再 swap —— back buffer 的残留内容从此不
  // 被依赖（区域重绘此前依赖"back 保留上一帧"，GL 规范未定义、驱动 flip 轮转
  // 下残留可能是旧帧 → 实测偶发闪烁：按钮切换/拖窗）。内容自持，跨平台确定。
  Framebuffer app_fbo_;
  int pic_img_ = 0;          // mpv FBO 纹理的 nanovg 图像句柄
  unsigned pic_tex_ = 0;     // 注册时的 GL 纹理名（变化则重注册）
  // 注册时的**内容尺寸**。必须与 nvgImagePattern 的 w/h 一致：
  // nvgImagePattern 的 w/h 是"把纹理的哪个区域映射到目标矩形"，给容量
  // （alloc_width/height，含 64px padding + 1.25× 冗余）会把未渲染的空白边
  // 一起映射进来，画面被压缩/偏移、底栏被顶出视野（"窗口烂掉"）。
  int pic_w_ = 0;
  int pic_h_ = 0;
  // 缩略图预览（仅渲染线程访问）
  int thumb_img_ = 0;        // 悬停点画面的 nanovg 图像句柄（0 = 无）
  uint64_t thumb_seq_ = 0;   // 已上传的结果代数（变化才重建）
  float preview_posted_ = -1.f;  // 最近投递的悬停百分比（-1 = 未悬停，节流用）
  double prev_now_ = 0;

  // —— v0.4.0 d68 统一指针三态机（UI_INTERACTION_PLAN §2/§3）——
  // PRESSED →（CLICK | DRAGGED）；单/双击走四事件三段式（单击=UP1 立即、
  // 双击=DOWN2 立即、窗口=UP1→DOWN2）。状态机跑在 frame() 跳帧闸门**之前**
  // （边沿一帧不能漏），成员即跨帧状态（不进 render_state 槽——ViewInput
  // 快照每帧全新，这些字段不写进快照）。
  enum class PtrState { Idle, Pressed, Dragged };
  enum class PtrRegion { None, TopBar, Widget, Stage };
  PtrState ptr_state_ = PtrState::Idle;
  PtrRegion ptr_region_ = PtrRegion::None;  // 按下边沿时归类，弹起沿用
  bool down_prev_ = false;                  // 左键上一帧状态（边沿检测）
  bool in_dbl_ = false;   // 本次按住 = 双击第二击（UP2 不产生单击动作）
  double up1_at_ = 0;     // 三段式②记账：最近一次单击抬起（UP1）时刻；0 = 未武装
  float up1_x_ = 0, up1_y_ = 0;  // UP1 位置（dblRect 判定）
  // d78：播放/全屏态舞台单击挂起确认（防双击夹带暂停）。UP1 不立即发
  // PlayPause，挂起 win 内无 DOWN2 到来才补发；DOWN2 成立 = 取消挂起。
  bool pend_single_ = false;
  float press_x_ = 0, press_y_ = 0;  // 按下点（kDragPx 防抖基准）
  bool mid_prev_ = false;

  // —— d45 右键上下文菜单（渲染线程自有，同 press_in_stage_ 模式：
  //     跨帧存活靠成员变量本身，不进 render_state 槽——ViewInput 快照每帧全新，
  //     这些字段不写进快照，因此不存在被清零的问题）——
  bool ctx_open_ = false;            // 菜单开着
  float ctx_anchor_x_ = 0, ctx_anchor_y_ = 0;  // 弹出锚点（右键按下点，弹出后固定）
  int ctx_press_ = -1;               // 左键按住的条目下标（-1 = 无/菜单外）
  bool ctx_lprev_ = false;           // 菜单开时的左键上一帧状态
  bool right_prev_ = false;          // 右键上一帧状态（边沿检测）
  // —— d169 倍速档位弹层（渲染线程自有，跨帧靠成员本身；不进 render_state 槽）——
  bool speed_open_ = false;          // 弹层开着
  int speed_press_ = -1;             // 左键按住的条目下标（-1 = 无/弹层外）
  bool speed_lprev_ = false;         // 弹层开时的左键上一帧状态
  bool ctx_right_suppress_ = false;  // 菜单开时再按右键=关闭；该次弹起不得重开菜单
  float right_x_ = 0, right_y_ = 0;  // 右键按下位置（6px 防抖基准）
  bool show_info_ = false;           // d146：三合一徽章总开关（唯一菜单勾选项；持久化经 UiPrefChanged）
  double media_at_ = 0;              // 媒体数据 2Hz 轮询时刻
  BadgeMedia media_{};               // 2Hz 轮询缓存（绘制与量化共用同一份）
  std::atomic<bool> menu_dismiss_{false};  // 主线程 ESC 请求（见 request_menu_dismiss）

  // —— d43 按需渲染（脏帧）基准 ——
  // 静默期（无输入/无快照变化/无活动动画）不出帧不 swap：GL 零工作，已呈现画面
  // 由合成器保持。下一帧的"是否变化"判定与这几份基准比较。
  ViewInput last_in_key_{};      // 上一次出帧的输入（比较时清零逐帧必变字段）
  PlaybackSnapshot last_snap_{}; // 上一次出帧的播放快照
  uint32_t last_hover_key_ = 0;  // d51：上一次出帧的悬停目标位掩码（坐标变化裁决基准）
  UiRegions last_regions_{};     // d59：上一次出帧的区域集（归因差集基准；只在出帧推进）
  int last_w_ = -1, last_h_ = -1;
  bool ever_drawn_ = false;
  double last_keepalive_ = 0;    // 静默期每 2s 强制重绘一帧（表面丢失自愈）
  std::atomic<bool> force_dirty_{false};  // set_pip/set_friendly_name 等 setter 置位
  // —— v0.4.0 d56 FR_DBG_DAMAGE=3 诊断（出帧原因限频日志）——
  double last_dmg_log_ = 0;      // 上次 DMG3 日志时刻（限频 1s）
  double last_draw_at_ = 0;      // 上次出帧时刻（DMG3 的 dt 分母）

  // —— v0.4.0 d62 → d146 R5 徽章内容基准（自举修复）——
  // 归因表的徽章列条目改为「内容变化才 invalidate」：量化值与本基准比较。
  // 基准只在**出帧路径**推进（跳帧不存回铁律）——在归因阶段推进的话，紧跟
  // 的限帧/静默跳帧会把变化吞掉（基准已动、屏幕没画 = 徽章停更到心跳）。
  // 量化公式与绘制段 snprintf 格式共用 badge_quant（禁单改一处）。
  BadgeQ badge_q_drawn_{};  // 上次实际画出的徽章内容（逐字段 eq = 无需重绘）
  // —— v0.4.0 d63 动画限帧 ——
  double last_anim_draw_ = 0;     // 上次纯动画类出帧时刻（30fps 闸门基准）
  // —— d161 拖拽缩放视频降帧 ——
  double last_live_video_ = 0;    // 上次 live_resize 中 mpv 全管线出帧时刻（30fps 闸门基准）

  // —— d47 自适应等待 ——
  // 下一轮 wait_wake 的时长：出帧后 8ms（动画/播放逐帧推进），跳帧后 60ms
  //（缩略图就绪这类"无事件变化"的感知粒度；输入/发布类变化全部经 wake()
  // 即时唤醒，不受此值影响）。d94 起等待走条件变量，不再是 GLFW 事件循环。
  double next_wait_ = 0.008;

  // —— d94 唤醒通道（渲染线程等待 / 任意线程唤醒）——
  // 谓词用自增序号：条件变量允许假唤醒，只看 quit_ 会丢唤醒（同值 notify 被吞）。
  std::mutex wake_m_;
  std::condition_variable wake_cv_;
  uint64_t wake_seq_ = 0;   // 受 wake_m_ 保护

  // —— d50 快照按版本号缓存（frame 每次读快照的成本消减）——
  // EventBus 快照唯一写点 publish_player 每次必推版本号；版本不变 = 内容必同，
  // 复用 snap_cache_ 跳过 mutex + 3 个字符串的整份拷贝。snap_cache_ 始终镜像
  // 最近一次读到的内容（与 d43 的出帧基准 last_snap_ 语义不同：后者只在出帧
  // 路径更新，不能挪用）。
  uint64_t snap_ver_seen_ = 0;
  PlaybackSnapshot snap_cache_{};

  // —— d53 视频帧旗标驱动（渲染线程自有）——
  // 播放中出帧率尊重视频帧率：mpv 报新帧（MPV_RENDER_UPDATE_FRAME）才出帧。
  // update() 会消费旗标，故 frame() 每 wake 只 peek 一次并**累积**进本成员；
  // skip/限流路径不清（旗标躺在成员里），真正渲染成功才清 —— 视频帧永不丢。
  bool mpv_frame_pending_ = false;

  // —— d44 常驻帧率显示（诊断）——
  bool show_fps_ = false;        // UI 渲染帧率（两次出帧间隔的 EMA）
  bool show_video_fps_ = false;  // 视频播放帧率（mpv estimated-vf-fps，2Hz 轮询）
  double fps_ema_ = 0;           // UI 帧率平滑值（0 = 无样本）
  double fps_last_draw_ = 0;     // 上一次出帧时刻（EMA 分母用，跳帧期间不打断）
  // d146：video_fps_at_/video_fps_ 已并入 media_at_/media_（BadgeMedia，见上）
};

// 把视图回调包装成"投递意图"（渲染线程 → 主线程）
ViewCallbacks make_intent_callbacks(ViewStateChannel& ch);

// d47：ViewInput"是否影响画面"的逐字段比较（主线程按需发布用）。
// 渲染侧原同表的 input_changed_core 已随 d59 归因表撤销 —— 出帧判定并入
// render_loop.cpp frame() 的矩形归因条目（字段级比较进了各条目）。
// time 驱动字段 now/dt 与渲染线程自有字段 btns/drag_*/click_consumed 不参与——
// 它们不构成重绘理由。
// ⚠️ 新增会改变画面的 ViewInput 字段必须同步加进本表 + 渲染侧归因条目。
bool input_changed(const ViewInput& a, const ViewInput& b);

}  // namespace fr
