#pragma once
// 待机态视图（等待投屏）。视觉定稿前：完整 chrome（顶栏三键 + 底栏）常显，
// 中央设备名 + 引导文案；结构与播放态一致，投屏进入后无缝过渡。
// 底栏与三键来自 ui/views/chrome（与播放态**共用一份实现**，避免两处行为漂移）。
// 可用性按"有无媒体会话"分档：媒体类控件（播放·暂停 / 上下集 / 进度条）无会话时禁用，
// 窗口类控件（音量 / 画中画 / 全屏）任何状态可用。
#include "ui/views/player_view.h"  // ViewInput / ViewCallbacks

struct NVGcontext;
typedef struct NVGcontext NVGcontext;

namespace fr {

struct Theme;
struct PlaybackSnapshot;

void draw_empty_state(NVGcontext* vg, const Theme& t, float w, float h,
                      const char* friendly_name, const PlaybackSnapshot& snap, ViewInput& in,
                      ViewCallbacks& cb, const DmgRect* clip = nullptr);

// 画中画小窗内的待机内容（缩小版：设备名 + 等待投屏；仅窗口底色 + 两行文字）
void draw_standby_mini(NVGcontext* vg, const Theme& t, float w, float h,
                       const char* friendly_name, const DmgRect* clip = nullptr);

}  // namespace fr
