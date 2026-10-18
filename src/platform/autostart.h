#pragma once
// 开机启动（托盘勾选项的真值来源；平台差异收口于 platform，规则 5）：
//   Windows = HKCU\Software\Microsoft\Windows\CurrentVersion\Run 的 "FlashRec" 值
//   Linux   = $XDG_CONFIG_HOME/autostart/flashrec.desktop（无 XDG 时 ~/.config）
// 状态真值 = 系统配置本身，不写 settings.json —— 用户在系统里删掉入口后，菜单
// 勾选下次打开即如实反映。可执行文件路径变化（便携包挪目录）在 enabled() 读取
// 时就地修正，避免开机启动静默指向旧路径。
namespace fr::autostart {

// 当前系统是否已配置为开机启动（未配置/读取失败 = false；旧路径自动修正）。
bool enabled();

// 写入（on=true）或移除（on=false）开机启动入口。返回是否成功。
bool set_enabled(bool on);

}  // namespace fr::autostart
