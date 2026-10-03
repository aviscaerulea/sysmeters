// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#include "collector_claude.hpp"
#include "config.hpp"
#include "logger.hpp"
#include "window.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

#include <filesystem>
namespace fs = std::filesystem;

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

// wide 文字列を UTF-8 の std::string へ変換する
// ログ出力・設定パスの受け渡しは UTF-8 に統一する。（logger はバイト列をそのまま書き出す）
// 変換失敗時は空文字列を返す
static std::string to_utf8(const std::wstring& ws) {
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

// 実行ファイルと同じディレクトリの設定ファイルパスを返す（UTF-8 エンコード）
static std::string get_config_path() {
    wchar_t exe_path[MAX_PATH] = {};
    DWORD exe_len = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    // 取得失敗または切り詰め時はカレントディレクトリ基準の相対パスへフォールバック
    if (exe_len == 0 || exe_len >= MAX_PATH) return "sysmeters.toml";

    std::string out = to_utf8((fs::path(exe_path).parent_path() / L"sysmeters.toml").wstring());
    return out.empty() ? "sysmeters.toml" : out;
}

// コマンドラインの第 1 引数が --statusline か判定する
static bool is_statusline_mode() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;
    bool hit = (argc >= 2 && wcscmp(argv[1], L"--statusline") == 0);
    LocalFree(argv);
    return hit;
}

// statusline 連携モードの本体：stdin の statusline JSON を連携ファイルへ書き出す
//
// Claude Code の statusline が起動し、常駐中の sysmeters がその書き出しを取り込む。
// 標準出力には何も書かない（statusline の表示は呼び出し元スクリプトの責務）。
// 書き出し失敗は statusline の表示を妨げないよう常に 0 で終了し、ログも残さない
// （Claude Code が応答ごとに起動するため、ログ初期化の費用と出力量を避ける）。
// stdin が無効・1MB 超のときは書かない
static int run_statusline() {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    if (in == nullptr || in == INVALID_HANDLE_VALUE) return 0;
    static constexpr size_t MAX_STDIN_BYTES = 1 * 1024 * 1024;
    std::string body;
    char buf[8192];
    DWORD read = 0;
    while (ReadFile(in, buf, sizeof(buf), &read, nullptr) && read > 0) {
        if (body.size() + read > MAX_STDIN_BYTES) return 0;
        body.append(buf, read);
    }
    AppConfig cfg = load_config(get_config_path());
    write_claude_statusline(body, cfg.claude_sub.enable ? cfg.claude_sub.config_dir : std::wstring());
    return 0;
}

int main() {
    // statusline 連携モード：stdin の statusline JSON を書き出して即終了する。
    // 多重起動排他より前に置く（後ろだと常駐中の sysmeters へ WM_CLOSE を送ってしまうため）
    if (is_statusline_mode()) return run_statusline();

    // nudge の CreateProcessW は相対名（例：claude.exe）をコマンドラインで受けるため、
    // 実行ファイル検索順からカレントディレクトリを除外し、CWD に置かれた
    // 偽実行ファイルの起動を防ぐ。この変数は nudge で起動する子プロセスにも継承されるが、
    // 効果は「CWD の実行ファイルを拾わない」という安全側のみのため許容する
    SetEnvironmentVariableW(L"NoDefaultCurrentDirectoryInExePath", L"1");

    // 多重起動排他（Named Mutex）
    // 排他の根拠はミューテックスの所有権獲得とする。既存インスタンスに WM_CLOSE を送り、
    // 旧プロセスの解放を最大 90 秒待つ。獲得できなければ多重起動と判断して自分が終了する。
    // 待ち時間は旧プロセスの終了処理の直列待機の最悪合計に余裕を加えた幅とする。
    // （更新確認スレッド join 約 11 秒 + 警告音 5 秒 + Claude フェッチ 15 秒 × 2 アカウント
    // + IP フェッチ 15 秒 ≒ 61 秒。Claude の 2 本は中断フラグへ応答しない真のハング時に
    // 待ちが直列化するため 2 本分を見込む）各コレクタの停止タイムアウトを変更した際は
    // ここも見直すこと
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"sysmeters-mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND prev = FindWindowW(L"SystemMetersWnd", nullptr);
        if (prev) PostMessage(prev, WM_CLOSE, 0, 0);
        DWORD wait = WaitForSingleObject(mutex, 90000);
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

    // 設定ファイルの試行パスと読み込み結果をログに残す。
    // 個人設定（sysmeters.local.toml）が反映されない問い合わせ時に、
    // どこを見に行ったかをログだけで切り分けられるようにする。
    log_info("config base : %s (loaded=%s)",
             cfg.base_path_used.c_str(),
             cfg.base_path_loaded ? "true" : "false");
    if (!cfg.local_path_used.empty())
        log_info("config local: %s (loaded=%s)",
                 cfg.local_path_used.c_str(),
                 cfg.local_path_loaded ? "true" : "false");

    // Claude アカウント設定の認識結果を起動時にダンプする。
    // サブが無効化された場合、その理由は cfg.config_error として既に上で出力済み。
    // wide 文字列は %ls で渡さず UTF-8 へ変換して %s で出力する。
    // narrow vsnprintf の %ls は "C" ロケール下で非 ASCII 文字の変換に失敗し、
    // その時点でログ行が切れて以降のフィールドが欠落するため
    log_info("claude main: enable=%s name='%s' nudge_enable=%s",
             cfg.claude_main.enable ? "true" : "false",
             to_utf8(cfg.claude_main.name).c_str(),
             cfg.claude_main.nudge_enable ? "true" : "false");
    log_info("claude sub : enable=%s name='%s' nudge_enable=%s config_dir='%s'",
             cfg.claude_sub.enable ? "true" : "false",
             to_utf8(cfg.claude_sub.name).c_str(),
             cfg.claude_sub.nudge_enable ? "true" : "false",
             to_utf8(cfg.claude_sub.config_dir).c_str());

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
