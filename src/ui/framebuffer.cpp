#include "ui/framebuffer.h"

#include "app/log.h"

namespace fr {

Framebuffer::~Framebuffer() {
  if (tex_) glDeleteTextures(1, &tex_);
  if (fbo_) glDeleteFramebuffers(1, &fbo_);
}

void Framebuffer::create_storage(int aw, int ah) {
  if (tex_) glDeleteTextures(1, &tex_);
  if (fbo_) glDeleteFramebuffers(1, &fbo_);

  glGenTextures(1, &tex_);
  glBindTexture(GL_TEXTURE_2D, tex_);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, aw, ah, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glGenFramebuffers(1, &fbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_, 0);
  const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (!ok) {
    FR_LOG_ERROR("[UI] FBO 创建失败 {}x{}", aw, ah);
    alloc_w_ = alloc_h_ = 0;
    return;
  }
  alloc_w_ = aw;
  alloc_h_ = ah;
}

bool Framebuffer::ensure(int w, int h) {
  if (w <= 0 || h <= 0) return false;

  // 容量必须**严丝合缝等于内容尺寸**，任何变化（含缩小）都精确重建：
  // 贴图端 nvgImagePattern 的 extent 给的就是纹理容量（见 render_loop.cpp），
  // 若容量 > 内容（曾经的棘轮分配会在"缩窗后"留下这种状态），图像会被按
  // 1:1 像素画在窗口原点 —— 窗口比容量小时只能看到纹理左上角一块，其余被裁掉，
  // 表现为"视频按屏幕分辨率渲染、不随窗口缩放"（d19 用户实测）。
  // 拖拽中的高频重建已由 live_resize 冻结挡住（拖动中不调 ensure），故这里
  // 无需棘轮余量；每轮拖拽结束后至多重建一次。
  if (fbo_ && w == alloc_w_ && h == alloc_h_) {
    w_ = w;
    h_ = h;
    return true;
  }
  w_ = w;
  h_ = h;
  create_storage(w, h);
  return alloc_w_ == w && alloc_h_ == h;
}

void Framebuffer::bind() {
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
  // 视口覆盖**整张纹理**（容量）。容量恒等于内容（ensure 保证），二者在此已无差别，
  // 仍用 alloc_* 表达"视口必须覆盖整张纹理"的语义。
  glViewport(0, 0, alloc_w_, alloc_h_);
}

}  // namespace fr
