// server_guard.c - tiny Windows console guard for the DLSS5NR web UI.
//
// start_ui.bat / Start_DLSS5NR.bat run THIS exe instead of `node web/server.js` directly.
// The guard:
//   1. launches node web/server.js --open as a child process on the same console;
//   2. installs a SetConsoleCtrlHandler, so when the user closes the console window (X),
//      presses Ctrl+C, or the system delivers Ctrl+Break, the handler runs BEFORE the
//      process would be torn down by the OS;
//   3. on shutdown it deletes the disposable temp dirs (.tmp_uploads, .frame_previews)
//      and then exits;
//   4. if the node child exits by itself (crash / killed externally) the main thread
//      notices and sweeps the same dirs right away.
//
// v2: writes a diagnostic log (<root>\server_guard.log) covering handler registration,
// shutdown paths, per-file delete retries and any file that could not be removed, so a
// "window closed but files remained" report can be traced instead of guessed.
//
// This moves temp-file cleanup from "at the next startup" to "the moment the service
// window is closed". A genuinely abrupt kill (Task Manager ending both processes, power
// loss) cannot be caught by anything userland - server.js keeps its startup sweep as the
// last-resort fallback for exactly that case.
//
// Build (x64, static CRT so the exe has no VC runtime dependency; kernel32 + CRT only):
//   cl server_guard.c /O1 /MT /W3 /nologo /Fe:server_guard.exe
//
// Dev self-test:  server_guard.exe --clean   (sweep temp dirs and exit, no node launch)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include <stdarg.h>

static wchar_t g_root[MAX_PATH];        // directory holding this exe (= project root)
static HANDLE  g_child = NULL;
static volatile LONG g_done = 0;

static void log_msg(const char *fmt, ...) {
    wchar_t p[MAX_PATH * 2];
    swprintf_s(p, MAX_PATH * 2, L"%s\\server_guard.log", g_root);
    HANDLE h = CreateFileW(p, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    char buf[1024];
    int n = 0;
    SYSTEMTIME st;
    GetLocalTime(&st);
    n += sprintf_s(buf + n, (size_t)(sizeof(buf) - n),
                   "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    n += vsprintf_s(buf + n, (size_t)(sizeof(buf) - n), fmt, ap);
    va_end(ap);
    n += sprintf_s(buf + n, (size_t)(sizeof(buf) - n), "\r\n");
    DWORD w;
    WriteFile(h, buf, (DWORD)n, &w, NULL);
    CloseHandle(h);
}

// Recursively delete everything under dir. Retries each file a few times to ride out a
// transient lock (Defender scan / an engine child closing its handles). Returns the number
// of files that could not be deleted; every failure is logged with its error code.
static int delete_tree(const wchar_t *dir) {
    int fail = 0;
    wchar_t pat[MAX_PATH * 2];
    swprintf_s(pat, MAX_PATH * 2, L"%s\\*", dir);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND) {
            log_msg("  无法打开目录 %ls (错误码 %lu)", dir, GetLastError());
        }
        return 0;
    }
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        wchar_t full[MAX_PATH * 2];
        swprintf_s(full, MAX_PATH * 2, L"%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            fail += delete_tree(full);
            if (!RemoveDirectoryW(full)) {
                DWORD e = GetLastError();
                if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND) {
                    log_msg("  无法删除目录 %ls (错误码 %lu)", full, e);
                }
            }
        } else {
            DWORD a = GetFileAttributesW(full);
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) {
                SetFileAttributesW(full, a & ~FILE_ATTRIBUTE_READONLY);   // let DeleteFile through
            }
            BOOL ok = FALSE;
            for (int i = 0; i < 5; ++i) {
                if (DeleteFileW(full)) { ok = TRUE; break; }
                Sleep(200);
            }
            if (!ok) {
                DWORD e = GetLastError();
                log_msg("  删除失败 %ls (错误码 %lu, 重试5次后放弃)", full, e);
                fail++;
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return fail;
}

static int cleanup_temp_dirs(void) {
    int fail = 0;
    wchar_t p[MAX_PATH * 2];
    swprintf_s(p, MAX_PATH * 2, L"%s\\.tmp_uploads", g_root);
    int f1 = delete_tree(p);
    swprintf_s(p, MAX_PATH * 2, L"%s\\.frame_previews", g_root);
    int f2 = delete_tree(p);
    log_msg("清理完成: .tmp_uploads 失败=%d, .frame_previews 失败=%d", f1, f2);
    return f1 + f2;
}

