// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#include "collector_claude.hpp"
#include "resource.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <tlhelp32.h>
#include <shlobj.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

#include <json.hpp>
#include <string>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstring>

namespace fs = std::filesystem;
using json = nlohmann::json;

// キャッシュファイルのパス
static fs::path cache_usage_path() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    return fs::path(tmp) / L"claude-usage-cache.json";
}

static fs::path cache_plan_path() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    return fs::path(tmp) / L"claude-plan-cache.json";
}

// credentials.json から OAuth トークンを取得する
static std::string get_token() {
    wchar_t home[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, home);
    fs::path creds = fs::path(home) / L".claude" / L".credentials.json";

    std::ifstream ifs(creds);
    if (!ifs.is_open()) return {};
    try {
        auto j = json::parse(ifs);
        return j["claudeAiOauth"]["accessToken"].get<std::string>();
    }
    catch (...) { return {}; }
}

// UNIX タイムスタンプを取得する
static double now_ts() {
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

// API 失敗時のネガティブキャッシュ有効期間（秒）
static constexpr double NEGATIVE_TTL = 60.0;

// キャッシュ JSON を読む。TTL 内なら内容を返す。期限切れなら null。
// エラーキャッシュ（"error" フィールドあり）は NEGATIVE_TTL で判定する。
static json read_cache(const fs::path& path, double ttl) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return nullptr;
    try {
        auto j = json::parse(ifs);
        double ts = j.value("_ts", 0.0);
        double effective_ttl = j.contains("error") ? NEGATIVE_TTL : ttl;
        if (now_ts() - ts < effective_ttl) return j;
    }
    catch (...) {}
    return nullptr;
}

// WinHTTP でシンプルな GET リクエストを発行し、レスポンスボディを返す
static std::string http_get(const std::wstring& host, const std::wstring& path,
                            const std::string& token, const std::string& beta_header) {
    HINTERNET session = WinHttpOpen(L"sysmeters/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!session) return {};

    HINTERNET conn = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn) { WinHttpCloseHandle(session); return {}; }

    HINTERNET req = WinHttpOpenRequest(conn, L"GET", path.c_str(),
        nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE);
    if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(session); return {}; }

    // タイムアウト設定（shutdown() の 2 秒待機より短くしてスレッドが確実に完了できるようにする）
    DWORD timeout_ms = 1500;
    WinHttpSetOption(req, WINHTTP_OPTION_SEND_TIMEOUT,    &timeout_ms, sizeof(timeout_ms));
    WinHttpSetOption(req, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout_ms, sizeof(timeout_ms));

    // ヘッダー追加
    std::wstring auth = L"Authorization: Bearer " + std::wstring(token.begin(), token.end());
    WinHttpAddRequestHeaders(req, auth.c_str(), -1L, WINHTTP_ADDREQ_FLAG_ADD);

    std::wstring beta = L"anthropic-beta: " + std::wstring(beta_header.begin(), beta_header.end());
    WinHttpAddRequestHeaders(req, beta.c_str(), -1L, WINHTTP_ADDREQ_FLAG_ADD);

    WinHttpAddRequestHeaders(req, L"Accept: application/json", -1L, WINHTTP_ADDREQ_FLAG_ADD);

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

// ISO 8601 UTC 日時文字列（例: "2025-03-08T14:30:00Z"）を UTC time_t に変換する
//
// "YYYY-MM-DDTHH:MM:SS" までをパースし _mkgmtime で UTC time_t を返す。
// パース失敗または変換エラーの場合は -1 を返す。
static time_t parse_iso8601_utc(const std::string& iso) {
    if (iso.empty()) return -1;
    int year, mon, day, hour, min, sec;
    if (sscanf_s(iso.c_str(), "%d-%d-%dT%d:%d:%d", &year, &mon, &day, &hour, &min, &sec) < 6)
        return -1;
    struct tm utc_t{};
    utc_t.tm_year = year - 1900; utc_t.tm_mon = mon - 1; utc_t.tm_mday = day;
    utc_t.tm_hour = hour; utc_t.tm_min = min; utc_t.tm_sec = sec;
    return _mkgmtime(&utc_t);
}

