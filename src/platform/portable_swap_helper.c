// portable_swap_helper.c — ★d495：便携版自动换装助手（独立进程，d474 决议落地）
//
// 职责单一：主程序退出后，用已校验的更新 zip 原地替换便携程序目录并重启新版。
// Windows 文件锁语义（运行中 exe/dll 不可删除覆盖）决定了换装只能由第三方进程
// 在主进程死后完成 —— 这就是本进程存在的唯一理由；Linux 无此锁，不需要对等物
// （AppImage 走 d461 shell 助手 / deb 走 d473 pkexec，见 update_install.cpp）。
//
// 流程（对齐 d461 AppImage 助手）：
//   等主进程退出(20s) → 清残留(_old/_new/flashrec_swap.old) → tar 解压 zip 到 _new
//   → 校验布局(_new\flashrec.exe) → 自体改名让位(flashrec_swap.old，运行中 exe 可
//   改名不可删) → 旧程序整体挪入 _old → _new 挪入程序目录 → 起新版 → 4s 探活
//   → 活：清 _old(自体映像锁删不掉，留待下次更新清理)删 zip；死：还原 _old。
//   任一步失败 → 还原备份 → 打开 zip 所在文件夹（人工交接回退）。
//
// 约束（d474）：本进程不联网、不提权；zip 哈希复核由主程序在拉起前完成。
// 解压用系统自带 tar.exe（bsdtar，Win10 1803+；缺失即回退人工路径）。
// 用法：flashrec_swap.exe --pid <主进程id> --zip <更新包路径> --dir <程序目录>

#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <tlhelp32.h>

#ifndef ARRAYSIZE
#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

static FILE *g_log = NULL;

// ---- 窄 UTF-8 写日志（%APPDATA%\FlashRec\logs\swap_helper.log，每场覆盖）----
static void log_wide(const wchar_t *s) {
  if (!g_log || !s) return;
  const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
  if (n <= 0) return;
  char buf[1024];
  WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, (int)sizeof(buf) - 1, NULL, NULL);
  fputs(buf, g_log);
  fputc('\n', g_log);
  fflush(g_log);
}

static void swap_log(const wchar_t *fmt, ...) {
  if (!g_log) return;
  wchar_t line[1024];
  va_list ap;
  va_start(ap, fmt);
  _vsnwprintf_s(line, ARRAYSIZE(line), _TRUNCATE, fmt, ap);
  va_end(ap);
  log_wide(line);
}

static void log_open(void) {
  wchar_t appdata[MAX_PATH] = {0};
  const DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, ARRAYSIZE(appdata));
  if (n == 0 || n >= ARRAYSIZE(appdata)) return;
  wchar_t path[MAX_PATH];
  if (_snwprintf_s(path, ARRAYSIZE(path), _TRUNCATE,
                   L"%s\\FlashRec\\logs\\swap_helper.log", appdata) <= 0)
    return;
  FILE *f = NULL;
  if (_wfopen_s(&f, path, L"w") != 0 || f == NULL) return;  // 日志尽力而为
  g_log = f;
}

// ---- 小工具 ----
static BOOL file_exists(const wchar_t *path) {
  const DWORD attr = GetFileAttributesW(path);
  return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

// 尽力而为删除目录树；SHFileOperationW 需要双 NUL 结尾路径。
// FOF_ALLOWUNDO：成功路径上 _old 进回收站而非直删（用户误放个人文件时有兜底）。
static void delete_tree(const wchar_t *path, BOOL allow_undo) {
  wchar_t buf[MAX_PATH * 2];
  if (_snwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, L"%s", path) <= 0) return;
  buf[wcslen(buf) + 1] = L'\0';  // 双 NUL
  SHFILEOPSTRUCTW op = {0};
  op.hwnd = NULL;
  op.wFunc = FO_DELETE;
  op.pFrom = buf;
  op.fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI |
              (allow_undo ? FOF_ALLOWUNDO : 0);
  SHFileOperationW(&op);  // 失败不致命：留待下次更新清理
}

static void open_zip_folder(const wchar_t *zip) {
  wchar_t args[MAX_PATH * 2];
  if (_snwprintf_s(args, ARRAYSIZE(args), _TRUNCATE, L"/select,\"%s\"", zip) <= 0)
    return;
  ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
}

static DWORD wait_pid_exit(DWORD pid, DWORD timeout_ms) {
  if (pid == 0) return WAIT_OBJECT_0;
  const HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
  if (h == NULL) return WAIT_OBJECT_0;  // 进程已不在
  const DWORD rc = WaitForSingleObject(h, timeout_ms);
  CloseHandle(h);
  return rc;
}

