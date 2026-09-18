#pragma once

namespace fr::platform {

// 结构化异常（Windows SEH）捕获到的现场。非 Windows 平台恒为 0。
struct SehFault {
  unsigned long code = 0;   // 异常码，例如 0xC0000005（访问违例）
  void* address = nullptr;  // 触发异常的指令地址（可据此判断崩在哪个模块）
};

// 在结构化异常保护下执行 job(arg)。
//
// 返回 true  = job 正常返回；
// 返回 false = 捕获到硬件级故障，现场写入 out_fault（out_fault 可为 nullptr）。
//
// 为什么需要它：显卡驱动的 OpenGL 实现会在编译着色器等操作里**直接抛访问违例**
// （本机实测：AMD HD 6570 / Catalyst 15.7.1 的 atio6axx.dll 写 NULL+0x10，
// 异常地址 0x5d4b804c 固定不变）。C++ 的 try/catch 抓不到这类异常，进程会当场死；
// 只有 SEH 能在调用点接住它，从而让进程活下来并降级到软件渲染。
//
// 只捕获硬件级故障（访问违例 / 非法指令 / 特权指令 / 整数除零）；
// C++ 异常等其它代码一律继续向外传播，避免掩盖正常控制流。
bool sehCall(void (*job)(void*), void* arg, SehFault* out_fault);

}  // namespace fr::platform
