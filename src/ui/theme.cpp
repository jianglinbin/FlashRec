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

Theme make_frost() {
  Theme t;
  C(barBg, "rgba(250,250,252,0.93)");
  C(topBarBg, "#fafafc");
  C(barBorderTopBar, "rgba(0,0,0,0.07)");
  C(barBorderBottomBar, "rgba(0,0,0,0.07)");
  C(titleText, "#1c1d21");
  C(subText, "#6b6f76");
  C(mutedText, "#6b6f76");
  C(badgeBg, "rgba(0,0,0,0.06)");
  C(badgeText, "#6b6f76");
  C(icon, "#3a3f47");
  C(iconDim, "#6b6f76");
  C(iconDisabled, "rgba(28,29,33,0.3)");
  C(winIcon, "#6b6f76");
  C(winHoverBg, "rgba(0,0,0,0.08)");
  C(winClosePressBg, "#c50f1f");
  C(winPressBg, "rgba(0,0,0,0.14)");
  C(winCloseHoverBg, "#e81123");
  C(winCloseHoverIcon, "#ffffff");
  C(track, "rgba(0,0,0,0.12)");
  C(buffer, "rgba(0,0,0,0.26)");
  C(played, "#1c1d21");
  C(knob, "#1c1d21");
  C(knobHaloDrag, "rgba(0,0,0,0.12)");
  C(chapterTick, "rgba(0,0,0,0.35)");
  C(playButtonBg, "rgba(250,250,252,0.9)");
  C(playButtonBorder, "rgba(0,0,0,0)");
  C(playButtonIcon, "#1c1d21");
  C(previewBg, "#ffffff");
  C(previewBorder, "rgba(0,0,0,0.14)");
  C(previewTimeBg, "rgba(255,255,255,0.95)");
  C(previewTimeText, "#1c1d21");
  C(stageBg, "#23262b");
  C(btnHoverBg, "rgba(0,0,0,0.10)");
  C(btnPressBg, "rgba(0,0,0,0.18)");
  C(muteIcon, "rgba(28,29,33,0.42)");
  C(muteTrack, "rgba(0,0,0,0.08)");
  C(muteKnob, "rgba(28,29,33,0.28)");
  SHAPE(t, 12, 2, true, 0, true, 0, false);
  LAYOUT(t);
  return t;
}

Theme make_amber() {
  Theme t;
  C(barBg, "rgba(26,21,15,0.87)");
  C(topBarBg, "#1a150f");
  C(barBorderTopBar, "rgba(224,167,94,0.18)");
  C(barBorderBottomBar, "rgba(224,167,94,0.18)");
  C(titleText, "#f0e6d8");
  C(subText, "#b9a894");
  C(mutedText, "#e0a75e");
  C(badgeBg, "rgba(224,167,94,0.15)");
  C(badgeText, "#e0a75e");
  C(icon, "#c9b295");
  C(iconDim, "#c9b295");
  C(iconDisabled, "rgba(240,230,216,0.3)");
  C(winIcon, "#c9b295");
  C(winHoverBg, "rgba(224,167,94,0.16)");
  C(winClosePressBg, "#c50f1f");
  C(winPressBg, "rgba(224,167,94,0.26)");
  C(winCloseHoverBg, "#e81123");
  C(winCloseHoverIcon, "#ffffff");
  C(track, "rgba(255,255,255,0.14)");
  C(buffer, "rgba(224,167,94,0.4)");
  C(played, "#e0a75e");
  C(knob, "#fff8ec");
  C(knobHaloDrag, "rgba(224,167,94,0.2)");
  C(chapterTick, "rgba(224,167,94,0.55)");
  C(playButtonBg, "rgba(26,21,15,0.7)");
  C(playButtonBorder, "rgba(0,0,0,0)");
  C(playButtonIcon, "#f0e6d8");
  C(previewBg, "#171310");
  C(previewBorder, "rgba(224,167,94,0.3)");
  C(previewTimeBg, "rgba(23,19,16,0.92)");
  C(previewTimeText, "#f0e6d8");
  C(stageBg, "#23262b");
  C(btnHoverBg, "rgba(255,236,205,0.16)");
  C(btnPressBg, "rgba(255,236,205,0.26)");
  C(muteIcon, "rgba(255,236,205,0.6)");
  C(muteTrack, "rgba(255,236,205,0.12)");
  C(muteKnob, "rgba(255,236,205,0.45)");
  SHAPE(t, 12, 2, true, 0, true, 0, false);
  LAYOUT(t);
  return t;
}

