#include "ui/views/update_dialog.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <nanovg.h>

#include "app/i18n.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace fr {

namespace {

// UTF-8 码点长度（坏字节按 1 字节前进，绝不越界 —— 文案来自内置表/远端说明）
size_t utf8_step(const char* p, size_t remain) {
  const unsigned char c = (unsigned char)*p;
  size_t n = 1;
  if (c >= 0xF0) n = 4;
  else if (c >= 0xE0) n = 3;
  else if (c >= 0xC0) n = 2;
  return std::min(n, remain);
}

// 按内容宽度折行（中文无空格 → 逐码点折），最多 max_lines 行；超出在末行加省略号。
std::vector<std::string> wrap_text(NVGcontext* vg, float size, const char* text, float max_w,
                                   int max_lines) {
  std::vector<std::string> lines;
  if (text == nullptr || max_lines <= 0) return lines;
  const size_t len = std::strlen(text);
  size_t i = 0;
  std::string cur;
  bool truncated = false;
  while (i < len) {
    if (text[i] == '\n') {
      lines.push_back(cur);
      cur.clear();
      ++i;
      if ((int)lines.size() >= max_lines && i < len) {
        truncated = true;
        break;
      }
      continue;
    }
    const size_t n = utf8_step(text + i, len - i);
    std::string next = cur;
    next.append(text + i, n);
    if (!cur.empty() && text_width(vg, size, next.c_str()) > max_w) {
      lines.push_back(cur);
      cur.assign(text + i, n);
      if ((int)lines.size() >= max_lines) {
        truncated = i + n < len;
        cur.clear();
        break;
      }
    } else {
      cur.swap(next);
    }
    i += n;
  }
  if (!cur.empty() && (int)lines.size() < max_lines) lines.push_back(cur);
  if (lines.size() > (size_t)max_lines) lines.resize((size_t)max_lines);
  if (truncated && !lines.empty()) lines.back() += "…";
  return lines;
}

float dialog_pad(const Theme& t) { return std::max(18.f, t.layout.titleFont * 1.5f); }

float dialog_card_width(NVGcontext* vg, const Theme& t, float win_w, const UpdateUi& u) {
  const UpdateButtons b = update_dialog_buttons(u);
  float total = 0.f;
  for (int i = 0; i < b.count; ++i)
    total += std::max(72.f, text_width(vg, t.layout.titleFont,
                                       update_button_label(b.kinds[i], u)) +
                                 28.f);
  if (b.count > 1) total += 10.f * (b.count - 1);
  float w = std::min(560.f, win_w - 32.f);
  w = std::max(w, total + dialog_pad(t) * 2.f);
  w = std::min(w, win_w - 16.f);
  return std::max(280.f, w);
}

struct DialogContent {
  std::string title;
  std::string version;
  std::string size;
  std::string progress_text;
  std::string notes_title;
  std::vector<std::string> hint;
  std::vector<std::string> error;
  std::vector<std::string> warning;
  std::vector<std::string> notes;
  bool progress_bar = false;
};

DialogContent build_content(NVGcontext* vg, const Theme& t, const UpdateUi& u, float content_w,
                            int notes_cap) {
  DialogContent c;
  const float font = t.layout.titleFont;
  const float small = t.layout.titleFont * 0.92f;
  const bool has_version = u.version[0] != '\0';
  switch (u.stage) {
    case UpdateStage::Checking: c.title = tr(i18n::StrId::UpdDlgTitleChecking); break;
    case UpdateStage::Downloading:
      c.title = tr(i18n::StrId::UpdDlgTitleDownloading);
      break;
    case UpdateStage::UpToDate: c.title = tr(i18n::StrId::UpdDlgTitleUpToDate); break;
    case UpdateStage::Skipped: c.title = tr(i18n::StrId::UpdDlgTitleSkipped); break;
    case UpdateStage::Failed: c.title = tr(i18n::StrId::UpdDlgTitleFailed); break;
    case UpdateStage::Installing: c.title = tr(i18n::StrId::UpdDlgTitleInstalling); break;
    case UpdateStage::Ready:
    case UpdateStage::ManualOnly:
      c.title = has_version
                    ? i18n::trf(i18n::StrId::UpdDlgTitleAvailableFmt, {u.version})
                    : tr(i18n::StrId::UpdTrayCheck);
      break;
    case UpdateStage::Hidden: break;
  }
  if (has_version && u.current[0] && u.stage != UpdateStage::UpToDate &&
      u.stage != UpdateStage::Skipped && u.stage != UpdateStage::Checking)
    c.version = i18n::trf(i18n::StrId::UpdDlgVersionFmt, {u.current, u.version});
  if (u.size[0] && (u.stage == UpdateStage::Ready || u.stage == UpdateStage::Downloading ||
                    (u.stage == UpdateStage::ManualOnly && u.download_available)))
    c.size = i18n::trf(i18n::StrId::UpdDlgSizeFmt, {u.size});

  if (u.stage == UpdateStage::Downloading) {
    c.progress_bar = true;
    if (u.progress >= 0.f) {
      char pct[16];
      std::snprintf(pct, sizeof(pct), "%d", (int)std::lround(u.progress * 100.f));
      c.progress_text = i18n::trf(i18n::StrId::UpdDlgProgressFmt, {pct});
    }
  }

  switch (u.stage) {
    case UpdateStage::Ready:
      switch (u.asset) {
        case platform::InstallShape::WindowsPortable:
          c.hint = wrap_text(vg, small, tr(i18n::StrId::UpdDlgPortableHint), content_w, 3);
          break;
        case platform::InstallShape::LinuxDeb:
          c.hint = wrap_text(vg, small, tr(i18n::StrId::UpdDlgDebHint), content_w, 3);
          break;
        case platform::InstallShape::Unknown:
          c.hint = wrap_text(vg, small, tr(i18n::StrId::UpdDlgDevHint), content_w, 3);
          break;
        default:
          c.hint = wrap_text(vg, small, tr(i18n::StrId::UpdDlgReadyHint), content_w, 2);
          break;
      }
      break;
    case UpdateStage::ManualOnly:
      c.hint = wrap_text(vg, small,
                         tr(u.download_available ? i18n::StrId::UpdDlgManualHint
                                                 : i18n::StrId::UpdDlgNoAssetHint),
                         content_w, 3);
      break;
    case UpdateStage::Installing:
      c.hint = wrap_text(vg, small, tr(i18n::StrId::UpdDlgInstallingHint), content_w, 2);
      break;
    case UpdateStage::Failed: {
      const char* reason = u.error[0] ? u.error : tr(i18n::StrId::UpdErrUnknown);
      c.error = wrap_text(vg, small, reason, content_w, 3);
      break;
    }
    default: break;
  }
  if (u.stage == UpdateStage::Ready && u.cast_active)
    c.warning = wrap_text(vg, font, tr(i18n::StrId::UpdDlgCastWarning), content_w, 2);

  if ((u.stage == UpdateStage::Ready || u.stage == UpdateStage::ManualOnly) &&
      u.notes[0] && notes_cap > 0) {
    c.notes = wrap_text(vg, small, u.notes, content_w, notes_cap);
    if (!c.notes.empty()) c.notes_title = tr(i18n::StrId::UpdDlgNotesTitle);
  }
  return c;
}

struct LayoutParts {
  UpdateDialogLayout lay;
  DialogContent content;
  float title_h = 0;
  float font = 0, font_title = 0, small = 0;
};

LayoutParts build_layout(NVGcontext* vg, const Theme& t, float win_w, float win_h,
                         const UpdateUi& u, int notes_cap) {
  const auto& L = t.layout;
  LayoutParts p;
  const float pad = dialog_pad(t);
  const float card_w = dialog_card_width(vg, t, win_w, u);
  const float content_w = card_w - pad * 2.f;
  p.font = L.titleFont;
  p.font_title = L.titleFont * 1.35f;
  p.small = L.titleFont * 0.92f;
  p.title_h = p.font_title * 1.35f;
  p.content = build_content(vg, t, u, content_w, notes_cap);

  UpdateDialogLayout& lay = p.lay;
  lay.pad = pad;
  lay.content_w = content_w;
  lay.w = card_w;
  lay.row_h = p.font * 1.55f;
  lay.notes_line_h = p.small * 1.55f;
  lay.notes_cap = notes_cap;

  const float btn_gap = 10.f;
  const UpdateButtons b = update_dialog_buttons(u);
  lay.btn_count = b.count;
  lay.btn_h = std::max(28.f, p.font * 2.4f);
  float total = 0.f;
  for (int i = 0; i < b.count; ++i) {
    lay.btn_w[i] = std::max(72.f, text_width(vg, p.font, update_button_label(b.kinds[i], u)) +
                                     28.f);
    total += lay.btn_w[i];
  }
  if (b.count > 1) total += btn_gap * (b.count - 1);

  float top = pad;
  lay.title_y = top + p.title_h * 0.5f;
  top += p.title_h + p.font * 0.6f;
  if (!p.content.version.empty()) {
    lay.version_y = top + lay.row_h * 0.5f;
    top += lay.row_h;
  }
  if (!p.content.size.empty()) {
    lay.size_y = top + lay.row_h * 0.5f;
    top += lay.row_h;
  }
  if (p.content.progress_bar) {
    if (!p.content.progress_text.empty()) {
      lay.progress_text_y = top + lay.row_h * 0.5f;
      top += lay.row_h;
    }
    lay.progress_y = top;
    top += 6.f + lay.row_h * 0.5f;
  }
  if (!p.content.hint.empty()) {
    lay.hint_y = top + lay.row_h * 0.5f;
    top += lay.row_h * (float)p.content.hint.size();
  }
  if (!p.content.error.empty()) {
    lay.error_y = top + lay.row_h * 0.5f;
    top += lay.row_h * (float)p.content.error.size();
  }
  if (!p.content.warning.empty()) {
    top += lay.row_h * 0.3f;
    lay.warning_y = top + lay.row_h * 0.5f;
    top += lay.row_h * (float)p.content.warning.size();
  }
  if (!p.content.notes.empty()) {
    lay.notes_title_y = top + lay.row_h * 0.5f;
    top += lay.row_h;
    lay.notes_y = top + lay.notes_line_h * 0.5f;
    top += lay.notes_line_h * (float)p.content.notes.size();
  }
  top += pad * 0.6f;
  lay.btn_y = top;
  top += lay.btn_h + pad * 0.75f;
  lay.h = top;
  lay.x = std::round((win_w - lay.w) * 0.5f);
  lay.y = std::round((win_h - lay.h) * 0.5f);
  if (lay.y < 10.f) lay.y = 10.f;
  if (lay.y + lay.h > win_h - 8.f) lay.y = std::max(10.f, win_h - 8.f - lay.h);
  // 上面各 y 以卡片顶为原点累计，绘制按窗口绝对坐标读取，这里统一补上卡片偏移。
  const float dy = lay.y;
  lay.title_y += dy;
  lay.version_y += dy;
  lay.size_y += dy;
  lay.progress_text_y += dy;
  lay.progress_y += dy;
  lay.hint_y += dy;
  lay.error_y += dy;
  lay.warning_y += dy;
  lay.notes_title_y += dy;
  lay.notes_y += dy;
  lay.btn_y += dy;
  float bx = lay.x + lay.w - pad;
  for (int i = b.count - 1; i >= 0; --i) {
    bx -= lay.btn_w[i];
    lay.btn_x[i] = bx;
    bx -= btn_gap;
  }
  return p;
}

}  // namespace