// ISO 8601 UTC 日時文字列を JST に変換して表示文字列に変換する
//
// _mkgmtime で UTC time_t を求め +9h してから gmtime_s で JST broken-down time を得る。
// mktime（ローカル時刻解釈）を避けることで月またぎ・年またぎを正確に処理する。
// 曜日は wchar_t 出力で日本語を正しく扱う（char + %hs 変換による文字化けを防ぐ）。
static void format_reset_time(const std::string& iso, bool include_date, wchar_t* out, int out_len) {
    time_t utc_ts = parse_iso8601_utc(iso);
    if (utc_ts == -1) { swprintf_s(out, out_len, L"-"); return; }

    // UTC → JST (+9h)、月またぎ・年またぎも正確に処理
    time_t jst_ts = utc_ts + 9 * 3600;
    struct tm jst_t{};
    gmtime_s(&jst_t, &jst_ts);

    const wchar_t* wd[] = {L"日", L"月", L"火", L"水", L"木", L"金", L"土"};

    if (include_date) {
        swprintf_s(out, out_len, L"%d/%d %s %02d:%02d",
                   jst_t.tm_mon + 1, jst_t.tm_mday, wd[jst_t.tm_wday],
                   jst_t.tm_hour, jst_t.tm_min);
    }
    else {
        swprintf_s(out, out_len, L"%02d:%02d", jst_t.tm_hour, jst_t.tm_min);
    }
}

// 均等消費ペースの算出（resets_at ISO 文字列とウィンドウ秒数から計算）
//
// 現在時刻からリセット時刻までの残り秒数を求め、
// 経過割合（elapsed / window_secs）を 0〜100 にクランプして返す。
// パース失敗または残り時間がウィンドウを超える場合は 0 を返す（赤色表示しない安全側）。
static float calc_expected_pct(const std::string& iso, double window_secs) {
    time_t resets_ts = parse_iso8601_utc(iso);
    if (resets_ts == -1) return 0.f;

    double remaining = static_cast<double>(resets_ts) - now_ts();
    if (remaining < 0.0) remaining = 0.0;
    if (remaining > window_secs) return 0.f;  // ウィンドウ外はペース不定

    double elapsed = window_secs - remaining;
    float expected = static_cast<float>(elapsed / window_secs * 100.0);
    return std::clamp(expected, 0.f, 100.f);
}

static const char* BETA_HEADER = "oauth-2025-04-20";

