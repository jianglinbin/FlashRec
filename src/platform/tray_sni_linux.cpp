// 托盘 SNI 后端（d84）：org.kde.StatusNotifierItem + com.canonical.dbusmenu
// 最小实现（sd-bus / libsystemd，运行时零新增依赖）。目标宿主：
// GNOME（ubuntu-appindicators 扩展，配合 dash-to-panel）/ KDE / 新式面板。
// 线程约定：create/poll/remove 均由主循环驱动，菜单/激活回调因此在主线程
// 触发（与 Windows 路径一致，回调里可安全触碰窗口与事件总线）。
#include "platform/tray_linux_backend.h"

#include <systemd/sd-bus.h>

// stb_image 声明模式（STB_IMAGE_IMPLEMENTATION 定义在 nanovg.c，C 链接符号共享）
#include "stb_image.h"

#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include "app/log.h"

namespace fr {
namespace tray {

namespace {

constexpr int kIconSize = 24;
constexpr const char* kSniIface = "org.kde.StatusNotifierItem";
constexpr const char* kSniPath = "/StatusNotifierItem";
constexpr const char* kMenuIface = "com.canonical.dbusmenu";
constexpr const char* kMenuPath = "/StatusNotifierMenu";
constexpr const char* kWatcherName = "org.kde.StatusNotifierWatcher";
constexpr const char* kWatcherPath = "/StatusNotifierWatcher";

class SniBackend;

SniBackend* g_s = nullptr;  // 单例指针（回调走它；仅主线程读写）

class SniBackend final : public Backend {
 public:
  ~SniBackend() override { remove(); }

  bool create(GLFWwindow* win, const char* tooltip, const char* icon_png,
              const TrayCallbacks& cb) override;
  void poll() override;
  void remove() override;

  // 菜单数据：每次请求实时取（与 Windows 一致，标签可随播放态变化）。
  // dbusmenu item id = provider 数组索引 + 1（0 固定为根）。
  std::vector<TrayMenuItem> menu() const {
    return cb_.menu_provider ? cb_.menu_provider() : std::vector<TrayMenuItem>{};
  }
  int app_id_of(int dbusmenu_id) const {
    const auto items = menu();
    if (dbusmenu_id >= 1 && dbusmenu_id <= (int)items.size())
      return items[dbusmenu_id - 1].id;
    return 0;
  }
  void fire_toggle() const {
    if (cb_.on_toggle_window) cb_.on_toggle_window();
  }
  void fire_action(int app_id) const {
    if (cb_.on_menu_action && app_id != 0) cb_.on_menu_action(app_id);
  }
  // 菜单内容可能已变（播放/暂停标签翻转）：广播 revision 让宿主重新拉布局。
  void bump_revision() {
    ++revision_;
    sd_bus_emit_signal(bus_, kMenuPath, kMenuIface, "LayoutUpdated", "ui",
                       revision_, (int32_t)0);
  }

