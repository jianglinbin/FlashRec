#include "ui/nanovg_gl_impl.h"

#include <glad/gl.h>
#include <nanovg.h>
#define NANOVG_GL3_IMPLEMENTATION
#include <nanovg_gl.h>

#include "app/log.h"
#include "platform/paths.h"

namespace fr {

bool NanoVgGL::init() {
  // antialias + stencil stroking：圆角栏与描边图标必需
  ctx_ = nvgCreateGL3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
  if (!ctx_) {
    FR_LOG_ERROR("[UI] nanovg GL3 初始化失败");
    return false;
  }
  std::string font = paths::find_cjk_font();
  if (!font.empty()) {
    font_regular_ = nvgCreateFont(ctx_, "cjk", font.c_str());
    if (font_regular_ < 0)
      FR_LOG_WARN("[UI] CJK 字体装载失败 {}", font);
    else
      FR_LOG_INFO("[UI] CJK 字体 {}", font);
  } else {
    FR_LOG_WARN("[UI] 未找到 CJK 字体，中文将显示为方块");
  }
  return true;
}

void NanoVgGL::shutdown() {
  if (ctx_) {
    nvgDeleteGL3(ctx_);
    ctx_ = nullptr;
  }
}

int NanoVgGL::create_image_from_handle(unsigned tex, int w, int h, int flags) {
  if (!ctx_ || tex == 0 || w <= 0 || h <= 0) return 0;
  // NODELETE：句柄图像销毁时不 glDeleteTextures，GL 纹理归 FBO 管理
  return nvglCreateImageFromHandleGL3(ctx_, tex, w, h, flags | NVG_IMAGE_NODELETE);
}

int NanoVgGL::create_image_mem(unsigned char* data, int len) {
  if (!ctx_ || !data || len <= 0) return 0;
  return nvgCreateImageMem(ctx_, 0, data, len);  // 内存图像（jpg/png 字节 → nanovg 句柄）
}

void NanoVgGL::delete_image(int handle) {
  if (ctx_ && handle != 0) nvgDeleteImage(ctx_, handle);
}

}  // namespace fr