Theme make_teal() {
  Theme t;
  C(barBg, "rgba(12,26,26,0.88)");
  C(topBarBg, "#0c1a1a");
  C(barBorderTopBar, "rgba(45,212,191,0.18)");
  C(barBorderBottomBar, "rgba(45,212,191,0.18)");
  C(titleText, "#dff5f0");
  C(subText, "#8fb5ae");
  C(mutedText, "#2dd4bf");
  C(badgeBg, "rgba(45,212,191,0.14)");
  C(badgeText, "#2dd4bf");
  C(icon, "#9dc4bd");
  C(iconDim, "#9dc4bd");
  C(iconDisabled, "rgba(223,245,240,0.3)");
  C(winIcon, "#9dc4bd");
  C(winHoverBg, "rgba(45,212,191,0.14)");
  C(winClosePressBg, "#c50f1f");
  C(winPressBg, "rgba(45,212,191,0.24)");
  C(winCloseHoverBg, "#e81123");
  C(winCloseHoverIcon, "#ffffff");
  C(track, "rgba(255,255,255,0.13)");
  C(buffer, "rgba(45,212,191,0.38)");
  C(played, "#2dd4bf");
  C(knob, "#eafffb");
  C(knobHaloDrag, "rgba(45,212,191,0.2)");
  C(chapterTick, "rgba(45,212,191,0.55)");
  C(playButtonBg, "rgba(12,26,26,0.72)");
  C(playButtonBorder, "rgba(0,0,0,0)");
  C(playButtonIcon, "#eafffb");
  C(previewBg, "#0d1b1b");
  C(previewBorder, "rgba(45,212,191,0.3)");
  C(previewTimeBg, "rgba(13,27,27,0.92)");
  C(previewTimeText, "#dff5f0");
  C(stageBg, "#23262b");
  C(btnHoverBg, "rgba(205,244,240,0.16)");
  C(btnPressBg, "rgba(205,244,240,0.26)");
  C(muteIcon, "rgba(205,244,240,0.6)");
  C(muteTrack, "rgba(205,244,240,0.12)");
  C(muteKnob, "rgba(205,244,240,0.45)");
  SHAPE(t, 12, 2, true, 0, true, 0, false);
  LAYOUT(t);
  return t;
}

Theme make_neon() {
  Theme t;
  C(barBg, "rgba(16,12,24,0.9)");
  C(topBarBg, "#100c18");
  C(barBorderTopBar, "rgba(255,77,157,0.3)");
  C(barBorderBottomBar, "rgba(255,77,157,0.3)");
  C(titleText, "#f2e9f7");
  C(subText, "#a693b8");
  C(mutedText, "#ff4d9d");
  C(badgeBg, "rgba(255,77,157,0.16)");
  C(badgeText, "#ff4d9d");
  C(icon, "#b9a4c6");
  C(iconDim, "#b9a4c6");
  C(iconDisabled, "rgba(242,233,247,0.3)");
  C(winIcon, "#b9a4c6");
  C(winHoverBg, "rgba(255,77,157,0.16)");
  C(winClosePressBg, "#c50f1f");
  C(winPressBg, "rgba(255,77,157,0.26)");
  C(winCloseHoverBg, "#e81123");
  C(winCloseHoverIcon, "#ffffff");
  C(track, "rgba(255,255,255,0.12)");
  C(buffer, "rgba(255,77,157,0.32)");
  C(played, "#ff4d9d");
  C(knob, "#ffffff");
  C(knobHaloDrag, "rgba(255,77,157,0.22)");
  C(chapterTick, "rgba(255,77,157,0.5)");
  C(playButtonBg, "rgba(16,12,24,0.75)");
  C(playButtonBorder, "rgba(255,77,157,0.35)");
  C(playButtonIcon, "#ff4d9d");
  C(previewBg, "#120e1a");
  C(previewBorder, "rgba(255,77,157,0.32)");
  C(previewTimeBg, "rgba(18,14,26,0.92)");
  C(previewTimeText, "#f2e9f7");
  C(stageBg, "#23262b");
  C(btnHoverBg, "rgba(255,120,220,0.20)");
  C(btnPressBg, "rgba(255,120,220,0.32)");
  C(muteIcon, "rgba(255,120,220,0.62)");
  C(muteTrack, "rgba(255,120,220,0.12)");
  C(muteKnob, "rgba(255,120,220,0.5)");
  SHAPE(t, 12, 2, true, 0, true, 0, true);
  LAYOUT(t);
  return t;
}

