// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#include "update_check.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <json.hpp>
#include "logger.hpp"
#include <cwchar>
#include <cstdio>

using json = nlohmann::json;

// GitHub API の host とパス（最新正式版リリース。pre-release / draft を除外）
static constexpr wchar_t GH_HOST[] = L"api.github.com";
static constexpr wchar_t GH_PATH[] = L"/repos/aviscaerulea/sysmeters/releases/latest";

// バージョン文字列から MAJOR.MINOR.PATCH を抽出する
//
// 先頭の "v"/"V" を除去し、"-" 以降（"-dirty" 等のサフィックス）を切り捨ててからパースする。
// 3 要素が揃わなければ false を返す（"dev" 等のパース不能文字列を弾く）。
static bool parse_version(const std::wstring& ver, int& major, int& minor, int& patch) {
    std::wstring s = ver;
    if (!s.empty() && (s[0] == L'v' || s[0] == L'V')) s = s.substr(1);
    auto dash = s.find(L'-');
    if (dash != std::wstring::npos) s = s.substr(0, dash);

    int a = 0, b = 0, c = 0;
    if (swscanf_s(s.c_str(), L"%d.%d.%d", &a, &b, &c) != 3) return false;
    major = a; minor = b; patch = c;
    return true;
}

// latest が current より新しいバージョンなら true を返す
//
// major→minor→patch の順で比較する。どちらかパース失敗時は false（更新通知を出さない）。
static bool is_newer_version(const std::wstring& latest, const std::wstring& current) {
    int lMaj, lMin, lPat, cMaj, cMin, cPat;
    if (!parse_version(latest,  lMaj, lMin, lPat)) return false;
    if (!parse_version(current, cMaj, cMin, cPat)) return false;
    if (lMaj != cMaj) return lMaj > cMaj;
    if (lMin != cMin) return lMin > cMin;
    return lPat > cPat;
}

// GitHub API へ HTTPS GET を発行し、レスポンスボディ（UTF-8）を返す
//
// GitHub は User-Agent ヘッダ必須。認証は不要（公開リポジトリ）。
// 失敗時は空文字を返す。collector_claude の http_get と同等のタイムアウトを設定する。
static std::string github_get() {
    HINTERNET session = WinHttpOpen(L"sysmeters/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!session) return {};

    HINTERNET conn = WinHttpConnect(session, GH_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn) { WinHttpCloseHandle(session); return {}; }

    HINTERNET req = WinHttpOpenRequest(conn, L"GET", GH_PATH,
        nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE);
    if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(session); return {}; }

    DWORD timeout_ms = 1500;
    WinHttpSetOption(req, WINHTTP_OPTION_RESOLVE_TIMEOUT, &timeout_ms, sizeof(timeout_ms));
    WinHttpSetOption(req, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout_ms, sizeof(timeout_ms));
    WinHttpSetOption(req, WINHTTP_OPTION_SEND_TIMEOUT,    &timeout_ms, sizeof(timeout_ms));
    WinHttpSetOption(req, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout_ms, sizeof(timeout_ms));

    WinHttpAddRequestHeaders(req, L"Accept: application/vnd.github+json",
                             -1L, WINHTTP_ADDREQ_FLAG_ADD);

    std::string body;
    if (WinHttpSendRequest(req, nullptr, 0, nullptr, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
        DWORD status_code = 0;
        DWORD status_size = sizeof(status_code);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            nullptr, &status_code, &status_size, nullptr);
        if (status_code != 200) {
            char msg[48];
            snprintf(msg, sizeof(msg), "update check: HTTP %lu", status_code);
            log_error(msg);
        }
        else {
            // レスポンスボディ取得（1MB 上限）
            // 受信タイムアウトは部分受信 1 回ごとに適用され合計時間に上限がないため、
            // 終了処理の join がハングしないよう総時間デッドラインを設ける
            static constexpr size_t MAX_RESP_BYTES = 1 * 1024 * 1024;
            const ULONGLONG deadline = GetTickCount64() + 30000;
            DWORD size = 0;
            do {
                if (GetTickCount64() > deadline) {
                    log_error("update check: read deadline exceeded");
                    body.clear();
                    break;
                }
                if (!WinHttpQueryDataAvailable(req, &size)) break;
                if (size == 0) break;
                if (body.size() + size > MAX_RESP_BYTES) {
                    log_error("update check: response too large");
                    body.clear();
                    break;
                }
                std::string chunk(size, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(req, chunk.data(), size, &read)) break;
                body.append(chunk, 0, read);
            } while (size > 0);
        }
    }
    else {
        log_error("update check: request failed (offline?)");
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return body;
}

// UTF-8 文字列を wstring へ変換する（失敗時は空）
static std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

UpdateResult check_for_updates(const std::wstring& current_version) {
    UpdateResult result;

    std::string body = github_get();
    if (body.empty()) return result;

    std::wstring tag;
    try {
        json j = json::parse(body);
        tag = utf8_to_wide(j.value("tag_name", std::string{}));
    }
    catch (...) {
        log_error("update check: JSON parse failed");
        return result;
    }
    if (tag.empty()) return result;

    if (is_newer_version(tag, current_version)) {
        result.available  = true;
        result.latest_tag = tag;
    }
    return result;
}
