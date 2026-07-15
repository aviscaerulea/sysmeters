// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#pragma once
#include <cstdint>
#include <string>

// Claude アカウント単位の設定
//
// メイン/サブの 2 アカウントを区別して保持する。
// - メイン：固定で `%USERPROFILE%\.claude` を使うため config_dir は空
// - サブ：TOML `[claude_sub] config_dir` でディレクトリを指定する
// `enable` は TOML 上の明示的 ON/OFF キー。サブはデフォルト OFF とし、ユーザの明示有効化を要求する。
// ロード時に config_dir 検証に失敗すると enable=false に強制される
struct ClaudeAccountConfig {
    std::wstring name         = L"Main";   // ヘッダ表示ラベル（[claude] name / [claude_sub] name）
    std::wstring config_dir;               // .claude ディレクトリの絶対パス（空=メイン: ~/.claude）
    bool         nudge_enable = false;     // このアカウントで nudge を発火させるか
    bool         enable       = false;     // アカウント機能の有効化（メインはロード時に true 強制）
};

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
    uint32_t col_claude_scoped_bar = 0xCE5C4A;  // 朱色（上位モデル専用 7d ミニバー。claude_bar のアンバーと親和する暖色）
    uint32_t col_claude_scoped_bar_warn = 0xEF5350;  // 警告赤（上位モデル専用 7d ミニバーが 100% 到達時。COL_WARN_RED と同値）
    // 使い切り不能検知（underuse）発火中の 7d バー未使用部分の背景色。
    // 通常のバー背景（0x2A2A2A）よりわずかに明るい寒色で、アンバー系の暖色塗りと対比させ
    // 「消費が足りない」ことを控えめに示す。
    uint32_t col_claude_underuse_bg = 0x243048;  // 淡い暗青
    uint32_t col_cpu_core   = 0xCC923E;  // アンバー（同上）

    // 警告色の閾値
    float warn_cpu_pct       = 95.f;  // CPU 使用率（%）
    float warn_gpu_pct       = 95.f;  // GPU 使用率（%）
    float warn_mem_pct       = 90.f;  // RAM/VRAM 使用率（%）
    float warn_disk_space_pct = 90.f; // Disk Space 使用率（%）
    float warn_claude_5h_pct = 20.f;   // Claude 5h 理想ペースからの超過閾値（%）。デフォルト 20 = ウィンドウ 1/5（1 時間分）
    float warn_claude_7d_pct =  7.14f; // Claude 7d 理想ペースからの超過閾値（%）。デフォルト 7.14 = ウィンドウ 1/14（12 時間分）
    float warn_claude_over   =  0.f;  // Claude 超過料金の警告閾値（ドル）。デフォルト 0.f = $0 超えで即赤表示
    float warn_disk_gbh      = 10.f;  // Disk 書き込み量（GB/h）
    float warn_temp_caution  = 70.f;  // 温度注意・オレンジ表示（℃）
    float warn_temp_critical = 90.f;  // 温度危険・赤表示（℃）
    int   warn_uptime_days   = 7;     // OS アップタイム（日）
    int   warn_processes     = 1000;  // プロセス数の警告閾値
    int   warn_threads       = 10000; // スレッド数の警告閾値
    int   warn_handles       = 1000000; // ハンドル数の警告閾値

    // 警告音設定
    bool  alert_sound        = true;  // 警告音有効/無効
    // ガードトーン長（ミリ秒、再生前後の 19kHz 不可聴トーン）
    // BLE ヘッドフォンの省電力移行を防ぐ。冒頭・末尾に共通で適用する。0 で無効。
    int   guard_tone_ms      = 1500;
    float reset_cpu_pct      = 90.f;  // CPU 使用率の警告音リセット閾値（%）
    float reset_gpu_pct      = 90.f;  // GPU 使用率の警告音リセット閾値（%）
    float reset_mem_pct       = 85.f;  // RAM/VRAM の警告音リセット閾値（%）
    float reset_disk_space_pct = 85.f; // Disk Space の警告音リセット閾値（%）
    float reset_temp         = 85.f;  // CPU/GPU/NVMe 温度の警告音リセット閾値（℃）
    float reset_disk_gbh     =  5.f;  // Disk 書き込みの警告音リセット閾値（GB/h）
    float reset_claude_5h_pct =  0.f;  // Claude 5h の警告音リセット閾値（%超過）。0 = 理想ペース以下に戻ったら即リセット
    float reset_claude_7d_pct =  0.f;  // Claude 7d の警告音リセット閾値（%超過）。0 = 理想ペース以下に戻ったら即リセット

    // Claude Code 表示設定（両アカウント共通）
    int  claude_usage_interval_sec = 120;  // Usage API のポーリング間隔（秒）
    // 5h バー上に「直近 N 分間で増加した使用率」を濃色（COL_WSL_MEM 相当）で重ね塗り表示するウィンドウ幅（分）。
    // 消費ペースを視覚化する補助表示。メイン/サブ共通。0 で機能無効。
    // サニティチェックで 0〜60 にクランプする。
    int  claude_delta_window_min = 5;
    // 7d バー上の増加分濃色オーバーレイのウィンドウ幅（分）。delta_window_min の 7d 版。
    // 7d 使い切り不能検知（underuse）の平均消費ペース算出ウィンドウも兼ねる。
    // 0 で 7d オーバーレイと underuse 検知の両方が無効。
    // サニティチェックで 0〜2880 にクランプする。（「直近ペース」の意味を保つ上限として 48 時間）
    int  claude_delta_window_7d_min = 720;
    // 上位モデル専用 7d ミニバーの縦幅（px）。0 = 非表示、最大 4（7d バー下の行内余白に収まる上限）
    int  claude_scoped_bar_px = 2;
    // 使い切り不能検知（underuse 警告、7d バー専用）
    // 直近 delta_window_7d_min 分の平均消費ペースで残り時間を消費し続けても
    // 予測到達率が underuse_warn_pct に届かない＝挽回不能を検知し、
    // 7d バーの未使用部分の背景を暗青（col_claude_underuse_bg）にする。
    // ウィンドウ開始から delta_window_7d_min 分が経過するまでと、ペース推定不可
    //（履歴不足・増加なし）の間は判定しない。（安全側）
    // 表示のみで警告音・Toast は発しない。メイン/サブ共通
    bool  claude_underuse_enable   = true;
    // 予測到達率の目標値（%）。外挿予測がこの値未満なら警告表示。
    // サニティチェックで 0〜100 にクランプする
    float claude_underuse_warn_pct = 98.f;
    // nudge 実行時のコマンドライン。
    // 最小コストで API 使用状況を更新させるため、安全モード、最軽量モデル、最低推論努力で
    // 即終了するプロンプトをデフォルトとする。
    // サブ向け実行時は CLAUDE_CONFIG_DIR 環境変数に claude_sub.config_dir を一時設定して起動する。
    // （Claude CLI に --config-dir コマンドオプションは存在せず、設定ディレクトリの上書きは
    // 環境変数経由のみ）
    std::string claude_nudge_cmd    = "claude.exe --safe-mode --model haiku --effort low -p \"say 'ok'\"";

    // 5h バー上に PT 平日 5:00〜10:59 を暗赤色で重ねるピーク時間帯背景表示。
    // 2026-05 に Anthropic がピーク時間帯レート制限を撤廃したためデフォルト OFF。
    bool show_peak_bar = false;

    // メイン/サブそれぞれのアカウント設定
    // メインはロード時に enable=true 強制、サブはデフォルト OFF
    ClaudeAccountConfig claude_main;
    ClaudeAccountConfig claude_sub;

    // Claude Code 制限強化時間 Toast 通知（ローカル平日 21:00 固定）
    // 2026-05 にピーク時間帯レート制限が撤廃されたためデフォルト OFF。
    bool         notify_peak_limit_enable = false;
    bool         notify_peak_limit_sound  = false;
    std::wstring notify_peak_limit_title  = L"Claude Code";
    std::wstring notify_peak_limit_body   = L"平日 21:00〜03:00 ピーク制限時間です";

    // 更新チェック設定
    // 起動時に GitHub の最新リリースを確認し、新版があれば Toast 通知とトレイメニューで知らせる。
    bool update_check_enabled = true;

    // プロセス優先度制御
    bool priority_control_enable     = false;  // 隠蔽率に応じた優先度自動制御の ON/OFF
    int  priority_check_interval_sec = 5;      // 隠蔽率チェック周期（秒）
    int  priority_visible_range_pct  = 20;     // 隠蔽率がこれ未満で ABOVE_NORMAL（可視判定範囲幅、%）
    int  priority_hidden_range_pct   = 20;     // 隠蔽率が 100 - これ以上で BELOW_NORMAL（隠蔽判定範囲幅、%）

    // ログ出力先ディレクトリ（実行ファイルからの相対パス、または絶対パス）
    std::string log_dir = "logs";

    // 設定読み込み時のエラーメッセージ（空ならエラーなし）
    // load_config は log_init より前に呼ばれるためログ出力できない。
    // log_init の直後にこのフィールドを確認してログ出力する。
    std::string config_error;

    // 設定ファイルの試行パスと読み込み結果（UTF-8 のフルパス）
    // 起動時のトラブルシュート用。load_config は log_init より前に呼ばれるため、
    // ここに記録しておき、log_init 後に main 側で log_info 出力する。
    // base_path_used は load_config 引数のフルパス、loaded は ifstream::is_open() 成功フラグ。
    // local_path_used は base のファイル名拡張子直前に ".local" を挿入したパス。
    std::string base_path_used;
    bool        base_path_loaded  = false;
    std::string local_path_used;
    bool        local_path_loaded = false;
};

// TOML ファイルからの設定読み込み
// ファイルが存在しない場合はデフォルト値を返す。
AppConfig load_config(const std::string& path);