UpdateButtons update_dialog_buttons(const UpdateUi& u) {
  UpdateButtons b;
  auto set = [&](UpdateButtonKind a, UpdateButtonKind c, UpdateButtonKind p) {
    b.kinds[0] = a;
    b.kinds[1] = c;
    b.kinds[2] = p;
    b.count = 3;
  };
  auto set2 = [&](UpdateButtonKind a, UpdateButtonKind p) {
    b.kinds[0] = a;
    b.kinds[1] = p;
    b.count = 2;
  };
  auto set1 = [&](UpdateButtonKind a) {
    b.kinds[0] = a;
    b.count = 1;
  };
  switch (u.stage) {
    case UpdateStage::Hidden:
    case UpdateStage::Installing: b.count = 0; break;
    case UpdateStage::Checking:
    case UpdateStage::Downloading:
    case UpdateStage::UpToDate:
    case UpdateStage::Skipped: set1(UpdateButtonKind::Close); break;
    case UpdateStage::Failed:
      if (u.can_retry) set2(UpdateButtonKind::Later, UpdateButtonKind::Retry);
      else set1(UpdateButtonKind::Close);
      break;
    case UpdateStage::ManualOnly:
      if (u.download_available) set(UpdateButtonKind::Skip, UpdateButtonKind::Later,
                                    UpdateButtonKind::Download);
      else set2(UpdateButtonKind::Close, UpdateButtonKind::OpenPage);
      break;
    case UpdateStage::Ready:
      if (u.asset == platform::InstallShape::WindowsSetup)
        set(UpdateButtonKind::Skip, UpdateButtonKind::Later, UpdateButtonKind::Install);
      else if (u.asset == platform::InstallShape::LinuxAppImage)
        set(UpdateButtonKind::Skip, UpdateButtonKind::Later, UpdateButtonKind::InstallRestart);
      else if (u.asset == platform::InstallShape::WindowsPortable)
        set(UpdateButtonKind::Skip, UpdateButtonKind::Later, UpdateButtonKind::OpenFolder);
      else if (u.asset == platform::InstallShape::LinuxDeb)
        set(UpdateButtonKind::Skip, UpdateButtonKind::Later, UpdateButtonKind::OpenFolderDeb);
      else
        set2(UpdateButtonKind::Close, UpdateButtonKind::OpenPage);
      break;
  }
  return b;
}

