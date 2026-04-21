// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#include "config.hpp"
#include <toml.hpp>
#include <algorithm>
#include <fstream>

AppConfig load_config(const std::string& path) {
    AppConfig cfg;

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return cfg;  // ファイルなし → デフォルト値で返す

    try {
        auto data = toml::parse(ifs, path);

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
        cfg.warn_mem_pct       = get_float("threshold", "mem_pct",       cfg.warn_mem_pct);
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

        try { cfg.alert_sound = toml::find_or<bool>(data, "threshold", "alert_sound", cfg.alert_sound); }
        catch (...) {}
        cfg.reset_cpu_pct       = get_float("threshold", "reset_cpu_pct",       cfg.reset_cpu_pct);
        cfg.reset_gpu_pct       = get_float("threshold", "reset_gpu_pct",       cfg.reset_gpu_pct);
        cfg.reset_mem_pct       = get_float("threshold", "reset_mem_pct",       cfg.reset_mem_pct);
        cfg.reset_temp          = get_float("threshold", "reset_temp",          cfg.reset_temp);
        cfg.reset_disk_gbh      = get_float("threshold", "reset_disk_gbh",      cfg.reset_disk_gbh);
        cfg.reset_claude_5h_pct = get_float("threshold", "reset_claude_5h_pct", cfg.reset_claude_5h_pct);
        cfg.reset_claude_7d_pct = get_float("threshold", "reset_claude_7d_pct", cfg.reset_claude_7d_pct);

        try { cfg.priority_control_enable     = toml::find_or<bool>(data, "process", "priority_control",   cfg.priority_control_enable); } catch (...) {}
        cfg.priority_check_interval_sec = get_int("process", "check_interval_sec", cfg.priority_check_interval_sec);

        cfg.log_dir = toml::find_or<std::string>(data, "log", "dir", cfg.log_dir);
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
    cfg.warn_mem_pct       = std::clamp(cfg.warn_mem_pct,       0.f, 100.f);
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
    cfg.warn_handles     = std::clamp(cfg.warn_handles,   0, 999999);

    // 警告音リセット閾値のサニティチェック
    //
    // リセット閾値は警告閾値未満でなければヒステリシスが機能しないため、強制補正する。
    cfg.reset_cpu_pct       = std::clamp(cfg.reset_cpu_pct,       0.f, 100.f);
    cfg.reset_gpu_pct       = std::clamp(cfg.reset_gpu_pct,       0.f, 100.f);
    cfg.reset_mem_pct       = std::clamp(cfg.reset_mem_pct,       0.f, 100.f);
    cfg.reset_temp          = std::clamp(cfg.reset_temp,          0.f, 200.f);
    cfg.reset_disk_gbh      = std::max(0.f, cfg.reset_disk_gbh);
    cfg.reset_claude_5h_pct = std::clamp(cfg.reset_claude_5h_pct, 0.f, 100.f);
    cfg.reset_claude_7d_pct = std::clamp(cfg.reset_claude_7d_pct, 0.f, 100.f);
    if (cfg.reset_cpu_pct       >= cfg.warn_cpu_pct)       cfg.reset_cpu_pct       = std::max(0.f, cfg.warn_cpu_pct       - 5.f);
    if (cfg.reset_gpu_pct       >= cfg.warn_gpu_pct)       cfg.reset_gpu_pct       = std::max(0.f, cfg.warn_gpu_pct       - 5.f);
    if (cfg.reset_mem_pct       >= cfg.warn_mem_pct)       cfg.reset_mem_pct       = std::max(0.f, cfg.warn_mem_pct       - 5.f);
    if (cfg.reset_temp          >= cfg.warn_temp_critical) cfg.reset_temp          = std::max(0.f, cfg.warn_temp_critical - 5.f);
    if (cfg.reset_disk_gbh      >= cfg.warn_disk_gbh)      cfg.reset_disk_gbh      = std::max(0.f, cfg.warn_disk_gbh      - 1.f);
    if (cfg.reset_claude_5h_pct >= cfg.warn_claude_5h_pct) cfg.reset_claude_5h_pct = std::max(0.f, cfg.warn_claude_5h_pct - 5.f);
    if (cfg.reset_claude_7d_pct >= cfg.warn_claude_7d_pct) cfg.reset_claude_7d_pct = std::max(0.f, cfg.warn_claude_7d_pct - 5.f);

    return cfg;
}
