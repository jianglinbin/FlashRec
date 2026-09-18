#pragma once
// 缩略图预览：第二 mpv_handle + 后台 worker（AGENTS.md 规则 1/4：mpv API 只在 player 层）。
//
// 用途：悬停/拖拽进度条时在浮层里显示目标位置的画面（IINA 式预览）。
// 管线：主线程 request(pos, uri) → worker（latest-wins，只处理最新请求）→
//   对同一 URI：loadfile（paused）等元数据 → clean_outdir → seek absolute exact
//   → vo=image 在帧显示时把目标帧写成 jpg → 读回字节 → 结果槽发布（seq 递增）。
// 渲染线程经 latest() 取字节，nvgCreateImageMem 建 nanovg 图像（seq 变化才重建）。
//
// 为什么走磁盘中转：worker 线程没有 GL 上下文，渲染 API 用不了；vo=image 是
// libmpv 自带的"渲到文件"出口，与主实例（vo=libmpv + FBO）互不干扰。
// mpv 支持同进程多 handle；第二实例 hwdec=no（软件解一帧足够快，且不与
// 主实例争硬件解码器上下文）。
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct mpv_handle;

namespace fr {

struct ThumbFrame {
  uint64_t seq = 0;                  // 结果代数；0 = 从未产出
  double pos = -1;                   // 该帧对应的绝对秒
  std::vector<unsigned char> bytes;  // jpg 文件字节（可直接喂 nvgCreateImageMem）
};

class ThumbPreview {
 public:
  bool init(const std::string& outdir);  // outdir：vo-image 写帧目录（自动创建）
  void shutdown();                       // 先停 worker 再销毁 mpv（顺序不可反）

  // 主线程：请求某绝对秒的缩略图。worker 只保留最新请求（拖拽中高频调用安全）。
  void request(double pos, const std::string& uri);
  bool ready() const { return mpv_ != nullptr; }

  // 渲染线程：取最新结果（拷贝；从未产出时 seq == 0）。
  // d52：先 latest_seq() 廉价查代数，变化了才调本函数 —— 结果槽是 jpg 全量字节
  //（可达 100KB+），以前每个 wake（含静默跳帧）都 mutex+深拷贝一遍是白吃。
  ThumbFrame latest();
  uint64_t latest_seq() const { return pub_seq_.load(std::memory_order_acquire); }

 private:
  void worker_loop();
  bool load(const std::string& uri);   // loadfile + 等元数据（duration > 0）
  bool seek_and_grab(double pos);      // seek 到目标秒并取回帧字节
  std::string newest_jpg();            // outdir 里最新的 .jpg（空 = 还没有）
  void clean_outdir();                 // 清空 outdir 里的旧帧（专用缓存目录，仅动 .jpg）

  mpv_handle* mpv_ = nullptr;
  std::string outdir_;
  std::thread th_;
  std::atomic<bool> quit_{false};

  // —— 请求槽（latest-wins；写方 = 主线程，读方 = worker）——
  std::mutex req_mu_;
  std::condition_variable cv_;
  bool has_req_ = false;
  double req_pos_ = -1;
  std::string req_uri_;

  // —— 结果槽（写方 = worker，读方 = 渲染线程）——
  std::mutex res_mu_;
  ThumbFrame res_;
  std::atomic<uint64_t> pub_seq_{0};  // d52：res_ 代数的镜像（worker 发布后推，渲染线程廉价前查）

  // —— worker 私有 ——
  std::string cur_uri_;  // 已加载的 URI（空 = 未加载）
  double cur_pos_ = -1;  // 最近一次成功取帧的秒（近重复请求去重）
};

}  // namespace fr
