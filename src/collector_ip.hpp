// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#pragma once
#include "metrics.hpp"
#include <windows.h>
#include <atomic>
#include <mutex>

// グローバル IP アドレスの収集
//
// checkip.amazonaws.com への HTTPS GET でグローバル IP を取得する。
// WinHTTP バックグラウンドスレッドで非同期取得し、完了時に WM_IP_DONE を通知する。
// 取得間隔は呼び出し元タイマー（5 分）で制御する。
class IpCollector {
public:
    // HWND はバックグラウンドスレッド完了時に WM_IP_DONE を投げる先
    void init(HWND notify_wnd);

    // 非同期 IP 取得を開始する（fetching_ が true の間は再起動しない）
    void update();

    // WM_IP_DONE 受信時にメインスレッドで呼ぶ
    void apply_result(NetMetrics& out);

    void shutdown();
    ~IpCollector() { shutdown(); }

private:
    HWND notify_wnd_           = nullptr;
    std::atomic<bool> fetching_ = false;
    std::mutex result_mutex_;

    // バックグラウンドで取得した結果（仮置き）
    wchar_t pending_ip_[48]  = {};
    bool    pending_avail_   = false;

    static DWORD WINAPI fetch_thread(LPVOID param);
    void do_fetch();
};
