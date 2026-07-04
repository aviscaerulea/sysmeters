// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#pragma once
#include <ctime>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "ring_buffer.hpp"

// 全メトリクスデータ構造体

// OS 情報：マシン名、OS バージョン、アップタイム（起動時 1 回取得 + 毎秒更新）
struct OsMetrics {
    wchar_t machine_name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    wchar_t os_label[64] = {};    // "Windows 11 Pro (24H2 26100)" 形式
    ULONGLONG uptime_ms = 0;
};

// CPU：全体使用率（面グラフ）+ コア別縦バー + 温度（横バー）+ システム統計
struct CpuMetrics {
    RingBuffer<float, 60> total_history;  // 全体使用率履歴（%）
    float total_pct  = 0.f;
    int   core_count = 0;               // 論理コア数（collector init 時に設定）
    std::vector<float> core_pct;        // 論理コア別使用率（%）、core_count 要素
    float temp_celsius = 0.f;           // CPU 温度
    bool  temp_avail   = false;         // PawnIO 温度取得成功フラグ
    char  name[48]     = {};            // CPU ブランド名（CPUID 取得）
    int processes = 0;  // 実行中プロセス数
    int threads   = 0;  // 実行中スレッド数
    int handles   = 0;  // オープン中ハンドル数
};

// GPU：使用率（面グラフ）+ 温度（横バー）
struct GpuMetrics {
    RingBuffer<float, 60> usage_history; // 使用率履歴（%）
    float usage_pct    = 0.f;
    float temp_celsius = 0.f;
    bool  avail        = false;         // NVML ロード成功フラグ
    char  name[48]     = {};            // GPU 名（NVML 取得）
};

// RAM：横バー + 使用量表示 + ハードフォールト重畳グラフ
struct MemMetrics {
    float usage_pct = 0.f;
    float used_gb   = 0.f;
    float total_gb  = 0.f;
    float wsl_gb    = 0.f;  // vmmemWSL プロセスの Working Set 合計（WSL 非使用時は 0）
    RingBuffer<float, 60> hard_fault_history;  // ハードフォールト履歴（\Memory\Page Reads/sec）
};

// VRAM：面グラフ + 使用量表示
struct VramMetrics {
    RingBuffer<float, 60> usage_history;
    float usage_pct = 0.f;
    float used_gb   = 0.f;
    float total_gb  = 0.f;
    bool  avail     = false;
};

// Disk：I/O（Read/Write 面グラフ）+ 空き容量（横バー）+ S.M.A.R.T.
struct DiskMetrics {
    RingBuffer<float, 60> read_history;  // Read MB/s
    RingBuffer<float, 60> write_history; // Write MB/s
    float read_mbps  = 0.f;
    float write_mbps = 0.f;
    char  drive      = 'C';
    // ディスク空き容量（5 秒間隔で更新）
    float used_pct   = 0.f;  // 使用率（0〜100%）
    float used_gb    = 0.f;  // 使用量（GB）
    float total_gb   = 0.f;  // 総容量（GB）
    // NVMe S.M.A.R.T.（1 時間間隔で更新）
    int   phys_drive      = -1;    // 物理ドライブ番号（init 時に解決）
    float smart_write_gbh    = 0.f;  // 時間あたり書き込み量（GB/h）
    float smart_temp_celsius = 0.f;  // NVMe コンポジット温度（°C）
    bool  smart_avail        = false;
    bool  smart_temp_avail   = false;  // 温度センサー実装済みフラグ（kelvin==0 は未実装）
};

// Network：送受信分離の面グラフ + グローバル IP
struct NetMetrics {
    RingBuffer<float, 60> send_history;  // 送信 KB/s
    RingBuffer<float, 60> recv_history;  // 受信 KB/s
    float send_kbps = 0.f;
    float recv_kbps = 0.f;
    // グローバル IP（checkip.amazonaws.com から 5 分ごとに取得）
    wchar_t global_ip[48] = {};  // IPv4 / IPv6 アドレス文字列
    bool    ip_avail      = false;
};

// Claude 5h 使用率の時系列サンプル
// バー上に直近 N 分間の増加分を濃色オーバーレイ表示するための履歴要素。
// ts はサンプル取得時刻（UTC time_t）、pct はそのときの 5h 使用率（%）
struct ClaudeHistorySample {
    time_t ts  = 0;
    float  pct = 0.f;
};

// Claude Code：レートリミット + セッション数
// アカウント別（メイン/サブ）にインスタンスを持つ。account_label は描画ヘッダの表示名
struct ClaudeMetrics {
    float five_h_pct    = 0.f;
    float seven_d_pct   = 0.f;
    wchar_t five_h_reset[20]  = {};      // L"HH:MM" 形式
    wchar_t seven_d_reset[32] = {};      // L"M/D 曜 HH:MM" 形式
    float five_h_expected_pct  = 0.f;   // 5h ウィンドウの均等消費ペース（%）
    float seven_d_expected_pct = 0.f;   // 7d ウィンドウの均等消費ペース（%）
    time_t five_h_resets_ts  = -1;     // 5h ウィンドウの resets_at（UTC time_t、未取得時 -1）
    time_t seven_d_resets_ts = -1;     // 7d ウィンドウの resets_at（UTC time_t、未取得時 -1）
    char  plan_label[16]    = {};       // "Max5", "Pro" 等
    int   session_count = 0;
    bool  avail         = false;
    bool  fetch_error   = false;      // Usage API 取得失敗フラグ（ERR 表示用）
    float extra_used_dollars = 0.f;   // 超過使用額（ドル換算：used_credits / 100）
    bool  extra_enabled      = false; // 超過料金が有効か（is_enabled）
    wchar_t account_label[24] = L"Main"; // 描画ヘッダ表示名（TOML name より反映）
    bool  account_enabled    = false; // このアカウントが有効化されているか（サブ未構成時 false）
    wchar_t fetched_at[8] = L"";      // Usage API 取得時刻（ローカル "HH:MM"、未取得時は空文字）
    time_t fetched_ts = 0;            // Usage API 実フェッチ時刻（UTC time_t、fetched_at の元値。未取得時 0）
    // 現在ウィンドウ内で観測した使用量増加 TOP3 の平均レート（%/秒、0 = 推定不可）
    // 使い切り不能検知（underuse 警告）の「追い上げ可能ペース」として描画側で外挿に使う。
    // バケット（5h：30 分 / 7d：6 時間）完了ごとに collector が更新する
    float five_h_top3_rate  = 0.f;
    float seven_d_top3_rate = 0.f;
    // 5h 使用率の時系列（直近 N+1 分を保持）
    // apply_result 呼び出し時に push し、保持期間外を先頭から破棄する。
    // 描画側で「現在値」と「N 分前の値」の差分を濃色オーバーレイとして表示する
    std::vector<ClaudeHistorySample> five_h_history;
};

// 全メトリクスを束ねる集約構造体
struct AllMetrics {
    OsMetrics   os;
    CpuMetrics  cpu;
    GpuMetrics  gpu;
    MemMetrics  mem;
    VramMetrics vram;
    DiskMetrics      disk_c;
    DiskMetrics      disk_d;
    NetMetrics       net;
    ClaudeMetrics claude_main;
    ClaudeMetrics claude_sub;
};
