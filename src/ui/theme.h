#pragma once
// 视觉 token：design/skins.json 的 C++ 形态（AGENTS.md 规则 6）。
// 颜色/圆角/形状只在本结构体；绘制代码禁止出现字面量颜色。
#include <string>

#include <nanovg.h>

namespace fr {

struct Theme {
  // —— 颜色 token（对齐 skins.json colors.*）——
  NVGcolor topBarBg;           // 顶栏不透明底色（窗口模式常显）
  NVGcolor barBg;              // 半透明栏底色（仅底栏使用）
  NVGcolor barBorderTopBar;
  NVGcolor barBorderBottomBar;
  NVGcolor titleText;
  NVGcolor subText;
  NVGcolor mutedText;
  NVGcolor badgeBg;
  NVGcolor badgeText;
  NVGcolor icon;
  NVGcolor iconDim;
  NVGcolor iconDisabled;       // 无媒体时的禁用图标色
  NVGcolor winIcon;
  NVGcolor winHoverBg;
  NVGcolor winPressBg;         // 按下态底色（比 hover 深一档）
  NVGcolor winCloseHoverBg;
  NVGcolor winClosePressBg;
  NVGcolor winCloseHoverIcon;
  // 底栏按钮（与顶栏 winXxx 分开：两者尺寸/语义不同，共用 token 会让改一处动两处）
  NVGcolor btnHoverBg;
  NVGcolor btnPressBg;
  // 静音态（音量图标斜线、条与滑块弱化）
  NVGcolor muteIcon;
  NVGcolor muteTrack;
  NVGcolor muteKnob;
  NVGcolor track;
  NVGcolor buffer;
  NVGcolor played;
  NVGcolor knob;
  NVGcolor knobHaloDrag;
  NVGcolor chapterTick;
  NVGcolor playButtonBg;
  NVGcolor playButtonBorder;   // 仅 neon/mono 使用
  NVGcolor playButtonIcon;
  NVGcolor previewBg;
  NVGcolor previewBorder;
  NVGcolor previewTimeBg;
  NVGcolor previewTimeText;
  NVGcolor stageBg;            // 无画面时的舞台底色（skins.json stagePlaceholder）

  // —— 形状 token（对齐 skins.json shape.*）——
  struct Shape {
    float windowRadius = 12;
    float progressRadius = 2;
    bool knobCircle = true;        // false = 方形（mono）
    float knobSizeOverride = 0;    // >0 覆盖布局默认（mono=10）
    bool playButtonCircle = true;
    float playButtonSizeOverride = 0;  // >0 覆盖布局默认（mono=42）
    bool playButtonTinted = false;     // neon 描边
  } shape;

