// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#include "config.hpp"
#include "logger.hpp"
#include "window.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")

#include <filesystem>
namespace fs = std::filesystem;

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

// 実行ファイルと同じディレクトリの設定ファイルパスを返す（UTF-8 エンコード）
static std::string get_config_path() {
    wchar_t exe_path[MAX_PATH] = {};
    DWORD exe_len = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    // 取得失敗または切り詰め時はカレントディレクトリ基準の相対パスへフォールバック
    if (exe_len == 0 || exe_len >= MAX_PATH) return "sysmeters.toml";

    std::wstring ws = (fs::path(exe_path).parent_path() / L"sysmeters.toml").wstring();
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "sysmeters.toml";
    std::string out(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

int main() {
    // 多重起動排他（Named Mutex）
    // 排他の根拠はミューテックスの所有権獲得とする。既存インスタンスに WM_CLOSE を送り、
    // 旧プロセスの解放を最大 3 秒待つ。獲得できなければ多重起動と判断して自分が終了する
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"sysmeters-mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND prev = FindWindowW(L"SystemMetersWnd", nullptr);
        if (prev) PostMessage(prev, WM_CLOSE, 0, 0);
        DWORD wait = WaitForSingleObject(mutex, 3000);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
            CloseHandle(mutex);
            return 0;
        }
    }

    // COM 初期化（マルチスレッド対応）
    HRESULT hr_com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr_com) && hr_com != RPC_E_CHANGED_MODE) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return 1;
    }

    AppConfig cfg = load_config(get_config_path());
    log_init(cfg.log_dir);
    if (!cfg.config_error.empty())
        log_error("%s", cfg.config_error.c_str());
    log_info("sysmeters %s started", APP_VERSION);

    HINSTANCE hinst = GetModuleHandleW(nullptr);
    AppWindow window;
    if (!window.create(hinst, cfg)) {
        log_error("window creation failed");
        log_shutdown();
        MessageBoxW(nullptr, L"ウィンドウの作成に失敗しました。",
                    L"sysmeters", MB_ICONERROR);
        CoUninitialize();
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return 1;
    }

    window.run();
    log_info("sysmeters shutting down");
    log_shutdown();
    CoUninitialize();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}
