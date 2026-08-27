// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#pragma once
#include <ctime>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "ring_buffer.hpp"

// 全メトリクスデータ構造体

// OS 情報：マシン名、OS バージョン、アップタイム
// マシン名は起動時 1 回取得、アップタイムは 60 秒ごと、OS ラベルは 1 時間ごとに更新する
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

    // CPU 使用率トップのプロセス（トレイの「トッププロセス表示」ON 時のみ収集）
    // 名前は ".exe" を除いた exe 名で、同名 exe を合算した結果の最上位 1 件。
    // （四捨五入した整数 % が同値の間は直前選出を優先維持するため、生値の厳密な最大とは限らない）
    // 空文字は「実測値なし」を意味する。（表示 OFF、差分未確定、丸めて 0% のいずれか）
    // 描画側は空文字でも残像期間内なら直前の凍結値を薄く表示する。（renderer の残像管理を参照）
    // pct はシステム全体を 100% とする尺度で、total_pct と同一スケール
    wchar_t top_proc_name[64] = {};
    float   top_proc_pct      = 0.f;
};

// GPU：使用率（面グラフ）+ 温度（横バー）
struct GpuMetrics {
    RingBuffer<float, 60> usage_history; // 使用率履歴（%）
    float usage_pct    = 0.f;
    float temp_celsius = 0.f;
    bool  avail        = false;         // NVML ロード成功フラグ
    char  name[48]     = {};            // GPU 名（NVML 取得）

    // GPU 使用率トップのプロセス（トレイの「トッププロセス表示」ON 時のみ収集）
    // PDH の GPU Engine 使用率から 3D/Compute 系エンジンのみを対象に、同名 exe ×
    // アダプタ × エンジン種別の単位で合算し、その最大値を採った最上位 1 件。
    // （usage_pct が数えない Decode/Copy 等や別アダプタの負荷を混ぜないため。
    // 四捨五入した整数 % が同値の間は直前選出を優先維持するため、厳密な最大とは限らない）
    // 空文字の意味は CpuMetrics の同名フィールドと同一。
    // pct は usage_pct と同じ「3D/Compute 稼働率」を意図した参考値だが、測定源（PDH）と
    // 測定窓（約 2.7 秒平均）が異なるため厳密には一致しない。
    // 同種エンジン複数構成では合算が 100% を超え得るため書き込み側で 0〜100 にクランプする
    wchar_t top_proc_name[64] = {};
    float   top_proc_pct      = 0.f;
};

