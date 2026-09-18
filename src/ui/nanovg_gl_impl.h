#pragma once
// nanovg GL3.3 后端封装：创建/销毁上下文 + CJK 字体装载。
struct NVGcontext;

namespace fr {

class NanoVgGL {
 public:
  bool init();   // 需在有效 GL 上下文内调用
  void shutdown();
  NVGcontext* ctx() const { return ctx_; }
  int font_regular() const { return font_regular_; }

  // 把外部 GL 纹理（如 mpv 渲染目标 FBO 的颜色附件）注册为 nanovg 图像。
  // 返回 nvgImagePattern 可用的图像句柄；内部强制 NVG_IMAGE_NODELETE，
  // delete_image 不会销毁 GL 纹理本体（仍归 FBO 所有）。
  int create_image_from_handle(unsigned tex, int w, int h, int flags);
  // 从内存字节（jpg/png 编码数据）建 nanovg 图像；失败返回 0
  int create_image_mem(unsigned char* data, int len);
  void delete_image(int handle);

 private:
  NVGcontext* ctx_ = nullptr;
  int font_regular_ = -1;
};

}  // namespace fr
