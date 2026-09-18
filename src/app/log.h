#pragma once
// 日志框架（DMR_SOAP_SPEC §6）：spdlog 异步 + 5MB×7 轮转 + WARN+ 立即 flush。
// 层标签写进消息体：[SSDP] [HTTP] [SOAP] [GENA] [DMR] [MPV] [UI] [APP]
// 关联号：fr::next_req_id() 取 req#N，贯穿 请求→处理→状态变化→事件发出。
#include <spdlog/spdlog.h>

namespace fr {

void init_logger(const std::string& dir, const std::string& level);
void shutdown_logger();
int next_req_id();

}  // namespace fr

#define FR_LOG_TRACE(...) SPDLOG_TRACE(__VA_ARGS__)
#define FR_LOG_DEBUG(...) SPDLOG_DEBUG(__VA_ARGS__)
#define FR_LOG_INFO(...) SPDLOG_INFO(__VA_ARGS__)
#define FR_LOG_WARN(...) SPDLOG_WARN(__VA_ARGS__)
#define FR_LOG_ERROR(...) SPDLOG_ERROR(__VA_ARGS__)
