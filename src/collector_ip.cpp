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
// タイムアウトは全フェーズ 3000ms に設定する。
// cancel が true になると読み取りループを早期中断する（shutdown() の待機を有界にする）。
static std::string fetch_ip_body(const std::atomic<bool>& cancel) {
    HINTERNET session = WinHttpOpen(L"sysmeters/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!session) return {};

    HINTERNET conn = WinHttpConnect(session, L"checkip.amazonaws.com",
                                    INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn) { WinHttpCloseHandle(session); return {}; }

    HINTERNET req = WinHttpOpenRequest(conn, L"GET", L"/",
        nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE);
    if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(session); return {}; }

    // 全フェーズのタイムアウトを設定（失敗してもログを残して続行する）
    DWORD timeout_ms = 3000;
    auto set_timeout = [req, &timeout_ms](DWORD option) {
        if (!WinHttpSetOption(req, option, &timeout_ms, sizeof(timeout_ms)))
            log_error("IP fetch: WinHttpSetOption(%lu) failed (err=%lu)", option, GetLastError());
    };
    set_timeout(WINHTTP_OPTION_RESOLVE_TIMEOUT);
    set_timeout(WINHTTP_OPTION_CONNECT_TIMEOUT);
    set_timeout(WINHTTP_OPTION_SEND_TIMEOUT);
    set_timeout(WINHTTP_OPTION_RECEIVE_TIMEOUT);

    // IP アドレス文字列用途のため 4KB で十分
    static constexpr size_t MAX_RESP_BYTES = 4 * 1024;

    std::string body;
    if (WinHttpSendRequest(req, nullptr, 0, nullptr, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
        // 受信タイムアウトは部分受信 1 回ごとに適用され合計時間に上限がないため、
        // 総時間デッドラインと cancel チェックでループを有界にする
        const ULONGLONG deadline = GetTickCount64() + 30000;
        DWORD size = 0;
        do {
            if (cancel.load()) { body.clear(); break; }
            if (GetTickCount64() > deadline) {
                log_error("IP fetch: read deadline exceeded");
                body.clear();
                break;
            }
            if (!WinHttpQueryDataAvailable(req, &size)) break;
            if (size == 0) break;
            if (body.size() + size > MAX_RESP_BYTES) {
                log_error("IP response too large");
                body.clear();
                break;
            }
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
    std::string body = fetch_ip_body(shutting_down_);

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
}

DWORD WINAPI IpCollector::fetch_thread(LPVOID param) {
    auto* self = reinterpret_cast<IpCollector*>(param);
    self->do_fetch();
    self->fetching_.store(false);  // do_fetch 完了後の最終命令として実行
    return 0;
}

void IpCollector::init(HWND notify_wnd) {
    notify_wnd_.store(notify_wnd);
    // 登録失敗時も 5 分周期タイマーで追従できるため動作は継続する
    DWORD err = NotifyIpInterfaceChange(AF_UNSPEC, on_ip_change, this, FALSE, &notify_handle_);
    if (err != NO_ERROR) log_error("NotifyIpInterfaceChange failed (err=%lu)", err);
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
        // 前回スレッドのハンドルクローズ
        // fetching_.store(false) はスレッド関数の最終命令のため、CAS 成功時点でスレッドはほぼ終了済みである
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
    // 取得スレッドの読み取りループを早期中断させる
    shutting_down_.store(true);

    // PostMessage が破棄済み HWND に到達しないよう先に nullptr にする
    notify_wnd_.store(nullptr);

    // 通知登録を解除してからスレッド完了を待つ
    // CancelMibChangeNotify2 は実行中コールバックの完了をブロック待機するため、
    // 戻った後は新規コールバック呼び出しが来ないことが保証される
    if (notify_handle_) {
        CancelMibChangeNotify2(notify_handle_);
        notify_handle_ = nullptr;
    }
    // スレッドの完了を待つ
    // shutting_down_ により読み取りループはブロッキング呼び出し 1 回分（最大 3000ms）で
    // 中断するため、接続系フェーズ 3000ms × 4 + 余裕の 15 秒で十分
    if (fetch_thread_) {
        DWORD wr = WaitForSingleObject(fetch_thread_, 15000);
        if (wr != WAIT_OBJECT_0) {
            log_error("IpCollector::shutdown fetch_thread did not exit (wait=%lu)", wr);
        }
        CloseHandle(fetch_thread_);
        fetch_thread_ = nullptr;
    }
}
