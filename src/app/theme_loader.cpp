#include "app/theme_loader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

#include "app/log.h"

namespace fr {

namespace {

// ============================ 极简 JSON 解析器 ============================
// 支持 object/array/string/number/bool/null；递归深度与输入规模受限；
// 失败可定位（err），绝不抛异常/读越界。
struct JVal {
  enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<JVal> arr;
  std::vector<std::pair<std::string, JVal>> obj;

  const JVal* get(const char* k) const {
    if (t != Obj) return nullptr;
    for (const auto& kv : obj)
      if (kv.first == k) return &kv.second;
    return nullptr;
  }
};

class JParser {
 public:
  explicit JParser(const std::string& s) : s_(s) {}
  bool parse(JVal* out) {
    i_ = 0;
    skip_ws();
    if (!value(out, 0)) return false;
    skip_ws();
    if (i_ != s_.size()) return fail("trailing characters after JSON value");
    return true;
  }
  const std::string& err() const { return err_; }

 private:
  const std::string& s_;
  size_t i_ = 0;
  std::string err_;

  void skip_ws() {
    while (i_ < s_.size() &&
           (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\r' || s_[i_] == '\n'))
      ++i_;
  }
  bool fail(const char* m) {
    if (err_.empty()) err_ = m;
    return false;
  }
  bool lit(const char* w) {
    const size_t n = std::strlen(w);
    if (s_.compare(i_, n, w) == 0) {
      i_ += n;
      return true;
    }
    return false;
  }
  bool str(std::string* out) {
    if (i_ >= s_.size() || s_[i_] != '"') return fail("expected string");
    ++i_;
    out->clear();
    while (i_ < s_.size()) {
      char c = s_[i_++];
      if (c == '"') return true;
      if (c == '\\') {
        if (i_ >= s_.size()) return fail("bad escape");
        char e = s_[i_++];
        switch (e) {
          case 'n': *out += '\n'; break;
          case 't': *out += '\t'; break;
          case 'r': *out += '\r'; break;
          case 'b': *out += '\b'; break;
          case 'f': *out += '\f'; break;
          case '"': *out += '"'; break;
          case '\\': *out += '\\'; break;
          case '/': *out += '/'; break;
          case 'u': {
            if (i_ + 4 > s_.size()) return fail("bad \\u");
            int cp = 0;
            for (int k = 0; k < 4; ++k) {
              char h = s_[i_ + k];
              int d = (h >= '0' && h <= '9')   ? h - '0'
                      : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                      : (h >= 'A' && h <= 'F') ? h - 'A' + 10
                                               : -1;
              if (d < 0) return fail("bad \\u hex");
              cp = cp * 16 + d;
            }
            i_ += 4;
            if (cp < 0x80) {
              *out += static_cast<char>(cp);
            } else if (cp < 0x800) {
              *out += static_cast<char>(0xC0 | (cp >> 6));
              *out += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
              *out += static_cast<char>(0xE0 | (cp >> 12));
              *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
              *out += static_cast<char>(0x80 | (cp & 0x3F));
            }
            break;
          }
          default: return fail("unknown escape");
        }
      } else {
        *out += c;
      }
    }
    return fail("unterminated string");
  }
  bool number(JVal* v) {
    const size_t start = i_;
    if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
    while (i_ < s_.size() && ((s_[i_] >= '0' && s_[i_] <= '9') || s_[i_] == '.' ||
                              s_[i_] == 'e' || s_[i_] == 'E' || s_[i_] == '+' ||
                              s_[i_] == '-'))
      ++i_;
    std::string tok = s_.substr(start, i_ - start);
    if (tok.empty()) return fail("expected value");
    char* end = nullptr;
    double d = std::strtod(tok.c_str(), &end);
    if (end == tok.c_str() || !std::isfinite(d)) return fail("bad number");
    v->t = JVal::Num;
    v->n = d;
    return true;
  }
  bool array(JVal* v, int depth) {
    if (i_ >= s_.size() || s_[i_] != '[') return fail("expected [");
    ++i_;
    v->t = JVal::Arr;
    skip_ws();
    if (i_ < s_.size() && s_[i_] == ']') {
      ++i_;
      return true;
    }
    for (;;) {
      JVal e;
      if (!value(&e, depth + 1)) return false;
      v->arr.push_back(std::move(e));
      skip_ws();
      if (i_ < s_.size() && s_[i_] == ',') {
        ++i_;
        continue;
      }
      if (i_ < s_.size() && s_[i_] == ']') {
        ++i_;
        return true;
      }
      return fail("expected , or ]");
    }
  }
  bool object(JVal* v, int depth) {
    if (i_ >= s_.size() || s_[i_] != '{') return fail("expected {");
    ++i_;
    v->t = JVal::Obj;
    skip_ws();
    if (i_ < s_.size() && s_[i_] == '}') {
      ++i_;
      return true;
    }
    for (;;) {
      skip_ws();
      std::string key;
      if (!str(&key)) return false;
      skip_ws();
      if (i_ >= s_.size() || s_[i_] != ':') return fail("expected :");
      ++i_;
      JVal val;
      if (!value(&val, depth + 1)) return false;
      v->obj.emplace_back(std::move(key), std::move(val));
      skip_ws();
      if (i_ < s_.size() && s_[i_] == ',') {
        ++i_;
        continue;
      }
      if (i_ < s_.size() && s_[i_] == '}') {
        ++i_;
        return true;
      }
      return fail("expected , or }");
    }
  }
  bool value(JVal* v, int depth) {
    if (depth > 64) return fail("nesting too deep");
    skip_ws();
    if (i_ >= s_.size()) return fail("unexpected end");
    const char c = s_[i_];
    if (c == '{') return object(v, depth);
    if (c == '[') return array(v, depth);
    if (c == '"') {
      v->t = JVal::Str;
      return str(&v->s);
    }
    if (lit("true")) {
      v->t = JVal::Bool;
      v->b = true;
      return true;
    }
    if (lit("false")) {
      v->t = JVal::Bool;
      v->b = false;
      return true;
    }
    if (lit("null")) {
      v->t = JVal::Null;
      return true;
    }
    return number(v);
  }
};

// ============================ 取值 / 校验 ============================

bool parse_color_checked(const std::string& s, NVGcolor* out) {
  auto hex = [](char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  if (s.size() >= 7 && s[0] == '#') {
    unsigned v = 0;
    for (int i = 1; i <= 6; ++i) {
      const int h = hex(s[i]);
      if (h < 0) return false;
      v = v * 16 + h;
    }
    *out = nvgRGB((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff);
    return true;
  }
  if (s.rfind("rgba(", 0) == 0) {
    float r, g, b, a;
    if (std::sscanf(s.c_str() + 5, "%f,%f,%f,%f", &r, &g, &b, &a) == 4) {
      *out = nvgRGBAf(r / 255.f, g / 255.f, b / 255.f, a);
      return true;
    }
    return false;
  }
  if (s.rfind("rgb(", 0) == 0) {
    float r, g, b;
    if (std::sscanf(s.c_str() + 4, "%f,%f,%f", &r, &g, &b) == 3) {
      *out = nvgRGBAf(r / 255.f, g / 255.f, b / 255.f, 1.f);
      return true;
    }
    return false;
  }
  return false;
}

const JVal* dig(const JVal& root, std::initializer_list<const char*> path) {
  const JVal* cur = &root;
  for (const char* k : path) {
    cur = cur->get(k);
    if (!cur) return nullptr;
  }
  return cur;
}

void num_field(const JVal& root, std::initializer_list<const char*> path, float* dst,
               const char* ctx, std::vector<std::string>* warns) {
  if (const JVal* v = dig(root, path)) {
    if (v->t == JVal::Num) {
      *dst = static_cast<float>(v->n);
      return;
    }
    if (warns) warns->push_back(std::string(ctx) + ": 非数值，保留默认");
  }
}

void dbl_field(const JVal& root, std::initializer_list<const char*> path, double* dst,
               const char* ctx, std::vector<std::string>* warns) {
  if (const JVal* v = dig(root, path)) {
    if (v->t == JVal::Num) {
      *dst = v->n;
      return;
    }
    if (warns) warns->push_back(std::string(ctx) + ": 非数值，保留默认");
  }
}

void bool_from_shape(const JVal& shape, const char* key, bool* dst, const char* ctx,
                     std::vector<std::string>* warns) {
  if (const JVal* v = shape.get(key)) {
    if (v->t == JVal::Bool) {
      *dst = v->b;
      return;
    }
    if (v->t == JVal::Str) {
      if (v->s == "circle") {
        *dst = true;
        return;
      }
      if (v->s == "square") {
        *dst = false;
        return;
      }
    }
    if (warns) warns->push_back(std::string(ctx) + "." + key + ": 非 circle/square，保留默认");
  }
}

// ===== 颜色表（名称 → 成员指针）=====
struct ColRef {
  const char* name;
  NVGcolor Theme::*p;
};
const ColRef kCols[] = {
    {"topBarBg", &Theme::topBarBg},
    {"barBg", &Theme::barBg},
    {"barBorderTopBar", &Theme::barBorderTopBar},
    {"barBorderBottomBar", &Theme::barBorderBottomBar},
    {"titleText", &Theme::titleText},
    {"subText", &Theme::subText},
    {"mutedText", &Theme::mutedText},
    {"badgeBg", &Theme::badgeBg},
    {"badgeText", &Theme::badgeText},
    {"icon", &Theme::icon},
    {"iconDim", &Theme::iconDim},
    {"iconDisabled", &Theme::iconDisabled},
    {"winIcon", &Theme::winIcon},
    {"winHoverBg", &Theme::winHoverBg},
    {"winPressBg", &Theme::winPressBg},
    {"winCloseHoverBg", &Theme::winCloseHoverBg},
    {"winClosePressBg", &Theme::winClosePressBg},
    {"winCloseHoverIcon", &Theme::winCloseHoverIcon},
    {"btnHoverBg", &Theme::btnHoverBg},
    {"btnPressBg", &Theme::btnPressBg},
    {"btnBg", &Theme::btnBg},  // d201 按钮常态底色
    {"btnFg", &Theme::btnFg},  // d201 按钮前景
    {"muteIcon", &Theme::muteIcon},
    {"muteTrack", &Theme::muteTrack},
    {"muteKnob", &Theme::muteKnob},
    {"track", &Theme::track},
    {"buffer", &Theme::buffer},
    {"played", &Theme::played},
    {"knob", &Theme::knob},
    {"knobHaloDrag", &Theme::knobHaloDrag},
    {"chapterTick", &Theme::chapterTick},
    {"playButtonBg", &Theme::playButtonBg},
    {"playButtonBorder", &Theme::playButtonBorder},
    {"playButtonIcon", &Theme::playButtonIcon},
    {"previewBg", &Theme::previewBg},
    {"previewBorder", &Theme::previewBorder},
    {"previewTimeBg", &Theme::previewTimeBg},
    {"previewTimeText", &Theme::previewTimeText},
    {"stageBg", &Theme::stageBg},
    {"stageBorder", &Theme::stageBorder},  // d193 视频区内嵌浮卡外描边
    {"stageText", &Theme::stageText},        // d220 舞台主文字色
    {"stageSubText", &Theme::stageSubText},  // d220 舞台次文字色
    {"winBorder", &Theme::winBorder},      // d198 窗口外框
    {"topBarBorder", &Theme::topBarBorder},  // d198 标题栏四边
    {"botBarBorder", &Theme::botBarBorder},  // d198 操作栏四边
};

void apply_colors(Theme* t, const JVal& colors, const char* ctx,
                  std::vector<std::string>* warns) {
  if (colors.t != JVal::Obj) return;
  for (const auto& kv : colors.obj) {
    const ColRef* ref = nullptr;
    for (const auto& c : kCols)
      if (kv.first == c.name) {
        ref = &c;
        break;
      }
    if (!ref) {
      if (warns) warns->push_back(std::string(ctx) + ": 未知颜色键 " + kv.first);
      continue;
    }
    if (kv.second.t != JVal::Str) {
      if (warns) warns->push_back(std::string(ctx) + "." + kv.first + ": 非字符串");
      continue;
    }
    NVGcolor col{};
    if (!parse_color_checked(kv.second.s, &col)) {
      if (warns) warns->push_back(std::string(ctx) + "." + kv.first + ": 颜色非法，保留默认");
      continue;
    }
    t->*(ref->p) = col;
  }
}

void apply_shape(Theme* t, const JVal& shape, const char* ctx, std::vector<std::string>* warns) {
  if (shape.t != JVal::Obj) return;
  num_field(shape, {"windowRadius"}, &t->shape.windowRadius, ctx, warns);
  num_field(shape, {"progressRadius"}, &t->shape.progressRadius, ctx, warns);
  num_field(shape, {"knobSizeOverride"}, &t->shape.knobSizeOverride, ctx, warns);
  num_field(shape, {"playButtonSizeOverride"}, &t->shape.playButtonSizeOverride, ctx, warns);
  bool_from_shape(shape, "knobShape", &t->shape.knobCircle, ctx, warns);
  bool_from_shape(shape, "playButtonShape", &t->shape.playButtonCircle, ctx, warns);
  if (const JVal* v = shape.get("playButtonTinted")) {
    if (v->t == JVal::Bool)
      t->shape.playButtonTinted = v->b;
    else if (warns)
      warns->push_back(std::string(ctx) + ".playButtonTinted: 非布尔");
  }
}

void apply_layout(Theme* t, const JVal& layout, const char* ctx,
                  std::vector<std::string>* warns) {
  if (layout.t != JVal::Obj) return;
  using L = Theme::Layout;
  L& l = t->layout;
  num_field(layout, {"topBar", "height"}, &l.topBarH, ctx, warns);
  num_field(layout, {"topBar", "paddingLeft"}, &l.topBarPadL, ctx, warns);
  num_field(layout, {"title", "fontSize"}, &l.titleFont, ctx, warns);
  num_field(layout, {"badge", "fontSize"}, &l.badgeFont, ctx, warns);
  num_field(layout, {"badge", "radius"}, &l.badgeRadius, ctx, warns);
  num_field(layout, {"badge", "paddingX"}, &l.badgePadX, ctx, warns);
  num_field(layout, {"badge", "paddingY"}, &l.badgePadY, ctx, warns);
  num_field(layout, {"badge", "marginLeft"}, &l.badgeMarginL, ctx, warns);
  num_field(layout, {"winButtons", "width"}, &l.winBtnW, ctx, warns);
  num_field(layout, {"winButtons", "height"}, &l.winBtnH, ctx, warns);
  num_field(layout, {"winButtons", "iconSize"}, &l.winIconSize, ctx, warns);
  num_field(layout, {"winButtons", "iconStroke"}, &l.winIconStroke, ctx, warns);
  num_field(layout, {"bottomBar", "height"}, &l.botBarH, ctx, warns);
  num_field(layout, {"bottomBar", "paddingX"}, &l.botBarPadX, ctx, warns);
  num_field(layout, {"bottomBar", "gap"}, &l.botBarGap, ctx, warns);
  num_field(layout, {"bottomBar", "button", "hitSize"}, &l.btnHit, ctx, warns);
  num_field(layout, {"bottomBar", "button", "boxSize"}, &l.btnBoxW, ctx, warns);
  num_field(layout, {"bottomBar", "button", "radius"}, &l.btnRadius, ctx, warns);
  num_field(layout, {"bottomBar", "button", "pitch"}, &l.btnPitch, ctx, warns);
  num_field(layout, {"bottomBar", "button", "iconSize"}, &l.btnIcon, ctx, warns);  // d204
  if (const JVal* v = dig(layout, {"bottomBar", "button", "circle"}))
    if (v->t == JVal::Bool) l.btnCircle = v->b;  // d204：底栏按钮圆形开关
  num_field(layout, {"progress", "trackHeight"}, &l.trackH, ctx, warns);
  num_field(layout, {"progress", "radius"}, &t->shape.progressRadius, ctx, warns);
  num_field(layout, {"progress", "knobSize"}, &l.knobSize, ctx, warns);
  num_field(layout, {"progress", "knobSizeHover"}, &l.knobSizeHover, ctx, warns);
  num_field(layout, {"progress", "knobHaloSizeDrag"}, &l.knobHaloDrag, ctx, warns);
  num_field(layout, {"progress", "chapterTickWidth"}, &l.chapterTickW, ctx, warns);
  num_field(layout, {"progress", "chapterTickHeight"}, &l.chapterTickH, ctx, warns);
  num_field(layout, {"thumbPreview", "width"}, &l.thumbW, ctx, warns);
  num_field(layout, {"thumbPreview", "height"}, &l.thumbH, ctx, warns);
  num_field(layout, {"thumbPreview", "radius"}, &l.thumbRadius, ctx, warns);
  num_field(layout, {"thumbPreview", "gapAboveProgress"}, &l.thumbGapAbove, ctx, warns);
  num_field(layout, {"volumeBar", "width"}, &l.volW, ctx, warns);
  num_field(layout, {"volumeBar", "height"}, &l.volH, ctx, warns);
  num_field(layout, {"volumeBar", "iconSize"}, &l.volIcon, ctx, warns);
  num_field(layout, {"volumeBar", "knobSize"}, &l.volKnob, ctx, warns);
  num_field(layout, {"volumeBar", "knobSizeHover"}, &l.volKnobHover, ctx, warns);
  num_field(layout, {"volumeBar", "hitPad"}, &l.volHitPad, ctx, warns);
  num_field(layout, {"timeText", "fontSize"}, &l.timeFont, ctx, warns);
  num_field(layout, {"speedControl", "fontSize"}, &l.speedFont, ctx, warns);
  num_field(layout, {"speedControl", "buttonWidth"}, &l.speedW, ctx, warns);
  num_field(layout, {"speedControl", "itemHeight"}, &l.speedItemH, ctx, warns);
  num_field(layout, {"speedControl", "menuPadY"}, &l.speedMenuPadY, ctx, warns);
  num_field(layout, {"playButtonCenter", "size"}, &l.centerBtnSize, ctx, warns);
  num_field(layout, {"playButtonCenter", "iconSize"}, &l.centerIconSize, ctx, warns);
  num_field(layout, {"border", "width"}, &l.borderWidth, ctx, warns);  // d198 三区块描边宽度
  // motion：毫秒 → 秒
  if (const JVal* v = dig(layout, {"motion", "idleHideMs"}))
    if (v->t == JVal::Num) l.idleHideSec = v->n / 1000.0;
  if (const JVal* v = dig(layout, {"motion", "barHoverHideMs"}))
    if (v->t == JVal::Num) l.barHoverHideSec = v->n / 1000.0;
  if (const JVal* v = dig(layout, {"motion", "fadeMs"}))
    if (v->t == JVal::Num) l.fadeSec = v->n / 1000.0;
  if (const JVal* v = dig(layout, {"motion", "hoverFadeMs"}))
    if (v->t == JVal::Num) l.hoverFadeSec = static_cast<float>(v->n / 1000.0);
  if (const JVal* v = dig(layout, {"motion", "pressFadeMs"}))
    if (v->t == JVal::Num) l.pressFadeSec = static_cast<float>(v->n / 1000.0);
  if (const JVal* v = dig(layout, {"motion", "pressScale"}))
    if (v->t == JVal::Num) l.btnPressScale = static_cast<float>(v->n);
  if (const JVal* v = dig(layout, {"motion", "dblClickMs"}))
    if (v->t == JVal::Num) l.dblClickSec = v->n / 1000.0;
}

// d193：视频区内嵌浮卡（stage: { inset, radius, border }）。inset>0 时画面收进内缩
// 圆角卡、卡外露 stageBg，border 为卡片描边（透明则不画）。
void apply_stage(Theme* t, const JVal& stage, const char* ctx, std::vector<std::string>* warns) {
  if (stage.t != JVal::Obj) return;
  num_field(stage, {"inset"}, &t->layout.stageInset, ctx, warns);
  num_field(stage, {"radius"}, &t->layout.stageRadius, ctx, warns);
  if (const JVal* v = stage.get("border")) {
    if (v->t != JVal::Str) {
      if (warns) warns->push_back(std::string(ctx) + ".stage.border: 非字符串");
    } else {
      NVGcolor c{};
      if (parse_color_checked(v->s, &c))
        t->stageBorder = c;
      else if (warns)
        warns->push_back(std::string(ctx) + ".stage.border: 颜色非法，保留默认");
    }
  }
  // fit: "cover"（短边填满/裁切）| "contain"（留边）—— 对应 mpv panscan
  if (const JVal* v = stage.get("fit")) {
    if (v->t != JVal::Str) {
      if (warns) warns->push_back(std::string(ctx) + ".stage.fit: 非字符串");
    } else if (v->s == "cover") {
      t->layout.stageCover = true;
    } else if (v->s == "contain") {
      t->layout.stageCover = false;
    } else if (warns) {
      warns->push_back(std::string(ctx) + ".stage.fit: 需 cover|contain，保留默认");
    }
  }
}

std::string read_file(const std::string& path, bool* ok) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    *ok = false;
    return {};
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  *ok = true;
  return ss.str();
}

bool parse_root(const std::string& path, JVal* root, std::string* err) {
  bool ok = false;
  const std::string text = read_file(path, &ok);
  if (!ok) {
    if (err) *err = "无法读取文件: " + path;
    return false;
  }
  JParser p(text);
  if (!p.parse(root)) {
    if (err) *err = "JSON 解析失败: " + p.err();
    return false;
  }
  if (root->t != JVal::Obj) {
    if (err) *err = "根不是对象";
    return false;
  }
  return true;
}

// schemaVersion 校验：> 支持上限（1）拒绝，避免按旧规则误读新文件。
bool check_version(const JVal& root, std::string* err) {
  if (const JVal* v = dig(root, {"meta", "schemaVersion"})) {
    if (v->t == JVal::Num && v->n > 1.0) {
      if (err) *err = "schemaVersion 过新（支持 1）";
      return false;
    }
  }
  return true;
}

}  // namespace

static bool bundle_internal(const std::string& path, const Theme& base,
                            std::vector<NamedTheme>* out, Theme* layout_base_out,
                            std::vector<std::string>* warns, std::string* err) {
  JVal root;
  if (!parse_root(path, &root, err)) return false;
  if (!check_version(root, err)) return false;
  const JVal* skins = root.get("skins");
  if (!skins || skins->t != JVal::Arr || skins->arr.empty()) {
    if (err) *err = "缺少 skins[]";
    return false;
  }
  // 全局 layout + stageBg 作为所有皮肤的基底
  Theme layout_base = base;
  if (const JVal* layout = root.get("layout")) apply_layout(&layout_base, *layout, "layout", warns);
  if (const JVal* sp = dig(root, {"stagePlaceholder", "bg"})) {
    if (sp->t == JVal::Str) {
      NVGcolor c{};
      if (parse_color_checked(sp->s, &c))
        layout_base.stageBg = c;
      else if (warns)
        warns->push_back("stagePlaceholder.bg: 颜色非法，保留默认");
    }
  }
  for (const JVal& s : skins->arr) {
    if (s.t != JVal::Obj) continue;
    NamedTheme nt;
    nt.theme = layout_base;
    if (const JVal* id = s.get("id"))
      if (id->t == JVal::Str) nt.meta.id = id->s;
    if (nt.meta.id.empty()) {
      if (warns) warns->push_back("皮肤缺少 id，跳过");
      continue;
    }
    if (const JVal* v = s.get("name"))
      if (v->t == JVal::Str) nt.meta.name = v->s;
    if (const JVal* v = s.get("personality"))
      if (v->t == JVal::Str) nt.meta.personality = v->s;
    if (const JVal* v = s.get("risk"))
      if (v->t == JVal::Str) nt.meta.risk = v->s;
    const std::string ctx = "skin[" + nt.meta.id + "]";
    if (const JVal* sh = s.get("shape")) apply_shape(&nt.theme, *sh, ctx.c_str(), warns);
    if (const JVal* co = s.get("colors")) apply_colors(&nt.theme, *co, ctx.c_str(), warns);
    if (const JVal* lo = s.get("layout")) apply_layout(&nt.theme, *lo, ctx.c_str(), warns);
    if (const JVal* st = s.get("stage")) apply_stage(&nt.theme, *st, ctx.c_str(), warns);
    out->push_back(std::move(nt));
  }
  if (layout_base_out) *layout_base_out = layout_base;
  return true;
}

bool load_skin_bundle(const std::string& path, const Theme& base,
                      std::vector<NamedTheme>* out, std::vector<std::string>* warns,
                      std::string* err) {
  return bundle_internal(path, base, out, nullptr, warns, err);
}

bool load_all_skins(const std::string& bundle_path, const std::string& user_dir,
                    const Theme& base, std::vector<NamedTheme>* out,
                    std::vector<std::string>* warns, std::string* err) {
  Theme layout_base = base;
  if (!bundle_internal(bundle_path, base, out, &layout_base, warns, err)) return false;
  // 用户目录：*.json，按文件名排序（稳定）；同名 id 覆盖内置。
  std::error_code ec;
  std::vector<std::string> files;
  if (std::filesystem::is_directory(user_dir, ec)) {
    for (const auto& e : std::filesystem::directory_iterator(user_dir, ec)) {
      if (!e.is_regular_file(ec)) continue;
      if (e.path().extension() == ".json") files.push_back(e.path().string());
    }
  }
  std::sort(files.begin(), files.end());
  for (const auto& f : files) {
    NamedTheme nt;
    std::string uerr;
    if (!load_skin_file(f, layout_base, &nt, warns, &uerr)) {
      if (warns) warns->push_back("用户皮肤跳过 " + f + "：" + uerr);
      continue;
    }
    bool replaced = false;
    for (auto& existing : *out) {
      if (existing.meta.id == nt.meta.id) {
        existing = std::move(nt);
        replaced = true;
        break;
      }
    }
    if (!replaced) out->push_back(std::move(nt));
    if (warns) warns->push_back("用户皮肤载入 " + nt.meta.id);
  }
  return true;
}

bool load_skin_file(const std::string& path, const Theme& base, NamedTheme* out,
                    std::vector<std::string>* warns, std::string* err) {
  JVal root;
  if (!parse_root(path, &root, err)) return false;
  if (!check_version(root, err)) return false;
  *out = NamedTheme{};
  out->theme = base;
  if (const JVal* v = root.get("id"))
    if (v->t == JVal::Str) out->meta.id = v->s;
  if (out->meta.id.empty()) {
    if (err) *err = "皮肤缺少 id";
    return false;
  }
  if (const JVal* v = root.get("name"))
    if (v->t == JVal::Str) out->meta.name = v->s;
  if (const JVal* v = root.get("personality"))
    if (v->t == JVal::Str) out->meta.personality = v->s;
  if (const JVal* v = root.get("risk"))
    if (v->t == JVal::Str) out->meta.risk = v->s;
  if (const JVal* sh = root.get("shape")) apply_shape(&out->theme, *sh, "skin", warns);
  if (const JVal* co = root.get("colors")) apply_colors(&out->theme, *co, "skin", warns);
  if (const JVal* lo = root.get("layout")) apply_layout(&out->theme, *lo, "skin", warns);
  if (const JVal* st = root.get("stage")) apply_stage(&out->theme, *st, "skin", warns);
  return true;
}

}  // namespace fr
