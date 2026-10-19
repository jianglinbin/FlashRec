#pragma once
// v0.6.4 d460：更新弹窗的跨线程显示数据（主线程写、渲染线程只读）。
// ViewInput 用 memcpy 跨线程搬运，故这里全部是定长缓冲/平凡类型（view_state.h 的
// static_assert 兜底）。业务真值（UpdateService 快照）在主线程，渲染线程只呈现。
#include <cstdint>

#include "platform/update_install.h"  // InstallShape（ui → platform 允许，规则 2）

namespace fr {

enum class UpdateStage : uint8_t {
  Hidden = 0,   // 无弹窗
  Checking,     // 手动检查中
  Downloading,  // 下载中（进度见 progress）
  Ready,        // 下载并校验完成，待安装
  UpToDate,     // 已是最新（短暂显示后自动收起）
  Skipped,      // 已跳过此版本（短暂显示后自动收起）
  Failed,       // 检查/下载失败（可重试）
  ManualOnly,   // 有新版本但无资产或未自动下载（下载/打开发布页）
  Installing,   // 正在退出并安装（禁止重复操作）
};

struct UpdateUi {
  UpdateStage stage = UpdateStage::Hidden;
  platform::InstallShape asset = platform::InstallShape::Unknown;
  bool prompt = false;           // 弹窗显示（最高模态）
  bool can_retry = false;        // Failed 时「重试」是否可用
  bool download_available = false;  // ManualOnly：安装包可下载（否则只提示发布页）
  bool cast_active = false;      // 当前有投屏/镜像会话 → 显示中断警告
  bool confirm_install = false;  // 投屏中已点过一次安装（主线程置位）
  float progress = -1.f;         // 0..1；<0 = 不确定
  char version[32] = {};         // 新版本号（不含 v 前缀）
  char current[32] = {};         // 当前版本号
  char size[24] = {};            // 安装包大小（已格式化）
  char error[192] = {};          // 失败原因（主线程取 i18n 后填入）
  char notes[608] = {};          // 更新说明（纯文本，最多 8 行）
};

}  // namespace fr
