#pragma once
// 离屏 FBO：mpv 渲染目标 + UI 合成源。
// 无边框透明圆角窗口：mpv → FBO → 主 framebuffer 上以圆角路径贴图 → nanovg 叠 UI。
//
// 分配策略（**关键约束，改前必读**）：
//   - **容量恒等于内容尺寸**（w == alloc_w，h == alloc_h），任何尺寸变化（含缩小）
//     都精确重建。两条理由：
//     1. mpv 的 `mpv_opengl_fbo.w/h` 语义是"必须是 framebuffer 的尺寸"（render_gl.h），
//        它只据此设置自己的 viewport 和确定宽高比，**不负责把画面居中摆进更大的纹理**。
//        容量大于内容 → 纹理上留下"内容之外的空白"，贴图时混进来，表现为
//        "窗口穿了 / 画面被挤到一角"（d16/d17 教训）。
//     2. 贴图端 nvgImagePattern 的 extent 给的是纹理容量：容量 > 内容时图像被按
//        1:1 像素画在窗口原点，窗口比容量小就只能看到左上角一块 —— 表现为
//        "视频按屏幕分辨率渲染、不随窗口缩放"（d19 用户实测，曾经的棘轮分配
//        在"缩窗后"正是这种状态）。
//   - 拖拽调整大小期间的高频重建已由 live_resize 冻结挡住（render_loop 里拖动中
//     不调 ensure、不重渲），故这里不再需要棘轮余量；每轮拖拽结束至多重建一次。
#include <glad/gl.h>

namespace fr {

class Framebuffer {
 public:
  ~Framebuffer();

  // 确保容量可渲染 w×h（容量恒等于内容，尺寸变化即精确重建）。返回 true 表示可渲染。
  bool ensure(int w, int h);

  void bind();                 // mpv 渲染目标（视口按容量 alloc_w_/alloc_h_ 设置）
  GLuint texture() const { return tex_; }
  GLuint fbo() const { return fbo_; }

  // 内容尺寸 == 容量尺寸（ensure 保证二者恒等；保留两套查询只是语义分工）
  int width() const { return w_; }
  int height() const { return h_; }
  // 已分配的纹理/FBO 容量。**渲染视线、mpv fbo.w/h、贴图采样区域都必须用它。**
  int alloc_width() const { return alloc_w_; }
  int alloc_height() const { return alloc_h_; }

 private:
  void create_storage(int aw, int ah);

  GLuint fbo_ = 0, tex_ = 0;
  int w_ = 0, h_ = 0;                 // 内容尺寸（== 容量）
  int alloc_w_ = 0, alloc_h_ = 0;     // 已分配容量（== 内容）
};

}  // namespace fr