// ---- tar 解压（系统自带 bsdtar，内容嗅探识别 zip；-C 目标目录必须先存在）----
static BOOL extract_with_tar(const wchar_t *zip, const wchar_t *dest) {
  if (!CreateDirectoryW(dest, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
    swap_log(L"创建解压目录失败 gle=%lu", GetLastError());
    return FALSE;
  }
  wchar_t sys[MAX_PATH];
  const UINT syslen = GetSystemDirectoryW(sys, ARRAYSIZE(sys));
  if (syslen == 0 || syslen >= ARRAYSIZE(sys) - 8) return FALSE;
  wchar_t tar[MAX_PATH];
  if (_snwprintf_s(tar, ARRAYSIZE(tar), _TRUNCATE, L"%s\\tar.exe", sys) <= 0)
    return FALSE;
  if (!file_exists(tar)) {
    swap_log(L"tar.exe 不存在（Win10 1803 前的系统），回退人工交接");
    return FALSE;
  }
  wchar_t cmd[MAX_PATH * 3];
  if (_snwprintf_s(cmd, ARRAYSIZE(cmd), _TRUNCATE, L"\"%s\" -xf \"%s\" -C \"%s\"", tar,
                   zip, dest) <= 0)
    return FALSE;
  STARTUPINFOW si = {0};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {0};
  if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si,
                      &pi)) {
    swap_log(L"tar 启动失败 gle=%lu", GetLastError());
    return FALSE;
  }
  WaitForSingleObject(pi.hProcess, 120000);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (code != 0) {
    swap_log(L"tar 解压退出码 %lu", code);
    return FALSE;
  }
  return TRUE;
}

