#pragma once
// 单实例（CPP_REWRITE_PLAN M5）：已运行则把旧窗口拉到前台并退出新进程。
namespace fr::single_instance {

// true = 本进程成为主实例；false = 已有实例（调用方应立即退出）
bool acquire();
// 把已运行实例的窗口带到前台
void raise_existing();

}  // namespace fr::single_instance
