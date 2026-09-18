#pragma once
// 区域重绘（增量绘制）的脏区矩形集。v0.4.0 / REGION_REDRAW_PLAN.md §5。
//
// 语义对应 Windows 的 Update Region：主线程/渲染线程把「这帧变化了哪里」归因成
// 一组矩形（归因表见 render_loop.cpp 顶部注释），绘制侧只重画这些矩形。
// 约束（铁律，勿改语义）：
//   - 坐标为**逻辑像素**（与 ViewInput/nanovg 同系，原点左上）；GL scissor 的
//     y 翻转换算只准用 gl_scissor_y()（一处实现，y 翻转写错 = 脏区整体上下颠倒）。
//   - full_ = 整窗：绘制侧必须走全量路径（关 scissor + 全窗 clear + clip=nullptr）。
//     凡尺寸/形态变化、on_refresh、2s 心跳等一律 add_all（render_loop 归因表）。
//   - add 满槽（kMaxRects）自动合并升级为 full —— 绝不允许"漏画"变残影（§6 回退）。
namespace fr {

struct DmgRect {
  int x = 0, y = 0, w = 0, h = 0;
};

// GL scissor 原点在左下、逻辑坐标原点在左上：唯一换算点（规划 §11 y 翻转对策）。
inline int gl_scissor_y(int win_h, const DmgRect& r) { return win_h - (r.y + r.h); }

inline bool rects_overlap(const DmgRect& a, const DmgRect& b) {
  return a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y;
}

class Damage {
 public:
  static constexpr int kMaxRects = 8;

  // 加入一个脏矩形：空矩形忽略、夹取由调用方保证（regions 侧已按窗口截断）；
  // 与已有矩形相交则并入（取外接框）。满槽自动升级整窗。
  void add(int x, int y, int w, int h) {
    if (full_) return;
    if (w <= 0 || h <= 0) return;
    const DmgRect r{x, y, w, h};
    for (int i = 0; i < n_; i++) {
      if (rects_overlap(r_[i], r)) {
        const int x0 = r_[i].x < x ? r_[i].x : x;
        const int y0 = r_[i].y < y ? r_[i].y : y;
        const int x1 = r_[i].x + r_[i].w > x + w ? r_[i].x + r_[i].w : x + w;
        const int y1 = r_[i].y + r_[i].h > y + h ? r_[i].y + r_[i].h : y + h;
        r_[i] = {x0, y0, x1 - x0, y1 - y0};
        return;
      }
    }
    if (n_ >= kMaxRects) {
      // 满槽：全部合并成一个外接框并升级整窗（回退规则，§6）
      for (int i = 1; i < n_; i++) merge_into(r_[0], r_[i]);
      r_[0] = {0, 0, 0, 0};  // full 态矩形值不参与绘制（绘制侧走全量路径）
      n_ = 1;
      full_ = true;
      return;
    }
    r_[n_++] = r;
  }

  // 整窗（force full）。矩形值仅记录用，绘制侧以 full() 为准。
  void add_all(int w, int h) {
    r_[0] = {0, 0, w, h};
    n_ = 1;
    full_ = true;
  }

  void clear() {
    n_ = 0;
    full_ = false;
  }

  bool empty() const { return n_ == 0 && !full_; }
  bool full() const { return full_; }
  int count() const { return n_; }
  const DmgRect& operator[](int i) const { return r_[i]; }

  long long area() const {
    long long a = 0;
    for (int i = 0; i < n_; i++) a += (long long)r_[i].w * r_[i].h;
    return a;
  }

 private:
  static void merge_into(DmgRect& a, const DmgRect& b) {
    const int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    const int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    if (b.x < a.x) a.x = b.x;
    if (b.y < a.y) a.y = b.y;
    a.w = x1 - a.x;
    a.h = y1 - a.y;
  }

  DmgRect r_[kMaxRects];
  int n_ = 0;
  bool full_ = false;
};

// 并集外接框（bbox）。v0.4.0 P0/P1 出帧路径采用**单矩形**（bbox）绘制：
// 本项目"交互语义与绘制"耦合在同一函数链（立即模式），逐矩形循环会让
// button_hit/clicked 边沿随矩形数重复触发 —— 交互/绘制分离重构完成前，
// 多矩形逐区绘制不启用（规划 §5 "必要时先合并成 bbox" 路径）。
inline DmgRect damage_bbox(const Damage& d) {
  DmgRect b{};
  for (int i = 0; i < d.count(); i++) {
    const DmgRect& r = d[i];
    if (b.w == 0 || b.h == 0) {
      b = r;
      continue;
    }
    const int x1 = b.x + b.w > r.x + r.w ? b.x + b.w : r.x + r.w;
    const int y1 = b.y + b.h > r.y + r.h ? b.y + b.h : r.y + r.h;
    if (r.x < b.x) b.x = r.x;
    if (r.y < b.y) b.y = r.y;
    b.w = x1 - b.x;
    b.h = y1 - b.y;
  }
  return b;
}

}  // namespace fr
