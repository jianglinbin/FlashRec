# 依赖解析：优先用 tools/fetch_deps.sh 预取到 third_party/ 的源码；
# 缺失时回退 FetchContent 在线拉取（国内网络建议先跑预取脚本）。
include(FetchContent)

set(FR_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# ---- glfw ----
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
if (EXISTS ${FR_ROOT}/third_party/glfw/CMakeLists.txt)
  set(GLFW_DIR ${FR_ROOT}/third_party/glfw)
  add_subdirectory(${GLFW_DIR} build-glfw EXCLUDE_FROM_ALL)
else()
  FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4)
  FetchContent_MakeAvailable(glfw)
endif()

# ---- glfw 补丁源（d90：Wayland 原生窗口移动）----
# 规则 8：third_party/ 只读 —— 不改 GLFW 源码，改为向 glfw 目标追加我方一个 .c。
# 编进 glfw 目标（而非 flashrec）是刻意的：只有这样才继承 GLFW 的内部包含目录、
# 平台宏与 wayland-scanner 生成的协议头，从而能直接用 _glfw 全局状态。
# 仅 Wayland 后端构建时需要；X11 / Win32 有真正的 glfwSetWindowPos，用不上。
if (UNIX AND NOT APPLE AND GLFW_BUILD_WAYLAND)
  target_sources(glfw PRIVATE ${FR_ROOT}/src/platform/glfw_wayland_move.c)
  set(FR_HAVE_WL_MOVE ON)
endif()

# ---- spdlog ----
set(SPDLOG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_WARNINGS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
if (EXISTS ${FR_ROOT}/third_party/spdlog/CMakeLists.txt)
  add_subdirectory(${FR_ROOT}/third_party/spdlog build-spdlog EXCLUDE_FROM_ALL)
else()
  FetchContent_Declare(spdlog GIT_REPOSITORY https://github.com/gabime/spdlog.git GIT_TAG v1.14.1)
  FetchContent_MakeAvailable(spdlog)
endif()

# ---- pupnp (libupnp) ----
set(UPNP_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(UPNP_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

# 多网卡补丁（v0.5.0，见 MULTI_NIC_PLAN.md）：必须在 add_subdirectory **之前**应用，
# 否则首次编译拿到的还是未打补丁的源码。开关 -DFR_MULTI_IF=OFF 可整体关闭。
# 双向/幂等/唯一性判定统一在 cmake/pupnp_patch_util.cmake；可先用
# python tools/pupnp_patch_check.py 做不开构建系统的锚点校验。
include(${CMAKE_CURRENT_LIST_DIR}/pupnp_multi_if.cmake)
fr_apply_pupnp_multi_if()

# HTTP 入口日志（原 d22 的手工补丁，本版纳入机制）：独立模块、独立开关
# FR_HTTP_TRACE，与 FR_MULTI_IF 解耦 —— 目的是消灭"重拉依赖后补丁静默丢失、
# log_http_entry 变哑开关"这一类问题。
include(${CMAKE_CURRENT_LIST_DIR}/pupnp_http_trace.cmake)
fr_apply_pupnp_http_trace()

# 请求原文「收件箱」（d144）：独立模块、独立开关 FR_SOAP_INBOX。与 FR_HTTP_TRACE 的分工是
# 一行摘要 vs 整段原文 —— 前者体积小可长开，后者用于取证。挂点在 dispatch_request 的
# **第一条语句**，比任何内部逻辑都早，因此不可能被下游分支吞掉。
include(${CMAKE_CURRENT_LIST_DIR}/pupnp_soap_inbox.cmake)
fr_apply_pupnp_soap_inbox()

if (EXISTS ${FR_ROOT}/third_party/pupnp/CMakeLists.txt)
  add_subdirectory(${FR_ROOT}/third_party/pupnp build-pupnp EXCLUDE_FROM_ALL)
  set(UPNP_LIBS upnp_static ixml_static)
  if (FR_MULTI_IF)
    # 我方多网卡模块编进 pupnp 静态库（与 d90 对 glfw 的做法同理：规则 8 只允许追加
    # 我方源文件，不就地改第三方）；include 目录让第三方源码能 #include "platform/net_if.h"
    target_sources(upnp_static PRIVATE ${FR_ROOT}/src/platform/net_if.c)
    target_include_directories(upnp_static PRIVATE ${FR_ROOT}/src)
    # ⚠️ 必须补 /utf-8：根 CMakeLists 只给 flashrec 目标设了它，而 net_if.c 带中文注释，
    # 在 MSVC 默认代码页(936)下会被误解析（C4819 → 后续代码被吞 → 一堆莫名其妙的
    # C2071/C2065/C1004）。pupnp 自身是纯 ASCII，加此项无副作用。
    target_compile_options(upnp_static PRIVATE $<$<C_COMPILER_ID:MSVC>:/utf-8>)
  endif()
else()
  FetchContent_Declare(pupnp GIT_REPOSITORY https://github.com/pupnp/pupnp.git GIT_TAG branch-1.14.x)
  FetchContent_MakeAvailable(pupnp)
  set(UPNP_LIBS upnp_static ixml_static)
endif()

# ---- glad（GL 3.3 loader，tools/fetch_deps.sh 离线生成）----
if (EXISTS ${FR_ROOT}/third_party/glad/src/gl.c)
  set(GLAD_INCLUDE_DIR ${FR_ROOT}/third_party/glad/include)
  add_library(glad STATIC ${FR_ROOT}/third_party/glad/src/gl.c)
  target_include_directories(glad PUBLIC ${GLAD_INCLUDE_DIR})
else()
  message(FATAL_ERROR "缺 third_party/glad —— 先执行: bash tools/fetch_deps.sh")
endif()

# ---- nanovg（无 CMakeLists，源码直接编进目标）----
if (EXISTS ${FR_ROOT}/third_party/nanovg/src/nanovg.c)
  set(NANOVG_INCLUDE_DIR ${FR_ROOT}/third_party/nanovg/src)
else()
  FetchContent_Declare(nanovg GIT_REPOSITORY https://github.com/memononen/nanovg.git GIT_TAG master)
  FetchContent_MakeAvailable(nanovg)
  set(NANOVG_INCLUDE_DIR ${nanovg_SOURCE_DIR}/src)
endif()

# ---- libmpv（Windows：预编译开发包；Linux：pkg-config 系统库）----
if (WIN32)
  set(MPV_DIR ${FR_ROOT}/third_party/mpv)
  if (EXISTS ${MPV_DIR}/libmpv.dll.a)
    set(MPV_LIBRARY ${MPV_DIR}/libmpv.dll.a)
    set(MPV_DLL ${MPV_DIR}/libmpv-2.dll)
  elseif (EXISTS ${MPV_DIR}/lib/x86_64/libmpv.dll.a)
    set(MPV_LIBRARY ${MPV_DIR}/lib/x86_64/libmpv.dll.a)
    set(MPV_DLL ${MPV_DIR}/lib/x86_64/libmpv-2.dll)
  else()
    message(FATAL_ERROR "缺 third_party/mpv —— 先执行: bash tools/fetch_deps.sh")
  endif()
  set(MPV_INCLUDE_DIR ${MPV_DIR}/include)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)
  set(MPV_LIBRARY PkgConfig::MPV)   # IMPORTED target 自带 include 目录与链接
  set(MPV_INCLUDE_DIR)
endif()
