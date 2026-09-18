#include "app/log.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>

#include <spdlog/async.h>
#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace fr {

namespace {
std::atomic<int> g_req{0};
}  // namespace

int next_req_id() { return g_req.fetch_add(1) + 1; }

void init_logger(const std::string& dir, const std::string& level) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  try {
    auto file = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(dir + "/flashrec.log",
                                                                       5 * 1024 * 1024, 7);
    auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console->set_level(spdlog::level::info);
    spdlog::init_thread_pool(8192, 1);
    auto logger = std::make_shared<spdlog::async_logger>(
        "flashrec", spdlog::sinks_init_list{file, console}, spdlog::thread_pool(),
        spdlog::async_overflow_policy::block);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    logger->set_level(spdlog::level::from_str(level));
    logger->flush_on(spdlog::level::warn);  // WARN+ 立即 flush
    spdlog::set_default_logger(logger);
    FR_LOG_INFO("[APP] 日志初始化完成 dir={} level={}", dir, level);
  } catch (const std::exception& e) {
    // 日志起不来也不能拦住程序：降级为仅控制台
    auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto logger = std::make_shared<spdlog::logger>("flashrec", console);
    logger->set_pattern("[%H:%M:%S.%e] [%l] %v");
    spdlog::set_default_logger(logger);
    FR_LOG_ERROR("[APP] 文件日志初始化失败：{}（降级为控制台）", e.what());
  }
}

void shutdown_logger() { spdlog::shutdown(); }

}  // namespace fr
