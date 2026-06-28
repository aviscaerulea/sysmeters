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
#include "logger.hpp"
#include <string>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstring>
#include <cwchar>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

// credentials.json から OAuth トークンを取得する
// creds_path_ はインスタンス固有（アカウント別）。空のときはトークン取得を諦める
std::string ClaudeCollector::get_token() {
    if (creds_path_.empty()) return {};
    std::ifstream ifs(creds_path_);
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
//
// 失敗時は空文字を返す。out_status が非 null なら HTTP ステータスコードを書き込む。
// cancel が非 null の場合、読み取りループの各周回でフラグを確認し、立っていれば即中断する。
// 各 WinHTTP 呼び出しはタイムアウトで有界のため、関数全体の所要時間も有界になる。
static std::string http_get(const std::wstring& host, const std::wstring& path,
                            const std::string& token, const std::string& beta_header,
                            int* out_status = nullptr,
                            const std::atomic<bool>* cancel = nullptr) {
    HINTERNET session = WinHttpOpen(L"sysmeters/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!session) return {};

    HINTERNET conn = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn) { WinHttpCloseHandle(session); return {}; }

    HINTERNET req = WinHttpOpenRequest(conn, L"GET", path.c_str(),
        nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE);
    if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(session); return {}; }

    // 全フェーズのタイムアウトを設定（各 API 呼び出しのブロック時間を 1500ms に有界化する）
    constexpr int timeout_ms = 1500;
    if (!WinHttpSetTimeouts(req, timeout_ms, timeout_ms, timeout_ms, timeout_ms))
        log_error("WinHttpSetTimeouts failed (err=%lu)", GetLastError());

    std::wstring auth = L"Authorization: Bearer " + std::wstring(token.begin(), token.end());
    WinHttpAddRequestHeaders(req, auth.c_str(), -1L, WINHTTP_ADDREQ_FLAG_ADD);

    std::wstring beta = L"anthropic-beta: " + std::wstring(beta_header.begin(), beta_header.end());
    WinHttpAddRequestHeaders(req, beta.c_str(), -1L, WINHTTP_ADDREQ_FLAG_ADD);

    WinHttpAddRequestHeaders(req, L"Accept: application/json", -1L, WINHTTP_ADDREQ_FLAG_ADD);

    std::string body;
    if (WinHttpSendRequest(req, nullptr, 0, nullptr, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
        DWORD status_code = 0;
        DWORD status_size = sizeof(status_code);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            nullptr, &status_code, &status_size, nullptr);
        if (out_status) *out_status = static_cast<int>(status_code);
        if (status_code != 200) {
            if (status_code == 401)      log_error("HTTP 401 unauthorized");
            else if (status_code == 429) log_error("HTTP 429 too many requests");
            else {
                char msg[32];
                snprintf(msg, sizeof(msg), "HTTP %lu", status_code);
                log_error(msg);
            }
        }
        else {
            // レスポンスボディ取得（1MB 上限）
            // 各周回で cancel フラグと総時間デッドラインを確認し、
            // サーバが小刻みに送り続けるケースでもループ全体を有界にする
            static constexpr size_t MAX_RESP_BYTES = 1 * 1024 * 1024;
            static constexpr ULONGLONG READ_DEADLINE_MS = 30000;
            const ULONGLONG read_start = GetTickCount64();
            DWORD size = 0;
            do {
                if (cancel && cancel->load()) { body.clear(); break; }
                if (GetTickCount64() - read_start > READ_DEADLINE_MS) {
                    log_error("claude response read deadline exceeded");
                    body.clear();
                    break;
                }
                if (!WinHttpQueryDataAvailable(req, &size)) break;
                if (size == 0) break;
                if (body.size() + size > MAX_RESP_BYTES) {
                    log_error("claude response too large");
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
    // パース値の妥当性チェック（_mkgmtime の暗黙補正と年フィールドのオーバーフローを防ぐ）
    if (year < 1970 || year > 2200 ||
        mon < 1 || mon > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 60)
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

// TTL 無視で前回キャッシュの内容を返す
//
// 起動直後の API 取得完了までの空白を埋めるため、期限切れキャッシュからでも
// 前回値を暫定表示用に取り出す。エラーキャッシュ（"error" フィールドあり）は無効として null を返す。
static json read_cache_raw(const fs::path& path) {
    if (path.empty()) return nullptr;
    std::ifstream ifs(path);
    if (!ifs.is_open()) return nullptr;
    try {
        auto j = json::parse(ifs);
        if (j.contains("error")) return nullptr;
        return j;
    }
    catch (...) {}
    return nullptr;
}

// Usage API レスポンス JSON を ClaudeMetrics に反映する
//
// do_fetch（API/キャッシュ経由）と init（前回キャッシュ復元）で共有する。
// usage_j が null の場合は何もしない。成功時のみ result.avail を true にする。
static void apply_usage_json(const json& usage_j, ClaudeMetrics& result) {
    if (usage_j == nullptr) return;
    try {
        // 非 const operator[] は存在しないキーを自動挿入するため、value() で副作用なく読む
        const json empty_obj = json::object();
        const json& fh = usage_j.contains("five_hour") ? usage_j.at("five_hour") : empty_obj;
        const json& sd = usage_j.contains("seven_day") ? usage_j.at("seven_day") : empty_obj;
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

        // 超過料金情報（extra_usage）
        if (usage_j.contains("extra_usage") && usage_j.at("extra_usage").is_object()) {
            const json& eu = usage_j.at("extra_usage");
            result.extra_enabled      = eu.value("is_enabled", false);
            result.extra_used_dollars = static_cast<float>(eu.value("used_credits", 0.0)) / 100.f;
        }
    }
    catch (const nlohmann::json::exception& e) { log_error("%s", e.what()); }
}

// Plan API キャッシュ JSON（label 化済み）を ClaudeMetrics に反映する
//
// do_fetch（label 変換後）と init（キャッシュ復元）で共有する。
// plan_j は {label, _ts} 形式を前提（生の memberships 形式ではない）。
static void apply_plan_json(const json& plan_j, ClaudeMetrics& result) {
    if (plan_j == nullptr) return;
    try {
        std::string label = plan_j.value("label", "");
        strncpy_s(result.plan_label, sizeof(result.plan_label), label.c_str(), _TRUNCATE);
    }
    catch (const nlohmann::json::exception& e) { log_error("%s", e.what()); }
}

// Anthropic API beta ヘッダの値（OAuth エンドポイント有効化用）。API バージョン更新時は要変更。
static const char* BETA_HEADER = "oauth-2025-04-20";

// キャッシュヒット or API 取得を行うヘルパー
//
// キャッシュ有効期間内ならキャッシュを返す。ネガティブキャッシュ（"error" フィールドあり）は nullptr。
// キャッシュなし・期限切れなら HTTP GET を実行し、失敗時はエラーキャッシュを保存して nullptr を返す。
// 戻り値に "_ts" が含まれていればキャッシュヒット、含まれなければ新規取得を示す。
static json fetch_or_cache(const fs::path& cache_path, double ttl,
                            const std::wstring& api_path, const std::string& token,
                            int* out_status = nullptr,
                            const std::atomic<bool>* cancel = nullptr) {
    json j = read_cache(cache_path, ttl);
    if (j != nullptr) {
        if (j.contains("error")) return nullptr;  // ネガティブキャッシュ
        return j;
    }
    if (token.empty()) return nullptr;

    std::string body = http_get(L"api.anthropic.com", api_path, token, BETA_HEADER, out_status, cancel);
    if (body.empty()) {
        try {
            std::ofstream ofs(cache_path);
            ofs << json{{"error", true}, {"_ts", now_ts()}}.dump();
        }
        catch (...) {}
        return nullptr;
    }
    try {
        return json::parse(body);
    }
    catch (const nlohmann::json::exception& e) {
        log_error("%s", e.what());
        return nullptr;
    }
}

// ネガティブキャッシュファイルを削除する（"error" フィールドが含まれている場合のみ）
static void clear_negative_cache(const fs::path& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return;
    try {
        auto j = json::parse(ifs);
        ifs.close();
        if (j.contains("error")) fs::remove(path);
    }
    catch (...) {}
}

// 5h リセット後 nudge：claude.exe を環境変数で構成して起動する
//
// メイン（config_dir_ 空）は親プロセス環境を継承して起動する。
// サブ（config_dir_ 非空）は親プロセス環境 + CLAUDE_CONFIG_DIR=<config_dir_> を加えた一時環境で
// 起動する。Claude CLI には --config-dir コマンドオプションが存在せず、設定ディレクトリの上書きは
// CLAUDE_CONFIG_DIR 環境変数経由でのみ可能なため。sysmeters 自身のプロセス親環境は変更せず、
// CreateProcess の lpEnvironment で子プロセスにだけ設定する。
void ClaudeCollector::run_nudge() {
    log_info("claude nudge: 5h reset is past, running (account=%d): %s",
             account_index_, nudge_cmd_.c_str());

    // nudge_cmd_（UTF-8）を Wide へ変換する。CreateProcessW の第 2 引数は書き換え可能バッファ
    int wlen = MultiByteToWideChar(CP_UTF8, 0, nudge_cmd_.c_str(), -1, nullptr, 0);
    if (wlen <= 0) {
        log_error("claude nudge: MultiByteToWideChar failed (err=%lu)", GetLastError());
        return;
    }
    std::vector<wchar_t> wcmd(static_cast<size_t>(wlen));
    MultiByteToWideChar(CP_UTF8, 0, nudge_cmd_.c_str(), -1, wcmd.data(), wlen);

    // サブの場合は親環境変数 + CLAUDE_CONFIG_DIR を含む環境ブロックを構築する
    std::vector<wchar_t> env_block;
    LPVOID lp_env = nullptr;
    DWORD flags = CREATE_NO_WINDOW;
    if (!config_dir_.empty()) {
        LPWCH parent = GetEnvironmentStringsW();
        if (!parent) {
            log_error("claude nudge: GetEnvironmentStringsW failed (err=%lu)", GetLastError());
            return;
        }
        const std::wstring key_eq = L"CLAUDE_CONFIG_DIR=";
        for (const wchar_t* p = parent; *p; p += wcslen(p) + 1) {
            size_t entry_len = wcslen(p);
            // 既存の CLAUDE_CONFIG_DIR は除外し、自前で追加する
            if (entry_len >= key_eq.size() &&
                _wcsnicmp(p, key_eq.c_str(), key_eq.size()) == 0) {
                continue;
            }
            env_block.insert(env_block.end(), p, p + entry_len);
            env_block.push_back(L'\0');
        }
        FreeEnvironmentStringsW(parent);

        env_block.insert(env_block.end(), key_eq.begin(), key_eq.end());
        env_block.insert(env_block.end(), config_dir_.begin(), config_dir_.end());
        env_block.push_back(L'\0');
        env_block.push_back(L'\0');  // 環境ブロックは二重 NUL 終端
        lp_env = env_block.data();
        flags |= CREATE_UNICODE_ENVIRONMENT;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    if (CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE,
                       flags, lp_env, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    else {
        log_error("claude nudge: CreateProcess failed (err=%lu)", GetLastError());
    }
}

// バックグラウンドで Usage API + Account API を叩く
void ClaudeCollector::do_fetch() {
    // 初回フェッチ時はネガティブキャッシュを削除して必ず API を叩く
    if (first_fetch_) {
        clear_negative_cache(cache_usage_path_);
        clear_negative_cache(cache_plan_path_);
        first_fetch_ = false;
    }

    // API エラーやキャッシュ無効期間中も前回の有効データを保持する
    ClaudeMetrics result;
    { std::lock_guard<std::mutex> lock(result_mutex_); result = pending_; }

    // 毎時 0 分はキャッシュを削除して最新データを強制取得する
    time_t now_t = static_cast<time_t>(now_ts());
    struct tm local_t{};
    localtime_s(&local_t, &now_t);
    if (local_t.tm_min == 0) {
        std::error_code ec;
        fs::remove(cache_usage_path_, ec);
        fs::remove(cache_plan_path_, ec);
    }

    std::string token = get_token();

    // --- Usage API ---
    int usage_status = 0;
    json usage_j = fetch_or_cache(cache_usage_path_, usage_ttl_, L"/api/oauth/usage", token, &usage_status, &shutdown_);
    if (usage_j == nullptr && !token.empty()) {
        log_error("Claude Usage API failed");
        // 401 は認証切れ → 次回フェッチ時にネガティブキャッシュを削除して即再取得させる
        if (usage_status == 401) first_fetch_ = true;
    }
    if (usage_j != nullptr && !usage_j.contains("_ts")) {
        // 新規 API 取得 → タイムスタンプを付与してキャッシュ保存
        try {
            usage_j["_ts"] = now_ts();
            std::ofstream ofs(cache_usage_path_);
            ofs << usage_j.dump();
        }
        catch (const nlohmann::json::exception& e) { log_error("%s", e.what()); }
    }

    apply_usage_json(usage_j, result);
    result.fetch_error = (usage_j == nullptr);

    // --- 5h リセット後の nudge（claude.exe 起動による使用状況の更新促進）---
    // 初回フェッチでは現在値を記録するのみ（起動直後の意図しない発火を防ぐ）。
    // 2 回目以降のフェッチでリセット日時が変わった（= 新しいウィンドウに入った）とき発火する。
    if (nudge_enable_ && usage_j != nullptr && result.avail && result.five_h_resets_ts > 0) {
        time_t now = static_cast<time_t>(now_ts());
        if (result.five_h_resets_ts < now &&
            result.five_h_resets_ts != last_nudge_resets_ts_) {
            bool should_run = (last_nudge_resets_ts_ != -1);
            last_nudge_resets_ts_ = result.five_h_resets_ts;
            if (should_run) {
                run_nudge();
            }
        }
    }

    // --- Account API（プランラベル）---
    // shutdown 要求時は次のリクエストへ進まず早期終了する
    if (shutdown_.load()) { fetching_.store(false); return; }

    json plan_j = fetch_or_cache(cache_plan_path_, 3600.0, L"/api/oauth/account", token, nullptr, &shutdown_);
    if (plan_j != nullptr && !plan_j.contains("_ts")) {
        // 新規 API 取得 → tier→label 変換してキャッシュ保存
        try {
            auto& ms = plan_j["memberships"];
            if (!ms.is_array() || ms.empty()) {
                log_error("Claude Plan API: no memberships");
                plan_j = nullptr;
            }
            else {
                // memberships を全走査して最も高位の Claude Code 対応プランを選ぶ。
                // 背景：Team 契約者は memberships[0] が個人 Organization（tier=default_claude_ai）で、
                //       Team 組織は memberships[1] 以降に入る（実調査の結果。例： [1] name="Carecom" tier="default_raven"
                //       capabilities=["chat","raven"]）。Anthropic Console API 用組織が紛れる場合もあり、
                //       capabilities が "chat" を含まない組織は Claude Code では使えないため評価対象から外す。
                // 優先順位：Max20 > Max5 > Max > Team > Pro > Free。（Anthropic の usage 枠の値付けに準拠）
                auto eval_org = [](const json& org) -> std::pair<int, std::string> {
                    if (!org.is_object()) return {-1, ""};
                    bool has_chat = false, has_raven = false, has_claude_max = false;
                    if (org.contains("capabilities") && org["capabilities"].is_array()) {
                        for (const auto& c : org["capabilities"]) {
                            if (!c.is_string()) continue;
                            const std::string s = c.get<std::string>();
                            if (s == "chat")        has_chat = true;
                            else if (s == "raven")  has_raven = true;
                            else if (s == "claude_max") has_claude_max = true;
                        }
                    }
                    if (!has_chat) return {-1, ""};
                    std::string tier;
                    if (org.contains("rate_limit_tier") && org["rate_limit_tier"].is_string())
                        tier = org["rate_limit_tier"].get<std::string>();
                    if (has_claude_max || tier.find("max") != std::string::npos) {
                        if (tier.find("20x") != std::string::npos) return {5, "Max20"};
                        if (tier.find("5x")  != std::string::npos) return {4, "Max5"};
                        return {3, "Max"};
                    }
                    if (has_raven || tier == "default_raven") return {2, "Team"};
                    if (tier.find("pro") != std::string::npos) return {1, "Pro"};
                    if (tier == "default_claude_ai")          return {0, "Free"};
                    // 既知 tier 判定をすべて通過した想定外の値を吸収する。将来追加プラン、Enterprise 等の
                    // 特殊枠、API レスポンス形式の変化が来た時の保険。生 tier 名を露出させても利用者に意味が
                    // 伝わらないため "unknown" に統一する。rank=0 は Free と同列だが、走査側は `>` 比較で
                    // 上書きするため、既知プラン（Free 含む）が同時に存在すればそちらが優先される。
                    return {0, "unknown"};
                };
                int best_rank = -1;
                std::string label = "unknown";
                for (const auto& m : ms) {
                    auto [rank, l] = eval_org(m.value("organization", json::object()));
                    if (rank > best_rank) { best_rank = rank; label = l; }
                }

                plan_j = json{{"label", label}, {"_ts", now_ts()}};
                std::ofstream ofs(cache_plan_path_);
                ofs << plan_j.dump();
            }
        }
        catch (const nlohmann::json::exception& e) {
            log_error("%s", e.what());
            plan_j = nullptr;
        }
    }

    apply_plan_json(plan_j, result);

    {
        std::lock_guard<std::mutex> lock(result_mutex_);
        pending_ = result;
    }
    // wParam にアカウント識別子を載せる（0=Main, 1=Sub）。受信側は wParam で apply_result の振り分けを行う
    if (HWND wnd = notify_wnd_.load()) PostMessage(wnd, WM_CLAUDE_DONE, static_cast<WPARAM>(account_index_), 0);
    fetching_.store(false);
}

DWORD WINAPI ClaudeCollector::fetch_thread(LPVOID param) {
    reinterpret_cast<ClaudeCollector*>(param)->do_fetch();
    return 0;
}

void ClaudeCollector::init(HWND notify_wnd, int account_index,
                           const std::wstring& config_dir, const std::string& cache_suffix,
                           int usage_interval_sec,
                           bool nudge_enable, const std::string& nudge_cmd) {
    notify_wnd_.store(notify_wnd);
    account_index_ = account_index;
    usage_ttl_ = static_cast<double>(usage_interval_sec);
    nudge_enable_ = nudge_enable;
    nudge_cmd_    = nudge_cmd;
    config_dir_   = config_dir;

    // インスタンス固有の credentials/キャッシュパスを構築する
    // ホームディレクトリや TEMP の取得に失敗した場合は空 path のままにし、
    // 後段の I/O は静かに失敗させる（既存挙動互換）
    // config_dir が空のときはメイン（~/.claude）、非空のときはサブ（指定ディレクトリ直下）から credentials を読む
    if (!config_dir.empty()) {
        creds_path_ = fs::path(config_dir) / L".credentials.json";
    }
    else {
        wchar_t home[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, home))) {
            creds_path_ = fs::path(home) / L".claude" / L".credentials.json";
        }
    }
    wchar_t tmp[MAX_PATH];
    DWORD tlen = GetTempPathW(MAX_PATH, tmp);
    if (tlen != 0 && tlen <= MAX_PATH) {
        // メインは既存ファイル名と互換になるよう suffix 空を維持する
        std::string usage_name = "claude-usage-cache" + cache_suffix + ".json";
        std::string plan_name  = "claude-plan-cache"  + cache_suffix + ".json";
        cache_usage_path_ = fs::path(tmp) / fs::path(usage_name);
        cache_plan_path_  = fs::path(tmp) / fs::path(plan_name);
    }

    // API 取得完了までの空白を埋めるため、TTL 無視で前回キャッシュを暫定値として読み込む
    ClaudeMetrics result;
    apply_usage_json(read_cache_raw(cache_usage_path_), result);
    apply_plan_json (read_cache_raw(cache_plan_path_),  result);
    std::lock_guard<std::mutex> lock(result_mutex_);
    pending_ = result;
}

// PEB の必要分のみ独自定義する
// 64bit プロセスの PEB レイアウトは Windows 10 / 11 で安定しているが、Microsoft 文書上は不安定領域。
// 失敗時はフォールバックで main 扱いとし、サブの誤分類は許容する設計
using NtQueryInformationProcessFn = LONG (NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
struct ProcessBasicInformationLite {
    LONG      ExitStatus;
    PVOID     PebBaseAddress;
    ULONG_PTR AffinityMask;
    LONG      BasePriority;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR InheritedFromUniqueProcessId;
};
// 64bit PEB の ProcessParameters は offset 0x20
// RTL_USER_PROCESS_PARAMETERS の Environment は offset 0x80（環境変数ブロックへの PVOID）
static constexpr SIZE_T PEB_OFFSET_PROCESS_PARAMS_X64       = 0x20;
static constexpr SIZE_T UPP_OFFSET_ENVIRONMENT_X64          = 0x80;

// ntdll!NtQueryInformationProcess を 1 度だけ取得する
// magic static は C++11 でスレッドセーフな単一回初期化が言語規格上保証されているため、
// 関数内 static 変数の遅延初期化に潜む二重チェックロック問題を回避できる
static NtQueryInformationProcessFn get_nt_query_info() {
    static const NtQueryInformationProcessFn fn = []() -> NtQueryInformationProcessFn {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) return nullptr;
        return reinterpret_cast<NtQueryInformationProcessFn>(
            GetProcAddress(ntdll, "NtQueryInformationProcess"));
    }();
    return fn;
}

// 指定 PID のプロセス環境変数から指定キーの値を返す
//
// CLAUDE_CONFIG_DIR のような特定キーの値を取り出すために PEB の Environment ブロックを読む。
// 失敗時は空文字を返す（呼び出し側で main 扱いにフォールバックされる）。
// 32bit プロセス（WoW64）は x64 PEB オフセットで読めないため空を返す。現行 claude.exe は
// 64bit のため実害なし、将来 32bit ビルドが配布されてもサブの誤分類は許容する設計
static std::wstring read_process_env_var(DWORD pid, const std::wstring& name) {
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc) return {};

    // WoW64（32bit プロセス）は x64 PEB レイアウトと不一致のため早期 return
    BOOL is_wow64 = FALSE;
    if (IsWow64Process(proc, &is_wow64) && is_wow64) {
        CloseHandle(proc);
        return {};
    }

    std::wstring result;
    const NtQueryInformationProcessFn nt = get_nt_query_info();
    if (nt) {
        ProcessBasicInformationLite pbi{};
        ULONG ret_len = 0;
        // ProcessBasicInformation = 0
        if (nt(proc, 0, &pbi, sizeof(pbi), &ret_len) == 0 && pbi.PebBaseAddress) {
            PVOID upp = nullptr;
            SIZE_T n = 0;
            // PEB から ProcessParameters のアドレスを読む
            if (ReadProcessMemory(proc,
                    reinterpret_cast<BYTE*>(pbi.PebBaseAddress) + PEB_OFFSET_PROCESS_PARAMS_X64,
                    &upp, sizeof(upp), &n) && n == sizeof(upp) && upp) {
                // ProcessParameters から環境変数ブロックの先頭アドレスを読む
                PVOID env_block = nullptr;
                if (ReadProcessMemory(proc,
                        reinterpret_cast<BYTE*>(upp) + UPP_OFFSET_ENVIRONMENT_X64,
                        &env_block, sizeof(env_block), &n) && n == sizeof(env_block) && env_block) {
                    // 環境変数ブロックを 64KB まで読み取る
                    // 形式は "KEY=VAL\0KEY=VAL\0...\0\0"（UTF-16）。
                    // EnvironmentSize のオフセットは更に不安定なため、固定上限で読んで終端 NUL を探す方式とする
                    constexpr SIZE_T MAX_ENV_BYTES = 64 * 1024;
                    std::vector<wchar_t> buf(MAX_ENV_BYTES / sizeof(wchar_t));
                    SIZE_T read = 0;
                    if (ReadProcessMemory(proc, env_block, buf.data(), MAX_ENV_BYTES, &read)
                        && read >= sizeof(wchar_t)) {
                        const SIZE_T chars = read / sizeof(wchar_t);
                        const std::wstring prefix = name + L"=";
                        const wchar_t* p = buf.data();
                        const wchar_t* end = p + chars;
                        while (p < end && *p) {
                            // 各エントリは NUL 終端の wstring（読み取り範囲外にはみ出さないよう wcsnlen でクランプ）
                            size_t entry_len = wcsnlen(p, static_cast<size_t>(end - p));
                            if (entry_len >= prefix.size() &&
                                _wcsnicmp(p, prefix.c_str(), prefix.size()) == 0) {
                                result.assign(p + prefix.size(), p + entry_len);
                                break;
                            }
                            if (entry_len == static_cast<size_t>(end - p)) break;  // 終端未検出で範囲尽きた
                            p += entry_len + 1;
                        }
                    }
                }
            }
        }
    }
    CloseHandle(proc);
    return result;
}

ClaudeSessionCount count_claude_sessions_split(const std::wstring& sub_config_dir) {
    ClaudeSessionCount result{};

    // サブ用パスを事前に正規化しておく
    // 構成済みでも正規化に失敗した場合は sub_norm 空のままになる。検知できない誤分類を避けるため
    // 区別してログに残す
    std::wstring sub_norm;
    if (!sub_config_dir.empty()) {
        wchar_t norm[MAX_PATH];
        DWORD len = GetFullPathNameW(sub_config_dir.c_str(), MAX_PATH, norm, nullptr);
        if (len > 0 && len < MAX_PATH) {
            sub_norm = norm;
        }
        else {
            log_error("count_claude_sessions_split: GetFullPathNameW failed for sub_config_dir (len=%lu) — all sessions counted as main", len);
        }
    }

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return result;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"claude.exe") == 0) {
                bool is_sub = false;
                if (!sub_norm.empty()) {
                    std::wstring env_val = read_process_env_var(pe.th32ProcessID, L"CLAUDE_CONFIG_DIR");
                    if (!env_val.empty()) {
                        wchar_t norm[MAX_PATH];
                        DWORD len = GetFullPathNameW(env_val.c_str(), MAX_PATH, norm, nullptr);
                        if (len > 0 && len < MAX_PATH && _wcsicmp(norm, sub_norm.c_str()) == 0) {
                            is_sub = true;
                        }
                    }
                }
                if (is_sub) ++result.sub_count;
                else        ++result.main_count;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return result;
}

void ClaudeCollector::update(ClaudeMetrics& out) {
    // session_count は呼び出し側 (window.cpp) が count_claude_sessions_split で
    // メイン/サブ一括計算したものを書き込むため、ここでは触らない

    if (!fetching_.load()) {
        if (fetch_thread_ && WaitForSingleObject(fetch_thread_, 0) == WAIT_OBJECT_0) {
            CloseHandle(fetch_thread_);
            fetch_thread_ = nullptr;
        }
        if (!fetch_thread_) {
            fetching_.store(true);
            fetch_thread_ = CreateThread(nullptr, 0, fetch_thread, this, 0, nullptr);
            if (!fetch_thread_) fetching_.store(false);  // 起動失敗時は次回タイマーで再試行できるようリセット
        }
    }
}

void ClaudeCollector::apply_result(ClaudeMetrics& out, int delta_window_min) {
    std::lock_guard<std::mutex> lock(result_mutex_);
    // セッション数・アカウントラベル・有効化フラグは window.cpp 側で管理しているため
    // pending_（fetch 結果）で上書きされないよう退避する。
    // 5h 履歴も pending_ には無いため退避してから戻す
    int sessions = out.session_count;
    wchar_t label_keep[24];
    wcsncpy_s(label_keep, out.account_label, _TRUNCATE);
    bool enabled_keep = out.account_enabled;
    std::vector<ClaudeHistorySample> hist_keep = std::move(out.five_h_history);
    out = pending_;
    out.session_count = sessions;
    wcsncpy_s(out.account_label, label_keep, _TRUNCATE);
    out.account_enabled = enabled_keep;
    out.five_h_history = std::move(hist_keep);

    // 5h 履歴に現在値を追加し、保持期間外を破棄する
    // データ未取得（avail=false）時は履歴の信頼性が無いため push しない。
    // 保持期間は (delta_window_min + 1) × 60 秒（N 分前のサンプル参照に必要な分 + バッファ 1 分）
    if (out.avail) {
        time_t now = time(nullptr);
        out.five_h_history.push_back({now, out.five_h_pct});
        time_t cutoff = now - static_cast<time_t>((delta_window_min + 1) * 60);
        auto it = std::find_if(out.five_h_history.begin(), out.five_h_history.end(),
            [cutoff](const ClaudeHistorySample& s) { return s.ts >= cutoff; });
        if (it != out.five_h_history.begin())
            out.five_h_history.erase(out.five_h_history.begin(), it);
    }
}

// 中断要求のみ。スレッドの完了は待たない
// 複数コレクタ（main/sub）を並行停止するため、フラグ立てと join 待ちを分離してある。
// 呼び出し側は両方の request_shutdown() を先に呼んでから wait_shutdown() を順に呼ぶことで、
// 順次 shutdown() の 15 秒待ちが直列に積み上がるのを避ける
void ClaudeCollector::request_shutdown() {
    notify_wnd_.store(nullptr);

    // fetch スレッドへ中断を要求する。
    // 同期呼び出し進行中のリクエストハンドルを別スレッドから閉じるのは WinHTTP の禁止事項のため、
    // フラグ確認とタイムアウトによる自発終了に任せる
    shutdown_.store(true);
}

// スレッドの完了を待つ
// 各 WinHTTP 呼び出しは 1500ms タイムアウトで有界、読み取りループは周回ごとに shutdown
// フラグを確認するため、残存時間は「ブロック中の 1 操作のタイムアウト＋α」に収まる。
// 15 秒はその十分な余裕。万一タイムアウトしてもハンドルへの介入はせずログのみとする
void ClaudeCollector::wait_shutdown() {
    if (fetch_thread_) {
        DWORD wr = WaitForSingleObject(fetch_thread_, 15000);
        if (wr != WAIT_OBJECT_0) {
            log_error("ClaudeCollector::shutdown fetch_thread did not exit (wait=%lu)", wr);
        }
        CloseHandle(fetch_thread_);
        fetch_thread_ = nullptr;
    }
}
