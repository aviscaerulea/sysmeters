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

// fetch スレッドのハング判定閾値（ms）。TIMER_CLAUDE の 60 秒周期を 1.5 周期分としてマージンを取る。
// 通常時の do_fetch 所要は数秒以内で終わる。
// Usage API / Account API の各 HTTP は WinHttpSetTimeouts の 1500ms + 読み取り 30 秒デッドラインで
// 有界のため、90 秒を超える継続は WinHTTP 内部ハング等の異常とみなす
static constexpr uint64_t FETCH_STALL_TIMEOUT_MS = 90000;

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

// [ts, pct] 形式の 1 要素をサンプルへ変換する。数値でない要素は false を返す（呼び出し側でスキップする）
static bool parse_hist_sample(const json& elem, ClaudeHistorySample& out) {
    if (!elem.is_array() || elem.size() < 2 || !elem[0].is_number() || !elem[1].is_number())
        return false;
    out.ts  = static_cast<time_t>(elem[0].get<double>());
    out.pct = static_cast<float>(elem[1].get<double>());
    return true;
}

// JSON 配列（[[ts, pct], ...] 形式）を履歴ベクタへ変換する。
// 数値でない要素はスキップする。（古いサンプルは呼び出し側の push_and_trim が自然に落とすため
// ここでは鮮度チェックをしない。未来時刻のサンプルもレート算出側の span<=0 ガードで無害）
static std::vector<ClaudeHistorySample> parse_hist_array(const json& arr) {
    std::vector<ClaudeHistorySample> hist;
    if (!arr.is_array()) return hist;
    for (const auto& elem : arr) {
        ClaudeHistorySample s;
        if (parse_hist_sample(elem, s)) hist.push_back(s);
    }
    return hist;
}

// 履歴ベクタを JSON 配列（[[ts, pct], ...] 形式）へ変換する
static json hist_to_json_array(const std::vector<ClaudeHistorySample>& hist) {
    json arr = json::array();
    for (const auto& s : hist)
        arr.push_back(json::array({static_cast<double>(s.ts), static_cast<double>(s.pct)}));
    return arr;
}

// 5h/7d 履歴キャッシュ（{"five_h":[[ts,pct],...], "seven_d":[[ts,pct],...]}）を読み込む
//
// init() から一度だけ呼ばれ、起動直後の履歴（restored_hist5_ / restored_hist7_）の種にする。
// read_cache_raw を再利用するため TTL 判定はなく、ファイル未存在・parse 失敗時は
// out_five / out_seven を空のまま返す。（現状互換：履歴なしからスタートする挙動に退化する）
static void load_history_cache(const fs::path& path,
                               std::vector<ClaudeHistorySample>& out_five,
                               std::vector<ClaudeHistorySample>& out_seven) {
    out_five.clear();
    out_seven.clear();
    json j = read_cache_raw(path);
    if (j.is_null()) return;
    try {
        out_five  = parse_hist_array(j.value("five_h",  json::array()));
        out_seven = parse_hist_array(j.value("seven_d", json::array()));
    }
    catch (...) {}
}

// 5h/7d 履歴キャッシュを直接上書き保存する
//
// apply_result が avail 時に毎回呼ぶ。既存の usage/plan キャッシュと同じ流儀で
// テンポラリ→リネームは行わず ofstream で直接上書きする。（クラッシュ時は次回 parse 失敗となり
// 復元なしに退化するだけで許容する）
static void save_history_cache(const fs::path& path,
                               const std::vector<ClaudeHistorySample>& five,
                               const std::vector<ClaudeHistorySample>& seven) {
    if (path.empty()) return;
    try {
        json j;
        j["five_h"]  = hist_to_json_array(five);
        j["seven_d"] = hist_to_json_array(seven);
        std::ofstream ofs(path);
        ofs << j.dump();
    }
    catch (...) {}
}

// null 値を欠落と同一視して JSON から数値を読む
//
// Usage API はウィンドウ非アクティブ時や未使用項目に「キーは存在するが値は null」を返す。
// json::value() はこのケースでデフォルト値を返さず type_error.302 を投げるため使わない。
static double json_num(const nlohmann::json& j, const char* key, double def) {
    auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? it->get<double>() : def;
}