// ---- 目录搬运（同卷 rename；exclude1/exclude2 为全路径，可 NULL）----
static BOOL move_all(const wchar_t *from, const wchar_t *to,
                     const wchar_t *exclude1, const wchar_t *exclude2) {
  wchar_t pattern[MAX_PATH];
  if (_snwprintf_s(pattern, ARRAYSIZE(pattern), _TRUNCATE, L"%s\\*", from) <= 0)
    return FALSE;
  WIN32_FIND_DATAW fd;
  HANDLE find = FindFirstFileW(pattern, &fd);
  if (find == INVALID_HANDLE_VALUE) return FALSE;
  BOOL ok = TRUE;
  do {
    if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
    if (wcscmp(fd.cFileName, L"_old") == 0 || wcscmp(fd.cFileName, L"_new") == 0)
      continue;
    wchar_t src[MAX_PATH], dst[MAX_PATH];
    if (_snwprintf_s(src, ARRAYSIZE(src), _TRUNCATE, L"%s\\%s", from, fd.cFileName) <= 0)
      continue;
    if (_snwprintf_s(dst, ARRAYSIZE(dst), _TRUNCATE, L"%s\\%s", to, fd.cFileName) <= 0)
      continue;
    if (exclude1 && _wcsicmp(src, exclude1) == 0) continue;
    if (exclude2 && _wcsicmp(src, exclude2) == 0) continue;
    if (!MoveFileExW(src, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      swap_log(L"挪动失败 %ls gle=%lu", src, GetLastError());
      ok = FALSE;
      break;
    }
  } while (FindNextFileW(find, &fd));
  FindClose(find);
  return ok;
}

static BOOL restore_backup(const wchar_t *dir, const wchar_t *old_dir) {
  swap_log(L"还原备份 _old → 程序目录");
  const BOOL ok = move_all(old_dir, dir, NULL, NULL);
  RemoveDirectoryW(old_dir);
  return ok;
}

// ---- 探活：程序目录下的 flashrec.exe 进程是否存在（按映像全路径前缀判归属）----
static BOOL flashrec_alive_under(const wchar_t *dir) {
  const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return FALSE;
  PROCESSENTRY32W pe = {0};
  pe.dwSize = sizeof(pe);
  BOOL alive = FALSE;
  if (Process32FirstW(snap, &pe)) {
    do {
      if (_wcsicmp(pe.szExeFile, L"flashrec.exe") != 0) continue;
      const HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                   pe.th32ProcessID);
      if (p == NULL) continue;
      wchar_t img[MAX_PATH];
      DWORD size = ARRAYSIZE(img);
      if (QueryFullProcessImageNameW(p, 0, img, &size)) {
        const size_t dlen = wcslen(dir);
        if (wcslen(img) > dlen && _wcsnicmp(img, dir, (int)dlen) == 0 &&
            img[dlen] == L'\\')
          alive = TRUE;
      }
      CloseHandle(p);
    } while (!alive && Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  return alive;
}

// ---- 失败收尾：尽力删掉已落位的新助手 + 还原 + 打开 zip 所在文件夹 ----
static DWORD fail_path(const wchar_t *dir, const wchar_t *old_dir,
                       const wchar_t *zip) {
  wchar_t leftover[MAX_PATH];
  if (_snwprintf_s(leftover, ARRAYSIZE(leftover), _TRUNCATE, L"%s\\flashrec_swap.exe",
                   dir) > 0)
    DeleteFileW(leftover);  // 还原后不应残留坏包带来的助手（自体在运行则删不掉，无害）
  restore_backup(dir, old_dir);
  open_zip_folder(zip);
  swap_log(L"换装失败，已还原并回退人工交接");
  return 1;
}

int wmain(int argc, wchar_t **argv) {
  DWORD pid = 0;
  const wchar_t *zip = NULL;
  const wchar_t *dir = NULL;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (_wcsicmp(argv[i], L"--pid") == 0) pid = (DWORD)_wtoi(argv[i + 1]);
    else if (_wcsicmp(argv[i], L"--zip") == 0) zip = argv[i + 1];
    else if (_wcsicmp(argv[i], L"--dir") == 0) dir = argv[i + 1];
  }
  log_open();
  swap_log(L"---- 换装助手启动 pid=%lu zip=%ls dir=%ls", pid, zip ? zip : L"(null)",
       dir ? dir : L"(null)");
  if (pid == 0 || zip == NULL || dir == NULL) {
    swap_log(L"参数不完整");
    return 2;
  }
  // 规整 dir：去尾部反斜杠（保留 "C:\" 根）；分隔符统一反斜杠（bsdtar 对
  // "C:/..." 的盘符冒号会按远端主机语法误判，必须原生反斜杠）。
  wchar_t zipbuf[MAX_PATH], dirdir[MAX_PATH];
  if (_snwprintf_s(zipbuf, ARRAYSIZE(zipbuf), _TRUNCATE, L"%s", zip) <= 0) return 2;
  for (wchar_t *p = zipbuf; *p; ++p)
    if (*p == L'/') *p = L'\\';
  zip = zipbuf;
  if (_snwprintf_s(dirdir, ARRAYSIZE(dirdir), _TRUNCATE, L"%s", dir) <= 0) return 2;
  for (wchar_t *p = dirdir; *p; ++p)
    if (*p == L'/') *p = L'\\';
  while (wcslen(dirdir) > 3 && dirdir[wcslen(dirdir) - 1] == L'\\')
    dirdir[wcslen(dirdir) - 1] = L'\0';

  wchar_t old_dir[MAX_PATH], new_dir[MAX_PATH], exe_new[MAX_PATH], exe_run[MAX_PATH];
  if (_snwprintf_s(old_dir, ARRAYSIZE(old_dir), _TRUNCATE, L"%s\\_old", dirdir) <= 0 ||
      _snwprintf_s(new_dir, ARRAYSIZE(new_dir), _TRUNCATE, L"%s\\_new", dirdir) <= 0 ||
      _snwprintf_s(exe_new, ARRAYSIZE(exe_new), _TRUNCATE, L"%s\\_new\\flashrec.exe",
                   dirdir) <= 0 ||
      _snwprintf_s(exe_run, ARRAYSIZE(exe_run), _TRUNCATE, L"%s\\flashrec.exe",
                   dirdir) <= 0)
    return 2;

  // 1) 等主进程退出。超时 = 用户取消了退出，什么都别动，静默退出。
  if (wait_pid_exit(pid, 20000) != WAIT_OBJECT_0) {
    swap_log(L"主进程 20s 未退出（疑似用户取消），不动现场");
    return 1;
  }
  swap_log(L"主进程已退出");

  // 2) 清上次残留（上场的 _old/_new 与助手自体改名残留 flashrec_swap.old）
  delete_tree(old_dir, FALSE);
  delete_tree(new_dir, FALSE);
  {
    wchar_t stale_self[MAX_PATH];
    if (_snwprintf_s(stale_self, ARRAYSIZE(stale_self), _TRUNCATE, L"%s\\flashrec_swap.old",
                     dirdir) > 0)
      DeleteFileW(stale_self);  // 上场助手旧映像（当时运行中删不掉）
  }

  // 3) 解压 + 布局校验。发布 zip 实为「同名顶层目录」包裹（d495 原假设平铺，
  //    0.6.4 真包证伪）→ 根上无 exe 时找唯一含 flashrec.exe 的子目录作安装源。
  if (!extract_with_tar(zip, new_dir)) {
    delete_tree(new_dir, FALSE);
    open_zip_folder(zip);
    return 1;
  }
  wchar_t src[MAX_PATH];
  if (file_exists(exe_new)) {
    if (_snwprintf_s(src, ARRAYSIZE(src), _TRUNCATE, L"%s", new_dir) <= 0) return 2;
  } else {
    // 扫 _new 一级子目录：恰好一个且其中含 flashrec.exe → 即安装源
    int found = 0;
    wchar_t pattern[MAX_PATH];
    if (_snwprintf_s(pattern, ARRAYSIZE(pattern), _TRUNCATE, L"%s\\*", new_dir) <= 0)
      return 2;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) {
      swap_log(L"解压目录不可读");
      delete_tree(new_dir, FALSE);
      open_zip_folder(zip);
      return 1;
    }
    do {
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
          continue;
        wchar_t cand[MAX_PATH];
        if (_snwprintf_s(cand, ARRAYSIZE(cand), _TRUNCATE, L"%s\\%s", new_dir,
                         fd.cFileName) <= 0)
          continue;
        wchar_t cand_exe[MAX_PATH];
        if (_snwprintf_s(cand_exe, ARRAYSIZE(cand_exe), _TRUNCATE,
                         L"%s\\flashrec.exe", cand) <= 0)
          continue;
        if (file_exists(cand_exe)) {
          if (_snwprintf_s(src, ARRAYSIZE(src), _TRUNCATE, L"%s", cand) <= 0) continue;
          ++found;
        }
      }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    if (found != 1) {
      swap_log(L"解压产物缺 flashrec.exe（布局不符，候选 %d 个）", found);
      delete_tree(new_dir, FALSE);
      open_zip_folder(zip);
      return 1;
    }
    swap_log(L"布局：包裹目录，安装源 = _new 下唯一子目录");
  }

  // 4) 自体改名让位：运行中 exe 可改名不可删/覆盖，让出文件名给新包的助手。
  //    改名后的旧映像**排除出搬运**、留在程序目录根（18KB），因进程存活删不掉，
  //    由下次更新的残留清理删除 —— 换取 _old 备份可整体进回收站。
  wchar_t self_path[MAX_PATH], self_old[MAX_PATH];
  GetModuleFileNameW(NULL, self_path, ARRAYSIZE(self_path));
  if (_snwprintf_s(self_old, ARRAYSIZE(self_old), _TRUNCATE, L"%s\\flashrec_swap.old",
                   dirdir) <= 0)
    return 2;
  if (!MoveFileExW(self_path, self_old, MOVEFILE_REPLACE_EXISTING)) {
    swap_log(L"自体改名失败 gle=%lu —— 新助手本轮无法落位，走失败路径", GetLastError());
    return fail_path(dirdir, old_dir, zip);
  }

  // 5) 备份：旧程序整体挪入 _old（同卷 rename；排除 zip 与已改名的自体旧映像）
  if (!CreateDirectoryW(old_dir, NULL)) {
    swap_log(L"创建 _old 失败 gle=%lu", GetLastError());
    return fail_path(dirdir, old_dir, zip);
  }
  if (!move_all(dirdir, old_dir, zip, self_old)) return fail_path(dirdir, old_dir, zip);

  // 6) 安装：安装源（_new 根或包裹子目录）挪入程序目录；挪空后先删包裹子目录
  //    再删 _new（真机 0.6.4 包为包裹布局，漏删包裹子目录会让 _new 残留）。
  if (!move_all(src, dirdir, NULL, NULL)) return fail_path(dirdir, old_dir, zip);
  if (src != new_dir) RemoveDirectoryW(src);
  RemoveDirectoryW(new_dir);

  // 7) 起新版
  STARTUPINFOW si = {0};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {0};
  if (!CreateProcessW(exe_run, NULL, NULL, NULL, FALSE, 0, NULL, dirdir, &si, &pi)) {
    swap_log(L"新版启动失败 gle=%lu", GetLastError());
    return fail_path(dirdir, old_dir, zip);
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  // 8) 探活：4s 后进程仍在 = 换装成功
  Sleep(4000);
  if (!flashrec_alive_under(dirdir)) {
    swap_log(L"新版 4s 内退出");
    return fail_path(dirdir, old_dir, zip);
  }

  // 9) 成功收尾：_old 整体进回收站（自体旧映像已排除在外）、删 zip
  delete_tree(old_dir, TRUE);
  DeleteFileW(zip);
  swap_log(L"换装成功（助手旧映像 %s 留待下次更新清理）", self_old);
  return 0;
}
