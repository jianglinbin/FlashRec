# Linux 桌面集成（d93）：生成并安装 .desktop 与 hicolor 图标。
#
# 为什么必须随包：Wayland 下窗口的图标/名字/归组**全部**由合成器按窗口的 app_id
# 匹配同名桌面项得到；xzclient 侧没有任何"给窗口挂图标"的协议（glfwSetWindowIcon
# 在 Wayland 后端是空操作）。少了这个文件，任务栏只能显示通用图标。
#
# ⚠️ FLASHREC_APP_ID 必须与 src/platform/window_glfw.cpp 里的 kWaylandAppId 一致。
#
# ⚠️ 模板 `assets/linux/*.desktop.in` 里刻意**不写 `#` 注释行**：桌面项解析器众多，
#    appimagetool 自带的那个对注释的支持就不如 GLib 的 GKeyFile 稳定，而 deb 与
#    AppImage 两份桌面项必须同源同内容（AppImage 的 AppRun 还要拿它去注册），
#    所以说明一律留在本文件，模板只放严格的 [Desktop Entry] 键值。
#
# 布局（d95 起为 FHS：exe 在 <prefix>/bin，assets 在 <prefix>/share/flashrec/assets）：
#   ① 打包：由 tools/package_linux.sh 以 -DCMAKE_INSTALL_PREFIX=/usr + DESTDIR 安装，
#      Exec 即 /usr/bin/flashrec，桌面项落 /usr/share/applications；
#   ② 用户级安装：configure 时给 -DCMAKE_INSTALL_PREFIX=$HOME/.local，则 exe 落在
#      ~/.local/bin/flashrec（已在 PATH 上），桌面项落 ~/.local/share/applications，
#      装着即生效、无需 root。
# 注意：Exec 在 configure 期写死为 <CMAKE_INSTALL_PREFIX>/bin/flashrec；若安装时才用
# `cmake --install --prefix` 覆写前缀，Exec 会失准（图标不受影响）—— 要么 configure
# 期就定好前缀并用 DESTDIR 做暂存，要么装完手改 Exec。

set(FLASHREC_APP_ID "com.flashrec.FlashRec" CACHE INTERNAL "Wayland app_id / 桌面项文件名")
set(FLASHREC_EXEC "${CMAKE_INSTALL_PREFIX}/bin/flashrec")

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/assets/linux/${FLASHREC_APP_ID}.desktop.in"
  "${CMAKE_CURRENT_BINARY_DIR}/${FLASHREC_APP_ID}.desktop"
  @ONLY)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${FLASHREC_APP_ID}.desktop"
        DESTINATION share/applications)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/app_256.png"
        DESTINATION share/icons/hicolor/256x256/apps
        RENAME "${FLASHREC_APP_ID}.png")
