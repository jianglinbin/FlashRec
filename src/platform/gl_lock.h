#pragma once
// GL 上下文互斥（渲染线程化必需）。
//
// 背景：macOS 的 Cocoa 会在主线程处理窗口 resize 时调用 NSOpenGLView update 重建
// drawable；若渲染线程同时在用同一上下文发 GL 命令，两者会撞在同一个 drawable 上
// → 随机崩溃（GLFW issue #1997，Intel Mac 与 Apple Silicon 均复现；Fyne 为此退回
// 单线程）。Apple 官方要求跨线程使用 NSOpenGLView 时必须做互斥：
//
//   "you must set up mutex locking ... Applications that use Objective-C with
//    multithreading can lock contexts using the functions CGLLockContext and
//    CGLUnlockContext."
//   —— Apple, OpenGL Programming Guide for Mac
//
// 关键点：**必须是 CGLLockContext，不能用普通 mutex** —— 它需要与 Cocoa 主线程
// 内部持有的同一把锁互斥，自建 mutex 起不到保护作用。
//
// Windows / Linux 的 GL 上下文本身支持跨线程使用（配合 context 迁移），
// 故为空实现，零开销。
namespace fr {

// 进入 GL 临界区。渲染线程每帧用 GL_GUARD 包裹，或手动成对调用。
void gl_lock();
void gl_unlock();

// 作用域守卫：RAII 成对加解锁，避免异常/提前 return 漏解锁
class GlLockGuard {
 public:
  GlLockGuard() { gl_lock(); }
  ~GlLockGuard() { gl_unlock(); }
  GlLockGuard(const GlLockGuard&) = delete;
  GlLockGuard& operator=(const GlLockGuard&) = delete;
};

#define FR_GL_GUARD() ::fr::GlLockGuard _fr_gl_guard_scope_

}  // namespace fr