const char* update_button_label(UpdateButtonKind k, const UpdateUi& u) {
  switch (k) {
    case UpdateButtonKind::Install:
      return tr(u.cast_active && u.confirm_install ? i18n::StrId::UpdBtnInstallAnyway
                                                   : i18n::StrId::UpdBtnInstall);
    case UpdateButtonKind::InstallRestart:
      return tr(u.cast_active && u.confirm_install ? i18n::StrId::UpdBtnInstallAnyway
                                                   : i18n::StrId::UpdBtnInstallRestart);
    case UpdateButtonKind::Download: return tr(i18n::StrId::UpdBtnDownload);
    case UpdateButtonKind::Later: return tr(i18n::StrId::UpdBtnLater);
    case UpdateButtonKind::Skip: return tr(i18n::StrId::UpdBtnSkip);
    case UpdateButtonKind::Retry: return tr(i18n::StrId::UpdBtnRetry);
    case UpdateButtonKind::OpenFolder: return tr(i18n::StrId::UpdBtnOpenFolder);
    case UpdateButtonKind::OpenFolderDeb: return tr(i18n::StrId::UpdBtnOpenFolderDeb);
    case UpdateButtonKind::OpenPage: return tr(i18n::StrId::UpdBtnOpenPage);
    case UpdateButtonKind::Close: return tr(i18n::StrId::UpdBtnClose);
    case UpdateButtonKind::None: break;
  }
  return "";
}