Theme make_mono() {
  Theme t;
  C(barBg, "rgba(6,6,7,0.94)");
  C(topBarBg, "#060607");
  C(barBorderTopBar, "rgba(255,255,255,0.14)");
  C(barBorderBottomBar, "rgba(255,255,255,0.14)");
  C(titleText, "#ffffff");
  C(subText, "rgba(255,255,255,0.62)");
  C(mutedText, "rgba(255,255,255,0.6)");
  C(badgeBg, "rgba(255,255,255,0.1)");
  C(badgeText, "rgba(255,255,255,0.6)");
  C(icon, "rgba(255,255,255,0.8)");
  C(iconDim, "rgba(255,255,255,0.75)");
  C(iconDisabled, "rgba(255,255,255,0.32)");
  C(winIcon, "rgba(255,255,255,0.75)");
  C(winHoverBg, "rgba(255,255,255,0.14)");
  C(winClosePressBg, "#c50f1f");
  C(winPressBg, "rgba(255,255,255,0.22)");
  C(winCloseHoverBg, "#e81123");
  C(winCloseHoverIcon, "#ffffff");
  C(track, "rgba(255,255,255,0.14)");
  C(buffer, "rgba(255,255,255,0.3)");
  C(played, "#ffffff");
  C(knob, "#ffffff");
  C(knobHaloDrag, "rgba(255,255,255,0.18)");
  C(chapterTick, "rgba(255,255,255,0.45)");
  C(playButtonBg, "rgba(6,6,7,0.6)");
  C(playButtonBorder, "rgba(255,255,255,0.3)");
  C(playButtonIcon, "#ffffff");
  C(previewBg, "#0b0b0c");
  C(previewBorder, "rgba(255,255,255,0.3)");
  C(previewTimeBg, "rgba(11,11,12,0.94)");
  C(previewTimeText, "#ffffff");
  C(stageBg, "#23262b");
  C(btnHoverBg, "rgba(255,255,255,0.2)");
  C(btnPressBg, "rgba(255,255,255,0.3)");
  C(muteIcon, "rgba(255,255,255,0.7)");
  C(muteTrack, "rgba(255,255,255,0.12)");
  C(muteKnob, "rgba(255,255,255,0.5)");
  SHAPE(t, 0, 0, false, 10, false, 42, true);
  LAYOUT(t);
  return t;
}

struct SkinFactory {
  const char* id;
  Theme (*make)();
};

const SkinFactory kFactories[] = {
    {"fluent", make_fluent}, {"frost", make_frost}, {"amber", make_amber},
    {"teal", make_teal},     {"neon", make_neon},   {"mono", make_mono},
};

}  // namespace

const char* Theme::kSkinIds[6] = {"fluent", "frost", "amber", "teal", "neon", "mono"};

Theme Theme::make_default() { return make_fluent(); }

const Theme& Theme::get(const std::string& id) {
  // 每皮肤构造一次、缓存复用（朴素静态表足够）
  static Theme cache[6];
  static bool inited[6] = {false, false, false, false, false, false};
  for (int i = 0; i < 6; i++) {
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
