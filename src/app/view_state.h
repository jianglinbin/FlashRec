#pragma once
// 跨线程视图状态：主线程（消息/输入）↔ 渲染线程（绘制/hit-test）的数据通道。
//
// 分工（AGENTS.md 规则 4：状态转移只在主线程串行执行）：
//   主线程 —— 输入事件写入、窗口形态、DMR/播放状态机、窗口操作。
//   渲染线程 —— nanovg 绘制 + 控件 hit-test + ButtonFx 动画推进。
//
// 为什么 hit-test 在渲染线程：立即模式 GUI 的 hit-test 与绘制共用同一份几何
// （主题布局），且 ButtonFx 是跨帧时钟状态。放渲染线程可以避免"每帧把几何
// 算两遍"，也避免锁。控件"被点击"的结论通过命令队列回投主线程执行。
//
// 通道构成：
//   1) 入向（主线程 → 渲染）：主线程自有 master_ 在 publish() 时整块复刻进双缓冲，
//      配 seqlock 版本号（无锁）。
//   2) 出向（渲染 → 主线程）：UiCommand 队列 + mutex（点击/拖动产生的意图）。
//   3) 播放状态：EventBus::snapshot()（已有 mutex，声明任意线程可调），不走本类。
//   4) 渲染线程回写的跨帧状态：独占槽 state_（无同步；btns + 滑条拖拽）。
//
// 三条容易踩的线（前两条是 d18 修的"控制栏闪烁"根因）：
//   a. `writable()` 必须返回**主线程自有**的内存，不能返回共享双缓冲的某一格 ——
//      main.cpp 会把返回值绑成长期引用（`ViewInput& in`），一旦 publish() 交换了
//      索引，那个引用就指向渲染线程的读缓冲，主线程此后每帧都在破坏渲染输入。
//   b. 只做"原子交换索引"不够：整块 memcpy 也不是原子的，渲染线程可能读到
//      "前半新、后半旧"的混合体 → 悬停/按压态忽亮忽灭。故配 seqlock。
//   c. 渲染线程不能回写输入双缓冲（旧实现的 front_for_render 就是），
//      那两块内存的读方是渲染线程、写方是主线程，回写会互相覆盖。故单开 state_ 槽。
//
// 渲染线程侧的两步（缺一不可；跨帧状态 = btns + 滑条拖拽三字段）：
//   1. 帧首把上一帧的跨帧状态装回输入副本 —— ButtonFx 是跨帧时钟状态
//      （widgets.h: "跨帧由调用方持有"），必须逐帧续接。曾经漏了这一步：
//      渲染线程从快照拿到的 btns 永远是主线程的全零副本，press_id 每帧归零、
//      clicked 边沿永远判不出来 → 所有按钮点了没反应（d19 修；
//      d24 又补了同类漏网之鱼 drag_*：漏了 → 滑条拖拽每帧清零、全废）。
//   2. 帧末把推进后的状态存回本槽。
// 注意：此槽的读方与写方都是渲染线程自己，主线程从不碰，故无需同步；
//      它**不参与** publish()/input_snapshot() 的搬运（主线程的 master_ 里
//      这些字段恒为默认值）。
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <type_traits>

#include "ui/views/player_view.h"  // ViewInput

namespace fr {

// 本通道用 memcpy 搬运 ViewInput，故它必须是可平凡复制的
// （加字段时若引入 std::string / 容器，这条断言会立刻拦下 —— 届时改回真拷贝赋值）。
static_assert(std::is_trivially_copyable<ViewInput>::value,
              "ViewInput 必须可平凡复制：ViewStateChannel 用 memcpy 跨线程搬运它");

// 渲染线程产出的"用户意图"，交主线程执行。
// 只表达意图（要做什么），不含状态 —— 状态的唯一真值仍在主线程。
struct UiIntent {
  enum class Kind {
    PlayPause, Prev, Next, ToggleFullscreen, ToggleMaximize, Minimize, Close, Pip,
    Seek, Volume, Mute, Preview, DismissMenu, DragMove, ClipboardPlay, ClipboardDismiss,
    UiPrefChanged,  // R1：右键菜单勾选项变化 → 主线程落盘 settings.json
  };
  // R1：可持久化 UI 偏好 id（UiPrefChanged 的 pref 载荷；与 settings.json 键一一对应）
  enum class UiPref { ShowInfo = 1, CastAutoFullscreen };
  // d68：DragMove 的来源区域（§2「指令恒产生，消费与否由主线程按区域决定」）。
  // 目前唯一消费者 = TopBar（窗口移动/最大化拖还原跟手）；Stage/Widget 仅表示
  // 「拖动已被三态机识别」，主线程收到即丢弃（无消费者区域可不响应）。
  enum class DragRegion { None, TopBar, Widget, Stage };
  Kind kind;
  double value = 0;   // Seek: 秒；Volume: 0..1；Preview: 进度百分比 0..1
  bool flag = false;  // Mute: true=静音；UiPrefChanged: 勾选后的新值
  DragRegion region = DragRegion::None;  // 仅 DragMove 使用
  int pref = 0;       // 仅 UiPrefChanged：UiPref 枚举值
};

class ViewStateChannel {
 public:
  // ———————————————— 主线程侧 ————————————————

