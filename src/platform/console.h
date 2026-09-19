#pragma once
// 控制台可用性（规则 5：平台差异只在本目录）。
//
// 目标行为（2026-09-19）：
//   - 默认（GUI 子系统，FLASHREC_CONSOLE=OFF）**不弹控制台**；
//   - 命令行带 `--console` 时才获得一个可写控制台；
//   - 控制台存在时把输入/输出代码页设为 UTF-8，消除中文日志乱码
//     （spdlog 的 Windows sink 走 WriteConsoleA，按控制台代码页解码）。
namespace fr {

// 当前进程是否已有一个可写控制台（控制台子系统构建 / 已附着父控制台 / 已本目录申请）。
bool console_available();

// 申请控制台：无控制台时优先附着父进程控制台，失败则新建；随后重定向标准流、
// 让标准句柄有效并把代码页设为 UTF-8。
// 返回 true = 控制台已就绪；false = 拿不到（仅 GUI 子系统 + 无父控制台 + 新建失败）。
bool console_open();

// 控制台存在则把代码页设为 UTF-8（不存在时空操作；非 Windows 空操作）。
void console_set_utf8();

}  // namespace fr
