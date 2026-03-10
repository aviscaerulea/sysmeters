// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#pragma once
#include <cstdint>
#include <string>

// アプリケーション設定（TOML から読み込む）
struct AppConfig {
    // ウィンドウ
    int   win_x     = 20;
    int   win_y     = 20;
    int   win_width = 500;

    // 配色（0xRRGGBB）
    uint32_t col_background = 0x1A1A1A;
    uint32_t col_border     = 0x3C3C3C;
    uint32_t col_text       = 0xD4D4D4;
    uint32_t col_graph_fill = 0xCC923E;  // アンバー（Material amber 約 20% 減光）
    uint32_t col_disk_read  = 0xCCA366;  // 明るいアンバー（同上）
    uint32_t col_disk_write = 0xCC861E;  // 暗めアンバー（同上）
    uint32_t col_net_recv   = 0xCCA366;  // 明るいアンバー（同上）
    uint32_t col_net_send   = 0xCC923E;  // アンバー（同上）
    uint32_t col_claude_bar = 0xCC923E;  // アンバー（同上）
    uint32_t col_cpu_core   = 0xCC923E;  // アンバー（同上）

    // ログ出力先ディレクトリ（実行ファイルからの相対パス、または絶対パス）
    std::string log_dir = "logs";
};

// TOML ファイルから設定を読み込む。ファイルが存在しない場合はデフォルト値を使用する。
AppConfig load_config(const std::string& path);