// Kill pid AND its whole child tree (ffmpeg/realesr spawned by node) so file handles are
// released before we sweep. TerminateProcess alone would orphan the grandchildren.
static void kill_tree(DWORD pid) {
    wchar_t cmd[128];
    swprintf_s(cmd, 128, L"taskkill /F /T /PID %lu", pid);
    PROCESS_INFORMATION pi = {0};
    STARTUPINFOW si = {0};
    si.cb = sizeof(si);
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                      CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

static void shutdown_now(const char *why) {
    if (InterlockedExchange(&g_done, 1) != 0) return;   // already handled
    log_msg("收到关闭信号: %s", why);
    if (g_child) {
        // kill node AND any ffmpeg/realesr it spawned; only then sweep, so no process can
        // recreate or hold files under .frame_previews while we delete it.
        kill_tree(GetProcessId(g_child));
        WaitForSingleObject(g_child, 3000);
        CloseHandle(g_child);
        g_child = NULL;
    }
    int fail = cleanup_temp_dirs();
    log_msg("已退出, 残留清理失败数=%d", fail);
    ExitProcess(fail ? 3 : 0);
}

// Called by the OS on a fresh thread when the console sends Ctrl+C / Ctrl+Break /
// Ctrl+Close (window X). Returning TRUE marks the event handled; without it the process
// would simply be killed and we would never get a chance to clean up.
static BOOL WINAPI ctrl_handler(DWORD type) {
    if (type == CTRL_C_EVENT)      { log_msg("控制台事件: CTRL_C");      shutdown_now("ctrl_c"); }
    else if (type == CTRL_BREAK_EVENT) { log_msg("控制台事件: CTRL_BREAK"); shutdown_now("ctrl_break"); }
    else if (type == CTRL_CLOSE_EVENT) { log_msg("控制台事件: 窗口被关闭 (X)"); shutdown_now("ctrl_close"); }
    else return FALSE;              // not ours
    return TRUE;
}

int wmain(int argc, wchar_t **argv) {
    // Root = folder containing server_guard.exe (project root by construction).
    DWORD len = GetModuleFileNameW(NULL, g_root, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return 1;
    wchar_t *slash = wcsrchr(g_root, L'\\');
    if (slash) *slash = 0;
    SetCurrentDirectoryW(g_root);       // so "web\server.js" always resolves from project root
    log_msg("=== DLSS5NR 服务守护 v1.4 启动, 根目录=%ls", g_root);

    // Dev self-test: just sweep the temp dirs, no server involved.
    if (argc > 1 && wcscmp(argv[1], L"--clean") == 0) {
        int fail = cleanup_temp_dirs();
        log_msg("--clean 完成退出, 残留失败数=%d", fail);
        return fail ? 3 : 0;
    }

    // Locate node.exe: bundled tools\node.exe (release layout) first, then PATH (dev layout).
    wchar_t node[MAX_PATH * 2];
    swprintf_s(node, MAX_PATH * 2, L"%s\\tools\\node.exe", g_root);
    if (GetFileAttributesW(node) == INVALID_FILE_ATTRIBUTES) {
        DWORD n = SearchPathW(NULL, L"node.exe", NULL, MAX_PATH, node, NULL);
        if (n == 0 || n >= MAX_PATH) {
            log_msg("未找到 node.exe (tools 目录或 PATH 中都没有)");
            printf("ERROR: node.exe not found.\n");
            printf("  Expected bundled tools\\node.exe or node.exe on PATH.\n");
            return 2;
        }
    }
    log_msg("node 路径 = %ls", node);

    wchar_t cmd[MAX_PATH * 2 + 96];
    swprintf_s(cmd, MAX_PATH * 2 + 96, L"\"%s\" \"%s\\web\\server.js\" --open", node, g_root);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(node, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        log_msg("启动 node 失败, 错误码=%lu", GetLastError());
        printf("ERROR: failed to launch node (error %lu).\n", GetLastError());
        return 2;
    }
    g_child = pi.hProcess;
    CloseHandle(pi.hThread);
    log_msg("node 已启动, 进程号=%lu", pi.dwProcessId);

    BOOL reg = SetConsoleCtrlHandler(ctrl_handler, TRUE);
    log_msg("注册关闭事件处理器 -> %s", reg ? "成功" : "失败");

    // Normal path: wait until the node child is gone (crash / killed / self-exit),
    // then sweep the temp dirs here on the main thread.
    DWORD rc = 1;
    DWORD wr = WaitForSingleObject(g_child, INFINITE);
    log_msg("等待子进程结束返回 %lu (g_done=%ld)", wr, (long)g_done);
    if (wr == WAIT_OBJECT_0 && InterlockedExchange(&g_done, 1) == 0) {
        DWORD code = 1;
        GetExitCodeProcess(g_child, &code);
        rc = code;
        log_msg("子进程已退出 code=%lu, 开始清理临时目录", code);
        int fail = cleanup_temp_dirs();
        log_msg("已退出, 残留清理失败数=%d", fail);
    }
    CloseHandle(g_child);
    g_child = NULL;
    log_msg("guard exit code=%lu", rc);
    return (int)rc;
}
