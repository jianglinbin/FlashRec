#pragma once
// v0.6.4 d460：自动更新弹窗（全屏遮罩 + 居中卡片）。
// 绘制与 hit-test 共用 update_dialog_layout（几何唯一来源，延续区域重绘铁律）。
// 几何按临时浮层惯例由现有 Theme token 组合（同 draw_help），不新增 skins.json token。
#include <nanovg.h>

#include "ui/damage.h"
#include "ui/views/update_ui.h"
#include "ui/widgets.h"  // BtnState

namespace fr {

struct Theme;

// 弹窗按钮语义（渲染线程映射为 UiIntent，主线程执行）。
enum class UpdateButtonKind : uint8_t {
  None = 0,
  Install,          // Windows 安装版：立即安装
  InstallRestart,   // AppImage：立即更新并重启
  Download,         // ManualOnly：下载更新
  Later,            // 稍后
  Skip,             // 跳过此版本
  Retry,            // 重试
  OpenFolder,       // 便携 ZIP：打开下载文件夹
  OpenFolderDeb,    // deb：打开所在文件夹
  OpenPage,         // 打开发布页
  Close,            // 关闭
};

struct UpdateButtons {
  UpdateButtonKind kinds[3] = {UpdateButtonKind::None, UpdateButtonKind::None,
                               UpdateButtonKind::None};  // [0] 左 … [count-1] 右（主操作恒在最右）
  int count = 0;
};

UpdateButtons update_dialog_buttons(const UpdateUi& u);
const char* update_button_label(UpdateButtonKind k, const UpdateUi& u);

struct UpdateDialogLayout {
  float x = 0, y = 0, w = 0, h = 0;
  float pad = 0, content_w = 0;
  float title_y = 0;        // 标题中线
  float version_y = 0;      // 版本行中线（0 = 无）
  float size_y = 0;         // 大小行中线（0 = 无）
  float progress_text_y = 0;  // 进度文字中线（0 = 无）
  float progress_y = 0;     // 进度条顶边（content_x 起，content_w 宽）
  float hint_y = 0;         // 提示首行中线（0 = 无）
  float error_y = 0;        // 错误首行中线（0 = 无）
  float warning_y = 0;      // 投屏警告首行中线（0 = 无）
  float notes_title_y = 0;  // 「更新说明」中线（0 = 无）
  float notes_y = 0;        // 说明首行中线（0 = 无）
  float btn_y = 0, btn_h = 0;
  float btn_x[3] = {}, btn_w[3] = {};
  int btn_count = 0;
  // 行高（绘制侧与布局同源）
  float row_h = 0, notes_line_h = 0;
  int notes_cap = 8;  // 小窗口时说明区收行数（绘制侧必须用同一个值以免溢出卡片）
};

UpdateDialogLayout update_dialog_layout(NVGcontext* vg, const Theme& t, float win_w,
                                        float win_h, const UpdateUi& u);
int update_dialog_button_at(const UpdateDialogLayout& lay, float mx, float my);  // -1 = 无

// states：btn_count 个按钮的 hover/press 缓动（渲染线程经 button_hit 推进）
void draw_update_dialog(NVGcontext* vg, const Theme& t, float win_w, float win_h,
                        const UpdateUi& u, const UpdateDialogLayout& lay,
                        const BtnState* states, const DmgRect* clip);

}  // namespace fr
