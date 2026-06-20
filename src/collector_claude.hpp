// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#pragma once
#include "metrics.hpp"
#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>

// Claude Code レートリミット情報とセッション数の収集
//
// - セッション数：CreateToolhelp32Snapshot でプロセス列挙（メインスレッド）
// - レートリミット：WinHTTP でバックグラウンドスレッドから非同期取得
//   - ~/.claude/.credentials.json から OAuth トークン取得
//   - Anthropic Usage API / Account API を呼び出し
//   - $TEMP に JSON キャッシュを保存（Usage: usage_interval_sec 秒、Plan: 3600秒）
class ClaudeCollector {
public:
    // HWND はバックグラウンドスレッド完了時に WM_CLAUDE_DONE を投げる先
    void init(HWND notify_wnd, int usage_interval_sec = 120,
              bool nudge_enable = false, const std::string& nudge_cmd = {});

    // 1秒ごとに呼び出す（セッション数取得 + 非同期 API トリガー）
    void update(ClaudeMetrics& out);

    // 取得結果を out へ反映する（WM_CLAUDE_DONE 受信後、メインスレッドから呼ぶ）
    void apply_result(ClaudeMetrics& out);

    void shutdown();
    ~ClaudeCollector() { shutdown(); }

private:
    std::atomic<HWND>  notify_wnd_ = nullptr;
    double             usage_ttl_  = 120.0;    // Usage API のキャッシュ TTL（秒）
    bool               nudge_enable_ = false;  // 5h リセット後に claude.exe を起動するか
    std::string        nudge_cmd_;             // 起動コマンドライン
    time_t             last_nudge_resets_ts_ = -1;  // 前回 nudge 実行時の 5h resets_ts（重複抑制）
    std::atomic<bool>  fetching_   = false;
    std::atomic<bool>  shutdown_   = false;    // shutdown 要求フラグ（fetch スレッドの早期中断用）
    HANDLE             fetch_thread_ = nullptr;
    std::mutex         result_mutex_;
    bool               first_fetch_ = true;   // 初回フェッチフラグ（ネガティブキャッシュ無視に使用）

    // バックグラウンドで取得した結果（仮置き）
    ClaudeMetrics pending_{};

    static DWORD WINAPI fetch_thread(LPVOID param);
    void do_fetch();

    int count_claude_sessions();
};