UpdateDialogLayout update_dialog_layout(NVGcontext* vg, const Theme& t, float win_w,
                                        float win_h, const UpdateUi& u) {
  LayoutParts p = build_layout(vg, t, win_w, win_h, u, 8);
  // 小窗口：说明区按可用高度收行，避免卡片顶出窗口
  if (u.notes[0] && (u.stage == UpdateStage::Ready || u.stage == UpdateStage::ManualOnly)) {
    const float max_h = std::max(180.f, win_h - 18.f);
    if (p.lay.h > max_h) {
      const int cut = (int)std::ceil((p.lay.h - max_h) / std::max(1.f, p.lay.notes_line_h));
      p = build_layout(vg, t, win_w, win_h, u, std::max(0, 8 - cut));
    }
  }
  return p.lay;
}

int update_dialog_button_at(const UpdateDialogLayout& lay, float mx, float my) {
  if (my < lay.btn_y || my >= lay.btn_y + lay.btn_h) return -1;
  for (int i = 0; i < lay.btn_count; ++i) {
    if (mx >= lay.btn_x[i] && mx < lay.btn_x[i] + lay.btn_w[i]) return i;
  }
  return -1;
}

void draw_update_dialog(NVGcontext* vg, const Theme& t, float win_w, float win_h,
                        const UpdateUi& u, const UpdateDialogLayout& lay,
                        const BtnState* states, const DmgRect* clip) {
  if (clip && !dmg_hit(clip, 0.f, 0.f, win_w, win_h)) return;
  const auto& L = t.layout;
  const LayoutParts p = build_layout(vg, t, win_w, win_h, u, lay.notes_cap);
  const DialogContent& c = p.content;

  // 全屏遮罩（压暗底层；点击不关闭 —— 主线程通过按钮意图处理）
  nvgGlobalAlpha(vg, 0.55f);
  nvgBeginPath(vg);
  nvgRect(vg, 0.f, 0.f, win_w, win_h);
  nvgFillColor(vg, nvgRGBA(0x00, 0x00, 0x00, 160));
  nvgFill(vg);
  nvgGlobalAlpha(vg, 1.f);

  // 卡片（不透明，避免区域重绘在遮罩上叠色）
  rounded_rect(vg, lay.x, lay.y, lay.w, lay.h, L.ctxRadius, t.topBarBg);
  nvgBeginPath(vg);
  nvgRoundedRect(vg, lay.x + 0.5f, lay.y + 0.5f, lay.w - 1.f, lay.h - 1.f, L.ctxRadius);
  nvgStrokeWidth(vg, 1.f);
  nvgStrokeColor(vg, t.barBorderTopBar);
  nvgStroke(vg);

  const float content_x = lay.x + lay.pad;
  text(vg, t, content_x, lay.title_y, p.font_title, t.icon, c.title.c_str(),
       NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
  if (!c.version.empty())
    text(vg, t, content_x, lay.version_y, p.font, t.subText, c.version.c_str(),
         NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
  if (!c.size.empty())
    text(vg, t, content_x, lay.size_y, p.font, t.mutedText, c.size.c_str(),
         NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
  if (!c.progress_text.empty())
    text(vg, t, content_x, lay.progress_text_y, p.small, t.subText, c.progress_text.c_str(),
         NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
  if (c.progress_bar) {
    const float bar_h = 5.f;
    rounded_rect(vg, content_x, lay.progress_y, lay.content_w, bar_h,
                 std::min(t.shape.progressRadius, bar_h * 0.5f), t.track);
    if (u.progress > 0.f) {
      const float fw = lay.content_w * std::clamp(u.progress, 0.f, 1.f);
      rounded_rect(vg, content_x, lay.progress_y, fw, bar_h,
                   std::min(t.shape.progressRadius, bar_h * 0.5f), t.played);
    }
  }
  float fy = lay.hint_y;
  for (const auto& line : c.hint) {
    text_truncated(vg, t, content_x, fy, p.small, t.subText, line.c_str(), lay.content_w);
    fy += lay.row_h;
  }
  fy = lay.error_y;
  for (const auto& line : c.error) {
    text_truncated(vg, t, content_x, fy, p.small, t.mutedText, line.c_str(), lay.content_w);
    fy += lay.row_h;
  }
  fy = lay.warning_y;
  for (const auto& line : c.warning) {
    text_truncated(vg, t, content_x, fy, p.font, t.icon, line.c_str(), lay.content_w);
    fy += lay.row_h;
  }
  if (!c.notes_title.empty()) {
    text(vg, t, content_x, lay.notes_title_y, p.small, t.mutedText, c.notes_title.c_str(),
         NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    fy = lay.notes_y;
    for (const auto& line : c.notes) {
      text_truncated(vg, t, content_x, fy, p.small, t.subText, line.c_str(), lay.content_w);
      fy += lay.notes_line_h;
    }
  }

  const UpdateButtons b = update_dialog_buttons(u);
  for (int i = 0; i < lay.btn_count && i < b.count; ++i) {
    const BtnState st = states ? states[i] : BtnState{};
    const bool primary = i == lay.btn_count - 1;
    NVGcolor bg = primary ? t.played : t.badgeBg;
    bg.a = std::min(1.f, bg.a * (1.f + 1.2f * st.hover));
    if (st.press > 0.01f) bg.a *= 1.f - 0.3f * st.press;
    const float sc = 1.f - 0.04f * st.press;
    const float bw = lay.btn_w[i] * sc;
    const float bh = lay.btn_h * sc;
    const float bx = lay.btn_x[i] + (lay.btn_w[i] - bw) * 0.5f;
    const float by = lay.btn_y + (lay.btn_h - bh) * 0.5f;
    rounded_rect(vg, bx, by, bw, bh, std::min(L.ctxRadius * 0.75f, bh * 0.35f), bg);
    const NVGcolor tc = primary ? t.topBarBg : t.badgeText;
    text(vg, t, bx + bw * 0.5f, by + bh * 0.5f, p.font, tc,
         update_button_label(b.kinds[i], u), NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
  }
}

}  // namespace fr