  sd_bus* bus_ = nullptr;
  sd_bus_slot* sni_slot_ = nullptr;
  sd_bus_slot* menu_slot_ = nullptr;
  std::string well_known_;
  std::string title_;
  std::vector<uint8_t> argb_;  // 24x24，网络字节序 ARGB（非预乘）
  uint32_t revision_ = 1;
  TrayCallbacks cb_;
};

// ---------- 通用属性 getter（字符串类：Category/Id/Title/Status/各 IconName）----------

int prop_str(sd_bus* bus, const char* path, const char* interface,
             const char* property, sd_bus_message* reply,
             void* userdata, sd_bus_error* err) {
  (void)bus; (void)path; (void)interface; (void)err; (void)userdata;
  const char* v = "";
  if (!std::strcmp(property, "Category")) v = "ApplicationStatus";
  else if (!std::strcmp(property, "Id")) v = "FlashRec";
  else if (!std::strcmp(property, "Status")) v = "Active";
  else if (!std::strcmp(property, "Title")) v = g_s ? g_s->title_.c_str() : "";
  return sd_bus_message_append(reply, "s", v) < 0 ? -ENOMEM : 1;
}

int prop_window_id(sd_bus* bus, const char* path, const char* interface,
                   const char* property, sd_bus_message* reply, void* userdata,
                   sd_bus_error* err) {
  (void)bus; (void)path; (void)interface; (void)property; (void)err; (void)userdata;
  return sd_bus_message_append(reply, "i", 0) < 0 ? -ENOMEM : 1;
}

// a(iiay)：宽、高、网络字节序 ARGB 字节流。仅 IconPixmap 携带像素，
// Overlay/Attention 恒为空数组。
int prop_pixmap(sd_bus* bus, const char* path, const char* interface,
                const char* property, sd_bus_message* reply, void* userdata,
                sd_bus_error* err) {
  (void)bus; (void)path; (void)interface; (void)err; (void)userdata;
  const bool full = !std::strcmp(property, "IconPixmap");
  static const std::vector<uint8_t> kEmpty;
  const std::vector<uint8_t>& a =
      (g_s && full && !g_s->argb_.empty()) ? g_s->argb_ : kEmpty;
  int r = sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "(iiay)");
  if (r >= 0 && !a.empty()) {
    r = sd_bus_message_open_container(reply, SD_BUS_TYPE_STRUCT, "iiay");
    if (r >= 0) {
      r = sd_bus_message_append(reply, "ii", kIconSize, kIconSize);
      if (r >= 0)
        r = sd_bus_message_append_array(reply, 'y', a.data(), a.size());
      r |= sd_bus_message_close_container(reply);
    }
  }
  if (r >= 0) r = sd_bus_message_close_container(reply);
  return r < 0 ? -ENOMEM : 1;
}

// (sa(iiay)ss)：icon 名（空）+ pixmap（空）+ 标题 + 正文。
int prop_tooltip(sd_bus* bus, const char* path, const char* interface,
                 const char* property, sd_bus_message* reply, void* userdata,
                 sd_bus_error* err) {
  (void)bus; (void)path; (void)interface; (void)property; (void)err; (void)userdata;
  int r = sd_bus_message_open_container(reply, SD_BUS_TYPE_STRUCT,
                                        "sa(iiay)ss");
  if (r >= 0) {
    r = sd_bus_message_append(reply, "s", "");
    if (r >= 0) {
      r = sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "(iiay)");
      if (r >= 0) r = sd_bus_message_close_container(reply);
    }
    if (r >= 0)
      r = sd_bus_message_append(reply, "ss",
                                g_s ? g_s->title_.c_str() : "", "");
    r |= sd_bus_message_close_container(reply);
  }
  return r < 0 ? -ENOMEM : 1;
}

int prop_menu_path(sd_bus* bus, const char* path, const char* interface,
                   const char* property, sd_bus_message* reply, void* userdata,
                   sd_bus_error* err) {
  (void)bus; (void)path; (void)interface; (void)property; (void)err; (void)userdata;
  return sd_bus_message_append(reply, "o", kMenuPath) < 0 ? -ENOMEM : 1;
}

// ---------- SNI 方法 ----------

int sni_activate(sd_bus_message* m, void* userdata, sd_bus_error* err) {
  (void)userdata; (void)err;
  if (g_s) g_s->fire_toggle();  // 左键激活 = 显示/隐藏主窗口（XEmbed 同语义）
  return sd_bus_reply_method_return(m, "");
}

int sni_nop(sd_bus_message* m, void* userdata, sd_bus_error* err) {
  (void)userdata; (void)err;
  return sd_bus_reply_method_return(m, "");  // ContextMenu/Scroll：空实现
}

// ---------- DBusMenu 属性序列化 ----------

void append_item_props(sd_bus_message* m, const TrayMenuItem& it, int* r) {
  *r |= sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY, "{sv}");
  if (it.sep) {
    *r |= sd_bus_message_append(m, "{sv}", "type", "s", "separator");
  } else {
    *r |= sd_bus_message_append(m, "{sv}", "type", "s", "standard");
    *r |= sd_bus_message_append(m, "{sv}", "label", "s", it.label.c_str());
    *r |= sd_bus_message_append(m, "{sv}", "enabled", "b",
                                (int)(it.enabled ? 1 : 0));
  }
  *r |= sd_bus_message_append(m, "{sv}", "visible", "b", (int)1);
  *r |= sd_bus_message_close_container(m);
}

