// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#include "config.hpp"
#include "window.hpp"
#define WIN32_LEAN_AND_MEAN
#define _WIN32_DCOM
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
    wchar_t exe_path[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);

    std::wstring ws = (fs::path(exe_path).parent_path() / L"sysmeters.toml").wstring();
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "sysmeters.toml";
    std::string out(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

int main() {
    // 多重起動の排他（Named Mutex）
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"sysmeters-mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // 既存プロセスのウィンドウを探して終了要求
        HWND prev = FindWindowW(L"SystemMetersWnd", nullptr);
        if (prev) PostMessage(prev, WM_CLOSE, 0, 0);
        // 最大 3 秒待機して既存プロセスの終了を確認する
        for (int i = 0; i < 30; ++i) {
            if (!FindWindowW(L"SystemMetersWnd", nullptr)) break;
            Sleep(100);
        }
        // タイムアウト：既存プロセスが終了しなければ自分が終了する
        if (FindWindowW(L"SystemMetersWnd", nullptr)) {
            ReleaseMutex(mutex);
            CloseHandle(mutex);
            return 0;
        }
    }

    // COM 初期化（WMI 使用のため、マルチスレッド対応）
    HRESULT hr_com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr_com) && hr_com != RPC_E_CHANGED_MODE) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return 1;
    }
    // WMI プロキシ認証設定（CoInitializeSecurity を省くと WMI 接続が不安定になる）
    // 失敗（RPC_E_TOO_LATE 等）は致命的でないため無視する
    CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
        RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
        nullptr, EOAC_NONE, nullptr);

    AppConfig cfg = load_config(get_config_path());

    HINSTANCE hinst = GetModuleHandleW(nullptr);
    AppWindow window;
    if (!window.create(hinst, cfg)) {
        MessageBoxW(nullptr, L"ウィンドウの作成に失敗しました。\n管理者権限で実行してください。",
                    L"sysmeters", MB_ICONERROR);
        CoUninitialize();
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return 1;
    }

    window.run();
    CoUninitialize();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}
