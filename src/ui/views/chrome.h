#pragma once
// 两态共用 chrome（顶栏右上角三键 + 底栏）。**必须只有一份实现** ——
// 播放态与待机态各写一份、各自补交互，正是"同一个按钮两处行为不一致"的根因。
//
// 可见性与可用性是两个维度：
//   - 可见性（画不画）由调用方决定：播放态两栏随不操作自动隐去、全屏时顶栏不画；待机态常显。
//   - 可用性 media_enabled（= 有媒体会话）：播放·暂停 / 上下集 / 进度条 只在有会话时可用；
//     窗口类控件（音量 / 画中画 / 全屏）任何状态全量可用。
#include <string>
#include <vector>

#include <nanovg.h>

#include "ui/damage.h"  // DmgRect（draw_* 的 clip 参数，v0.4.0 d56）

namespace fr {

struct Theme;
struct PlaybackSnapshot;
struct ViewInput;
struct ViewCallbacks;

// 按钮 id（ButtonFx 数组下标；两态共用同一命名空间，禁止重复占用）
enum BtnId : int {
  kBtnPlay = 0,
  kBtnPrev,
  kBtnNext,
  kBtnStop,        // d158：无上/下一集数据时替换上一集/下一集两键（位置 = 两键中点）
  kBtnSpeed,       // d169：倍速键（倒计时之后、静音之前）
  kBtnMute,        // 音量图标 = 静音键
  kBtnPip,
  kBtnFullscreen,
  kBtnWinMin,
  kBtnWinMax,
  kBtnWinClose,
  kBtnCenterPlay,
  kBtnClipPlay,     // d74：剪贴板提示条「播放」按钮（横幅隐藏时 d57 冻结值清理）
  kBtnClipIgnore,   // d74：剪贴板提示条「忽略」按钮
  kBtnSkin,         // d182：顶栏「皮肤按钮」
  kBtnUpdateTop,    // v0.6.4 d464：顶栏「更新就绪」按钮（皮肤按钮左侧）
  kBtnHelpTop,      // v0.6.4 d471：顶栏「帮助」按钮（右侧按钮簇最左位，? 图标）
  kBtnUpdate0,      // v0.6.4 d460：更新弹窗三按钮（左 → 右）
  kBtnUpdate1,
  kBtnUpdate2,
  kBtnIdCount
};

// 底栏是否被下方控件占用（仅一个"这位置归不归 chrome"的判定来源）。
// consumed 比 any() 宽：它还要覆盖"媒体禁用位"与条上任意位置。
// 空 media 会话下媒体类控件不算占用，见实现说明。
struct BarHit {
  bool play = false, prev = false, next = false;
  bool stop = false;    // d158：与 prev/next 互斥（无上/下一集数据时才有效）
  bool track = false;   // 进度条条身（不含时间文字）
  bool time = false;    // 时间文字区
  bool mute = false, vol = false;
  bool speed = false;  // d169：倍速按钮（媒体类：无会话禁用）
  bool pip = false, fs = false;
  // 进度条几何（绘制与渲染线程算悬停百分比共用同一份，禁止两处各算）
  float track_x = 0, track_w = 0;
  // 点击是否已被底栏消费（= 上面任一位，或与媒体无关的栏内位置）
  bool consumed = false;
  bool any() const {
    return play || prev || next || stop || time || track || mute || vol || speed || pip || fs;
  }
};

// 底栏控件命中查询（几何唯一来源：绘制、命中、可见度、空白处判定全部走这里）。
// vg 仅用于量时间文字宽度（几何里时间占位宽与实际文字同源）；可传帧内任意时刻的上下文。
BarHit bar_hit(NVGcontext* vg, const Theme& t, float w, float h, const PlaybackSnapshot& snap,
               const ViewInput& in, bool media_enabled);

// 中央大播放键半径（暂停时才有实体，但热区几何独立于状态 —— 空白处判定必须能在
// 绘制之前问出"这一点是否落在中央键上"，否则点中央键会同时被当空白处单击处理）。
float center_play_radius(const Theme& t);

// 两栏可见度（0..1）：静置 idleHideSec 后淡出；悬停底栏任意控件 / 拖拽中保持可见
float chrome_fade(NVGcontext* vg, const Theme& t, float w, float h, const PlaybackSnapshot& snap,
                  const ViewInput& in, bool media_enabled);

// 右上角三键（热区各 winBtnW × topBarH，无缝排列）
void draw_win_buttons(NVGcontext* vg, const Theme& t, float w, ViewInput& in, ViewCallbacks& cb,
                      const DmgRect* clip = nullptr);

// —— d182：顶栏「皮肤按钮」（三键组左侧 skinBtnGap 处；绘制/命中同源）——
struct SkinBtnGeom {
  float x = 0, y = 0, w = 0, h = 0;
};
SkinBtnGeom skin_button_geom(const Theme& t, float win_w);
void draw_skin_button(NVGcontext* vg, const Theme& t, float w, ViewInput& in, ViewCallbacks& cb,
                      const DmgRect* clip = nullptr);

// —— v0.6.4 d464：顶栏「更新就绪」按钮（皮肤按钮左侧；仅 UpdateStage::Ready 可见）——
// 收起 = skinBtnSize 方块；悬停底板左扩出文字「更新软件」，图标中心全程不动。
// x/y/w/h 恒为**展开包络**（脏区/busy 归因用），收起命中由 draw 内按上一帧 hover 决定。
struct UpdateBtnGeom {
  float x = 0, y = 0, w = 0, h = 0;
  float right = 0;       // 包络右缘（皮肤按钮左侧 skinBtnGap 处）
  float base_w = 0;      // 收起方块宽
  float pill_w = 0;      // 完全展开底板宽
  float icon_cx = 0, icon_cy = 0;
};
UpdateBtnGeom update_button_geom(NVGcontext* vg, const Theme& t, float win_w);
void draw_update_button(NVGcontext* vg, const Theme& t, float w, ViewInput& in, ViewCallbacks& cb,
                        const DmgRect* clip = nullptr);

// —— v0.6.4 d471：顶栏「帮助」按钮（右侧按钮簇最左位；复用更新按钮的 pill 形态）——
// 收起 = ? 图标方块；悬停底板左扩出文字「帮助」。位置随更新按钮联动（d508 修正贴位）：
//   更新可见 → 右缘紧贴更新按钮收起位（图标左缘-间距；更新悬停展开时帮助平滑左移让位）；
//   更新不可见 → 右缘顶替皮肤按钮左侧空位（即更新按钮收起时的位置）。
// upd_hover 传当前帧 in.btns.hover[kBtnUpdateTop]（命中/绘制/归因同源）。
UpdateBtnGeom help_button_geom(NVGcontext* vg, const Theme& t, float win_w,
                               bool upd_visible, float upd_hover);
void draw_help_button(NVGcontext* vg, const Theme& t, float w, ViewInput& in, ViewCallbacks& cb,
                      const DmgRect* clip = nullptr);

// —— d182：皮肤弹层（绘制/命中共用同一份几何；names = 各皮肤显示名）——
struct SkinMenuLayout {
  float x = 0, y = 0, w = 0, h = 0, item_h = 0;
  std::vector<float> ys;
  int count = 0;
};
// anchor = 弹出锚点（顶栏按钮下缘 / 右键光标）；越界回夹进窗口
SkinMenuLayout skin_menu_layout(NVGcontext* vg, const Theme& t, float win_w, float win_h,
                                float anchor_x, float anchor_y,
                                const std::vector<std::string>& names);
int skin_menu_item_at(const SkinMenuLayout& lay, float px, float py);  // -1 = 无
void draw_skin_menu(NVGcontext* vg, const Theme& t, const SkinMenuLayout& lay,
                    const std::vector<std::string>& names, int cur, int hover,
                    const DmgRect* clip = nullptr);

// 顶栏是否被三键占用（拖动区判定 / 空白处判定用）。
// 定义在此：只有一行矩形判定，无需单独 .cpp；Theme 完整定义由调用方 include 保证。
inline bool topbar_hit(const Theme& t, float w, float mx, float my);

// 底栏（贴边半透明）。fade 由调用方给出：播放态传 chrome_fade()，待机态传 1。
// show_thumb_preview：悬停进度条时是否画缩略图浮层（仅播放态有画面时才有意义）。
// thumb_img：悬停点画面（nanovg 图像句柄，0 = 还没出图，只画占位框）。
// rad = 窗口圆角（0 = 全屏/最大化直角）：底栏只有下侧两角圆，圆角外不落地。
void draw_bottom_bar(NVGcontext* vg, const Theme& t, float w, float h,
                     const PlaybackSnapshot& snap, ViewInput& in, ViewCallbacks& cb,
                     bool media_enabled, float fade, bool show_thumb_preview,
                     float rad = 0.f, int thumb_img = 0,
                     const DmgRect* clip = nullptr);

// 常驻迷你进度线（d37）：贴底全宽 2px 细线，alpha = (1-fade)×max —— 面板隐去后
// 不消失，键盘拖动时兼作预览线（kb_progress_value 驱动）。无会话不画。
void draw_mini_progress(NVGcontext* vg, const Theme& t, float w, float h,
                        const PlaybackSnapshot& snap, const ViewInput& in, float fade,
                        const DmgRect* clip = nullptr);

// OSD 浮层（d28）：音量/进度反馈，底部中央；alpha 曲线由 in.now - in.osd_at 推算。
// 调用方 = player_view / empty_state（画中画不调）。与 chrome 淡出无关（两栏隐去也显示）。
void draw_osd(NVGcontext* vg, const Theme& t, float w, float h, const PlaybackSnapshot& snap,
              const ViewInput& in, const DmgRect* clip = nullptr);

// —— 右键上下文菜单（d45）——
// 独立于 ButtonFx/BtnId：菜单条目命中用自管下标，不占用按钮槽
//（kMax=16 已被 10 个 BtnId 占用，菜单再挂进去就爆了）。
enum class CtxAction {
  // ★d501：播放控制条目（PlayPause/Prev/Next/Stop）已从右键菜单撤除——播放控制走
  // 底栏按钮/空格键/媒体键；枚举值照 LanguageMenu（d421）先例保留不删。
  None, PlayPause, Prev, Next, Stop, ToggleFullscreen, Pip,
  // d146 R5：三项信息徽章合并为一项；R2：新增投屏自动全屏（两项均勾选 + R1 持久化）。
  // 旧 ShowInfo/ShowVideoFps/ShowUiFps 已废弃（台账记"已废弃"，编号不删）。
  // v0.6.1 d342：窗口置顶（独立于画中画的用户置顶开关，勾选项 + 持久化）。
  // v0.6.1 d346：窗口缩放四档（issue #1 体感扩展：快捷档 + 步进键；勾选当前档）。
  // v0.6.1 d347：四档收进二级子菜单——主菜单只留「窗口大小」父条目（sub=true 画 ▸）。
  ShowMediaInfo, CastAutoFullscreen, AlwaysOnTop, Skin, Close,
  WindowZoomMenu,  // d347：窗口大小子菜单父条目（子条目 = 下方 Zoom50–200）
  Zoom50, Zoom100, Zoom150, Zoom200,
  // v0.6.1 d353：解码选择二级子菜单（「解码」父条目 + 四档；取代 d352 的二值
  // DecodeToggle——真机证实 d3d11va-copy 花屏后需要可选硬解后端）。
  DecodeMenu, DecodeSw, DecodeAuto, DecodeNvdec, DecodeDxva2,
  // v0.6.1 d354：窗口缩放档位上限 300% → 500%（新增三档；数字键 1–7 同步扩展）。
  Zoom300, Zoom400, Zoom500,
  // d362 引入；d421 已从右键菜单撤除（语言切换只走系统托盘勾选组）——
  // 枚举值照 ShowInfo 先例保留不删（d438 起子菜单父条目身份直接用 CtxAction）。
  LanguageMenu, LanguageZh, LanguageEn,
  // 右键菜单「画面缩放」二级子菜单（父条目 + 七档；缩放真值 = 画面相对窗口的
  // mpv video-zoom，50–500%。档位与「窗口大小」子菜单同表，命名加 V 前缀区分）。
  VideoZoomMenu, VZoom50, VZoom100, VZoom150, VZoom200, VZoom300, VZoom400, VZoom500,
  // d426：画面填充二级子菜单（父条目 + 四档；真值 = settings.json player.fit_mode
  // 持久偏好，默认自动。档位名走 i18n FitMode* 键；模式本体见 player_controller.h
  // 的 FitMode 枚举，值一一对应）。
  FitModeMenu, FitModeAuto, FitModeCover, FitModeContain, FitModeStretch,
};

struct CtxMenuItem {
  const char* label = nullptr;  // nullptr = 分隔线（其余字段忽略）
  CtxAction action = CtxAction::None;
  bool enabled = true;
  bool checkable = false;       // 左侧留 ✓ 勾选位
  bool checked = false;
  bool sub = false;             // v0.6.1 d347：有二级子菜单（右侧画 ▸；展开由调用方管理）
};

// 弹出层可用区（UI 逻辑坐标，原点左上）。由调用方按窗口/显示器工作区算好；
// 布局函数只做纯几何，不再接触 win_w/win_h。
struct PopupArea {
  float l = 0, t = 0, r = 0, b = 0;
};

// 菜单几何（绘制与条目命中共用同一份，禁止两处各算）。
// ys[] 为条目在面板内的相对 y；面板位置由弹出锚点（右键按下点）+ 越界回夹决定。
struct CtxMenuLayout {
  // d438：条目容量唯一真值 —— render_loop 的条目数组按它开，布局循环按它填。
  // 满员 14（四个子菜单父条目 + 窗口模式「退出」；d501 撤播放控制条目后自 18 降），
  // 24 留余量；三处必须同源。
  static constexpr int kMaxItems = 24;
  float x = 0, y = 0, w = 0, h = 0;   // 面板矩形（窗口坐标）
  float item_h = 0, sep_h = 0, pad_y = 0;
  float ys[kMaxItems] = {};
  bool sep[kMaxItems] = {};
  int count = 0;
  bool contains(float px, float py) const;  // 面板矩形内（半开区间，与 item_at 同口径）
  int item_at(float px, float py) const;    // 命中条目下标（-1 = 无，含面板外）
};

// 光标处弹出（mx/my = 弹出锚点），越界回夹进可用区。vg 仅用于量条目文字宽。
// 保持既有"回夹"语义（不改用户习惯），只把边界从窗口换成显示器工作区交集。
CtxMenuLayout ctx_menu_layout(NVGcontext* vg, const Theme& t, const PopupArea& area,
                              const CtxMenuItem* items, int n, float mx, float my);

// d438：二级子菜单专用布局。
//   纵向：面板顶 = parent.y + ys[parent_idx]（与父条目顶边齐平）；
//         底部放不下只做必要上移，最后顶夹进可用区。
//   横向：优先父面板右侧；放不下翻到左侧；两侧都放不下选空间大的一侧再回夹。
// out_left（可空）回传是否落在父面板左侧（绘制层据此翻转 ▸ 箭头）。
CtxMenuLayout ctx_submenu_layout(NVGcontext* vg, const Theme& t, const PopupArea& area,
                                 const CtxMenuItem* items, int n,
                                 const CtxMenuLayout& parent, int parent_idx,
                                 bool* out_left);

// d438：父面板与子面板之间的桥接区（间隙 + 两侧纵向包络）。
// 子菜单开着时指针经过这里不收 —— 避免 2px 间隙/分隔线/内边距把子菜单误收起。
bool ctx_submenu_bridge_hit(const CtxMenuLayout& parent, const CtxMenuLayout& sub,
                            int parent_idx, float px, float py);

// d438：是否二级子菜单父条目（悬停展开 / 点击保持 / 子条目构造共用，
// 禁止各处手抄列表 —— 此前 release 分支漏掉 DecodeMenu 就是手抄漂移）。
inline bool ctx_is_submenu_parent(CtxAction a) {
  return a == CtxAction::DecodeMenu || a == CtxAction::WindowZoomMenu ||
         a == CtxAction::VideoZoomMenu || a == CtxAction::FitModeMenu;
}

// hover = 本帧悬停条目下标（调用方用 item_at 算好传入；-1 = 无）。
// sub_left_idx = 子菜单已翻到父面板左侧时，该父条目下标（箭头改画 ◂）；-1 = 不翻。
void draw_ctx_menu(NVGcontext* vg, const Theme& t, const CtxMenuLayout& lay,
                   const CtxMenuItem* items, int n, int hover,
                   const DmgRect* clip = nullptr, int sub_left_idx = -1);

// —— v0.6.1 d350：F1 帮助面板（全屏遮罩 + 居中卡片，快捷键两列清单）——
// 纯展示；点击关闭由调用方处理（render_loop 拦弹起 → ToggleHelp 意图）。
void draw_help(NVGcontext* vg, const Theme& t, float win_w, float win_h,
               const DmgRect* clip = nullptr);

// —— d169 倍速档位弹层（底栏倍速按钮上方；绘制/命中共用同一份几何）——
// 档位唯一真值：按钮标签 / 弹层条目 / 激活取值共用，禁止散落。
constexpr int kSpeedPresetCount = 7;
double speed_preset(int index);      // 越界返回 1.0
int speed_preset_nearest(double s);  // 与当前倍速最接近的档位下标（打勾用）

struct SpeedMenuLayout {
  float x = 0, y = 0, w = 0, h = 0;
  float item_h = 0;
  float ys[kSpeedPresetCount] = {};
  int count = 0;
};
// anchor_cx = 倍速按钮中心 x；bot_y = 底栏顶边（弹层贴其上方，横向居中并夹进窗口）
SpeedMenuLayout speed_menu_layout(NVGcontext* vg, const Theme& t, float win_w, float win_h,
                                  float anchor_cx, float bot_y);
int speed_menu_item_at(const SpeedMenuLayout& lay, float px, float py);  // -1 = 无
void draw_speed_menu(NVGcontext* vg, const Theme& t, const SpeedMenuLayout& lay,
                     double cur_speed, int hover, const DmgRect* clip = nullptr);

}  // namespace fr

// inline 实现必须在 Theme 完整定义可见之后（本文件被 include 时 Theme 已可见）
#include "ui/theme.h"

namespace fr {
inline bool topbar_hit(const Theme& t, float w, float mx, float my) {
  return my >= 0.f && my < t.layout.topBarH && mx >= w - 3.f * t.layout.winBtnW;
}
inline bool center_play_hit(const Theme& t, float w, float h, float mx, float my) {
  const float r = center_play_radius(t) * 0.5f;
  const float dx = mx - w * 0.5f, dy = my - h * 0.5f;
  return dx * dx + dy * dy <= r * r;
}
}  // namespace fr