  // 主线程的**自有**输入记录：主线程独占读写，全程可改（`ViewInput& in` 长期持有它）。
  //
  // 设计要点（d18 修正）：这里返回的是主线程自己的一块内存，**不是**共享双缓冲的某一格。
  // 旧实现返回 `buf_[1 - front_]`，而 main.cpp 把返回值绑成了一个长期引用 —— 第一次
  // publish() 交换索引后，那个引用指向的格子就变成了"前台"（渲染线程正在读的那块），
  // 于是主线程此后每帧都在写渲染线程的读缓冲 → 输入撕裂 → 控制栏闪烁。
  // 现在主线程只写 master_，共享双缓冲由 publish() 单向复制过去，两边彻底解耦。
  ViewInput& writable() { return master_; }

  // 把 master_ 的当前内容发布给渲染线程（无锁）。
  // 写入共享槽 + seqlock 版本号包裹：渲染线程若正好撞上，读到奇数版本会重读。
  void publish() {
    const int back = 1 - front_.load(std::memory_order_relaxed);
    seq_.fetch_add(1, std::memory_order_release);  // 进入奇数版：渲染线程重试
    std::memcpy(&buf_[back], &master_, sizeof(ViewInput));
    front_.store(back, std::memory_order_release);
    seq_.fetch_add(1, std::memory_order_release);  // 回到偶数版：快照可读
  }

  // 取出渲染线程投来的全部意图（主线程每帧调用一次）
  std::deque<UiIntent> drain_intents() {
    std::lock_guard<std::mutex> lk(mu_);
    std::deque<UiIntent> out;
    out.swap(intents_);
    return out;
  }

  // ———————————————— 渲染线程侧 ————————————————

  // 读取输入快照（副本 + seqlock 校验）。
  // 即便 publish() 是"整体 memcpy"，seqlock 仍必要：memcpy 本身不是原子的，
  // 渲染线程可能读到"前半新、后半旧"的混合体。
  ViewInput input_snapshot() const {
    for (int attempt = 0; attempt < 64; attempt++) {
      const uint32_t s1 = seq_.load(std::memory_order_acquire);
      if (s1 & 1u) continue;  // 主线程正在发布：中间态不可用，重来
      ViewInput out;
      std::memcpy(&out, &buf_[front_.load(std::memory_order_acquire)], sizeof(ViewInput));
      const uint32_t s2 = seq_.load(std::memory_order_acquire);
      if (s1 == s2) return out;  // 期间无人发布 → 快照自洽
    }
    // 极端情况下（主线程高频发布）退化为"读当前前台"：宁可某帧用稍旧的一版完整输入，
    // 也不让渲染线程空转。不是正确性问题。
    ViewInput out;
    std::memcpy(&out, &buf_[front_.load(std::memory_order_acquire)], sizeof(ViewInput));
    return out;
  }

  // —— 渲染线程独占的回写状态槽（跨帧状态，主线程从不碰）——
  // 收纳所有"渲染线程写、且必须活过下一帧"的输入字段：
  //   - btns：ButtonFx 动画（pressed→released 边沿依赖 was_down/press_id 跨帧存活，
  //     每帧从零起步 → 所有按钮点了没反应，d19 修）；
  //   - drag_progress / drag_volume / drag_value：滑条拖拽（快照每帧全新，渲染线程
  //     写进快照副本的状态到下一帧就被清零 → 光标一离开细条命中区拖动即中断、
  //     单击进度条不落位；与 d19 同类，漏续接的又一课）。
  // 帧首把这几项装回输入副本，帧末存回本槽。
  // **必须以引用取用并原地读写**。读方写方都是渲染线程自己，主线程从不碰，
  // 故无需同步（返回引用，锁在返回瞬间即失效）；它不参与 publish()/input_snapshot()
  // 的搬运（主线程 master_ 里这些字段恒为默认值）。
  ViewInput& render_state() { return state_; }

  // 投递意图给主线程
  void post_intent(UiIntent it) {
    std::lock_guard<std::mutex> lk(mu_);
    intents_.push_back(it);
  }

 private:
  ViewInput master_;  // 主线程自有（唯一可长期持有的引用）
  ViewInput buf_[2];  // 发布用双缓冲（写=主线程，读=渲染线程）
  std::atomic<int> front_{0};
  // seqlock 版本号：偶数 = 稳定可读，奇数 = 主线程正在发布（见 input_snapshot）。
  std::atomic<uint32_t> seq_{0};
  // 渲染线程独占的跨帧状态回写槽（主线程从不读，故无同步需求）。
  ViewInput state_;

  mutable std::mutex mu_;
  std::deque<UiIntent> intents_;
};

}  // namespace fr
