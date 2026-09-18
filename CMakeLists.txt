cmake_minimum_required(VERSION 3.24)

# 版本号是打包的唯一来源（deb 文件名 / CPack 元数据 / .desktop）；改版本先改这里，
# 且必须与 ledger/INDEX.md 中状态为「进行中」的版本一致。
project(FlashRec VERSION 0.5.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(FLASHREC_CONSOLE "保留控制台（开发期看日志；发布关闭）" ON)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(deps)

set(FR_SOURCES
  src/main.cpp
  src/app/config.cpp
  src/app/event_bus.cpp
  src/app/log.cpp
  src/app/str_util.cpp
  src/dmr/dmr_device.cpp
  src/dmr/gena.cpp
  src/dmr/soap_util.cpp
  src/app/render_loop.cpp
  src/player/mpv_bridge.cpp
  src/player/player_controller.cpp
  src/player/thumb_preview.cpp
  src/player/url_probe.cpp
  src/ui/framebuffer.cpp
  src/ui/nanovg_gl_impl.cpp
  src/ui/regions.cpp
  src/ui/theme.cpp
  src/ui/widgets.cpp
  src/ui/views/chrome.cpp
  src/ui/views/empty_state.cpp
  src/ui/views/player_view.cpp
  src/platform/gl_lock.cpp
  src/platform/seh_guard.cpp   # d125：SEH 兜底（渲染链接住显卡驱动异常 → 自动降级 SW）
  src/platform/crash_dump.cpp
  src/platform/interaction.cpp
  src/platform/keep_awake.cpp
  src/platform/paths.cpp
  src/platform/clipboard.cpp
  src/platform/single_instance.cpp
  src/platform/window_glfw.cpp
  src/platform/net_if.c   # 多网卡（d109）：接口枚举 / 按对端选本机 IP（也被 upnp_static 编译一份）
  third_party/nanovg/src/nanovg.c
)
if (WIN32)
  list(APPEND FR_SOURCES src/platform/tray_win32.cpp)  # 托盘（d81）：Shell_NotifyIcon + 原生菜单
else()
  list(APPEND FR_SOURCES src/platform/tray_linux.cpp)    # 托盘入口（d84）：SNI 优先 + XEmbed 兜底
  list(APPEND FR_SOURCES src/platform/tray_xembed_linux.cpp)
  # SNI 后端（d84，sd-bus/libsystemd）：缺 libsystemd-dev 时自动裁剪，仅剩 XEmbed
  find_path(SYSTEMD_INCLUDE_DIR systemd/sd-bus.h)
  find_library(SYSTEMD_LIBRARY NAMES systemd)
  if (SYSTEMD_INCLUDE_DIR AND SYSTEMD_LIBRARY)
    list(APPEND FR_SOURCES src/platform/tray_sni_linux.cpp)
    set(FR_HAVE_SNI ON)
  endif()
endif()

if (FLASHREC_CONSOLE)
  add_executable(flashrec ${FR_SOURCES})
else()
  add_executable(flashrec WIN32 ${FR_SOURCES})
endif()

target_include_directories(flashrec PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${NANOVG_INCLUDE_DIR}
  ${GLAD_INCLUDE_DIR}
  ${MPV_INCLUDE_DIR}
)

if (FR_HAVE_WL_MOVE)
  # d90：Wayland 原生窗口移动桥接已编入 glfw 目标，flashrec 侧启用调用
  # （见 src/platform/glfw_wayland_move.c 与 window_glfw.cpp::begin_system_move）
  target_compile_definitions(flashrec PRIVATE FLASHREC_HAVE_WL_MOVE)
endif()

# 版本号下发（d99）：main.cpp 的启动横幅与 UI 元信息都取它，避免再出现
# "project(VERSION) 是 0.4.0、启动日志却写死 v0.3.1" 这种双真值。
target_compile_definitions(flashrec PRIVATE FLASHREC_VERSION="${PROJECT_VERSION}")

if (MSVC)
  target_compile_options(flashrec PRIVATE /utf-8 /W3 /permissive- /Zi)
  target_compile_definitions(flashrec PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  if (NOT FLASHREC_CONSOLE)
    # 发布版在 MSVC 上必须显式指定入口点，否则链接失败。
    # add_executable(... WIN32 ...) 给的是 /SUBSYSTEM:WINDOWS，此时 CRT 的 exe_winmain.obj
    # 会去找 WinMain；而本项目的入口一直是普通 main()（src/main.cpp），于是：
    #   MSVCRT.lib(exe_winmain.obj) : error LNK2019: 无法解析的外部符号 WinMain
    #   bin\flashrec.exe : fatal error LNK1120: 1 个无法解析的外部符号
    # 关键区分：**子系统**决定"要不要分配控制台"，**入口点**决定"走哪条 CRT 启动路径"，
    # 两者可以解耦 —— 指定 mainCRTStartup 后仍是 GUI 子系统（用户双击不会先弹黑窗口），
    # 但 CRT 会去调 main()。MinGW 的 -mwindows 天然支持配 main()，所以这个坑只在 MSVC 上出现，
    # 而开发构建一直是 FLASHREC_CONSOLE=ON（控制台子系统），因此长期没被暴露。
    target_link_options(flashrec PRIVATE /ENTRY:mainCRTStartup)
  endif()
else()
  target_compile_options(flashrec PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# ---- 第三方源码降噪（MSVC /w，gcc -w）----
set_source_files_properties(third_party/nanovg/src/nanovg.c PROPERTIES COMPILE_OPTIONS
  "$<$<C_COMPILER_ID:MSVC>:/w>$<$<NOT:$<C_COMPILER_ID:MSVC>>:-w>")

target_link_libraries(flashrec PRIVATE
  glad
  glfw
  spdlog::spdlog_header_only
  ${UPNP_LIBS}
  ${MPV_LIBRARY}
)
if (WIN32)
  target_link_libraries(flashrec PRIVATE
    opengl32 gdi32 shell32 comctl32 dwmapi user32 ws2_32 iphlpapi winmm dbghelp)
else()
  find_package(Threads REQUIRED)
  find_package(X11 REQUIRED)                     # global_cursor 用 XQueryPointer（d39）
  target_include_directories(flashrec PRIVATE ${X11_INCLUDE_DIR})
  target_link_libraries(flashrec PRIVATE Threads::Threads ${CMAKE_DL_LIBS} ${X11_X11_LIB}
                                          ${X11_Xext_LIB})  # Xext：托盘图标形状掩码（d81）
  if (FR_HAVE_SNI)
    target_include_directories(flashrec PRIVATE ${SYSTEMD_INCLUDE_DIR})
    target_link_libraries(flashrec PRIVATE ${SYSTEMD_LIBRARY})
    target_compile_definitions(flashrec PRIVATE FLASHREC_HAVE_SNI)  # 托盘入口启用 SNI 探测（d84）
  endif()
endif()

if (WIN32)
  # mpv 运行库随 exe 走（Linux 用系统 libmpv2，无此步骤）
  add_custom_command(TARGET flashrec POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MPV_DLL} $<TARGET_FILE_DIR:flashrec>
    COMMENT "copy libmpv-2.dll"
  )
endif()

# assets 跟随 exe（便携布局）
add_custom_command(TARGET flashrec POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/assets $<TARGET_FILE_DIR:flashrec>/assets
  COMMENT "copy assets"
)

# ---- 安装布局（d95）----
# Windows：便携布局 —— exe + dll + assets 同目录，解压即用（install 前缀根）。
# Linux  ：FHS 布局 —— exe 进 <prefix>/bin，assets 进 <prefix>/share/flashrec/assets。
#   这是 deb / AppImage 的前提（打包根必须是 /usr），路径靠 src/platform/paths.cpp 里
#   新增的 "<exe>/../share/flashrec/assets" 搜索位配对，dev 便携布局不受影响。
if (WIN32)
  install(TARGETS flashrec RUNTIME DESTINATION .)
  install(FILES ${MPV_DLL} DESTINATION .)
  # 便携布局：exe + libmpv-2.dll + assets/ 同目录，解压即用（tools/package_windows.sh 直接
  # 用这条规则出 ZIP，不手工罗列文件 —— 手工列表迟早和这里漂移）。
  # 与 UNIX 分支同样排除两样「不该进包」的东西：
  #   .gitkeep  —— 占位文件，只是噪音
  #   linux/    —— .desktop 模板，是 Linux 构建期的输入，Windows 包里无意义
  install(DIRECTORY assets/ DESTINATION assets
          PATTERN ".gitkeep" EXCLUDE
          PATTERN "linux" EXCLUDE)
elseif (UNIX AND NOT APPLE)
  install(TARGETS flashrec RUNTIME DESTINATION bin)
  # assets 路径与 src/platform/paths.cpp 的 "<exe>/../share/flashrec/assets" 搜索位配对。
  # 排除两样「源码树专用、不该进包」的东西：
  #   .gitkeep        —— 占位文件，装进包只是噪音
  #   linux/          —— .desktop 模板，构建期输入，已由 linux_desktop.cmake 消费
  install(DIRECTORY assets/ DESTINATION share/flashrec/assets
          PATTERN ".gitkeep" EXCLUDE
          PATTERN "linux" EXCLUDE)
  # d93：桌面集成项（.desktop + hicolor 图标）—— Wayland 任务栏图标唯一来源
  include(linux_desktop)
  # d96：CPack DEB（deb 元数据 + 依赖推导），只为 .deb 服务
  include(packaging)
else()
  install(TARGETS flashrec RUNTIME DESTINATION .)
  install(DIRECTORY assets/ DESTINATION assets)
endif()
