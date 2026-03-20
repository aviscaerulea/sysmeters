// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#pragma once
#include "metrics.hpp"
#include <windows.h>
#include <atomic>
#include <mutex>

// Claude Code レートリミット情報とセッション数の収集
//
// - セッション数：CreateToolhelp32Snapshot でプロセス列挙（メインスレッド）
// - レートリミット：WinHTTP でバックグラウンドスレッドから非同期取得
//   - ~/.claude/.credentials.json から OAuth トークン取得
//   - Anthropic Usage API / Account API を呼び出し
//   - $TEMP に JSON キャッシュを保存（Usage 360秒、Plan 3600秒）
class ClaudeCollector {
public:
    // HWND はバックグラウンドスレッド完了時に WM_CLAUDE_DONE を投げる先
    void init(HWND notify_wnd);

    // 1秒ごとに呼び出す（セッション数取得 + 非同期 API トリガー）
    void update(ClaudeMetrics& out);

    // バックグラウンドスレッドから呼ばれる（WM_CLAUDE_DONE で通知後に呼ぶ）
    void apply_result(ClaudeMetrics& out);

    void shutdown();
    ~ClaudeCollector() { shutdown(); }

private:
    std::atomic<HWND> notify_wnd_ = nullptr;
    std::atomic<bool> fetching_ = false;
    HANDLE            fetch_thread_ = nullptr;
    std::mutex        result_mutex_;
    bool              first_fetch_ = true;   // 初回フェッチフラグ（ネガティブキャッシュ無視に使用）

    // バックグラウンドで取得した結果（仮置き）
    ClaudeMetrics pending_{};

    static DWORD WINAPI fetch_thread(LPVOID param);
    void do_fetch();

    int count_claude_sessions();
};
