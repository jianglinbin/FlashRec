#pragma once
// 运行配置：settings.json（扁平分组 JSON，DMR_SOAP_SPEC §8）。
// 缺失 / 坏值一律回默认，绝不让配置文件把程序搞挂。
#include <string>
#include <vector>

namespace fr {

// R4（d146）：按屏记忆条目（settings.json: ui.window.monitors[]，≤4 条，
// 超出淘汰 last_used 最旧）。两块分辨率不同的屏各记各的摆位，来回切换互不覆盖。
struct MonitorMemory {
  std::string fp;              // 屏指纹（pos+videomode+workarea；名字不可靠，指纹当身份）
  int x = 0, y = 0, w = 0, h = 0;
  bool maximized = false;
  bool fullscreen = false;
  long long last_used = 0;     // 单调秒
};

struct Config {
  // log
  std::string log_level = "info";
  bool log_trace_http_body = false;
  bool log_http_entry = false;  // d22：全量 HTTP 入口日志（pupnp fr_http_trace），默认关
  bool log_soap_inbox = false;  // d144：请求原文收件箱；d145 起默认关（取证完即关）
  // dlna
  int dlna_port = 49152;                       // TCP 监听端口（0=系统自动分配）
  bool dlna_auto_play_on_set_uri = true;       // R1：默认自动开播
  int dlna_progress_retention_sec = 60;        // R5：退出投屏进度保持期
  std::string friendly_name = "FlashRec 投屏接收";
  std::string skin = "fluent";                 // fluent/frost/amber/teal/neon/mono
  bool clipboard_play = true;                  // d74：剪贴板链接探测 → 提示条（clipboard.play）
  bool ui_show_fps = false;                    // d44：UI 渲染帧率常驻（诊断；老配置键保留，决策点 7）
  bool ui_show_video_fps = false;              // d44：视频播放帧率常驻（老配置键保留，决策点 7）
  bool ui_show_info = false;                   // d45→d146：完整媒体信息徽章总开关（右键菜单唯一勾选项）
  bool ui_cast_auto_fullscreen = false;        // R2：投屏自动全屏（默认关 = 静默投屏，决策点 4）
  std::string ui_window_prefer = "last";       // R4：目标屏优先序 last/cursor/primary
  std::vector<MonitorMemory> ui_window_monitors;  // R4：按屏记忆（解析自 ui.window.monitors[]）
  std::vector<std::string> protocol_info;      // 空 = 用默认 Sink 串（不硬编码在 dmr/）
  // live
  int live_reconnect_max = 3;                  // R6：断流重连次数上限
  // player（d163：音量/静音持久化 —— 上次调好的音量跨重启保留）
  int player_volume = 100;
  bool player_muted = false;
  double player_speed = 1.0;  // d169：倍速持久化（player.speed，夹到 0.25..4.0）

  static Config load(const std::string& path);
  // GetProtocolInfo 的 Sink 串（每行带 DLNA.ORG_OP=01;DLNA.ORG_CI=0）
  std::string sink_protocol_info() const;
  static const std::vector<std::string>& default_protocol_info();

  // —— R1（d146）：文本级修补 settings.json，不引 JSON 库 ——
  struct PatchKV {
    std::string group;    // 组路径，"log" / "ui" / "ui.window"（'.' 分段下钻，缺失逐层追加）
    std::string key;      // 组内键（缺失则追加到组尾）
    std::string raw_value;  // JSON 字面量：true / 123 / "text"（调用方负责引号与转义）
  };
  // 键值不变则不写盘。原子写（.tmp → 替换）；失败记 warn 返回 false，绝不影响运行。
  static bool patch(const std::string& path, const std::vector<PatchKV>& kvs);
  // R4：monitors[] 数组整体写回（调用方先按 fp 去重合并、限 4 条）
  static bool patch_monitors(const std::string& path,
                             const std::vector<MonitorMemory>& mons);
};

}  // namespace fr
