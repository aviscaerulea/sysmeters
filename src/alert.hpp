// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "config.hpp"
#include "metrics.hpp"
#include <vector>

// 警告音・Toast 通知管理
//
// 各監視項目のヒステリシス状態を保持し、閾値超過時に WASAPI で alert.wav を再生し、
// Toast 通知を発行する。BLE ヘッドフォン対策として、再生前後に 19kHz 不可聴トーンを挿入する。
class AlertManager {
public:
    // 監視項目 ID（ビットマスクで管理するため 32 以下を維持する）
    // DISK_0..7 / TEMP_NVME_0..7 は検出した固定ドライブの添字（kMaxDiskDrives 台分）に対応する。
    // 実際の検出台数がこれ未満でも未使用の添字が残るだけで安全（check() が発火させない）。
    enum Id {
        CPU, GPU, RAM, VRAM,
        DISK_0, DISK_1, DISK_2, DISK_3, DISK_4, DISK_5, DISK_6, DISK_7,
        TEMP_CPU, TEMP_GPU,
        TEMP_NVME_0, TEMP_NVME_1, TEMP_NVME_2, TEMP_NVME_3,
        TEMP_NVME_4, TEMP_NVME_5, TEMP_NVME_6, TEMP_NVME_7,
        DISK_GBH, UPTIME,
        CLAUDE_MAIN_5H, CLAUDE_MAIN_7D, CLAUDE_MAIN_OVER,
        CLAUDE_SUB_5H,  CLAUDE_SUB_7D,  CLAUDE_SUB_OVER,
        CLAUDE_MAIN_SCOPED, CLAUDE_SUB_SCOPED,
        COUNT_
    };
    static_assert(COUNT_ <= 32, "Id count exceeds uint32_t bitmask capacity");
    static_assert(DISK_7 - DISK_0 + 1 == kMaxDiskDrives, "DISK_* reservation must match kMaxDiskDrives");
    static_assert(TEMP_NVME_7 - TEMP_NVME_0 + 1 == kMaxDiskDrives, "TEMP_NVME_* reservation must match kMaxDiskDrives");

    // 監視項目 ID に対応する表示ラベルを返す
    // ディスク系ラベルはドライブレターを含むため init() で構築済みの内容を返す
    const wchar_t* label(Id id) const;

    // exe ディレクトリから alert.wav パスを解決し、存在確認する
    // cfg からガードトーン長を保持し、drives からディスク系ラベル（レター入り）を構築する
    void init(const AppConfig& cfg, const std::vector<char>& drives);

    // シャットダウンフラグを立て、再生スレッドの終了を待つ
    void shutdown();

    // 外部からの明示的な警告音再生（スケジュール通知など）
    // wav が利用不可の場合は何もしない。cfg.alert_sound の ON/OFF には関与しない
    void play_external();

    // 全メトリクスを評価し、新規に発火した項目のビットマスクを返す
    //
    // 戻り値：新規発火した Id のビットが立った uint32_t（0 = 発火なし）。
    // mute が true の場合、閾値判定とヒステリシス管理は通常どおり行うが警告音の再生を抑制する。
    // ただし always_mask のビットに対応する項目は mute 中でも警告音を再生する
    // （フルスクリーン抑制の例外「常に警告通知を有効にする」。mute が false のとき always_mask は無視される）。
    // Toast の発行は window 側が戻り値を見て行う。
    uint32_t check(const AllMetrics& m, const AppConfig& cfg, bool mute = false,
                   uint32_t always_mask = 0);

private:
    // CPU/GPU 警告判定に使う平均サンプル数（直近 10 サンプル ≒ 約 9 秒）
    static constexpr std::size_t AVG_SAMPLES = 10;

    bool           fired_[COUNT_] = {};     // true = 発火済み（リセット閾値未達まで再発火しない）
    wchar_t        wav_path_[MAX_PATH] = {};
    bool           wav_avail_    = false;
    int            guard_tone_ms_ = 1500;   // ガードトーン長（再生前後の 19kHz 不可聴トーン、ms）
    HANDLE         sound_thread_ = nullptr;

    // 検出ドライブ数と、init() で構築するディスク系ラベル（例：L"ディスク C: 使用率"）
    // 添字は DISK_0/TEMP_NVME_0 起点のオフセットに対応する
    int     disk_count_ = 0;
    wchar_t disk_label_[kMaxDiskDrives][24] = {};
    wchar_t nvme_label_[kMaxDiskDrives][24] = {};

    // バックグラウンドスレッドで WASAPI 再生を開始する
    void play();

    static DWORD WINAPI sound_thread_func(LPVOID param);
};
