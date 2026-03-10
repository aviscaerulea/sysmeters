// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#include "config.hpp"
#include <toml.hpp>
#include <fstream>

AppConfig load_config(const std::string& path) {
    AppConfig cfg;

    std::ifstream ifs(path);
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

        cfg.log_dir = toml::find_or<std::string>(data, "log", "dir", cfg.log_dir);
    }
    catch (...) {}  // パース失敗時はデフォルト値を使用

    return cfg;
}