  // —— 布局常量（skins.json layout.*，全皮肤共享不可变）——
  struct Layout {
    // 窗口
    int winW = 960;
    int winH = 540;
    // 顶栏
    float topBarH = 34;
    float topBarPadL = 14;
    // 底栏
    float botBarH = 44;
    float botBarPadX = 14;
    float botBarGap = 11;
    // 底栏按钮（热区矩形 > 底色矩形，图标居中；pitch = 相邻按钮中心距）
    float btnHit = 30;      // 命中热区边长（正方形）
    // 悬停/按压底色边长。必须 < btnPitch(34)：相邻按钮的底色不能相碰，
    // 否则过渡帧（前一钮 p=0.9 / 后一钮 p=0.1）两块半透底叠在一起，
    // 视觉上就成了一坨"超出按钮"的团块（窗口模式下窗口底不遮瑕，尤其明显）。
    float btnBoxW = 26;     // 悬停/按压底色边长
    float btnRadius = 7;    // 底色圆角
    float btnPitch = 34;    // 相邻按钮中心间距
    float btnIcon = 12;     // 按钮内图标尺寸（播放/上下集/画中画/全屏通用）
    // 文本
    float titleFont = 12;
    float badgeFont = 11;
    float badgeRadius = 3;
    float badgePadX = 7;
    float badgePadY = 2;
    float badgeMarginL = 10;
    float timeFont = 11;
    // 窗口三键
    float winBtnW = 44;
    float winBtnH = 34;
    float winIconSize = 10;
    float winIconStroke = 1.2f;
    // 进度条（垂直恒为条中线：所有子元素 cy = track 中心，禁止手算偏移）
    float trackH = 3;
    float knobSize = 13;
    float knobSizeHover = 18;
    float knobHaloDrag = 26;
    float chapterTickW = 2;
    float chapterTickH = 11;
    // 缩略图
    float thumbW = 96;
    float thumbH = 54;
    float thumbRadius = 6;
    float thumbGapAbove = 30;
    // 音量
    float volW = 38;
    float volH = 3;
    float volIcon = 13;
    float volKnob = 11;       // 滑块直径
    float volKnobHover = 15;  // 悬停/拖拽时滑块直径
    float volHitPad = 8;      // 热区在条两端外扩（滑块半径占位，避免端点难抓）
    // 倍速（d169）：底栏文字按钮 + 向上弹出的档位列表
    float speedFont = 11;     // 按钮/列表文字字号
    float speedW = 40;        // 倍速按钮宽（pill；高用 btnBoxW）
    float speedItemH = 24;    // 弹出列表条目高
    float speedMenuPadY = 5;  // 弹出列表上下内边距
    // 动效
    double idleHideSec = 2.5;   // 无操作隐去（d25：用户定 2.5s）
    double fadeSec = 0.3;       // 淡入淡出
    float hoverFadeSec = 0.12f;  // 按钮悬停淡入时长
    float pressFadeSec = 0.07f;  // 按钮按压/回弹时长
    float btnPressScale = 0.92f; // 按下时按钮/图标缩放系数
    double dblClickSec = 0.3;    // 空白处单击/双击消歧窗口（超过即判为单击）
    // 中央播放键
    float centerBtnSize = 56;
    float centerIconSize = 16;
    // OSD（d28；d29 精修：音量改竖向面板；锚点 = 左上角四分之一处，用户定值）
    float osdRadius = 8;       // 面板圆角
    float osdEdgeGap = 14;     // OSD 贴左缘（面板左缘与窗口左边线的间距，用户定值）
    float osdTopGap = 60;      // OSD 面板顶边距窗口顶部的固定间距（d35 用户定值：
                               //   取代旧 osdAnchorY=h*0.25 比例锚点——窗口越大面板越
                               //   往下掉；固定顶距全屏/大窗位置不漂移）
    float osdVolW = 64;        // 音量 OSD 面板（竖向）
    float osdVolH = 150;
    float osdBarThick = 4;     // 竖向音量条粗
    float osdBarLen = 70;      // 竖向音量条长（d31：88 会顶到百分比文字，缩短留出间距）
    float osdSeekH = 56;       // 进度/动作 OSD 面板（横向）高；宽度按内容自适应（d32 删固定宽）
    float osdMaxHRatio = 0.2f; // d160：横向面板高占窗口高的上限比 —— 小窗口整体等比收缩
                               //   （含图标/字号/间距），大窗口恒原生高；音量竖面板不参与
    float osdFont = 12;        // OSD 文字字号
    float miniLineH = 2;         // 常驻迷你进度线高（d37 用户定值：贴底、收窄、不消失、不显眼）
    float miniLineMaxAlpha = 0.85f;  // 迷你线最大不透明度（低调）
    double osdFadeIn = 0.15;   // 出现时长
    double osdHold = 1.2;      // 保持时长
    double osdFadeOut = 0.3;   // 淡出时长

    // —— d74 剪贴板链接提示条（横幅，画面顶部居中）——
    float promptFont = 12;       // URL 文本字号
    float promptBarH = 40;       // 横幅高
    float promptBtnW = 52;       // 动作按钮宽（播放 / 忽略）
    float promptBtnH = 24;       // 动作按钮高
    float promptBarRadius = 8;   // 横幅圆角
    float promptBtnRadius = 6;   // 动作按钮圆角
    float promptPad = 12;        // 横幅内边距（左右）
    float promptGap = 8;         // URL 文本与按钮组间距 / 按钮间距
    float promptMaxW = 640;      // 横幅最大宽（URL 过长 → 截断）
    double promptHoldSec = 10.0; // 停留时长（此后淡出，可算静止边界——归因/限帧安全）
    double promptFadeSec = 0.3;  // 淡入/淡出时长
  } layout;

  // —— 皮肤表 ——
  static const Theme& get(const std::string& id);  // 未知 id 回落 fluent
  static const char* kSkinIds[6];
};

// "#rrggbb" / "rgba(r,g,b,a)" → NVGcolor；解析失败返回品红（肉眼可查）
NVGcolor parse_color(const char* s);

}  // namespace fr