// ---------- DBusMenu 方法 ----------

// vtable handler 的 m 是请求消息（只读）；回复须 new_method_return 后构造并
// send（d85：直接往 m append 返回 -EPERM，曾致 GetLayout 全线失败、菜单缺失）。
// vtable handler 的 m 是请求消息（只读）；回复须 new_method_return 后构造并
// send（d85：直接往 m append 返回 -EPERM，曾致 GetLayout 全线失败、菜单缺失）。
int menu_get_layout(sd_bus_message* m, void* userdata, sd_bus_error* err) {
  (void)userdata; (void)err;
  if (!g_s) return sd_bus_error_set(err, "fr.TrayError", "tray not ready");
  int parent = 0, depth = 0;
  int r = sd_bus_message_read(m, "ii", &parent, &depth);
  if (r < 0) return r;
  r = sd_bus_message_skip(m, "as");
  if (r < 0) return r;

  sd_bus_message* reply = nullptr;
  r = sd_bus_message_new_method_return(m, &reply);
  if (r < 0) return r;
  const auto items = g_s->menu();
  // 回复 = (u revision, 根节点)；根节点 = (i id, a{sv} props, av children)，
  // 子项 VARIANT 挂在根 av 内；本项目菜单平铺一层，子项 av 恒空。
  r = sd_bus_message_append(reply, "u", g_s->revision_);
  if (r >= 0)
    r = sd_bus_message_open_container(reply, SD_BUS_TYPE_STRUCT, "ia{sv}av");
  if (r >= 0) r = sd_bus_message_append(reply, "i", parent);  // 根 id
  if (r >= 0)
    r = sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "{sv}");
  if (r >= 0)
    r = sd_bus_message_append(reply, "{sv}", "children-display", "s",
                              "submenu");
  if (r >= 0) r = sd_bus_message_close_container(reply);
  if (r >= 0)
    r = sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "v");
  for (size_t i = 0; r >= 0 && i < items.size(); ++i) {
    // VARIANT 内容必须是完整类型（带外括号）；STRUCT 的 content 才不带外括号
    r = sd_bus_message_open_container(reply, SD_BUS_TYPE_VARIANT,
                                      "(ia{sv}av)");
    if (r >= 0)
      r = sd_bus_message_open_container(reply, SD_BUS_TYPE_STRUCT,
                                        "ia{sv}av");
    if (r >= 0) r = sd_bus_message_append(reply, "i", (int)(i + 1));
    if (r >= 0) append_item_props(reply, items[i], &r);
    if (r >= 0)
      r = sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "v");
    if (r >= 0) r = sd_bus_message_close_container(reply);
    if (r >= 0) r = sd_bus_message_close_container(reply);  // struct
    if (r >= 0) r = sd_bus_message_close_container(reply);  // variant
  }
  if (r >= 0) r = sd_bus_message_close_container(reply);  // av
  if (r >= 0) r = sd_bus_message_close_container(reply);  // 根 struct
  if (r < 0) {
    sd_bus_message_unref(reply);
    return r;  // 负值 → sd-bus 自动回错误回复
  }
  r = sd_bus_message_send(reply);
  sd_bus_message_unref(reply);
  return r < 0 ? r : 1;
}

