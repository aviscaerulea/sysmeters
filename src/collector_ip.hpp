// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#pragma once
#include "metrics.hpp"
// netioapi.h（iphlpapi.h 経由）が ws2def.h/ws2ipdef.h を事前要求するため
// WIN32_LEAN_AND_MEAN で winsock.h を除外した上で winsock2.h を先行インクルードする
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
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
    std::atomic<HWND> notify_wnd_{nullptr};  // shutdown 後の PostMessage を防ぐため atomic
    std::atomic<bool> fetching_ = false;
    std::mutex result_mutex_;
    HANDLE notify_handle_       = nullptr;  // NotifyIpInterfaceChange の登録ハンドル

    // バックグラウンドで取得した結果（仮置き）
    wchar_t pending_ip_[48]  = {};
    bool    pending_avail_   = false;

    static DWORD WINAPI fetch_thread(LPVOID param);
    void do_fetch();

    // IP インタフェース変化コールバック（スレッドプール上で呼ばれる）
    static void WINAPI on_ip_change(PVOID context,
        PMIB_IPINTERFACE_ROW row, MIB_NOTIFICATION_TYPE type);
};
