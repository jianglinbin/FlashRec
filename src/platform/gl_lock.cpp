#include "platform/gl_lock.h"

// 各平台实现。平台宏只准出现在 src/platform/（AGENTS.md 规则 5）。

#if defined(__APPLE__)

// macOS：必须用 CGL 层锁，与 Cocoa 主线程内部的 drawable 访问互斥。
// 用普通 mutex 无效（见 gl_lock.h 注释中的 Apple 文档引用）。
#include <OpenGL/OpenGL.h>

namespace fr {

void gl_lock() {
  CGLContextObj ctx = CGLGetCurrentContext();
  if (ctx) CGLLockContext(ctx);
}

void gl_unlock() {
  CGLContextObj ctx = CGLGetCurrentContext();
  if (ctx) CGLUnlockContext(ctx);
}

}  // namespace fr

#else  // Windows / Linux：GL 上下文支持跨线程使用，无需额外互斥

namespace fr {

void gl_lock() {}
void gl_unlock() {}

}  // namespace fr

#endif
