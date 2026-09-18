// 托盘 XEmbed 后端（d81 实现，d84 拆为可插拔后端）：X11 系统托盘老标准，
// XFCE/MATE/Cinnamon 等面板有宿主。SNI 宿主存在时由入口优先选 SNI。
#include "platform/tray_linux_backend.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>

// stb_image 声明模式（STB_IMAGE_IMPLEMENTATION 定义在 nanovg.c，C 链接符号共享）
#include "stb_image.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "app/log.h"

namespace fr {
namespace tray {

namespace {

constexpr int kIconSize = 24;

struct DisplayCloser {
  void operator()(Display* d) const {
    if (d) XCloseDisplay(d);
  }
};

class XembedBackend final : public Backend {
 public:
  ~XembedBackend() override { remove(); }

  bool create(GLFWwindow* win, const char* tooltip, const char* icon_png,
              const TrayCallbacks& cb) override;
  void poll() override;
  void remove() override;

 private:
  void redraw_icon();

  std::unique_ptr<Display, DisplayCloser> dpy_;
  Window win_ = 0;                // 图标窗口（被托盘宿主 reparent）
  TrayCallbacks cb_;
  std::unique_ptr<XImage> img_;   // 常驻像素（Expose 重绘用）
  std::unique_ptr<char[]> img_buf_;
  Pixmap mask_ = 0;               // alpha 形状掩码
};

void XembedBackend::redraw_icon() {
  if (!dpy_ || !win_ || !img_) return;
  GC gc = XCreateGC(dpy_.get(), win_, 0, nullptr);
  XPutImage(dpy_.get(), win_, gc, img_.get(), 0, 0, 0, 0, kIconSize, kIconSize);
  XFreeGC(dpy_.get(), gc);
}

bool XembedBackend::create(GLFWwindow* win, const char* tooltip,
                           const char* icon_png, const TrayCallbacks& cb) {
  (void)win;
  if (dpy_) return true;  // 已创建
  if (!tooltip || !icon_png || !cb.on_toggle_window) return false;

  dpy_.reset(XOpenDisplay(nullptr));
  if (!dpy_) {
    FR_LOG_WARN("[TRAY] XOpenDisplay 失败，无托盘");
    return false;
  }
  Display* dpy = dpy_.get();
  const int screen = DefaultScreen(dpy);

  // —— XEmbed 宿主探测：_NET_SYSTEM_TRAY_S<screen> 选择子持有者 ——
  const std::string sel_name = "_NET_SYSTEM_TRAY_S" + std::to_string(screen);
  const Atom sel = XInternAtom(dpy, sel_name.c_str(), False);
  if (XGetSelectionOwner(dpy, sel) == None) {
    FR_LOG_WARN("[TRAY] 无 XEmbed 托盘宿主（{} 无持有者）", sel_name);
    dpy_.reset();
    return false;
  }

  // 图标窗口（形状掩码负责透明像素；背景黑仅作压平底色）
  XSetWindowAttributes swa{};
  swa.event_mask = ButtonPressMask | ExposureMask;
  swa.background_pixel = 0;
  win_ = XCreateWindow(dpy, DefaultRootWindow(dpy), 0, 0, kIconSize, kIconSize,
                       0, CopyFromParent, InputOutput, CopyFromParent,
                       CWEventMask | CWBackPixel, &swa);
  if (!win_) {
    dpy_.reset();
    return false;
  }
  XStoreName(dpy, win_, tooltip);  // 部分面板用 WM_NAME 当 tooltip

  // PNG 解码 → 最近邻缩放到 24×24 → XImage 像素 + 1bpp alpha 掩码
  int iw = 0, ih = 0, comp = 0;
  unsigned char* rgba = stbi_load(icon_png, &iw, &ih, &comp, 4);
  if (!rgba) {
    FR_LOG_WARN("[TRAY] 托盘图标解码失败 {}", icon_png);
    XDestroyWindow(dpy, win_);
    win_ = 0;
    dpy_.reset();
    return false;
  }
  Visual* vis = DefaultVisual(dpy, screen);
  const size_t row_bytes = ((size_t)kIconSize * 32 + 31) / 32 * 4;  // ZPixmap 32bpp 行对齐
  img_buf_.reset((char*)calloc(1, row_bytes * kIconSize));
  char* mask_bits = (char*)calloc(1, (size_t)((kIconSize + 7) / 8) * kIconSize);
  for (int y = 0; y < kIconSize; ++y) {
    for (int x = 0; x < kIconSize; ++x) {
      const int si = (y * ih / kIconSize) * iw + (x * iw / kIconSize);
      const unsigned char r = rgba[si * 4 + 0], g = rgba[si * 4 + 1];
      const unsigned char b = rgba[si * 4 + 2], a = rgba[si * 4 + 3];
      unsigned long pixel = 0;
      if (a > 24) {
        // 半透明压平到中性深灰（面板底色未知；真正的透明区由形状掩码裁掉）
        pixel = ((unsigned long)(r * a / 255) << 16) |
                ((unsigned long)(g * a / 255) << 8) | (unsigned long)(b * a / 255);
        mask_bits[(size_t)y * ((kIconSize + 7) / 8) + (x >> 3)] |=
            (char)(0x80 >> (x & 7));
      }
      memcpy(img_buf_.get() + (size_t)y * row_bytes + (size_t)x * 4, &pixel, 4);
    }
  }
  stbi_image_free(rgba);

  img_.reset(new XImage{});
  XImage* img = img_.get();
  memset(img, 0, sizeof(XImage));
  img->width = img->height = kIconSize;
  img->format = ZPixmap;
  img->data = img_buf_.get();
  img->byte_order = ImageByteOrder(dpy);
  img->bitmap_unit = img->bitmap_pad = 32;
  img->depth = DefaultDepth(dpy, screen);
  img->bits_per_pixel = 32;
  img->bytes_per_line = (int)row_bytes;
  img->red_mask = vis->red_mask;
  img->green_mask = vis->green_mask;
  img->blue_mask = vis->blue_mask;
  if (!XInitImage(img)) {
    FR_LOG_WARN("[TRAY] XInitImage 失败");
    XDestroyWindow(dpy, win_);
    win_ = 0;
    dpy_.reset();
    return false;
  }

  mask_ = XCreateBitmapFromData(dpy, win_, mask_bits, kIconSize, kIconSize);
  free(mask_bits);
  XShapeCombineMask(dpy, win_, ShapeClip, 0, 0, mask_, ShapeSet);
  redraw_icon();

  // —— 申请停靠：设 _XEMBED_INFO 属性 + ConvertSelection（systemtray-spec）——
  const Atom xembed_info = XInternAtom(dpy, "_XEMBED_INFO", False);
  long xembed[2] = {0, 1};  // version 0, XEMBED_MAPPED
  XChangeProperty(dpy, win_, xembed_info, xembed_info, 32, PropModeReplace,
                  (unsigned char*)xembed, 2);
  const Atom xembed_atom = XInternAtom(dpy, "_XEMBED", False);
  XConvertSelection(dpy, sel, xembed_atom, win_, win_, CurrentTime);
  XMapWindow(dpy, win_);
  XFlush(dpy);

  cb_ = cb;
  FR_LOG_INFO("[TRAY] 托盘已挂载（XEmbed → {}）", sel_name);
  return true;
}

void XembedBackend::poll() {
  // 由主循环短周期调用：处理图标窗口事件（GLFW 不接管本窗口）
  if (!dpy_ || !win_) return;
  while (XPending(dpy_.get())) {
    XEvent ev;
    XNextEvent(dpy_.get(), &ev);
    if (ev.type == ButtonPress) {
      if (ev.xbutton.button == Button1 && cb_.on_toggle_window)
        cb_.on_toggle_window();
    } else if (ev.type == Expose) {
      redraw_icon();
    }
  }
}

void XembedBackend::remove() {
  if (win_ && dpy_) {
    XUnmapWindow(dpy_.get(), win_);
    XDestroyWindow(dpy_.get(), win_);
    win_ = 0;
  }
  if (mask_ && dpy_) {
    XFreePixmap(dpy_.get(), mask_);
    mask_ = 0;
  }
  img_.reset();
  img_buf_.reset();
  dpy_.reset();
}

}  // namespace

Backend* xembed_backend() {
  static XembedBackend inst;
  return &inst;
}

}  // namespace tray
}  // namespace fr