// RAM：横バー + 使用量表示 + ハードフォールト重畳グラフ
struct MemMetrics {
    float usage_pct = 0.f;
    float used_gb   = 0.f;
    float total_gb  = 0.f;
    float wsl_gb    = 0.f;  // vmmem / vmmemWSL プロセスの Working Set 合計（WSL 非使用時は 0）
    // 使用率履歴（%）：警告音の平均判定専用（描画は瞬間値の横バーのみで履歴を使わない）
    // update() から push される。起動時の初回取得を除き TIMER_SLOW（2.0 秒）間隔のため、
    // N サンプルは概ね N × 2 秒の窓に相当する
    RingBuffer<float, 60> usage_history;
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

// 監視対象の固定ドライブ数上限
// AlertManager の警告 ID 予約数（DISK_0..7 / TEMP_NVME_0..7）と一致させる必要がある
// （警告ビットマスク uint32_t の 32 個制約に、他の監視項目分を差し引いた残りから逆算した値）
inline constexpr int kMaxDiskDrives = 8;

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

// Claude 使用率の時系列サンプル
// 5h/7d バーの濃色オーバーレイ用履歴で共用する。7d は使い切り不能検知（underuse）の
// 平均消費ペース算出にも使う。
// ts はサンプル取得時刻（UTC time_t）、pct はそのときの使用率（%）
struct ClaudeHistorySample {
    time_t ts  = 0;
    float  pct = 0.f;
};

// Claude レートリミットウィンドウ幅（秒）
constexpr double CLAUDE_WIN_5H_SECS = 5.0 * 3600;
constexpr double CLAUDE_WIN_7D_SECS = 7.0 * 24 * 3600;

// 均等消費ペースの理想位置（%）を現在時刻基準で算出する
// resets_ts（UTC）までの残り時間から経過割合を 0〜100 で返す。
// 未取得（resets_ts <= 0）、またはリセット時刻がウィンドウ幅より先（ウィンドウ外）の
// ときは 0 を返し、呼び出し側は 0 を「ペース不定＝判定しない」として扱う契約。
// 警告音判定（alert）と描画（警告色・緑線）が本関数を共用し、判定の乖離を防ぐ
inline float claude_expected_pct(time_t resets_ts, double window_secs) {
    if (resets_ts <= 0) return 0.f;  // 未取得（-1）または epoch（0）は無効
    double remaining = static_cast<double>(resets_ts) - static_cast<double>(time(nullptr));
    if (remaining < 0.0) remaining = 0.0;
    if (remaining > window_secs) return 0.f;
    float expected = static_cast<float>((window_secs - remaining) / window_secs * 100.0);
    return expected < 0.f ? 0.f : (expected > 100.f ? 100.f : expected);
}

// Claude Code：レートリミット + セッション数
// アカウント別（メイン/サブ）にインスタンスを持つ。account_label は描画ヘッダの表示名
struct ClaudeMetrics {
    float five_h_pct    = 0.f;
    float seven_d_pct   = 0.f;
    wchar_t five_h_reset[20]  = {};      // L"HH:MM" 形式
    wchar_t seven_d_reset[32] = {};      // L"M/D 曜 HH:MM" 形式
    time_t five_h_resets_ts  = -1;     // 5h ウィンドウの resets_at（UTC time_t、未取得時 -1）
    time_t seven_d_resets_ts = -1;     // 7d ウィンドウの resets_at（UTC time_t、未取得時 -1）
    char  plan_label[16]    = {};       // "Max5", "Pro" 等
    int   session_count = 0;
    bool  avail         = false;
    bool  fetch_error   = false;      // Usage API 取得失敗フラグ（Err 表示用）
    // OAuth トークン未取得フラグ（credentials.json 不在・パース失敗・accessToken 空）。
    // ログアウト状態を示し、立っている間は fetch_error に依らず Logout を表示する
    bool  token_missing = false;
    float extra_used_dollars = 0.f;   // 超過使用額（ドル換算：used_credits / 100）
    bool  extra_enabled      = false; // 超過料金が有効か（is_enabled）
    // モデルスコープ 7d 使用率（%）。Usage API limits[] の kind=="weekly_scoped" の percent。
    // Fable 等の上位モデル専用 7d 枠の消費率で、分母は専用枠の週次総容量（weekly_all とは別枠）。
    // API がエントリを返さない場合は -1（未提供 = ミニバー非表示）
    float seven_d_scoped_pct = -1.f;
    wchar_t account_label[24] = L"Main"; // 描画ヘッダ表示名（TOML name より反映）
    bool  account_enabled    = false; // このアカウントが有効化されているか（サブ未構成時 false）
    wchar_t fetched_at[8] = L"";      // Usage API 取得時刻（ローカル "H:MM"、時はゼロ埋めなし。未取得時は空文字）
    time_t fetched_ts = 0;            // Usage API 実フェッチ時刻（UTC time_t、fetched_at の元値。未取得時 0）
    // 5h / 7d 使用率の時系列（各 delta ウィンドウの N+1 分を保持）
    // apply_result 呼び出し時に push し、保持期間外を先頭から破棄する。
    // 描画側で「現在値」と「N 分前の値」の差分を濃色オーバーレイとして表示する。
    // seven_d_history は使い切り不能検知（underuse）の平均消費ペース算出にも使い、
    // 保持期間より古い直近 1 サンプル（アンカー）が停止明けの基準として残る場合がある
    std::vector<ClaudeHistorySample> five_h_history;
    std::vector<ClaudeHistorySample> seven_d_history;
};

// 全メトリクスを束ねる集約構造体
struct AllMetrics {
    OsMetrics   os;
    CpuMetrics  cpu;
    GpuMetrics  gpu;
    MemMetrics  mem;
    VramMetrics vram;
    // 起動時に検出した固定ドライブ（レター昇順、最大 kMaxDiskDrives 台）
    // DiskCollector::init() 呼び出し前に要素数を確定させ、以後 resize しないこと
    // （RingBuffer の履歴データがコピー・移動で破壊されるため）
    std::vector<DiskMetrics> disks;
    NetMetrics       net;
    ClaudeMetrics claude_main;
    ClaudeMetrics claude_sub;
};