int menu_get_property(sd_bus_message* m, void* userdata, sd_bus_error* err) {
  (void)userdata;
  if (!g_s) return sd_bus_error_set(err, "fr.TrayError", "tray not ready");
  int id = 0;
  const char* prop = nullptr;
  int r = sd_bus_message_read(m, "is", &id, &prop);
  if (r < 0) return r;
  const auto items = g_s->menu();
  const TrayMenuItem* it =
      (id >= 1 && id <= (int)items.size()) ? &items[id - 1] : nullptr;
  sd_bus_message* reply = nullptr;
  r = sd_bus_message_new_method_return(m, &reply);
  if (r < 0) return r;
  const char* s_val = "";
  int b_val = 0;
  const char* v_sig = "s";
  if (it && prop && !std::strcmp(prop, "label")) {
    s_val = it->sep ? "" : it->label.c_str();
  } else if (it && prop && !std::strcmp(prop, "enabled")) {
    v_sig = "b";
    b_val = (it->enabled && !it->sep) ? 1 : 0;
  } else if (it && prop && !std::strcmp(prop, "type")) {
    s_val = it->sep ? "separator" : "standard";
  } else if (it && prop && !std::strcmp(prop, "visible")) {
    v_sig = "b";
    b_val = 1;
  }
  r = sd_bus_message_open_container(reply, SD_BUS_TYPE_VARIANT, v_sig);
  if (r >= 0)
    r = v_sig[0] == 'b' ? sd_bus_message_append(reply, "b", b_val)
                        : sd_bus_message_append(reply, "s", s_val);
  if (r >= 0) r = sd_bus_message_close_container(reply);
  if (r < 0) {
    sd_bus_message_unref(reply);
    return r;
  }
  r = sd_bus_message_send(reply);
  sd_bus_message_unref(reply);
  return r < 0 ? r : 1;
}

int menu_get_group(sd_bus_message* m, void* userdata, sd_bus_error* err) {
  (void)userdata;
  if (!g_s) return sd_bus_error_set(err, "fr.TrayError", "tray not ready");
  const auto items = g_s->menu();
  // 读 ids（空数组 = 全部），跳过 propertyNames 过滤（返回全量，宿主可容忍）
  std::vector<int> ids;
  int r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "i");
  if (r < 0) return r;
  for (;;) {
    int v = 0;
    const int rr = sd_bus_message_read(m, "i", &v);
    if (rr <= 0) break;  // 0 = 数组读完，<0 = 错误
    ids.push_back(v);
  }
  r = sd_bus_message_exit_container(m);
  if (r < 0) return r;
  r = sd_bus_message_skip(m, "as");
  if (r < 0) return r;

  sd_bus_message* reply = nullptr;
  r = sd_bus_message_new_method_return(m, &reply);
  if (r < 0) return r;
  r = sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "(ia{sv})");
  const bool all = ids.empty();
  for (size_t i = 0; r >= 0 && i < items.size(); ++i) {
    const int id = (int)(i + 1);
    if (!all) {
      bool want = false;
      for (int want_id : ids)
        if (want_id == id) { want = true; break; }
      if (!want) continue;
    }
    r = sd_bus_message_open_container(reply, SD_BUS_TYPE_STRUCT, "ia{sv}");
    if (r >= 0) r = sd_bus_message_append(reply, "i", id);
    if (r >= 0) append_item_props(reply, items[i], &r);
    if (r >= 0) r = sd_bus_message_close_container(reply);
  }
  if (r >= 0) r = sd_bus_message_close_container(reply);
  if (r < 0) {
    sd_bus_message_unref(reply);
    return r;
  }
  r = sd_bus_message_send(reply);
  sd_bus_message_unref(reply);
  return r < 0 ? r : 1;
}

int menu_about_to_show(sd_bus_message* m, void* userdata, sd_bus_error* err) {
  (void)userdata; (void)err;
  // false = 布局无预变更（打开时宿主重新 GetLayout 拿实时条目）
  return sd_bus_reply_method_return(m, "b", 0);
}

