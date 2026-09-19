#include "ui/theme.h"

#include <cstring>

namespace fr {

NVGcolor parse_color(const char* s) {
  if (!s) return nvgRGBf(1, 0, 1);
  while (*s == ' ') s++;
  if (s[0] == '#') {
    unsigned v = 0;
    size_t len = strlen(s + 1);
    if (len >= 6) {
      for (int i = 1; i <= 6; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return nvgRGBf(1, 0, 1);
      }
      return nvgRGB((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff);
    }
    return nvgRGBf(1, 0, 1);
  }
  if (strncmp(s, "rgba(", 5) == 0) {
    float r = 0, g = 0, b = 0, a = 0;
    if (sscanf(s + 5, "%f,%f,%f,%f", &r, &g, &b, &a) == 4)
      return nvgRGBAf(r / 255.f, g / 255.f, b / 255.f, a);
    return nvgRGBf(1, 0, 1);
  }
  return nvgRGBf(1, 0, 1);
}

namespace {

#define C(name, str) t.name = parse_color(str)
#define SHAPE(t2, wr, pr, kcircle, kov, pbcircle, pbov, tint) \
  t2.shape.windowRadius = wr;                                 \
  t2.shape.progressRadius = pr;                               \
  t2.shape.knobCircle = kcircle;                              \
  t2.shape.knobSizeOverride = kov;                            \
  t2.shape.playButtonCircle = pbcircle;                       \
  t2.shape.playButtonSizeOverride = pbov;                     \
  t2.shape.playButtonTinted = tint;

// 布局常量（skins.json layout.*，全皮肤共享）：底栏按钮 / 音量滑块 / 消歧窗口
#define LAYOUT(t3)                              \
  t3.layout.btnHit = 30;                        \
  t3.layout.btnBoxW = 26;                       \
  t3.layout.btnRadius = 7;                      \
  t3.layout.btnPitch = 34;                      \
  t3.layout.btnIcon = 12;                       \
  t3.layout.volW = 76;                          \
  t3.layout.volH = 4;                           \
  t3.layout.volKnob = 11;                       \
  t3.layout.volKnobHover = 15;                  \
  t3.layout.volHitPad = 8;                      \
  t3.layout.dblClickSec = 0.3;


Theme make_fluent() {
  Theme t;
  C(barBg, "rgba(26,26,30,0.84)");
  C(topBarBg, "#1a1a1e");
  C(barBorderTopBar, "rgba(255,255,255,0.08)");
  C(barBorderBottomBar, "rgba(255,255,255,0.09)");
  C(titleText, "#e6e8ec");
  C(subText, "#babec6");
  C(mutedText, "#9aa0aa");
  C(badgeBg, "rgba(255,255,255,0.09)");
  C(badgeText, "#9aa0aa");
  C(icon, "#cfd3da");
  C(iconDim, "#b9bec7");
  C(iconDisabled, "rgba(255,255,255,0.3)");
  C(winIcon, "#b9bec7");
  C(winHoverBg, "rgba(255,255,255,0.12)");
  C(winClosePressBg, "#c50f1f");
  C(winPressBg, "rgba(255,255,255,0.2)");
  C(winCloseHoverBg, "#e81123");
  C(winCloseHoverIcon, "#ffffff");
  C(track, "rgba(255,255,255,0.18)");
  C(buffer, "rgba(255,255,255,0.38)");
  C(played, "#ffffff");
  C(knob, "#ffffff");
  C(knobHaloDrag, "rgba(255,255,255,0.16)");
  C(chapterTick, "rgba(255,255,255,0.5)");
  C(playButtonBg, "rgba(0,0,0,0.42)");
  C(playButtonBorder, "rgba(0,0,0,0)");
  C(playButtonIcon, "#ffffff");
  C(previewBg, "#101216");
  C(previewBorder, "rgba(255,255,255,0.24)");
  C(previewTimeBg, "rgba(16,18,22,0.9)");
  C(previewTimeText, "#e2e5ea");
  C(stageBg, "#23262b");
  C(btnHoverBg, "rgba(255,255,255,0.18)");
  C(btnPressBg, "rgba(255,255,255,0.28)");
  C(muteIcon, "rgba(255,255,255,0.72)");
  C(muteTrack, "rgba(255,255,255,0.12)");
  C(muteKnob, "rgba(255,255,255,0.5)");
  SHAPE(t, 12, 2, true, 0, true, 0, false);
  LAYOUT(t);
  return t;
}

struct SkinFactory {
  const char* id;
  Theme (*make)();
};

// 内置皮肤工厂：只保留「Fluent 深灰白」一套（其余皮肤全部走 assets/skins.json，
// 运行期从 JSON 装载、不编进二进制 —— 改皮肤不必重编）。
const SkinFactory kFactories[] = {
    {"fluent", make_fluent},
};

}  // namespace

const char* Theme::kSkinIds[1] = {"fluent"};

Theme Theme::make_default() { return make_fluent(); }

const Theme& Theme::get(const std::string& id) {
  // 每皮肤构造一次、缓存复用（朴素静态表足够）
  static Theme cache[1];
  static bool inited[1] = {false};
  for (int i = 0; i < kSkinCount; i++) {
    if (id == kSkinIds[i]) {
      if (!inited[i]) {
        cache[i] = kFactories[i].make();
        inited[i] = true;
      }
      return cache[i];
    }
  }
  return get("fluent");  // 未知 id 回落基准皮肤
}

}  // namespace fr
