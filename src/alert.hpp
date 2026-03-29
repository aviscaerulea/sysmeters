// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "config.hpp"
#include "metrics.hpp"

// 警告音・Toast 通知管理
//
// 各監視項目のヒステリシス状態を保持し、閾値超過時に WASAPI で alert.wav を再生し、
// Toast 通知を発行する。BLE ヘッドフォン対策として、再生前後に 19kHz 不可聴トーンを挿入する。
class AlertManager {
public:
    // 監視項目 ID（ビットマスクで管理するため 32 以下を維持する）
    enum Id {
        CPU, GPU, RAM, VRAM, DISK_C, DISK_D,
        TEMP_CPU, TEMP_GPU, TEMP_NVME_C, TEMP_NVME_D,
        DISK_GBH, UPTIME,
        CLAUDE_5H, CLAUDE_7D, CLAUDE_OVER,
        COUNT_
    };
    static_assert(COUNT_ <= 32, "Id count exceeds uint32_t bitmask capacity");

    // 監視項目 ID に対応する表示ラベルを返す
    static const wchar_t* label(Id id);

    // exe ディレクトリから alert.wav パスを解決し、存在確認する
    void init();

    // シャットダウンフラグを立て、再生スレッドの終了を待つ
    void shutdown();

    // 全メトリクスを評価し、新規に発火した項目のビットマスクを返す
    //
    // 戻り値: 新規発火した Id のビットが立った uint32_t（0 = 発火なし）。
    // 警告音・Toast の発行は window 側が戻り値を見て行う。
    uint32_t check(const AllMetrics& m, const AppConfig& cfg);

private:
    // CPU/GPU 警告判定に使う平均サンプル数（直近 10 サンプル ≒ 約 9 秒）
    static constexpr std::size_t AVG_SAMPLES = 10;

    bool           fired_[COUNT_] = {};     // true = 発火済み（リセット閾値未達まで再発火しない）
    wchar_t        wav_path_[MAX_PATH] = {};
    bool           wav_avail_    = false;
    HANDLE         sound_thread_ = nullptr;
    volatile bool  shutdown_     = false;   // true = アプリ終了中（再生中断用）

    // バックグラウンドスレッドで WASAPI 再生を開始する
    void play();

    static DWORD WINAPI sound_thread_func(LPVOID param);
};
