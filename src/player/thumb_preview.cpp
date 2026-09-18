#include "player/thumb_preview.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include <mpv/client.h>

#include "app/log.h"

namespace fr {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

bool ThumbPreview::init(const std::string& outdir) {
  outdir_ = outdir;
  std::error_code ec;
  fs::create_directories(outdir_, ec);
  if (ec) {
    FR_LOG_ERROR("[THUMB] 建帧目录失败: {} ({})", outdir_, ec.message());
    return false;
  }

  mpv_ = mpv_create();
  if (!mpv_) {
    FR_LOG_ERROR("[THUMB] mpv_create 失败");
    return false;
  }
  // vo=image：帧"显示"即写文件；paused + exact seek → 每次 seek 精确落一帧
  mpv_set_option_string(mpv_, "vo", "image");
  mpv_set_option_string(mpv_, "vo-image-format", "jpg");
  mpv_set_option_string(mpv_, "vo-image-jpeg-quality", "90");
  mpv_set_option_string(mpv_, "vo-image-outdir", outdir_.c_str());
  mpv_set_option_string(mpv_, "pause", "yes");       // 全程暂停：只为出帧，不播放
  mpv_set_option_string(mpv_, "keep-open", "yes");
  mpv_set_option_string(mpv_, "hwdec", "no");        // 软解一帧足够；不与主实例抢硬件解码器
  mpv_set_option_string(mpv_, "ao", "null");         // 无声（预览取帧不该出声）
  mpv_set_option_string(mpv_, "audio-display", "no");
  mpv_set_option_string(mpv_, "terminal", "no");
  mpv_set_option_string(mpv_, "osc", "no");
  mpv_set_option_string(mpv_, "input-default-bindings", "no");
  mpv_set_option_string(mpv_, "input-vo-keyboard", "no");
  mpv_set_option_string(mpv_, "input-cursor", "no");
  mpv_set_option_string(mpv_, "demuxer-max-bytes", "64MiB");
  if (mpv_initialize(mpv_) < 0) {
    FR_LOG_ERROR("[THUMB] mpv_initialize 失败");
    mpv_terminate_destroy(mpv_);
    mpv_ = nullptr;
    return false;
  }
  mpv_request_log_messages(mpv_, "warn");

  // FR_THUMB_STAGE：崩溃二分开关（排查段错误用）。
  //   1 = 只创建第二 mpv 句柄，不启动 worker；2（默认）= 完整管线。
  int stage = 2;
  if (const char* st = getenv("FR_THUMB_STAGE")) stage = atoi(st);
  FR_LOG_INFO("[THUMB] stage={}", stage);
  if (stage >= 2) {
    th_ = std::thread([this] { worker_loop(); });
  }
  FR_LOG_INFO("[THUMB] 缩略图预览就绪 outdir={}", outdir_);
  return true;
}

void ThumbPreview::shutdown() {
  quit_.store(true, std::memory_order_release);
  cv_.notify_all();
  if (th_.joinable()) th_.join();   // worker 必须先退（它独占 mpv_）
  if (mpv_) {
    mpv_terminate_destroy(mpv_);
    mpv_ = nullptr;
  }
}

void ThumbPreview::request(double pos, const std::string& uri) {
  if (!mpv_) return;
  {
    std::lock_guard<std::mutex> lk(req_mu_);
    req_pos_ = pos;
    req_uri_ = uri;
    has_req_ = true;
  }
  cv_.notify_all();
}

ThumbFrame ThumbPreview::latest() {
  std::lock_guard<std::mutex> lk(res_mu_);
  return res_;  // 拷贝（bytes 向量）；调用方先 latest_seq() 前查，代数变了才会进来（d52）
}

void ThumbPreview::worker_loop() {
  while (!quit_.load(std::memory_order_acquire)) {
    {
      std::unique_lock<std::mutex> lk(req_mu_);
      // d52：谓词等待（原 50ms 轮询 = 空闲时仍 20Hz 醒一次空转）。
      // request()/shutdown() 都带 notify_all，等待即醒，无轮询必要。
      cv_.wait(lk, [&] { return has_req_ || quit_.load(std::memory_order_acquire); });
    }
    if (quit_.load(std::memory_order_acquire)) break;

    double pos = -1;
    std::string uri;
    {
      std::lock_guard<std::mutex> lk(req_mu_);
      if (!has_req_) continue;
      pos = req_pos_;
      uri = req_uri_;
      has_req_ = false;  // latest-wins：取走即清，期间的堆积只保留最后一条
    }
    if (uri.empty()) continue;

    // URI 变化（换片）→ 重载；载入失败则回落"未加载"，下次请求重试
    if (uri != cur_uri_) {
      clean_outdir();
      if (!load(uri)) {
        cur_uri_.clear();
        cur_pos_ = -1;
        continue;
      }
      cur_uri_ = uri;
      cur_pos_ = -1;
    }
    if (pos < 0) continue;
    if (cur_pos_ >= 0 && std::fabs(pos - cur_pos_) < 0.5) continue;  // 近重复请求（悬停抖动）
    if (seek_and_grab(pos)) cur_pos_ = pos;
  }
}

bool ThumbPreview::load(const std::string& uri) {
  const char* cmd[] = {"loadfile", uri.c_str(), "replace", nullptr};
  if (mpv_command(mpv_, cmd) < 0) {
    FR_LOG_WARN("[THUMB] loadfile 失败: {}", uri);
    return false;
  }
  // 等元数据就绪（duration > 0）：网络流最长 20s，本机/局域网通常 <1s
  for (int i = 0; i < 400; ++i) {
    if (quit_.load(std::memory_order_acquire)) return false;
    double d = 0;
    if (mpv_get_property(mpv_, "duration", MPV_FORMAT_DOUBLE, &d) >= 0 && d > 0) return true;
    std::this_thread::sleep_for(50ms);
  }
  FR_LOG_WARN("[THUMB] 等元数据超时: {}", uri);
  return false;
}

bool ThumbPreview::seek_and_grab(double pos) {
  const double dur = [&] {
    double d = 0;
    mpv_get_property(mpv_, "duration", MPV_FORMAT_DOUBLE, &d);
    return d;
  }();
  // 越界/尾帧处理：末帧常为黑帧或解码边界，收进 dur-0.2
  if (dur > 0 && pos > dur - 0.2) pos = dur - 0.2 > 0 ? dur - 0.2 : 0;
  if (pos < 0) pos = 0;

  clean_outdir();  // seek 前清目录：之后出现的第一个 .jpg 就是目标帧

  char t[40];
  snprintf(t, sizeof(t), "%.3f", pos);
  const char* cmd[] = {"seek", t, "absolute", "exact", nullptr};
  if (mpv_command(mpv_, cmd) < 0) return false;

  // 等目标帧落盘（vo=image 在帧显示时写文件）；远端流 seek 可能慢，给 3s
  std::string file;
  for (int i = 0; i < 150; ++i) {
    if (quit_.load(std::memory_order_acquire)) return false;
    file = newest_jpg();
    if (!file.empty()) break;
    std::this_thread::sleep_for(20ms);
  }
  if (file.empty()) return false;

  std::ifstream f(file, std::ios::binary);
  std::vector<unsigned char> data((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
  f.close();
  std::error_code ec;
  fs::remove(file, ec);  // 读回即删，目录只留最新请求的临时产物
  if (data.empty()) return false;

  {
    std::lock_guard<std::mutex> lk(res_mu_);
    res_.seq++;
    res_.pos = pos;
    res_.bytes = std::move(data);
  }
  // d52：锁外推原子代数（release）——渲染线程 latest_seq() 不变就完全不碰本槽
  pub_seq_.store(res_.seq, std::memory_order_release);
  return true;
}

std::string ThumbPreview::newest_jpg() {
  std::error_code ec;
  fs::file_time_type best = fs::file_time_type::min();
  std::string best_path;
  for (const auto& e : fs::directory_iterator(outdir_, ec)) {
    if (ec || !e.is_regular_file(ec)) continue;
    std::string ext = e.path().extension().string();
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    if (ext != ".jpg" && ext != ".jpeg") continue;
    const auto t = e.last_write_time(ec);
    if (!ec && t > best) {
      best = t;
      best_path = e.path().string();
    }
  }
  return best_path;
}

void ThumbPreview::clean_outdir() {
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(outdir_, ec)) {
    if (ec) break;
    std::string ext = e.path().extension().string();
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    if (ext == ".jpg" || ext == ".jpeg") fs::remove(e.path(), ec);
  }
}

}  // namespace fr
