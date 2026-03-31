// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#include "collector_ip.hpp"
#include "logger.hpp"
#include "resource.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <string>
#include <algorithm>

// checkip.amazonaws.com から IP 文字列を取得する
//
// 成功時はレスポンスボディ（IP アドレス文字列）を返す。失敗時は空文字列。
// タイムアウトは全フェーズ 3000ms に設定する（shutdown() 最大待機 15 秒に収まる）。
static std::string fetch_ip_body() {
    HINTERNET session = WinHttpOpen(L"sysmeters/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!session) return {};

    HINTERNET conn = WinHttpConnect(session, L"checkip.amazonaws.com",
                                    INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn) { WinHttpCloseHandle(session); return {}; }

    HINTERNET req = WinHttpOpenRequest(conn, L"GET", L"/",
        nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE);
    if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(session); return {}; }

    // 全フェーズのタイムアウトを設定（shutdown() の最大待機 15 秒に収まるようにする）
    DWORD timeout_ms = 3000;
    WinHttpSetOption(req, WINHTTP_OPTION_RESOLVE_TIMEOUT, &timeout_ms, sizeof(timeout_ms));
    WinHttpSetOption(req, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout_ms, sizeof(timeout_ms));
    WinHttpSetOption(req, WINHTTP_OPTION_SEND_TIMEOUT,    &timeout_ms, sizeof(timeout_ms));
    WinHttpSetOption(req, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout_ms, sizeof(timeout_ms));

    std::string body;
    if (WinHttpSendRequest(req, nullptr, 0, nullptr, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
        DWORD size = 0;
        do {
            if (!WinHttpQueryDataAvailable(req, &size)) break;
            if (size == 0) break;
            std::string chunk(size, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(req, chunk.data(), size, &read)) break;
            body.append(chunk, 0, read);
        } while (size > 0);
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return body;
}

void IpCollector::do_fetch() {
    std::string body = fetch_ip_body();

    while (!body.empty() && std::isspace(static_cast<unsigned char>(body.front()))) body.erase(body.begin());
    while (!body.empty() && std::isspace(static_cast<unsigned char>(body.back())))  body.pop_back();

    auto is_valid_ip = [](const std::string& s) {
        if (s.empty()) return false;
        struct in_addr  a4;
        struct in6_addr a6;
        return inet_pton(AF_INET,  s.c_str(), &a4) == 1 ||
               inet_pton(AF_INET6, s.c_str(), &a6) == 1;
    };

    {
        std::lock_guard<std::mutex> lock(result_mutex_);
        // ASCII IP アドレスは byte == char のため size チェックで十分。
        // "<" で NUL 終端分のバッファを確保する
        if (is_valid_ip(body) && body.size() < std::size(pending_ip_)) {
            MultiByteToWideChar(CP_UTF8, 0, body.c_str(), -1,
                                pending_ip_, static_cast<int>(std::size(pending_ip_)));
            pending_avail_ = true;
        }
        else {
            log_error("IP fetch failed");
            pending_ip_[0] = L'\0';
            pending_avail_ = false;
        }
    }

    // shutdown 後に PostMessage が到達しないよう atomic で取得してからチェック
    if (HWND wnd = notify_wnd_.load()) PostMessage(wnd, WM_IP_DONE, 0, 0);
    fetching_.store(false);
}

DWORD WINAPI IpCollector::fetch_thread(LPVOID param) {
    reinterpret_cast<IpCollector*>(param)->do_fetch();
    return 0;
}

void IpCollector::init(HWND notify_wnd) {
    notify_wnd_.store(notify_wnd);
    NotifyIpInterfaceChange(AF_UNSPEC, on_ip_change, this, FALSE, &notify_handle_);
}

void WINAPI IpCollector::on_ip_change(PVOID context,
    PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE type) {
    // NIC 切断時はフェッチしても必ず失敗するためスキップする
    if (type == MibDeleteInstance) return;
    reinterpret_cast<IpCollector*>(context)->update();
}

void IpCollector::update() {
    bool expected = false;
    if (!fetching_.compare_exchange_strong(expected, true)) return;
    if (fetch_thread_) {
        // CAS 成功時点でフェッチ完了（fetching_.store(false)）は保証されているため安全にクローズできる
        CloseHandle(fetch_thread_);
        fetch_thread_ = nullptr;
    }
    fetch_thread_ = CreateThread(nullptr, 0, fetch_thread, this, 0, nullptr);
    if (!fetch_thread_) fetching_.store(false);
}

void IpCollector::apply_result(NetMetrics& out) {
    std::lock_guard<std::mutex> lock(result_mutex_);
    wmemcpy(out.global_ip, pending_ip_, std::size(out.global_ip));
    out.ip_avail = pending_avail_;
}

void IpCollector::shutdown() {
    // PostMessage が破棄済み HWND に到達しないよう先に nullptr にする
    notify_wnd_.store(nullptr);

    // 通知登録を解除してからスレッド完了を待つ
    // CancelMibChangeNotify2 は実行中コールバックの完了をブロック待機するため、
    // 戻った後は新規コールバック呼び出しが来ないことが保証される
    if (notify_handle_) {
        CancelMibChangeNotify2(notify_handle_);
        notify_handle_ = nullptr;
    }
    // スレッドの完了を待つ（最大 15 秒：タイムアウト 3000ms × 4 フェーズ + 余裕）
    if (fetch_thread_) {
        WaitForSingleObject(fetch_thread_, 15000);
        CloseHandle(fetch_thread_);
        fetch_thread_ = nullptr;
    }
}
