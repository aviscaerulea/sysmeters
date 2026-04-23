// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#include "config.hpp"
#include <toml.hpp>
#include <algorithm>
#include <fstream>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

AppConfig load_config(const std::string& path) {
    AppConfig cfg;

    // バイナリモードで開く
    // toml11 の istream 版がテキストモードの CRLF 変換でバッファ末尾に NUL を埋め込むバグへの回避策。
    // バイナリモードのまま toml11 に渡しても CRLF は許容される。
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return cfg;  // ファイルなし → デフォルト値で返す

    try {
        auto data = toml::parse(ifs, path);

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
            // 整数リテラル（例：95）と浮動小数点リテラル（例：95.0）の両方に対応する
            try { return static_cast<float>(toml::find_or<double>(data, sec, key, static_cast<double>(def))); }
            catch (...) {}
            try { return static_cast<float>(toml::find_or<int64_t>(data, sec, key, static_cast<int64_t>(def))); }
            catch (...) { return def; }
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

        cfg.priority_control_enable     = get_bool("process", "priority_control",   cfg.priority_control_enable);
        cfg.priority_check_interval_sec = get_int ("process", "check_interval_sec", cfg.priority_check_interval_sec);

        cfg.log_dir = toml::find_or<std::string>(data, "log", "dir", cfg.log_dir);

        cfg.notify_peak_limit_enable = get_bool ("notify", "peak_limit_enable", cfg.notify_peak_limit_enable);
        cfg.notify_peak_limit_sound  = get_bool ("notify", "peak_limit_sound",  cfg.notify_peak_limit_sound);
        cfg.notify_peak_limit_title  = get_wstr ("notify", "peak_limit_title",  cfg.notify_peak_limit_title);
        cfg.notify_peak_limit_body   = get_wstr ("notify", "peak_limit_body",   cfg.notify_peak_limit_body);
        // szInfoTitle は 64 wchar、szInfo は 256 wchar が上限
        if (cfg.notify_peak_limit_title.size() > 63) cfg.notify_peak_limit_title.resize(63);
        if (cfg.notify_peak_limit_body.size()  > 255) cfg.notify_peak_limit_body.resize(255);
    }
    catch (...) {
        cfg.config_error = "TOML parse failed: " + path;
    }

    cfg.priority_check_interval_sec = std::clamp(cfg.priority_check_interval_sec, 1, 300);

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
    cfg.warn_temp_caution  = std::clamp(cfg.warn_temp_caution,  0.f, 200.f);
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
    if (cfg.reset_cpu_pct       >= cfg.warn_cpu_pct)       cfg.reset_cpu_pct       = std::max(0.f, cfg.warn_cpu_pct       - 5.f);
    if (cfg.reset_gpu_pct       >= cfg.warn_gpu_pct)       cfg.reset_gpu_pct       = std::max(0.f, cfg.warn_gpu_pct       - 5.f);
    if (cfg.reset_mem_pct        >= cfg.warn_mem_pct)        cfg.reset_mem_pct        = std::max(0.f, cfg.warn_mem_pct        - 5.f);
    if (cfg.reset_disk_space_pct >= cfg.warn_disk_space_pct) cfg.reset_disk_space_pct = std::max(0.f, cfg.warn_disk_space_pct - 5.f);
    if (cfg.reset_temp          >= cfg.warn_temp_critical) cfg.reset_temp          = std::max(0.f, cfg.warn_temp_critical - 5.f);
    if (cfg.reset_disk_gbh      >= cfg.warn_disk_gbh)      cfg.reset_disk_gbh      = std::max(0.f, cfg.warn_disk_gbh      - 1.f);
    if (cfg.reset_claude_5h_pct >= cfg.warn_claude_5h_pct) cfg.reset_claude_5h_pct = std::max(0.f, cfg.warn_claude_5h_pct - 5.f);
    if (cfg.reset_claude_7d_pct >= cfg.warn_claude_7d_pct) cfg.reset_claude_7d_pct = std::max(0.f, cfg.warn_claude_7d_pct - 5.f);

    return cfg;
}
