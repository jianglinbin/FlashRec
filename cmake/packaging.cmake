# Linux 打包（d96）：CPack → .deb。
#
# 只负责 Debian 包。AppImage 不在这里做 —— CPack 的 AppImage 生成器定制能力弱
# （控制不了 excludelist、不给 AppRun 插入自注册），改由 tools/package_linux.sh
# 直连 linuxdeploy + appimagetool 手工组 AppDir，见 d97。
#
# 前置：install 规则必须已是 FHS 布局（CMakeLists.txt 的 UNIX 分支），
#       且 configure 时 CMAKE_INSTALL_PREFIX 必须是 /usr（deb 的根是 /usr）。
#
# 依赖推导：默认交给 dpkg-shlibdeps 自动算 —— 它按 ELF 的 DT_NEEDED 反查 dpkg 数据库
# 得到"提供该 so 的包名"，比手写列表准（手写列表一旦漏了 libX11 之类就会装完起不来）。
# 想手工控制就 -DFLASHREC_DEB_AUTO_SHLIBDEPS=OFF 并用 -DFLASHREC_DEB_DEPENDS="..." 覆盖。

option(FLASHREC_DEB_AUTO_SHLIBDEPS "用 dpkg-shlibdeps 自动推导 deb 运行时依赖" ON)
set(FLASHREC_DEB_DEPENDS
    "libmpv2, libx11-6, libxext6, libsystemd0, libc6, libstdc++6"
    CACHE STRING "手工 deb 依赖列表（仅 FLASHREC_DEB_AUTO_SHLIBDEPS=OFF 时生效）")

# ---- 包元数据 ----
set(CPACK_PACKAGE_NAME "flashrec")
set(CPACK_PACKAGE_VENDOR "FlashRec")
set(CPACK_PACKAGE_CONTACT "FlashRec <flashrec@localhost>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "DLNA 投屏接收端（DMR）")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "flashrec")

set(CPACK_GENERATOR "DEB")
set(CPACK_DEBIAN_PACKAGE_NAME "flashrec")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)          # flashrec_<ver>_<arch>.deb
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "FlashRec <flashrec@localhost>")
set(CPACK_DEBIAN_PACKAGE_SECTION "video")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
# 不写 CPACK_DEBIAN_PACKAGE_ARCHITECTURE：CPack 的 DEB 生成器默认取
# `dpkg --print-architecture` 的真实值（amd64/arm64/...），硬编码反而会在别的架构上出错。
set(CPACK_DEBIAN_PACKAGE_STRIP ON)
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "")
set(CPACK_DEBIAN_PACKAGE_DESCRIPTION
"FlashRec —— DLNA 投屏接收端（DMR）。

接收手机 / 电脑经 DLNA(UPnP AV) 推送的音视频：内置 mpv 播放内核与
无边框自绘 UI，托盘常驻；实现 AVTransport / RenderingControl /
ConnectionManager 全套 SOAP action 与 GENA 事件订阅。
")

# 中文字体是纯系统依赖（assets/fonts/ 里没有字体文件，见 src/platform/paths.cpp
# 的 find_cjk_font）：装完机器上没有 CJK 字体时 UI 中文会全变方块，所以明写出来。
# 用 Recommends 而非 Depends —— 缺字体程序仍能跑（只是英文/数字可读），
# 不该因此拒绝安装。
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "fonts-noto-cjk | fonts-wqy-microhei")

# 不再写 postinst 去跑 update-desktop-database / gtk-update-icon-cache：
# Debian/Ubuntu 的 dpkg trigger（desktop-file-utils、gtk-update-icon-cache）
# 会在解包 /usr/share/applications 与 /usr/share/icons/hicolor 时自动触发，
# 自己再跑一遍是重复劳动，且容易和 trigger 抢锁。
if (FLASHREC_DEB_AUTO_SHLIBDEPS)
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
else()
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "${FLASHREC_DEB_DEPENDS}")
endif()

include(CPack)
