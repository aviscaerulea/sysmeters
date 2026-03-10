// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#include "logger.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdarg>
#include <cstdio>

static CRITICAL_SECTION g_cs;
static HANDLE           g_file         = INVALID_HANDLE_VALUE;
static int              g_current_date = -1;                   // 日付跨ぎ検出用 YYYYMMDD 複合値（-1 = 未初期化）
static wchar_t          g_log_dir[MAX_PATH] = {};              // ログ出力先（ワイド文字）

// ログファイルを開く（日付が変わっていれば旧ファイルを閉じて新ファイルを開く）
//
// g_current_date は YYYYMMDD 複合値で年月日を一括比較する。wDay のみの比較では
// 月をまたいだ同日（例：1/31 → 3/31）でローテーションが起きない問題を防ぐ。
static void open_or_rotate() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    int date = st.wYear * 10000 + st.wMonth * 100 + st.wDay;

    if (g_file != INVALID_HANDLE_VALUE && g_current_date == date) return;

    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }

    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\sysmeters_%04d%02d%02d.log",
               g_log_dir, st.wYear, st.wMonth, st.wDay);

    g_file = CreateFileW(path,
        GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (g_file != INVALID_HANDLE_VALUE) {
        SetFilePointer(g_file, 0, nullptr, FILE_END);
        g_current_date = date;
    }
}

// 30 日超のログファイルを削除する
static void purge_old_logs() {
    SYSTEMTIME now_st;
    GetLocalTime(&now_st);

    FILETIME now_ft;
    SystemTimeToFileTime(&now_st, &now_ft);
    ULONGLONG now_ul = (static_cast<ULONGLONG>(now_ft.dwHighDateTime) << 32) | now_ft.dwLowDateTime;

    // 30 日 = 30 * 24 * 3600 * 10^7（100ns 単位）
    constexpr ULONGLONG DAYS30 = 30ULL * 24 * 3600 * 10000000;

    wchar_t pattern[MAX_PATH];
    swprintf_s(pattern, L"%s\\sysmeters_*.log", g_log_dir);

    WIN32_FIND_DATAW fd;
    HANDLE h_find = FindFirstFileW(pattern, &fd);
    if (h_find == INVALID_HANDLE_VALUE) return;

    do {
        int year = 0, month = 0, day = 0;
        if (swscanf_s(fd.cFileName, L"sysmeters_%4d%2d%2d.log", &year, &month, &day) == 3) {
            SYSTEMTIME file_st{};
            file_st.wYear  = static_cast<WORD>(year);
            file_st.wMonth = static_cast<WORD>(month);
            file_st.wDay   = static_cast<WORD>(day);

            FILETIME file_ft;
            if (SystemTimeToFileTime(&file_st, &file_ft)) {
                ULONGLONG file_ul = (static_cast<ULONGLONG>(file_ft.dwHighDateTime) << 32) | file_ft.dwLowDateTime;
                if (now_ul > file_ul && now_ul - file_ul > DAYS30) {
                    wchar_t del_path[MAX_PATH];
                    swprintf_s(del_path, L"%s\\%s", g_log_dir, fd.cFileName);
                    DeleteFileW(del_path);
                }
            }
        }
    } while (FindNextFileW(h_find, &fd));

    FindClose(h_find);
}

// ログを 1 行書き込む
//
// msg バッファへの vsnprintf は CS 外で行い、書き込み処理のみ CS で保護する。
// msg はスタック変数のため、複数スレッドが同時呼び出しても干渉しない。
static void log_write(const char* level, const char* fmt, va_list args) {
    // メッセージ最大 1023 文字（終端 1 バイト含む）
    static constexpr int MSG_BUF  = 1024;
    // タイムスタンプ（23） + ラベル（7） + スペース・改行（4）= 34 バイトのオーバーヘッド
    static constexpr int LINE_BUF = MSG_BUF + 64;

    char msg[MSG_BUF];
    vsnprintf(msg, sizeof(msg), fmt, args);

    EnterCriticalSection(&g_cs);
    open_or_rotate();

    if (g_file != INVALID_HANDLE_VALUE) {
        SYSTEMTIME st;
        GetLocalTime(&st);

        char line[LINE_BUF];
        int len = snprintf(line, sizeof(line),
            "%04d-%02d-%02d %02d:%02d:%02d [%s] %s\r\n",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond,
            level, msg);

        if (len > 0) {
            DWORD written;
            WriteFile(g_file, line, static_cast<DWORD>(len), &written, nullptr);
        }
    }

    LeaveCriticalSection(&g_cs);
}

void log_init(const std::string& dir) {
    InitializeCriticalSection(&g_cs);

    wchar_t wdir[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, dir.c_str(), -1, wdir, MAX_PATH);

    // 絶対パス判定（ドライブレター "X:" か UNC "\\\\" で始まる）
    // 注意：シングルバックスラッシュ始まり（例：\Windows\Temp）はドライブ相対パスだが
    // 使用シナリオにないため相対パスとして exe ディレクトリ基準で展開する。
    bool is_absolute = (wcslen(wdir) >= 2 && wdir[1] == L':') ||
                       (wdir[0] == L'\\' && wdir[1] == L'\\');

    if (is_absolute) {
        wcscpy_s(g_log_dir, wdir);
    }
    else {
        // 相対パス → 実行ファイルのディレクトリを基準に解決する
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        wchar_t* sep = wcsrchr(exe, L'\\');
        if (sep) sep[1] = L'\0';  // ファイル名を除去してディレクトリのみに（末尾 \ を保持）
        swprintf_s(g_log_dir, L"%s%s", exe, wdir);
    }

    CreateDirectoryW(g_log_dir, nullptr);  // 既存の場合は ERROR_ALREADY_EXISTS で無視される
    open_or_rotate();
    purge_old_logs();
}

void log_shutdown() {
    EnterCriticalSection(&g_cs);
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    LeaveCriticalSection(&g_cs);
    DeleteCriticalSection(&g_cs);
}

const wchar_t* log_get_dir() {
    return g_log_dir;
}

void log_info(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_write("INFO ", fmt, args);
    va_end(args);
}

void log_error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_write("ERROR", fmt, args);
    va_end(args);
}