// 菜单接口自身属性（Version/TextDirection/Status；独立小函数避免宏参数逗号）
int prop_menu_version(sd_bus*, const char*, const char*, const char*,
                      sd_bus_message* reply, void*, sd_bus_error*) {
  // DBusMenu 协议版本常量（当前 3），与我们的内部 revision 无关
  return sd_bus_message_append(reply, "u", 3) < 0 ? -ENOMEM : 1;
}

int prop_menu_text_dir(sd_bus*, const char*, const char*, const char*,
                       sd_bus_message* reply, void*, sd_bus_error*) {
  return sd_bus_message_append(reply, "s", "ltr") < 0 ? -ENOMEM : 1;
}

int prop_menu_status(sd_bus*, const char*, const char*, const char*,
                     sd_bus_message* reply, void*, sd_bus_error*) {
  return sd_bus_message_append(reply, "s", "normal") < 0 ? -ENOMEM : 1;
}

int menu_handle_click(SniBackend* s, int dbusmenu_id) {
  const int app_id = s->app_id_of(dbusmenu_id);
  if (app_id != 0) {
    s->fire_action(app_id);
    s->bump_revision();  // 标签可能翻转（播放/暂停），通知宿主刷新
  }
  return app_id;
}

int menu_event(sd_bus_message* m, void* userdata, sd_bus_error* err) {
  (void)userdata; (void)err;
  if (!g_s) return sd_bus_error_set(err, "fr.TrayError", "tray not ready");
  int id = 0;
  const char* event = nullptr;
  int r = sd_bus_message_read(m, "is", &id, &event);
  if (r < 0) return r;
  r = sd_bus_message_skip(m, "vu");
  if (r < 0) return r;
  if (event && !std::strcmp(event, "clicked")) menu_handle_click(g_s, id);
  return sd_bus_reply_method_return(m, "");
}

int menu_event_group(sd_bus_message* m, void* userdata, sd_bus_error* err) {
  (void)userdata; (void)err;
  if (!g_s) return sd_bus_error_set(err, "fr.TrayError", "tray not ready");
  // 签名 a(ia{sv})u：ids 数组（元素 = struct{i, props}）+ timestamp
  std::vector<int> clicked;
  int r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "(ia{sv})");
  if (r < 0) return r;
  for (;;) {
    const int rr = sd_bus_message_enter_container(m, SD_BUS_TYPE_STRUCT,
                                                  "ia{sv}");
    if (rr <= 0) {  // 0 = 数组遍历结束，<0 = 错误
      if (rr < 0) return rr;
      break;
    }
    int id = 0;
    r = sd_bus_message_read(m, "i", &id);
    if (r >= 0) r = sd_bus_message_skip(m, "a{sv}");
    if (r >= 0) r = sd_bus_message_exit_container(m);
    if (r < 0) return r;
    clicked.push_back(id);
  }
  r = sd_bus_message_exit_container(m);
  if (r < 0) return r;
  r = sd_bus_message_skip(m, "u");
  if (r < 0) return r;
  for (int id : clicked) menu_handle_click(g_s, id);
  return sd_bus_reply_method_return(m, "ai", (size_t)0);
}

int menu_start_batch(sd_bus_message* m, void* userdata, sd_bus_error* err) {
  (void)userdata; (void)err;
  return sd_bus_reply_method_return(m, "u", g_s ? g_s->revision_ : 1);
}