// バックグラウンドで Usage API + Account API を叩く
void ClaudeCollector::do_fetch() {
    ClaudeMetrics result{};

    // --- Usage API ---
    json usage_j = read_cache(cache_usage_path(), 360.0);
    if (usage_j != nullptr && usage_j.contains("error")) {
        usage_j = nullptr;  // ネガティブキャッシュヒット → API スキップ
    }
    else if (usage_j == nullptr) {
        std::string token = get_token();
        if (!token.empty()) {
            std::string body = http_get(L"api.anthropic.com", L"/api/oauth/usage", token, BETA_HEADER);
            if (!body.empty()) {
                try {
                    usage_j = json::parse(body);
                    usage_j["_ts"] = now_ts();
                    std::ofstream ofs(cache_usage_path());
                    ofs << usage_j.dump();
                }
                catch (...) {}
            }
            else {
                // HTTP 失敗 → エラーキャッシュ保存
                try {
                    std::ofstream ofs(cache_usage_path());
                    ofs << json{{"error", true}, {"_ts", now_ts()}}.dump();
                }
                catch (...) {}
            }
        }
    }

    if (usage_j != nullptr) {
        try {
            auto fh = usage_j["five_hour"];
            auto sd = usage_j["seven_day"];
            // utilization は API から 0〜100 の % 値で返る
            result.five_h_pct  = static_cast<float>(fh.value("utilization", 0.0));
            result.seven_d_pct = static_cast<float>(sd.value("utilization", 0.0));

            std::string fh_resets_at = fh.value("resets_at", "");
            std::string sd_resets_at = sd.value("resets_at", "");
            format_reset_time(fh_resets_at, false, result.five_h_reset, _countof(result.five_h_reset));
            format_reset_time(sd_resets_at, true,  result.seven_d_reset, _countof(result.seven_d_reset));
            result.five_h_expected_pct  = calc_expected_pct(fh_resets_at, 5.0 * 3600);
            result.seven_d_expected_pct = calc_expected_pct(sd_resets_at, 7.0 * 24 * 3600);
            result.five_h_resets_ts  = parse_iso8601_utc(fh_resets_at);
            result.seven_d_resets_ts = parse_iso8601_utc(sd_resets_at);
            result.avail = true;
        }
        catch (...) {}
    }

    // --- Account API（プランラベル）---
    json plan_j = read_cache(cache_plan_path(), 3600.0);
    if (plan_j != nullptr && plan_j.contains("error")) {
        plan_j = nullptr;  // ネガティブキャッシュヒット → API スキップ
    }
    else if (plan_j == nullptr) {
        std::string token = get_token();
        if (!token.empty()) {
            std::string body = http_get(L"api.anthropic.com", L"/api/oauth/account", token, BETA_HEADER);
            if (!body.empty()) {
                try {
                    plan_j = json::parse(body);
                    // プランラベル抽出
                    std::string tier = plan_j["memberships"][0]["organization"]["rate_limit_tier"].get<std::string>();
                    std::string label;
                    if (tier.find("20x") != std::string::npos)     label = "Max20";
                    else if (tier.find("5x") != std::string::npos) label = "Max5";
                    else if (tier.find("max") != std::string::npos) label = "Max";
                    else if (tier.find("pro") != std::string::npos) label = "Pro";

                    plan_j = json{{"label", label}, {"_ts", now_ts()}};
                    std::ofstream ofs(cache_plan_path());
                    ofs << plan_j.dump();
                }
                catch (...) {}
            }
            else {
                // HTTP 失敗 → エラーキャッシュ保存
                try {
                    std::ofstream ofs(cache_plan_path());
                    ofs << json{{"error", true}, {"_ts", now_ts()}}.dump();
                }
                catch (...) {}
            }
        }
    }

    if (plan_j != nullptr) {
        try {
            std::string label = plan_j.value("label", "");
            strncpy_s(result.plan_label, sizeof(result.plan_label), label.c_str(), _TRUNCATE);
        }
        catch (...) {}
    }

    // 結果を pending_ に書き込み、メインスレッドに通知
    {
        std::lock_guard<std::mutex> lock(result_mutex_);
        pending_ = result;
    }
    if (notify_wnd_) PostMessage(notify_wnd_, WM_CLAUDE_DONE, 0, 0);
    fetching_.store(false);
}

DWORD WINAPI ClaudeCollector::fetch_thread(LPVOID param) {
    reinterpret_cast<ClaudeCollector*>(param)->do_fetch();
    return 0;
}

void ClaudeCollector::init(HWND notify_wnd) {
    notify_wnd_ = notify_wnd;
}

int ClaudeCollector::count_claude_sessions() {
    int count = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"claude.exe") == 0) ++count;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return count;
}

void ClaudeCollector::update(ClaudeMetrics& out) {
    out.session_count = count_claude_sessions();

    // バックグラウンドスレッドが走っていなければ新たに起動
    if (!fetching_.load()) {
        fetching_.store(true);
        HANDLE h = CreateThread(nullptr, 0, fetch_thread, this, 0, nullptr);
        if (h) CloseHandle(h);
        else   fetching_.store(false);  // 起動失敗時は次回タイマーで再試行できるようリセット
    }
}

void ClaudeCollector::apply_result(ClaudeMetrics& out) {
    std::lock_guard<std::mutex> lock(result_mutex_);
    int sessions = out.session_count;  // セッション数は別途更新しているので保持
    out = pending_;
    out.session_count = sessions;
}

void ClaudeCollector::shutdown() {
    // スレッドの完了を待つ（簡易版：最大 2 秒）
    for (int i = 0; i < 20 && fetching_.load(); ++i) Sleep(100);
}