// null 値を欠落と同一視して JSON から文字列を読む（欠落・null は空文字）
static std::string json_str(const nlohmann::json& j, const char* key) {
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

// null 値を欠落と同一視して JSON から bool を読む
static bool json_bool(const nlohmann::json& j, const char* key, bool def) {
    auto it = j.find(key);
    return (it != j.end() && it->is_boolean()) ? it->get<bool>() : def;
}

// Usage API レスポンス JSON を ClaudeMetrics に反映する
//
// do_fetch（API/キャッシュ経由）と init（前回キャッシュ復元）で共有する。
// usage_j が null の場合は何もしない。成功時のみ result.avail を true にし、
// キャッシュ JSON に付与された "_ts"（do_fetch がキャッシュ保存時に付与、新規取得・
// キャッシュヒット・起動時復元のいずれでも保持）を result.fetched_ts に生値のまま格納し、
// あわせてローカル時刻 "HH:MM" に整形して result.fetched_at へ格納する。
// （fetched_at は画面表示用の取得時刻鮮度インジケータ、fetched_ts はペース追跡のサンプル時刻）
static void apply_usage_json(const json& usage_j, ClaudeMetrics& result) {
    if (usage_j == nullptr) return;
    try {
        // ウィンドウ失効中（次の使用が始まっていない状態）は five_hour / seven_day の
        // フィールド値、またはオブジェクトごと null になる。null は「値なし」として扱い、
        // 使用率 0% / リセット時刻「-」/ resets_ts = -1（アクティブウィンドウ無し）へ落とす。
        const json empty_obj = json::object();
        auto section = [&](const char* key) -> const json& {
            auto it = usage_j.find(key);
            return (it != usage_j.end() && it->is_object()) ? *it : empty_obj;
        };
        const json& fh = section("five_hour");
        const json& sd = section("seven_day");
        // utilization は API から 0〜100 の % 値で返る
        result.five_h_pct  = static_cast<float>(json_num(fh, "utilization", 0.0));
        result.seven_d_pct = static_cast<float>(json_num(sd, "utilization", 0.0));

        std::string fh_resets_at = json_str(fh, "resets_at");
        std::string sd_resets_at = json_str(sd, "resets_at");
        format_reset_time(fh_resets_at, false, result.five_h_reset, _countof(result.five_h_reset));
        format_reset_time(sd_resets_at, true,  result.seven_d_reset, _countof(result.seven_d_reset));
        result.five_h_expected_pct  = calc_expected_pct(fh_resets_at, 5.0 * 3600);
        result.seven_d_expected_pct = calc_expected_pct(sd_resets_at, 7.0 * 24 * 3600);
        result.five_h_resets_ts  = parse_iso8601_utc(fh_resets_at);
        result.seven_d_resets_ts = parse_iso8601_utc(sd_resets_at);
        result.avail = true;

        time_t fetched_ts = static_cast<time_t>(json_num(usage_j, "_ts", 0.0));
        result.fetched_ts = fetched_ts;
        if (fetched_ts > 0) {
            struct tm lt{};
            localtime_s(&lt, &fetched_ts);
            swprintf_s(result.fetched_at, L"%d:%02d", lt.tm_hour, lt.tm_min);
        }

        // 超過料金情報（extra_usage）
        if (usage_j.contains("extra_usage") && usage_j.at("extra_usage").is_object()) {
            const json& eu = usage_j.at("extra_usage");
            result.extra_enabled      = json_bool(eu, "is_enabled", false);
            result.extra_used_dollars = static_cast<float>(json_num(eu, "used_credits", 0.0)) / 100.f;
        }

        // モデルスコープ 7d 使用率（limits 配列の kind=="weekly_scoped"）
        // Fable 等の上位モデル専用 7d 枠の消費率。特定モデル名では絞らず weekly_scoped の
        // 最初のエントリを採用する（現状 1 件のみ観測。モデル世代交代への耐性を優先）。
        // 呼び出し元は前回値を引き継いだ result を渡すため、走査前に必ず -1（未提供）へ
        // リセットする。エントリが消えた場合に古い値が残り続けるのを防ぐ
        result.seven_d_scoped_pct = -1.f;
        if (usage_j.contains("limits") && usage_j.at("limits").is_array()) {
            for (const auto& lim : usage_j.at("limits")) {
                if (lim.is_object() && json_str(lim, "kind") == "weekly_scoped") {
                    result.seven_d_scoped_pct = static_cast<float>(json_num(lim, "percent", -1.0));
                    break;
                }
            }
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

// 5h リセット通過後の未消費間隙 nudge：claude.exe を環境変数で構成して起動する
//
// presumed = false はフェッチ成功データで間隙を確認した通常発火、true はフェッチ失敗（ERR）中に
// 監視対象の時計通過だけを根拠とする推定発火を示す。動作は同一でログ文言のみ区別する。
//
// メイン（config_dir_ 空）は親プロセス環境を継承して起動する。
// サブ（config_dir_ 非空）は親プロセス環境 + CLAUDE_CONFIG_DIR=<config_dir_> を加えた一時環境で
// 起動する。Claude CLI には --config-dir コマンドオプションが存在せず、設定ディレクトリの上書きは
// CLAUDE_CONFIG_DIR 環境変数経由でのみ可能なため。sysmeters 自身のプロセス親環境は変更せず、
// CreateProcess の lpEnvironment で子プロセスにだけ設定する。
void ClaudeCollector::run_nudge(bool presumed) {
    log_info("claude nudge: 5h window gap %s, running (account=%d): %s",
             presumed ? "presumed (usage fetch failing)" : "detected",
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
    // 自世代を捕獲する。update() の watchdog 発火や新スレッド起動で fetch_gen_ が進んでいれば、
    // 終端で pending_ 更新・WM_CLAUDE_DONE 通知・fetching_ 復帰をスキップし、遅延復帰した
    // 旧スレッドが新世代の状態を破壊しないようにする
    uint32_t my_gen = fetch_gen_.load();

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
    // トークン未取得＝ログアウト状態の判定。fetch_or_cache はトークン空だと HTTP を試みず
    // 即 nullptr を返すため、失敗ログもネガティブキャッシュも残らない。表示側が通信障害の
    // Err と区別して Logout を点灯できるよう、フラグとして毎フェッチで更新する
    result.token_missing = token.empty();

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

    // --- 5h リセット通過の nudge（claude.exe 起動による次ウィンドウの消費促進）---
    // アクティブな 5h ウィンドウ（未来の resets_ts）を監視対象として記憶し、
    // 「監視対象が過去 かつ 現在アクティブウィンドウが無い」＝リセット通過後に消費が
    // 始まっていない間隙を検知したら、そのウィンドウにつき 1 回 nudge_cmd を起動する。
    // 間隙は API の応答形式 2 通りで現れる：resets_at が null になる形式（rts = -1）と、
    // 過去の resets_at を返し続ける形式（rts が過去値）。
    // 監視対象は init 時のキャッシュ復元値でも種付けされるため、起動直後の間隙でも発火する。
    // nudge が機能して新ウィンドウが始まれば監視対象の更新側へ入るため、重複発火しない。
    // フェッチ失敗（ERR）中も、把握済み監視対象のリセット時刻通過は時計だけで確定できるため
    // 推定発火する（下の else if）。新ウィンドウが既に始まっているかは確認できないが、
    // 誤発火コストは極小（デフォルト cmd は haiku への極小プロンプト 1 回）で、重複はキー記録で
    // 抑止される。401 認証切れ由来の ERR では claude.exe 起動がトークンを更新し ERR 自体を
    // 治癒する副次効果もある。監視対象が未観測（-1）の間は根拠が無いため発火しない。
    if (usage_j != nullptr && result.avail) {
        time_t now = static_cast<time_t>(now_ts());
        time_t rts = result.five_h_resets_ts;   // -1 = アクティブウィンドウ無し（API null 応答）
        if (rts > now) {
            watched_5h_resets_ts_ = rts;
        }
        else if (nudge_enable_) {
            // 終了したウィンドウの識別子が不明なケース（null 形式の間隙中に起動し、キャッシュにも
            // resets_at が残っていない）は固定キー 1 で 1 回だけ発火させる。間隙の存在自体は
            // フェッチ成功データで確認できているため発火が正しく、重複はキーの記録で抑止できる
            time_t ended = (rts > 0) ? rts
                         : (watched_5h_resets_ts_ > 0) ? watched_5h_resets_ts_ : 1;
            if (ended <= now && ended != last_nudge_resets_ts_) {
                last_nudge_resets_ts_ = ended;
                run_nudge(false);
            }
        }
    }
    else if (usage_j == nullptr && nudge_enable_ && watched_5h_resets_ts_ > 0) {
        // ERR 経路：監視対象の時計通過のみを根拠に推定発火する。キーに watched 値を記録する
        // ため、ERR 復旧後の成功フェッチが同じ間隙を検知しても再発火しない
        time_t now = static_cast<time_t>(now_ts());
        if (watched_5h_resets_ts_ <= now && watched_5h_resets_ts_ != last_nudge_resets_ts_) {
            last_nudge_resets_ts_ = watched_5h_resets_ts_;
            run_nudge(true);
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

    // 世代不一致（自スレッドは watchdog で既に捨てられている）なら pending_ / 通知 / fetching_ を
    // 触らずに終了する。新世代のスレッドがすでに動いているため、pending_ を古い値で汚す・
    // fetching_ を誤って false にして watchdog を無効化するのを防ぐ
    if (my_gen != fetch_gen_.load()) return;

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
        std::string hist_name = "claude-history-cache" + cache_suffix + ".json";
        cache_hist_path_  = fs::path(tmp) / fs::path(hist_name);
    }

    // API 取得完了までの空白を埋めるため、TTL 無視で前回キャッシュを暫定値として読み込む
    ClaudeMetrics result;
    apply_usage_json(read_cache_raw(cache_usage_path_), result);
    apply_plan_json (read_cache_raw(cache_plan_path_),  result);

    // 5h/7d 履歴キャッシュを読み込み、起動直後の apply_result 初回呼び出しで
    // out 側の履歴（空）へ一度だけ種付けする。（詳細は apply_result 宣言部のコメント参照）
    load_history_cache(cache_hist_path_, restored_hist5_, restored_hist7_);

    // 起動直後の間隙検知を可能にするため、復元したキャッシュの 5h resets_ts を監視対象として
    // 種付けする。過去値でも可：フェッチ成功時にアクティブウィンドウが無ければ即 nudge となる
    if (result.five_h_resets_ts > 0) watched_5h_resets_ts_ = result.five_h_resets_ts;

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
                    // 環境変数ブロック（"KEY=VAL\0KEY=VAL\0...\0\0" の UTF-16）を最大 64KB まで、
                    // ページ境界に整列したチャンク単位で読み進める。
                    // ブロックのコミット済みサイズは通常数 KB で、先頭から 64KB を一括要求すると
                    // ReadProcessMemory が未コミット領域を跨いで全体失敗する。（この失敗により
                    // v1.23.1 まで環境変数が常に読めず、サブ判定が一度も機能していなかった）
                    // 各読み取りを 1 ページ内に収めれば、コミット済み範囲だけを確実に取得できる。
                    // EnvironmentSize フィールドのオフセットは OS バージョン間で不安定なため使わない
                    constexpr SIZE_T MAX_ENV_BYTES = 64 * 1024;
                    constexpr SIZE_T PAGE_BYTES    = 4096;
                    std::vector<wchar_t> buf;
                    buf.reserve(MAX_ENV_BYTES / sizeof(wchar_t));
                    SIZE_T total = 0;
                    while (total < MAX_ENV_BYTES) {
                        const ULONG_PTR addr = reinterpret_cast<ULONG_PTR>(env_block) + total;
                        SIZE_T chunk = PAGE_BYTES - (addr & (PAGE_BYTES - 1));  // 次のページ境界までに制限
                        if (total + chunk > MAX_ENV_BYTES) chunk = MAX_ENV_BYTES - total;
                        wchar_t page[PAGE_BYTES / sizeof(wchar_t)];
                        SIZE_T read = 0;
                        if (!ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(addr), page, chunk, &read)
                            || read == 0) {
                            break;  // 未コミット領域に到達＝環境ブロックの終端より後ろ
                        }
                        buf.insert(buf.end(), page, page + read / sizeof(wchar_t));
                        total += read;
                        if (read < chunk) break;  // 部分読みはこれ以上進めない
                    }
                    if (!buf.empty()) {
                        const SIZE_T chars = buf.size();
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

// パス末尾のセパレータを 1 文字除去する
// GetFullPathNameW は入力の末尾セパレータ有無をそのまま結果に伝搬させるため、
// TOML の config_dir と各プロセスの CLAUDE_CONFIG_DIR で末尾 "\" の有無が食い違うと
// _wcsicmp が一致しなくなり、サブアカウント判定が失敗する。ドライブルート
// （"X:\" や UNC "\\server\"）はセパレータ除去でパス意味論が変わるため、長さ 3 以下は温存する
static void strip_trailing_sep(wchar_t* path) {
    size_t len = wcslen(path);
    if (len > 3 && (path[len - 1] == L'\\' || path[len - 1] == L'/')) {
        path[len - 1] = L'\0';
    }
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
            strip_trailing_sep(norm);
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
                        if (len > 0 && len < MAX_PATH) {
                            strip_trailing_sep(norm);
                            if (_wcsicmp(norm, sub_norm.c_str()) == 0) {
                                is_sub = true;
                            }
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

    // ハング検知：fetching_ が true のまま FETCH_STALL_TIMEOUT_MS 以上経過している場合、
    // do_fetch スレッドが fetching_.store(false) に到達せず滞留したとみなす。
    // （WinHTTP 内部の無限待機など、http_get 側タイムアウトが効かないケースへの保険）
    // 経過時間は GetTickCount64 の単調時計で計測し、NTP 補正等の壁掛け時計飛びに強くする。
    // ハンドルは CloseHandle して閉じる。スレッド自体は生きている可能性があるが、
    // fetch_gen_ を進めるため、遅延復帰した旧スレッドは終端で pending_ / fetching_ を触らない。
    // TerminateThread はロック未解放・リソースリークの危険があるため使わない
    if (fetching_.load()) {
        uint64_t elapsed_ms = GetTickCount64() - fetch_start_tick_.load();
        if (elapsed_ms > FETCH_STALL_TIMEOUT_MS) {
            log_error("claude fetch stalled %llums (account=%d), resetting",
                      static_cast<unsigned long long>(elapsed_ms), account_index_);
            if (fetch_thread_) {
                CloseHandle(fetch_thread_);
                fetch_thread_ = nullptr;
            }
            fetch_gen_.fetch_add(1);  // 旧スレッドを非現世代に格下げ
            fetching_.store(false);
            // 停滞中に書かれた可能性のあるネガティブキャッシュを次回強制無視し、即再取得させる
            first_fetch_ = true;
        }
    }

    if (!fetching_.load()) {
        if (fetch_thread_ && WaitForSingleObject(fetch_thread_, 0) == WAIT_OBJECT_0) {
            CloseHandle(fetch_thread_);
            fetch_thread_ = nullptr;
        }
        if (!fetch_thread_) {
            fetch_gen_.fetch_add(1);
            fetch_start_tick_.store(GetTickCount64());
            fetching_.store(true);
            fetch_thread_ = CreateThread(nullptr, 0, fetch_thread, this, 0, nullptr);
            if (!fetch_thread_) fetching_.store(false);  // 起動失敗時は次回タイマーで再試行できるようリセット
        }
    }
}

void ClaudeCollector::apply_result(ClaudeMetrics& out, int delta_window_min, int delta_window_7d_min) {
    std::lock_guard<std::mutex> lock(result_mutex_);
    // セッション数・アカウントラベル・有効化フラグは window.cpp 側で管理しているため
    // pending_（fetch 結果）で上書きされないよう退避する。
    // 5h/7d 履歴も pending_ には無いため退避してから戻す
    int sessions = out.session_count;
    wchar_t label_keep[24];
    wcsncpy_s(label_keep, out.account_label, _TRUNCATE);
    bool enabled_keep = out.account_enabled;
    std::vector<ClaudeHistorySample> hist5_keep = std::move(out.five_h_history);
    std::vector<ClaudeHistorySample> hist7_keep = std::move(out.seven_d_history);
    // 起動直後の一度きりの種付け：out 側の履歴が空（アプリ起動後まだ 1 度も push していない）
    // かつ init() が復元した履歴が非空なら、それを種として採用する。
    // 採用後は復元メンバを明示的に空にし、以後の呼び出しで再利用されないようにする
    if (hist5_keep.empty() && !restored_hist5_.empty()) {
        hist5_keep = std::move(restored_hist5_);
        restored_hist5_.clear();
    }
    if (hist7_keep.empty() && !restored_hist7_.empty()) {
        hist7_keep = std::move(restored_hist7_);
        restored_hist7_.clear();
    }
    out = pending_;
    out.session_count = sessions;
    wcsncpy_s(out.account_label, label_keep, _TRUNCATE);
    out.account_enabled = enabled_keep;
    out.five_h_history  = std::move(hist5_keep);
    out.seven_d_history = std::move(hist7_keep);

    // 5h/7d 履歴に現在値を追加し、保持期間外を破棄する
    // データ未取得（avail=false）時は履歴の信頼性が無いため push しない。
    // 保持期間は (delta_window + 1) × 60 秒（N 分前のサンプル参照に必要な分 + バッファ 1 分）。
    // 7d は保持期間より古いサンプルのうち最新の 1 個をアンカーとして追加保持する。
    // アプリ・OS の停止明けに「停止前最後の観測」を基準とした実質ペース判定を即時再開する
    // ためで、5h には不要（anchor_floor = 0 で無効化）。
    // 7d ウィンドウのリセットで履歴はクリアしない。旧ウィンドウの高 pct サンプルが残っても、
    // 描画側の「増加分のみ描画」と「レート ≤ 0 は判定しない」条件で無害化される。
    // avail 時は push_and_trim 直後に cache_hist_path_ へ毎回上書き保存し、アプリ再起動後も
    // init() 経由で直近の履歴（7d はデフォルト 12h 分 + アンカー）を復元できるようにする。（クラッシュ耐性優先の設計）
    if (out.avail) {
        time_t now = time(nullptr);
        // anchor_floor > 0 のとき、cutoff より古いサンプルのうち最新の 1 個を
        // アンカーとして残す。（ts >= anchor_floor のものに限る。0 でアンカー無効）
        auto push_and_trim = [now](std::vector<ClaudeHistorySample>& hist, float pct, int win_min,
                                   time_t anchor_floor) {
            hist.push_back({now, pct});
            time_t cutoff = now - static_cast<time_t>(win_min + 1) * 60;
            auto it = std::find_if(hist.begin(), hist.end(),
                [cutoff](const ClaudeHistorySample& s) { return s.ts >= cutoff; });
            if (it != hist.begin()) {
                if (anchor_floor > 0 && std::prev(it)->ts >= anchor_floor) --it;
                hist.erase(hist.begin(), it);
            }
        };
        // 7d アンカーの下限時刻：窓幅の 2 倍（デフォルト 24h）より古い、または現行 7d
        // ウィンドウ開始より前のサンプルはアンカーにしない。前者は古すぎる基準による過剰な
        // 平滑化の防止、後者は旧ウィンドウの pct を跨いだ誤ったレート算出の防止。
        // delta_window_7d_min = 0 のときは anchor_floor = now となり自然に無効化される
        time_t anchor_floor = now - static_cast<time_t>(delta_window_7d_min) * 120;
        if (out.seven_d_resets_ts > 0) {
            time_t win_start = out.seven_d_resets_ts - static_cast<time_t>(7) * 24 * 3600;
            if (win_start > anchor_floor) anchor_floor = win_start;
        }
        push_and_trim(out.five_h_history,  out.five_h_pct,  delta_window_min, 0);
        push_and_trim(out.seven_d_history, out.seven_d_pct, delta_window_7d_min, anchor_floor);
        // 7d 保持上限は (delta_window_7d_min + 1) 分 ≒ 12h、取得サイクル ~60 秒で高々 750
        // サンプル前後・数十 KB のため、毎回書いても実害がない
        save_history_cache(cache_hist_path_, out.five_h_history, out.seven_d_history);
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
