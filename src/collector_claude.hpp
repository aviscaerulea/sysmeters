// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#pragma once
#include "metrics.hpp"
#include <windows.h>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>

// Claude Code レートリミット情報とセッション数の収集
//
// - セッション数：CreateToolhelp32Snapshot でプロセス列挙（メインスレッド）
// - レートリミット：WinHTTP でバックグラウンドスレッドから非同期取得
//   - credentials.json から OAuth トークン取得
//   - Anthropic Usage API / Account API を呼び出し
//   - $TEMP に JSON キャッシュを保存（Usage: usage_interval_sec 秒、Plan: 3600 秒）
//
// アカウントごとに 1 インスタンスを持つ。
// - account_index：WM_CLAUDE_DONE の wParam として送る識別子（0=Main, 1=Sub）
// - config_dir：空ならメイン（~/.claude）、非空ならサブの .claude ディレクトリ絶対パス
// - cache_suffix：キャッシュファイル名末尾に付くサフィックス（メイン=""、サブ="-sub"）
class ClaudeCollector {
public:
    // HWND はバックグラウンドスレッド完了時に WM_CLAUDE_DONE を投げる先
    void init(HWND notify_wnd, int account_index,
              const std::wstring& config_dir, const std::string& cache_suffix,
              int usage_interval_sec = 120,
              bool nudge_enable = false, const std::string& nudge_cmd = {});

    // 1 秒ごとに呼び出す（セッション数取得 + 非同期 API トリガー）
    void update(ClaudeMetrics& out);

    // 取得結果を out へ反映する（WM_CLAUDE_DONE 受信後、メインスレッドから呼ぶ）
    void apply_result(ClaudeMetrics& out);

    // 2 段階終了：複数コレクタを並行停止できるよう、フラグ立てと join 待ちを分離
    // 両方のコレクタに request_shutdown() を先に呼んでから wait_shutdown() を順に呼ぶことで、
    // 順次 shutdown() 呼び出しの 15 秒 * 2 待機を並列化して合計 15 秒に抑える
    void request_shutdown();
    void wait_shutdown();
    void shutdown() { request_shutdown(); wait_shutdown(); }
    ~ClaudeCollector() { shutdown(); }

private:
    std::atomic<HWND>  notify_wnd_ = nullptr;
    int                account_index_ = 0;     // 0=Main, 1=Sub。WM_CLAUDE_DONE の wParam として送る
    double             usage_ttl_  = 120.0;    // Usage API のキャッシュ TTL（秒）
    bool               nudge_enable_ = false;  // 5h リセット後に claude.exe を起動するか
    std::string        nudge_cmd_;             // 起動コマンドライン
    // サブアカウント用の .claude ディレクトリ。
    // 非空のとき nudge 実行時に CLAUDE_CONFIG_DIR 環境変数を一時設定して claude.exe を起動する。
    // Claude CLI には --config-dir コマンドオプションが存在せず、設定ディレクトリの上書きは
    // CLAUDE_CONFIG_DIR 環境変数経由でのみ可能。設定は子プロセスに限定するためレジストリやプロセス
    // 親環境は変更しない。
    std::wstring       config_dir_;
    time_t             last_nudge_resets_ts_ = -1;  // 前回 nudge 実行時の 5h resets_ts（重複抑制）
    std::atomic<bool>  fetching_   = false;
    std::atomic<bool>  shutdown_   = false;    // shutdown 要求フラグ（fetch スレッドの早期中断用）
    HANDLE             fetch_thread_ = nullptr;
    std::mutex         result_mutex_;
    bool               first_fetch_ = true;   // 初回フェッチフラグ（ネガティブキャッシュ無視に使用）

    // インスタンスごとに固定されるパス。init() で構築する
    // 取得失敗時は空 path のままにし、関連 I/O は静かに失敗させる（既存挙動互換）
    std::filesystem::path creds_path_;
    std::filesystem::path cache_usage_path_;
    std::filesystem::path cache_plan_path_;

    // バックグラウンドで取得した結果（仮置き）
    ClaudeMetrics pending_{};

    static DWORD WINAPI fetch_thread(LPVOID param);
    void do_fetch();
    void run_nudge();
    std::string get_token();
};

// claude.exe のセッション数をメイン/サブで分けて数える
// `sub_config_dir` が空またはディレクトリ正規化に失敗した場合は全件をメインとして数える。
// 各 claude.exe プロセスの PEB から環境変数ブロックを読み、CLAUDE_CONFIG_DIR の値を
// GetFullPathNameW で正規化して sub_config_dir と一致するものをサブとしてカウントする。
// OpenProcess 拒否や PEB 読み取り失敗、WoW64（32bit）プロセスは main 扱いにフォールバックする。
struct ClaudeSessionCount {
    int main_count = 0;
    int sub_count  = 0;
};
ClaudeSessionCount count_claude_sessions_split(const std::wstring& sub_config_dir);
