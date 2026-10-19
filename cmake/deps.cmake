# 依赖解析：优先用 tools/fetch_deps.sh 预取到 third_party/ 的源码；
# 缺失时回退 FetchContent 在线拉取（国内网络建议先跑预取脚本）。
include(FetchContent)

set(FR_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# ---- 内嵌 AirPlay（v0.6.0，AIRPLAY_EMBED_PLAN.md）----
# ON（默认，方案 §10.1）：把 UxPlay 的协议库编成静态库 uxplay_airplay 并挂 AirPlay 适配器。
# OFF：cast/ 不编译、uxplay.cmake 整体 return、OpenSSL/libplist 不 find，产物回归纯 DMR
#（模块化要求；也是"依赖装不上时"的退路）。
# 注意 event_bus 的 AirplayEvent 队列是**无条件**保留的（无生产者即空队列，零开销）。
option(FR_AIRPLAY "内嵌 AirPlay 接收端（UxPlay lib；开发默认 OFF，发布口径钉 ON —— 规划 §10）" OFF)

# pthreads4w 预编译前缀：pupnp 在 Windows 上 find_package(PTHREADS4W) 找它。
# ★与 FR_AIRPLAY 无关（DMR 侧一直需要）：新开的构建目录若不带上这个前缀，
#   配置会在 third_party/pupnp/CMakeLists.txt 直接失败。之前只存在于某个 build/
#   的缓存里，等于"换台机器/清目录就配不起来"——这里显式注入，让配置可复现。
if (WIN32 AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pthreads4w/install)
  list(APPEND CMAKE_PREFIX_PATH ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pthreads4w/install)
endif()

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

# ---- libcurl（v0.6.4 d449：自动更新 HTTP，双平台源码构建）----
# Windows 走 Schannel（系统证书库，不新增运行库依赖）；Linux 走 OpenSSL（系统 CA）。
# 只保留 HTTP(S)：其余协议、HTTP/2、压缩、文档与测试全部关闭，控制体积与配置依赖。
# 钉 git tag 而非预编译滚动资产（d445 教训：上游会清理旧 release asset）。
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
set(ENABLE_CURL_MANUAL OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(CURL_ENABLE_SSL ON CACHE BOOL "" FORCE)
set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH OFF CACHE BOOL "" FORCE)
set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_ALTSVC ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_AWS ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_COOKIES ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_DICT ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_DOH ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_FILE ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_FTP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_GOPHER ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_HSTS ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_HTTP_AUTH ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_IMAP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_IPFS ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAPS ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_MIME ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_MQTT ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_NETRC ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_POP3 ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_PROGRESS_METER ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_RTSP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_SMTP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_TELNET ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_TFTP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_WEBSOCKETS ON CACHE BOOL "" FORCE)
if (WIN32)
  set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
  set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
else()
  set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
  set(CURL_USE_SCHANNEL OFF CACHE BOOL "" FORCE)
endif()
if (EXISTS ${FR_ROOT}/third_party/curl/CMakeLists.txt)
  add_subdirectory(${FR_ROOT}/third_party/curl build-curl EXCLUDE_FROM_ALL)
else()
  FetchContent_Declare(curl GIT_REPOSITORY https://github.com/curl/curl.git
                            GIT_TAG curl-8_23_0)
  FetchContent_MakeAvailable(curl)
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
  # d420 精神的延续（GCC 侧）：pupnp 1.14.x 在 gcc -O2 下有三处 -Wformat-truncation
  # （miniserver.c:217/227、urlconfig.c:201，均为 snprintf 截断启发式误报）。第三方
  # 只读（规则 8），警告在构建层压掉；MSVC 无此诊断，无需处理。
  target_compile_options(upnp_static PRIVATE
      $<$<C_COMPILER_ID:GNU,Clang>:-Wno-format-truncation>)
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

# ---- UxPlay lib（内嵌 AirPlay，d237）----
# 必须在最后：它依赖上面已经定好的 FR_ROOT，且它导出的 FR_AIRPLAY_SOURCES /
# uxplay_airplay 目标由根 CMakeLists 消费。
include(${CMAKE_CURRENT_LIST_DIR}/uxplay.cmake)

# 请求原文收件箱（d242，M1 诊断设施）：构建期锚点补丁，必须在 uxplay.cmake 之后
# （它需要 FR_AIRPLAY 的判定），也必须在 add_subdirectory 编译之前（本项目静态库
# 是直接 add_library 加源文件，Configure 阶段就会读盘 ⇒ 顺序天然正确）。
include(${CMAKE_CURRENT_LIST_DIR}/airplay_inbox.cmake)
fr_apply_airplay_inbox()

# —— UxPlay 镜像补丁（d315）：空 type-0x01 包不再判死 ——
# 真机实证（2026-10-05，iPhone17,1）：即便已广播 features bit42（h265=true），
# iPhone 仍会间歇发出 payload_size==0 的 type-0x01 包；UxPlay 原判词（日志里
# 的「非 h264 且未置 bit42」）只是作者猜测，按致命处理 ⇒ unsupported_codec ⇒
# conn_reset ⇒ 整条镜像秒死（21:13/21:28/22:08/22:18 四次「投屏全灭」都是它，
# 与 h265 开关无关）。处置：记日志、丢本包、继续收流 —— 后续 type-0x00 视频
# 包正常到达即恢复；若真是不可解的流，既有的「500ms 无视频帧 → 纯音频回退」
# 照常兜底，不比现在更差。
option(FR_AIRPLAY_MIRROR_EMPTY01_PATCH "UxPlay 镜像补丁：空 type-0x01 包不判死" ON)
set(_mirror01_enable ${FR_AIRPLAY_MIRROR_EMPTY01_PATCH})
if (NOT FR_AIRPLAY)
  set(_mirror01_enable OFF)  # AirPlay 整体关掉时强制还原，源码树不留半补丁状态
endif()
fr_patch_toggle_file(${_mirror01_enable}
  "${FR_ROOT}/third_party/uxplay/lib/raop_rtp_mirror.c"
  "uxplay 镜像补丁"
  "lib/raop_rtp_mirror.c 空 type-0x01 包"
  [=[
                if (payload_size == 0) {
                    logger_log(raop_rtp_mirror->logger, LOGGER_ERR, "raop_rtp_mirror: received type 0x01 packet with no payload:\n"
                               "this indicates non-h264 video but Airplay features bit 42 (SupportsScreenMultiCodec) is not set\n"
                               "use startup option \"-h265\" to set this bit and support h265 (4K) video");
                    unsupported_codec = true;
                    break;
                }]=]
  [=[
                if (payload_size == 0) {
                    /* FLASHREC-PATCH d315 (cmake/deps.cmake): an empty type-0x01
                     * packet is NOT fatal. Field evidence (2026-10-05, iPhone17,1):
                     * it arrives intermittently even with features bit 42
                     * advertised, so the original guess logged below
                     * ("non-h264 without bit42") does not hold. Killing the
                     * mirror tears down the whole session while the phone keeps
                     * streaming. Log and carry on. */
                    logger_log(raop_rtp_mirror->logger, LOGGER_WARNING,
                               "raop_rtp_mirror: empty type-0x01 packet ignored (FLASHREC d315), keep stream");
                    break;
                }]=]
  "AIRPLAY_EMBED_PLAN.md")
