// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#include "config.hpp"
#include <toml.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// TOML テーブルの再帰上書きマージ
// over 側の値を base に上書きする。両者がテーブルのキーは再帰、それ以外は置換。
static void merge_overrides(toml::value& base, const toml::value& over) {
    if (!base.is_table() || !over.is_table()) {
        base = over;
        return;
    }
    auto& bt = base.as_table();
    for (const auto& kv : over.as_table()) {
        auto it = bt.find(kv.first);
        if (it != bt.end() && it->second.is_table() && kv.second.is_table())
            merge_overrides(it->second, kv.second);
        else
            bt[kv.first] = kv.second;
    }
}

AppConfig load_config(const std::string& path) {
    AppConfig cfg;

    // 起動時トラブルシュート用に、試行した base パスを記録する。
    // ファイルを開けなかった場合でも、どのパスを見に行ったかをログに残せるようにする。
    cfg.base_path_used = path;

    // バイナリモードで開く
    // toml11 の istream 版がテキストモードの CRLF 変換でバッファ末尾に NUL を埋め込むバグへの回避策。
    // バイナリモードのまま toml11 に渡しても CRLF は許容される。
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return cfg;  // ファイルなし → デフォルト値で返す
    cfg.base_path_loaded = true;

    try {
        auto data = toml::parse(ifs, path);

        // local 設定ファイルの読み込みとマージ
        // ".toml" の直前に ".local" を挿入してパスを導出する（例：sysmeters.local.toml）。
        // ファイルが存在する場合のみ適用し、解析失敗時は base 設定を維持する。
        {
            std::string local_path = path;
            const std::string ext = ".toml";
            if (local_path.size() >= ext.size() &&
                local_path.compare(local_path.size() - ext.size(), ext.size(), ext) == 0)
                local_path.insert(local_path.size() - ext.size(), ".local");

            // 試行した local パスを起動時ログ用に保持。ファイル不在でも記録する
            cfg.local_path_used = local_path;

            std::ifstream lifs(local_path, std::ios::binary);
            if (lifs.is_open()) {
                cfg.local_path_loaded = true;
                try {
                    auto local = toml::parse(lifs, local_path);
                    merge_overrides(data, local);
                }
                catch (...) {
                    cfg.config_error = "TOML parse failed: " + local_path;
                }
            }
        }

        // 型別の TOML 値取得ヘルパ（キー不在・型不一致・パースエラー時はデフォルト値を返す）
        auto get_int = [&](const char* sec, const char* key, int def) -> int {
            try { return toml::find_or(data, sec, key, def); }
            catch (...) { return def; }
        };
        auto get_u32 = [&](const char* sec, const char* key, uint32_t def) -> uint32_t {
            try {
                // TOML は整数として読み込む（0xRRGGBB 形式で記述されている）
                return static_cast<uint32_t>(toml::find_or<int64_t>(data, sec, key, static_cast<int64_t>(def)));
            }
            catch (...) { return def; }
        };
        auto get_float = [&](const char* sec, const char* key, float def) -> float {
            // TOML 数値型の互換取得
            // 整数リテラル（例：95）と浮動小数点リテラル（例：95.0）の両方に対応する。
            // find_or は型不一致を黙ってデフォルト値に置き換えるため、find で取得して型を自前判定する。
            try {
                const auto& v = toml::find(data, sec, key);
                if (v.is_floating()) return static_cast<float>(v.as_floating());
                if (v.is_integer())  return static_cast<float>(v.as_integer());
                return def;  // 数値以外の型
            }
            catch (...) { return def; }  // キー不在など
        };
        auto get_bool = [&](const char* sec, const char* key, bool def) -> bool {
            try { return toml::find_or<bool>(data, sec, key, def); }
            catch (...) { return def; }
        };
        // UTF-8 文字列を wstring として取得する（キー不在・変換失敗時はデフォルト値を返す）
        auto get_wstr = [&](const char* sec, const char* key,
                            const std::wstring& def) -> std::wstring {
            std::string u8;
            try { u8 = toml::find_or<std::string>(data, sec, key, std::string{}); }
            catch (...) { return def; }
            if (u8.empty()) return def;
            int n = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), -1, nullptr, 0);
            if (n <= 0) return def;
            std::wstring w(static_cast<size_t>(n - 1), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), -1, w.data(), n);
            return w;
        };

        cfg.win_x     = get_int("window", "x",     cfg.win_x);
        cfg.win_y     = get_int("window", "y",     cfg.win_y);
        cfg.win_width = get_int("window", "width", cfg.win_width);

        cfg.col_background = get_u32("color", "background", cfg.col_background);
        cfg.col_border     = get_u32("color", "border",     cfg.col_border);
        cfg.col_text       = get_u32("color", "text",       cfg.col_text);
        cfg.col_graph_fill = get_u32("color", "graph_fill", cfg.col_graph_fill);
        cfg.col_disk_read  = get_u32("color", "disk_read",  cfg.col_disk_read);
        cfg.col_disk_write = get_u32("color", "disk_write", cfg.col_disk_write);
        cfg.col_net_recv   = get_u32("color", "net_recv",   cfg.col_net_recv);
        cfg.col_net_send   = get_u32("color", "net_send",   cfg.col_net_send);
        cfg.col_claude_bar = get_u32("color", "claude_bar", cfg.col_claude_bar);
        cfg.col_cpu_core   = get_u32("color", "cpu_core",   cfg.col_cpu_core);

        cfg.warn_cpu_pct       = get_float("threshold", "cpu_pct",       cfg.warn_cpu_pct);
        cfg.warn_gpu_pct       = get_float("threshold", "gpu_pct",       cfg.warn_gpu_pct);
        cfg.warn_mem_pct        = get_float("threshold", "mem_pct",        cfg.warn_mem_pct);
        cfg.warn_disk_space_pct = get_float("threshold", "disk_space_pct", cfg.warn_disk_space_pct);
        cfg.warn_claude_5h_pct = get_float("threshold", "claude_5h_pct", cfg.warn_claude_5h_pct);
        cfg.warn_claude_7d_pct = get_float("threshold", "claude_7d_pct", cfg.warn_claude_7d_pct);
        cfg.warn_claude_over   = get_float("threshold", "claude_over",   cfg.warn_claude_over);
        cfg.warn_disk_gbh      = get_float("threshold", "disk_gbh",      cfg.warn_disk_gbh);
        cfg.warn_temp_caution  = get_float("threshold", "temp_caution",  cfg.warn_temp_caution);
        cfg.warn_temp_critical = get_float("threshold", "temp_critical",  cfg.warn_temp_critical);
        cfg.warn_uptime_days   = get_int  ("threshold", "uptime_days",   cfg.warn_uptime_days);
        cfg.warn_processes     = get_int  ("threshold", "processes",     cfg.warn_processes);
        cfg.warn_threads       = get_int  ("threshold", "threads",       cfg.warn_threads);
        cfg.warn_handles       = get_int  ("threshold", "handles",       cfg.warn_handles);

        cfg.alert_sound         = get_bool ("threshold", "alert_sound",         cfg.alert_sound);
        cfg.reset_cpu_pct       = get_float("threshold", "reset_cpu_pct",       cfg.reset_cpu_pct);
        cfg.reset_gpu_pct       = get_float("threshold", "reset_gpu_pct",       cfg.reset_gpu_pct);
        cfg.reset_mem_pct        = get_float("threshold", "reset_mem_pct",        cfg.reset_mem_pct);
        cfg.reset_disk_space_pct = get_float("threshold", "reset_disk_space_pct", cfg.reset_disk_space_pct);
        cfg.reset_temp          = get_float("threshold", "reset_temp",          cfg.reset_temp);
        cfg.reset_disk_gbh      = get_float("threshold", "reset_disk_gbh",      cfg.reset_disk_gbh);
        cfg.reset_claude_5h_pct = get_float("threshold", "reset_claude_5h_pct", cfg.reset_claude_5h_pct);
        cfg.reset_claude_7d_pct = get_float("threshold", "reset_claude_7d_pct", cfg.reset_claude_7d_pct);

        cfg.guard_tone_ms = get_int("guard", "tone_ms", cfg.guard_tone_ms);

        cfg.update_check_enabled = get_bool("update", "enabled", cfg.update_check_enabled);

        cfg.priority_control_enable     = get_bool("process", "priority_control",   cfg.priority_control_enable);
        cfg.priority_check_interval_sec = get_int ("process", "check_interval_sec", cfg.priority_check_interval_sec);
        cfg.priority_visible_range_pct  = get_int ("process", "visible_range_pct",  cfg.priority_visible_range_pct);
        cfg.priority_hidden_range_pct   = get_int ("process", "hidden_range_pct",   cfg.priority_hidden_range_pct);

        cfg.log_dir = toml::find_or<std::string>(data, "log", "dir", cfg.log_dir);

        cfg.claude_usage_interval_sec = get_int("claude", "usage_interval_sec", cfg.claude_usage_interval_sec);
        try { cfg.claude_nudge_cmd = toml::find_or<std::string>(data, "claude", "nudge_cmd", cfg.claude_nudge_cmd); }
        catch (...) {}
        cfg.show_peak_bar = get_bool("claude", "show_peak_bar", cfg.show_peak_bar);
        cfg.claude_delta_window_min = get_int("claude", "delta_window_min", cfg.claude_delta_window_min);
        cfg.claude_underuse_enable        = get_bool ("claude", "underuse_enable",        cfg.claude_underuse_enable);
        cfg.claude_underuse_warn_pct      = get_float("claude", "underuse_warn_pct",      cfg.claude_underuse_warn_pct);
        cfg.claude_underuse_ignore_5h_min = get_int  ("claude", "underuse_ignore_5h_min", cfg.claude_underuse_ignore_5h_min);
        cfg.claude_underuse_ignore_7d_min = get_int  ("claude", "underuse_ignore_7d_min", cfg.claude_underuse_ignore_7d_min);
        cfg.claude_underuse_pace_window_min = get_int("claude", "underuse_pace_window_min", cfg.claude_underuse_pace_window_min);
        cfg.claude_underuse_grace_5h_min    = get_int("claude", "underuse_grace_5h_min",    cfg.claude_underuse_grace_5h_min);
        cfg.claude_underuse_grace_7d_min    = get_int("claude", "underuse_grace_7d_min",    cfg.claude_underuse_grace_7d_min);

        // メインアカウント設定（[claude] セクション）
        // メインは ~/.claude を固定使用するため config_dir は持たない。
        // enable はロード時に true 強制（[claude] セクションで明示しなくても動く既存互換）。
        cfg.claude_main.name         = get_wstr("claude", "name",         L"Claude");
        cfg.claude_main.config_dir.clear();
        cfg.claude_main.nudge_enable = get_bool("claude", "nudge_enable", false);
        cfg.claude_main.enable       = true;

        // サブアカウント設定（[claude_sub] セクション）
        // enable は TOML 明示キー。デフォルト OFF とし、ユーザの明示有効化を要求する
        cfg.claude_sub.enable       = get_bool("claude_sub", "enable",       false);
        cfg.claude_sub.name         = get_wstr("claude_sub", "name",         L"Claude");
        cfg.claude_sub.config_dir   = get_wstr("claude_sub", "config_dir",   L"");
        cfg.claude_sub.nudge_enable = get_bool("claude_sub", "nudge_enable", false);

        // サブの config_dir 検証
        // enable=true でも config_dir 空 or 不在ディレクトリなら enable=false に強制し、エラーを蓄積する。
        // log_init より前のため log_error は使えず config_error に格納する
        if (cfg.claude_sub.enable) {
            if (cfg.claude_sub.config_dir.empty()) {
                cfg.claude_sub.enable = false;
                if (cfg.config_error.empty())
                    cfg.config_error = "[claude_sub] enable=true だが config_dir が空のため無効化";
            }
            else {
                std::error_code ec;
                if (!std::filesystem::is_directory(std::filesystem::path(cfg.claude_sub.config_dir), ec)) {
                    cfg.claude_sub.enable = false;
                    if (cfg.config_error.empty())
                        cfg.config_error = "[claude_sub] config_dir のディレクトリが見つからないため無効化";
                }
            }
        }

        cfg.notify_peak_limit_enable = get_bool ("notify", "peak_limit_enable", cfg.notify_peak_limit_enable);
        cfg.notify_peak_limit_sound  = get_bool ("notify", "peak_limit_sound",  cfg.notify_peak_limit_sound);
        cfg.notify_peak_limit_title  = get_wstr ("notify", "peak_limit_title",  cfg.notify_peak_limit_title);
        cfg.notify_peak_limit_body   = get_wstr ("notify", "peak_limit_body",   cfg.notify_peak_limit_body);
        // szInfoTitle は 64 wchar、szInfo は 256 wchar が上限
        // 境界直前が上位サロゲート（0xD800〜0xDBFF）の場合、ペアの途中で分断しないよう 1 文字分短くする
        auto trim_wstr = [](std::wstring& s, size_t max) {
            if (s.size() <= max) return;
            if (max > 0 && s[max - 1] >= 0xD800 && s[max - 1] <= 0xDBFF) --max;
            s.resize(max);
        };
        trim_wstr(cfg.notify_peak_limit_title, 63);
        trim_wstr(cfg.notify_peak_limit_body,  255);
    }
    catch (...) {
        cfg.config_error = "TOML parse failed: " + path;
    }

    cfg.priority_check_interval_sec = std::clamp(cfg.priority_check_interval_sec, 1, 300);
    // 優先度範囲幅のサニティチェック
    // 各値を [0, 49] にクランプする。両者の合計が最大 98 に抑えられるため NORMAL 範囲（100 - visible - hidden > 0）の消失を防ぐ。
    cfg.priority_visible_range_pct  = std::clamp(cfg.priority_visible_range_pct,  0, 49);
    cfg.priority_hidden_range_pct   = std::clamp(cfg.priority_hidden_range_pct,   0, 49);

    // Usage API ポーリング間隔のサニティチェック（30〜3600 秒）
    cfg.claude_usage_interval_sec = std::clamp(cfg.claude_usage_interval_sec, 30, 3600);

    // 5h 増加分濃色オーバーレイのウィンドウ幅サニティチェック（0〜60 分）
    // 上限 60 分は保持メモリの暴走防止と、5h ウィンドウ内で意味のある時間幅。0 は機能無効を意味する
    cfg.claude_delta_window_min = std::clamp(cfg.claude_delta_window_min, 0, 60);

    // 使い切り不能検知のサニティチェック
    // 目標到達率は 0〜100%、観測ウィンドウ幅は 30 分（PACE_MIN_SPAN_SECS 未満だと
    // 傾きが常に推定不可＝検知が恒久的に沈黙するため、これを下限とする）〜1 日、
    // 猶予時間・除外しきい値は各ウィンドウ長（5h=300 分、7d=10080 分）を上限とする
    cfg.claude_underuse_warn_pct      = std::clamp(cfg.claude_underuse_warn_pct,      0.f, 100.f);
    cfg.claude_underuse_pace_window_min = std::clamp(cfg.claude_underuse_pace_window_min, 30, 1440);
    cfg.claude_underuse_grace_5h_min  = std::clamp(cfg.claude_underuse_grace_5h_min,  0, 300);
    cfg.claude_underuse_grace_7d_min  = std::clamp(cfg.claude_underuse_grace_7d_min,  0, 10080);
    cfg.claude_underuse_ignore_5h_min = std::clamp(cfg.claude_underuse_ignore_5h_min, 0, 300);
    cfg.claude_underuse_ignore_7d_min = std::clamp(cfg.claude_underuse_ignore_7d_min, 0, 10080);

    // ガードトーン長のサニティチェック（0〜10 秒）
    cfg.guard_tone_ms = std::clamp(cfg.guard_tone_ms, 0, 10000);

    // win_width のサニティチェック
    //
    // win_width が 0 以下だと Direct2D のレンダーターゲット作成が失敗する。
    cfg.win_width = std::max(80, cfg.win_width);

    // 警告閾値のサニティチェック
    cfg.warn_cpu_pct       = std::clamp(cfg.warn_cpu_pct,       0.f, 100.f);
    cfg.warn_gpu_pct       = std::clamp(cfg.warn_gpu_pct,       0.f, 100.f);
    cfg.warn_mem_pct        = std::clamp(cfg.warn_mem_pct,        0.f, 100.f);
    cfg.warn_disk_space_pct = std::clamp(cfg.warn_disk_space_pct, 0.f, 100.f);
    cfg.warn_claude_5h_pct = std::clamp(cfg.warn_claude_5h_pct, 0.f, 100.f);
    cfg.warn_claude_7d_pct = std::clamp(cfg.warn_claude_7d_pct, 0.f, 100.f);
    cfg.warn_claude_over   = std::max(0.f, cfg.warn_claude_over);
    cfg.warn_disk_gbh      = std::max(0.f, cfg.warn_disk_gbh);
    // caution < critical の不変式を保つため、caution の上限を critical より 10 低い 190 とする。
    // 200 のまま許容すると critical 補正時に「min(caution+10, 200) == caution」となり caution == critical で退化する
    cfg.warn_temp_caution  = std::clamp(cfg.warn_temp_caution,  0.f, 190.f);
    cfg.warn_temp_critical = std::clamp(cfg.warn_temp_critical, 0.f, 200.f);
    // caution >= critical になると温度注意が表示されなくなるため、差を確保する
    if (cfg.warn_temp_caution >= cfg.warn_temp_critical)
        cfg.warn_temp_critical = std::min(cfg.warn_temp_caution + 10.f, 200.f);
    cfg.warn_uptime_days = std::max(0, cfg.warn_uptime_days);
    cfg.warn_processes   = std::clamp(cfg.warn_processes, 0, 999999);
    cfg.warn_threads     = std::clamp(cfg.warn_threads,   0, 999999);
    cfg.warn_handles     = std::clamp(cfg.warn_handles,   0, 9999999);

    // 警告音リセット閾値のサニティチェック
    //
    // リセット閾値は警告閾値未満でなければヒステリシスが機能しないため、強制補正する。
    cfg.reset_cpu_pct       = std::clamp(cfg.reset_cpu_pct,       0.f, 100.f);
    cfg.reset_gpu_pct       = std::clamp(cfg.reset_gpu_pct,       0.f, 100.f);
    cfg.reset_mem_pct        = std::clamp(cfg.reset_mem_pct,        0.f, 100.f);
    cfg.reset_disk_space_pct = std::clamp(cfg.reset_disk_space_pct, 0.f, 100.f);
    cfg.reset_temp          = std::clamp(cfg.reset_temp,          0.f, 200.f);
    cfg.reset_disk_gbh      = std::max(0.f, cfg.reset_disk_gbh);
    cfg.reset_claude_5h_pct = std::clamp(cfg.reset_claude_5h_pct, 0.f, 100.f);
    cfg.reset_claude_7d_pct = std::clamp(cfg.reset_claude_7d_pct, 0.f, 100.f);
    auto clamp_below = [](float& reset, float warn, float margin) {
        if (reset >= warn) reset = std::max(0.f, warn - margin);
    };
    clamp_below(cfg.reset_cpu_pct,        cfg.warn_cpu_pct,        5.f);
    clamp_below(cfg.reset_gpu_pct,        cfg.warn_gpu_pct,        5.f);
    clamp_below(cfg.reset_mem_pct,        cfg.warn_mem_pct,        5.f);
    clamp_below(cfg.reset_disk_space_pct, cfg.warn_disk_space_pct, 5.f);
    clamp_below(cfg.reset_temp,           cfg.warn_temp_critical,  5.f);
    clamp_below(cfg.reset_disk_gbh,       cfg.warn_disk_gbh,       1.f);
    clamp_below(cfg.reset_claude_5h_pct,  cfg.warn_claude_5h_pct,  5.f);
    clamp_below(cfg.reset_claude_7d_pct,  cfg.warn_claude_7d_pct,  5.f);

    return cfg;
}
