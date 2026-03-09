// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#include "collector_ip.hpp"
#include "resource.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <string>
#include <algorithm>

// checkip.amazonaws.com から IP 文字列を取得する
//
// 成功時はレスポンスボディ（IP アドレス文字列）を返す。失敗時は空文字列。
// タイムアウトは shutdown() の最大待機（4 秒）より短い 3000ms に設定する。
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

    // 全フェーズのタイムアウトを設定（shutdown() の最大待機 8 秒に収まるようにする）
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
            WinHttpQueryDataAvailable(req, &size);
            if (size == 0) break;
            std::string chunk(size, '\0');
            DWORD read = 0;
            WinHttpReadData(req, chunk.data(), size, &read);
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

    // 前後の空白（改行含む）を除去
    while (!body.empty() && std::isspace(static_cast<unsigned char>(body.front()))) body.erase(body.begin());
    while (!body.empty() && std::isspace(static_cast<unsigned char>(body.back())))  body.pop_back();

    // IPv4/IPv6 の文字種チェック（数字、a-f、ドット、コロンのみ許可）
    auto is_valid_ip = [](const std::string& s) {
        return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) {
            return std::isxdigit(c) || c == '.' || c == ':';
        });
    };

    {
        std::lock_guard<std::mutex> lock(result_mutex_);
        if (is_valid_ip(body) && body.size() < std::size(pending_ip_)) {
            MultiByteToWideChar(CP_UTF8, 0, body.c_str(), -1,
                                pending_ip_, static_cast<int>(std::size(pending_ip_)));
            pending_avail_ = true;
        }
        else {
            pending_ip_[0] = L'\0';
            pending_avail_ = false;
        }
    }

    PostMessage(notify_wnd_, WM_IP_DONE, 0, 0);
    fetching_.store(false);
}

DWORD WINAPI IpCollector::fetch_thread(LPVOID param) {
    reinterpret_cast<IpCollector*>(param)->do_fetch();
    return 0;
}

void IpCollector::init(HWND notify_wnd) {
    notify_wnd_ = notify_wnd;
}

void IpCollector::update() {
    if (!fetching_.load()) {
        fetching_.store(true);
        HANDLE h = CreateThread(nullptr, 0, fetch_thread, this, 0, nullptr);
        if (h) CloseHandle(h);
        else   fetching_.store(false);
    }
}

void IpCollector::apply_result(NetMetrics& out) {
    std::lock_guard<std::mutex> lock(result_mutex_);
    wmemcpy(out.global_ip, pending_ip_, std::size(out.global_ip));
    out.ip_avail = pending_avail_;
}

void IpCollector::shutdown() {
    // スレッドの完了を待つ（最大 15 秒：タイムアウト 3000ms × 4 フェーズ + 余裕）
    for (int i = 0; i < 150 && fetching_.load(); ++i) Sleep(100);
}