const sd_bus_vtable kSniVt[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Category", "s", prop_str, 0, 0),
    SD_BUS_PROPERTY("Id", "s", prop_str, 0, 0),
    SD_BUS_PROPERTY("Title", "s", prop_str, 0, 0),
    SD_BUS_PROPERTY("Status", "s", prop_str, 0, 0),
    SD_BUS_PROPERTY("WindowId", "i", prop_window_id, 0, 0),
    SD_BUS_PROPERTY("IconName", "s", prop_str, 0, 0),
    SD_BUS_PROPERTY("IconPixmap", "a(iiay)", prop_pixmap, 0, 0),
    SD_BUS_PROPERTY("OverlayIconName", "s", prop_str, 0, 0),
    SD_BUS_PROPERTY("OverlayIconPixmap", "a(iiay)", prop_pixmap, 0, 0),
    SD_BUS_PROPERTY("AttentionIconName", "s", prop_str, 0, 0),
    SD_BUS_PROPERTY("AttentionIconPixmap", "a(iiay)", prop_pixmap, 0, 0),
    SD_BUS_PROPERTY("AttentionMovieName", "s", prop_str, 0, 0),
    SD_BUS_PROPERTY("ToolTip", "(sa(iiay)ss)", prop_tooltip, 0, 0),
    SD_BUS_PROPERTY("Menu", "o", prop_menu_path, 0, 0),
    SD_BUS_METHOD("ContextMenu", "ii", "", sni_nop, 0),
    SD_BUS_METHOD("Activate", "ii", "", sni_activate, 0),
    SD_BUS_METHOD("SecondaryActivate", "ii", "", sni_activate, 0),
    SD_BUS_METHOD("Scroll", "iis", "", sni_nop, 0),
    SD_BUS_SIGNAL("NewTitle", "", 0),
    SD_BUS_SIGNAL("NewIcon", "", 0),
    SD_BUS_SIGNAL("NewToolTip", "", 0),
    SD_BUS_SIGNAL("NewStatus", "s", 0),
    SD_BUS_VTABLE_END,
};

const sd_bus_vtable kMenuVt[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Version", "u", prop_menu_version, 0, 0),
    SD_BUS_PROPERTY("TextDirection", "s", prop_menu_text_dir, 0, 0),
    SD_BUS_PROPERTY("Status", "s", prop_menu_status, 0, 0),
    SD_BUS_METHOD("GetLayout", "iias", "u(ia{sv}av)", menu_get_layout, 0),
    SD_BUS_METHOD("GetProperty", "is", "v", menu_get_property, 0),
    SD_BUS_METHOD("GetGroupProperties", "aias", "a(ia{sv})", menu_get_group, 0),
    SD_BUS_METHOD("AboutToShow", "i", "b", menu_about_to_show, 0),
    SD_BUS_METHOD("Event", "isvu", "", menu_event, 0),
    SD_BUS_METHOD("EventGroup", "a(ia{sv})u", "ai", menu_event_group, 0),
    SD_BUS_METHOD("StartLayoutBatch", "", "u", menu_start_batch, 0),
    SD_BUS_SIGNAL("LayoutUpdated", "ui", 0),
    SD_BUS_SIGNAL("ItemActivationRequested", "iu", 0),
    SD_BUS_SIGNAL("ItemsPropertiesUpdated", "a(ia{sv})a(ias)", 0),
    SD_BUS_VTABLE_END,
};

}  // namespace

// ---------- Backend 接口实现 ----------

