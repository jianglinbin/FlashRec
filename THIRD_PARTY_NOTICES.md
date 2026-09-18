# 第三方许可声明

FlashRec 自身采用 **MIT** 许可（见 `LICENSE`）。本项目在构建期通过 FetchContent /
`tools/fetch_deps.sh` 获取若干第三方库，这些源码**不在本仓库内**（`third_party/` 不进版本库），
其许可条款各自独立，列此以备分发二进制时核查。

| 组件 | 用途 | 许可证 |
|---|---|---|
| [pupnp (libupnp)](https://github.com/pupnp/pupnp) | UPnP / SSDP / SOAP / GENA 协议栈 | BSD-3-Clause |
| [libmpv (mpv)](https://github.com/mpv-player/mpv) | 播放后端（解码与渲染） | **GPLv2+ / LGPLv2.1+**（随构建选项而定） |
| [GLFW](https://github.com/glfw/glfw) | 窗口与输入 | zlib/libpng |
| [nanovg](https://github.com/memononen/nanovg) | 矢量绘制（自绘 UI） | zlib |
| [spdlog](https://github.com/gabime/spdlog) | 日志 | MIT |
| pthreads4w | Windows 上的 POSIX 线程（pupnp 依赖） | 见其自带许可文件 |

## 分发二进制时的注意点

- **libmpv 是最需要留意的一项**。本项目以**动态链接**方式使用它（Windows 为
  `libmpv-2.dll`，Linux 为系统 `libmpv.so` 或随 AppImage 捆绑）。若你重新分发时改为静态
  链接，或分发的是 GPL 选项下构建的 libmpv，则整个作品可能需要按 GPL 提供源码。
  以本项目默认的动态链接方式使用，通常不触发该要求——但这不是法律意见，正式分发前请
  自行确认所用 libmpv 构建的具体许可选项。
- **Windows 便携包 / 安装包**随附 `msvcp140.dll`、`vcruntime140.dll`、
  `vcruntime140_1.dll`（Visual C++ 运行库，约 1MB）。依据 Visual Studio 的再分发条款，
  随应用程序附带这些文件是允许的。这里刻意**不**捆绑完整的官方 redist 安装器，
  以免污染用户系统或与其他程序冲突。
- **AppImage** 内捆绑了 185 个共享库（由 linuxdeploy 收集），其中包含上述各依赖及其
  传递依赖。它们的许可由各自上游决定，打包脚本会剔除 glibc / 显卡驱动一类必须使用
  宿主机版本的库。

## 构建期工具

以下工具仅在**打包**时使用，不随产品分发，其许可不约束 FlashRec 本身：

- Inno Setup 6（生成 Windows 安装程序）—— 其自带的非商业使用条款适用于生成的安装器外壳，
  详见 [Inno Setup 许可](https://jrsoftware.org/files/license1.txt)。
- linuxdeploy / appimagetool（生成 AppImage）—— MIT。
- CPack（生成 .deb）—— BSD-3-Clause（CMake 自带）。
