// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "config.hpp"
#include "metrics.hpp"

// 警告音管理
//
// 各監視項目のヒステリシス状態を保持し、閾値超過時に WASAPI で alert.wav を再生する。
// BLE ヘッドフォン対策として、再生前後に 19kHz 不可聴トーンを挿入する。
class AlertManager {
public:
    // exe ディレクトリから alert.wav パスを解決し、存在確認する
    void init();

    // シャットダウンフラグを立て、再生スレッドの終了を待つ
    void shutdown();

    // 全メトリクスを評価し、閾値超過があれば WAV 再生する
    void check(const AllMetrics& m, const AppConfig& cfg);

private:
    // 監視項目 ID
    enum Id {
        CPU, GPU, RAM, VRAM, DISK_C, DISK_D,
        TEMP_CPU, TEMP_GPU, TEMP_NVME_C, TEMP_NVME_D,
        DISK_GBH, UPTIME,
        CLAUDE_5H, CLAUDE_7D, CLAUDE_OVER,
        COUNT_
    };

    bool           fired_[COUNT_] = {};     // true = 発火済み（リセット閾値未達まで再発火しない）
    wchar_t        wav_path_[MAX_PATH] = {};
    bool           wav_avail_    = false;
    HANDLE         sound_thread_ = nullptr;
    volatile bool  shutdown_     = false;   // true = アプリ終了中（再生中断用）

    // バックグラウンドスレッドで WASAPI 再生を開始する
    void play();

    static DWORD WINAPI sound_thread_func(LPVOID param);
};
