#!/bin/bash
# FlashRec 显隐链路验证脚本（d92/d94 验收用；在测试机本机执行）
#
# 验什么：
#   1) 点托盘（SNI Activate）后窗口**立即**出帧 —— 显窗那一帧的归因是 reasons=[forced]
#      （d92：Wayland 的 glfwShowWindow 不提交缓冲，不强制整窗重绘就会空等到 2s 心跳）。
#      ★ 这一项需要应用带 FR_DBG_DAMAGE=3 启动，否则日志里根本没有 DMG3 归因行。
#   2) 显/隐交替多次后主线程仍活着（属性查询能应答）—— d94 的跨线程事件循环死锁
#      会让主线程永久卡在 wl_display_roundtrip，表现为窗口不再出现 + 托盘全部超时。
#      这一项**任何启动方式都能验**（只看 busctl 是否应答）。
#
# 手法（仅在开 DMG3 时有意义）：先等一个 2s 心跳帧出现，再等 1.5s 才 Activate —— 此时
#       DMG3 的 1s 限频已过期，显窗那一帧的归因行必然被打出来（否则会被限频吞掉）。
#
# 前置：应用以 systemd-run 启动（journald 收 stdout，脚本才读得到日志）：
#   systemd-run --user --unit=flashrec --collect --setenv=DISPLAY=:0 \
#               --setenv=FR_DBG_DAMAGE=3 /home/<user>/flashrec/src/build/bin/flashrec
# 用法：tools/verify_show_toggle.sh [轮数，默认 4]
# 传到测试机：python tools/debian_run.py --put tools/verify_show_toggle.sh /home/<user>/
#             （sync_debian.py 只同步 src/assets/cmake，不含 tools/）

set -u
ROUNDS="${1:-4}"
UNIT=flashrec
# ⚠️ 必须按 **当前实例的 PID** 过滤日志：同一个 unit 名下的历史实例（比如带
# FR_DBG_DAMAGE=3 的调试实例）的 DMG3 行还留在 journal 里，不过滤会拿旧数据充数。
PID=$(pgrep -x flashrec | head -1)
if [ -z "$PID" ]; then
  echo "flashrec 没有在跑"
  exit 1
fi
J="journalctl --user -u $UNIT _PID=$PID --no-pager -n 3000"
echo "PID = $PID"

SNI=$(busctl --user list 2>/dev/null | awk '/kde\.StatusNotifierItem-[0-9]+/{print $1; exit}')
if [ -z "$SNI" ]; then
  echo "找不到 SNI 项（托盘未挂载？）—— 先确认应用已启动且日志有 [TRAY] 托盘已挂载"
  exit 1
fi
echo "SNI = $SNI"

HAVE_DMG3=$($J 2>/dev/null | grep -c 'DMG3')
if [ "$HAVE_DMG3" = "0" ]; then
  echo "⚠️  日志里没有 DMG3 归因行 —— 本次只能验「不卡死」。"
  echo "    要验「显窗立即出帧（forced）」，请带 FR_DBG_DAMAGE=3 重启后再跑："
  echo "    systemctl --user stop $UNIT; systemd-run --user --unit=$UNIT --collect \\"
  echo "        --setenv=DISPLAY=:0 --setenv=FR_DBG_DAMAGE=3 <exe>"
fi
echo

for i in $(seq 1 "$ROUNDS"); do
  if [ "$HAVE_DMG3" != "0" ]; then
    n=$($J 2>/dev/null | grep -c 'DMG3')
    for _ in $(seq 1 60); do
      [ "$($J 2>/dev/null | grep -c 'DMG3')" != "$n" ] && break
      sleep 0.2
    done
    sleep 1.5
  else
    sleep 0.5
  fi
  echo "== 第 $i 次 Activate @ $(date +%H:%M:%S.%3N)"
  if busctl --user call "$SNI" /StatusNotifierItem org.kde.StatusNotifierItem Activate \
       ii 0 0 --timeout=5; then
    echo "   reply=ok（未超时 = 主线程在泵事件）"
  else
    echo "   ★ reply 超时 = 主线程卡死（d94 类问题复发）"
  fi
  sleep 0.7
  if [ "$HAVE_DMG3" != "0" ]; then
    $J 2>/dev/null | grep -E 'DMG' | tail -2 | sed 's/.*flashrec\[[0-9]*\]: /  /'
  fi
done

echo
echo "== 末态主线程活性（能应答 = 没死锁）=="
timeout 5 busctl --user get-property "$SNI" /StatusNotifierItem \
  org.kde.StatusNotifierItem Category
