# 第三方 C 库「构建期锚点补丁」的通用工具（v0.5.0）
#
# 为什么这样做（AGENTS.md 规则 8：third_party/ 只读，要改走构建期 patch）：
#   不在源码树里提交改动，改为构建期做**字符串级锚点替换**。
#   · 幂等：已打过（检测到替换后特征）即跳过，重复 configure 安全
#   · 唯一性：锚点在文件里必须**恰好出现 1 次**，否则 FATAL_ERROR（拒绝误伤）
#   · 可逆：开关置 OFF 时把替换文本**换回原文**（真的还原源码树，不是"不应用"）
#   · 安全：锚点未命中直接 FATAL_ERROR —— 依赖升级导致结构变化时**构建立刻失败**，
#           绝不静默漏补；错误信息里带文件、锚点与文档指针
#   · 逻辑主体在我方源码，第三方源码里只留「调用我方函数」的钩子
#
# 用法：fr_pupnp_patch_toggle(<开关变量名> <相对 third_party/pupnp 的路径> <锚点原文> <替换后文本>)
#
# 换行处理：pupnp 源码是 CRLF，而 CMake 里书写多行锚点只能用 LF
#   ⇒ 匹配前把整份文件归一为 LF，写回时再按原样式还原（CRLF），
#     这样锚点写法与文件实际内容无关，也不会把文件改动扩大化。
function(fr_pupnp_patch_toggle enable rel anchor replacement)
  set(_f "${FR_ROOT}/third_party/pupnp/${rel}")
  if (NOT EXISTS "${_f}")
    message(FATAL_ERROR "[FlashRec] pupnp 锚点补丁：找不到 ${rel}")
  endif()

  file(READ "${_f}" _raw)

  string(FIND "${_raw}" "\r\n" _has_crlf)
  if (NOT _has_crlf EQUAL -1)
    set(_nl "\r\n")
    string(REPLACE "\r\n" "\n" _src "${_raw}")
  else()
    set(_nl "\n")
    set(_src "${_raw}")
  endif()

  # 当前状态统一以「替换后文本」是否存在来判定。
  # ⚠️ 不能用 anchor 当状态特征：许多锚点本身就是 replacement 的**子串**
  #    （A0/A2/S1/S4/W1 的替换以锚点开头、S1 的替换以锚点结尾），拿它判"是否原始状态"
  #    会把已打补丁的文件误判为原始 → OFF 时静默漏还原，源码树停在半补丁状态。
  #    2026-09-17 实测踩到：7 处里有 4 处漏还原。
  string(FIND "${_src}" "${replacement}" _patched)

  if (${enable})
    set(_from "${anchor}")
    set(_to "${replacement}")
    set(_what "应用")
    if (NOT _patched EQUAL -1)
      message(STATUS "[FlashRec] pupnp 锚点补丁：已是补丁后状态，跳过 ${rel}")
      return()
    endif()
  else()
    set(_from "${replacement}")
    set(_to "${anchor}")
    set(_what "还原")
    if (_patched EQUAL -1)
      message(STATUS "[FlashRec] pupnp 锚点补丁：本就未打补丁，跳过 ${rel}")
      return()
    endif()
  endif()

  # 待替换文本必须恰好命中 1 次：0 次 = 源码结构已变；>1 次 = 会误伤别处，都拒绝替换。
  string(LENGTH "${_src}" _len_before)
  string(LENGTH "${_from}" _from_len)
  string(REPLACE "${_from}" "" _stripped "${_src}")
  string(LENGTH "${_stripped}" _len_after)
  math(EXPR _hits "(${_len_before} - ${_len_after}) / ${_from_len}")

  if (_hits EQUAL 0)
    message(FATAL_ERROR
      "[FlashRec] pupnp 锚点补丁${_what}失败：锚点未命中 ${rel}\n"
      "  待替换文本：${_from}\n"
      "  libupnp 源码结构可能已变化（或源码树处于半补丁状态）——\n"
      "  请对照 MULTI_NIC_PLAN.md §3 更新锚点，或 tools/fetch_deps.sh 重拉干净源码。")
  elseif (NOT _hits EQUAL 1)
    message(FATAL_ERROR
      "[FlashRec] pupnp 锚点补丁${_what}失败：待替换文本不唯一 ${rel}"
      "（命中 ${_hits} 次，要求 1 次）\n"
      "  待替换文本：${_from}\n"
      "  请把锚点写得更具体（多带邻近行）后再构建，避免误伤无关代码。")
  endif()
  unset(_len_before)
  unset(_len_after)
  unset(_stripped)

  string(REPLACE "${_from}" "${_to}" _out "${_src}")
  if (NOT _nl STREQUAL "\n")
    string(REPLACE "\n" "${_nl}" _out "${_out}")
  endif()
  file(WRITE "${_f}" "${_out}")
  message(STATUS "[FlashRec] pupnp 锚点补丁：已${_what} ${rel}")
endfunction()
