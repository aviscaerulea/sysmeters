// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#pragma once
#include <cstdint>
#include <string>

// アプリケーション設定（TOML から読み込む）
struct AppConfig {
    // ウィンドウ
    int   win_x     = 20;
    int   win_y     = 20;
    int   win_width = 460;

    // 配色（0xRRGGBB）
    uint32_t col_background = 0x1A1A1A;
    uint32_t col_border     = 0x3C3C3C;
    uint32_t col_text       = 0xD4D4D4;
    uint32_t col_graph_fill = 0xCC923E;  // アンバー（Material amber 約 20% 減光）
    uint32_t col_disk_read  = 0xDDA858;  // Read/recv 用：少し明るいアンバー
    uint32_t col_disk_write = 0x8C642A;  // Write/send 用：暗めアンバー
    uint32_t col_net_recv   = 0xDDA858;  // Read/recv 用：少し明るいアンバー（disk_read と統一）
    uint32_t col_net_send   = 0x8C642A;  // Write/send 用：暗めアンバー（disk_write と統一）
    uint32_t col_claude_bar = 0xCC923E;  // アンバー（同上）
    uint32_t col_cpu_core   = 0xCC923E;  // アンバー（同上）

    // 警告色の閾値
    float warn_cpu_pct       = 95.f;  // CPU 使用率（%）
    float warn_gpu_pct       = 95.f;  // GPU 使用率（%）
    float warn_mem_pct       = 90.f;  // RAM/VRAM/Disk Space 使用率（%）
    float warn_claude_5h_pct = 90.f;  // Claude 5h レートリミット（%）
    float warn_claude_7d_pct = 90.f;  // Claude 7d レートリミット（%）
    float warn_claude_over   =  0.f;  // Claude 超過料金の警告閾値（ドル）。デフォルト 0.f = $0 超えで即赤表示
    float warn_disk_gbh      = 10.f;  // Disk 書き込み量（GB/h）
    float warn_temp_caution  = 70.f;  // 温度注意・オレンジ表示（℃）
    float warn_temp_critical = 90.f;  // 温度危険・赤表示（℃）
    int   warn_uptime_days   = 7;     // OS アップタイム（日）

    // 警告音設定
    bool  alert_sound        = true;  // 警告音有効/無効
    float reset_cpu_pct      = 90.f;  // CPU 使用率の警告音リセット閾値（%）
    float reset_gpu_pct      = 90.f;  // GPU 使用率の警告音リセット閾値（%）
    float reset_mem_pct      = 85.f;  // RAM/VRAM/Disk Space の警告音リセット閾値（%）
    float reset_temp         = 85.f;  // CPU/GPU/NVMe 温度の警告音リセット閾値（℃）
    float reset_disk_gbh     =  5.f;  // Disk 書き込みの警告音リセット閾値（GB/h）
    float reset_claude_5h_pct = 85.f; // Claude 5h の警告音リセット閾値（%）
    float reset_claude_7d_pct = 85.f; // Claude 7d の警告音リセット閾値（%）

    // ログ出力先ディレクトリ（実行ファイルからの相対パス、または絶対パス）
    std::string log_dir = "logs";

    // 設定読み込み時のエラーメッセージ（空ならエラーなし）
    //
    // load_config は log_init より前に呼ばれるためログ出力できない。
    // log_init の直後にこのフィールドを確認してログ出力すること。
    std::string config_error;
};

// TOML ファイルからの設定読み込み
// ファイルが存在しない場合はデフォルト値を返す。
AppConfig load_config(const std::string& path);
