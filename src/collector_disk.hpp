// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#pragma once
#include "metrics.hpp"

// ディスク I/O 収集（PDH、パーティション別 Read/Write バイト/秒）
class DiskCollector {
public:
    // 指定ドライブの PDH カウンタを初期化する（例：'C', 'D'）
    bool init(char drive_c, char drive_d);
    void update(DiskMetrics& c, DiskMetrics& d);
    // ディスク空き容量を更新する（GetDiskFreeSpaceExW、5 秒間隔を想定）
    void update_space(DiskMetrics& c, DiskMetrics& d);
    // NVMe S.M.A.R.T. 情報を更新する（1 時間間隔を想定）
    // 同一物理ドライブなら 1 回クエリして両方に設定する
    void update_smart(DiskMetrics& c, DiskMetrics& d);
    void shutdown();

    ~DiskCollector() { shutdown(); }

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