bool SniBackend::create(GLFWwindow* win, const char* tooltip,
                        const char* icon_png, const TrayCallbacks& cb) {
  (void)win;
  g_s = this;
  cb_ = cb;
  title_ = tooltip ? tooltip : "FlashRec";

  if (sd_bus_open_user(&bus_) < 0 || !bus_) {
    FR_LOG_WARN("[TRAY] SNI：会话总线不可用（DBUS_SESSION_BUS_ADDRESS/"
                "XDG_RUNTIME_DIR 缺失？）");
    bus_ = nullptr;
    g_s = nullptr;
    return false;
  }

  // PNG → 24×24 网络字节序 ARGB（SNI IconPixmap 直喂像素，不依赖图标主题）
  int iw = 0, ih = 0, comp = 0;
  unsigned char* rgba = stbi_load(icon_png, &iw, &ih, &comp, 4);
  if (!rgba) {
    FR_LOG_WARN("[TRAY] 托盘图标解码失败 {}", icon_png);
    remove();
    return false;
  }
  argb_.assign((size_t)kIconSize * kIconSize * 4, 0);
  for (int y = 0; y < kIconSize; ++y) {
    for (int x = 0; x < kIconSize; ++x) {
      const int si = (y * ih / kIconSize) * iw + (x * iw / kIconSize);
      uint8_t* d = &argb_[((size_t)y * kIconSize + x) * 4];
      d[0] = rgba[si * 4 + 3];  // A
      d[1] = rgba[si * 4 + 0];  // R
      d[2] = rgba[si * 4 + 1];  // G
      d[3] = rgba[si * 4 + 2];  // B
    }
  }
  stbi_image_free(rgba);

  if (sd_bus_add_object_vtable(bus_, &sni_slot_, kSniPath, kSniIface, kSniVt,
                               nullptr) < 0 ||
      sd_bus_add_object_vtable(bus_, &menu_slot_, kMenuPath, kMenuIface,
                               kMenuVt, nullptr) < 0) {
    FR_LOG_WARN("[TRAY] SNI：对象导出失败");
    remove();
    return false;
  }

  well_known_ = "org.kde.StatusNotifierItem-" +
                std::to_string((unsigned long)::getpid());
  if (sd_bus_request_name(bus_, well_known_.c_str(), 0) < 0) {
    FR_LOG_WARN("[TRAY] SNI：总线名申请失败 {}", well_known_);
    remove();
    return false;
  }

  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_message* rep = nullptr;
  const int r = sd_bus_call_method(bus_, kWatcherName, kWatcherPath,
                                   kWatcherName, "RegisterStatusNotifierItem",
                                   &err, &rep, "s", well_known_.c_str());
  sd_bus_message_unref(rep);
  if (r < 0) {
    FR_LOG_WARN("[TRAY] SNI：Watcher 注册失败（{}）",
                err.message ? err.message : "未知错误");
    sd_bus_error_free(&err);
    remove();
    return false;
  }
  sd_bus_flush(bus_);
  FR_LOG_INFO("[TRAY] 托盘已挂载（SNI → {}，{}）", kWatcherName, well_known_);
  return true;
}

void SniBackend::poll() {
  if (!bus_) return;
  // 非阻塞泵：处理到达的请求（宿主读属性/拉菜单/点击事件）直到无事可做
  for (;;) {
    const int r = sd_bus_process(bus_, nullptr);
    if (r <= 0) break;
  }
  sd_bus_flush(bus_);  // 尽力发出排队信号；小消息非阻塞级延迟
}

void SniBackend::remove() {
  if (bus_) {
    sd_bus_call_method(bus_, kWatcherName, kWatcherPath, kWatcherName,
                       "UnregisterStatusNotifierItem", nullptr, nullptr, "s",
                       well_known_.c_str());  // best effort
    if (menu_slot_) {
      sd_bus_slot_unref(menu_slot_);
      menu_slot_ = nullptr;
    }
    if (sni_slot_) {
      sd_bus_slot_unref(sni_slot_);
      sni_slot_ = nullptr;
    }
    if (!well_known_.empty()) sd_bus_release_name(bus_, well_known_.c_str());
    sd_bus_flush_close_unref(bus_);
    bus_ = nullptr;
  }
  argb_.clear();
  well_known_.clear();
  if (g_s == this) g_s = nullptr;
}

// ---------- 探测 + 工厂 ----------

bool sni_watcher_available() {
  sd_bus* b = nullptr;
  if (sd_bus_open_user(&b) < 0 || !b) return false;
  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_message* rep = nullptr;
  const int r = sd_bus_call_method(b, "org.freedesktop.DBus",
                                   "/org/freedesktop/DBus",
                                   "org.freedesktop.DBus", "GetNameOwner",
                                   &err, &rep, "s", kWatcherName);
  sd_bus_message_unref(rep);
  sd_bus_error_free(&err);
  sd_bus_flush_close_unref(b);
  return r >= 0;  // 有属主 = 有 SNI 宿主（ServiceUnknown 等均视为无）
}

Backend* sni_backend() {
  static SniBackend inst;
  return &inst;
}

}  // namespace tray
}  // namespace fr
