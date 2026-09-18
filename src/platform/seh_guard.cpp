#include "platform/seh_guard.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace fr::platform {

#ifdef _WIN32

namespace {

// 异常**筛选器**：返回 EXCEPTION_EXECUTE_HANDLER 则进 handler、否则继续向外传播。
// MSVC 规定 GetExceptionCode() / GetExceptionInformation() 只能在 __try 的筛选器
// 表达式里取值 —— 直接在 handler 体内调 GetExceptionInformation() 会报 C2707，
// 所以现场在筛选器里就写进 out。
long sehFilter(unsigned long code, const EXCEPTION_POINTERS* ep, SehFault* out) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
      if (out != nullptr && ep != nullptr && ep->ExceptionRecord != nullptr) {
        out->code = ep->ExceptionRecord->ExceptionCode;
        out->address = ep->ExceptionRecord->ExceptionAddress;
      }
      return EXCEPTION_EXECUTE_HANDLER;
    default:
      return EXCEPTION_CONTINUE_SEARCH;
  }
}

}  // namespace

bool sehCall(void (*job)(void*), void* arg, SehFault* out_fault) {
  __try {
    job(arg);
    return true;
  } __except (sehFilter(GetExceptionCode(), GetExceptionInformation(), out_fault)) {
    return false;
  }
}

#else  // !_WIN32：没有可捕获的结构化异常，直接执行

bool sehCall(void (*job)(void*), void* arg, SehFault*) {
  job(arg);
  return true;
}

#endif

}  // namespace fr::platform
