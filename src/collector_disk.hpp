// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#pragma once
#include "metrics.hpp"
#include <vector>

// ディスク I/O 収集（PDH、動的検出した固定ドライブ別の Read/Write バイト/秒）
class DiskCollector {
public:
    // 監視対象の固定ドライブを列挙する（起動時に 1 回だけ呼ぶ想定。ホットプラグは対象外）
    //
    // 条件：レターが 'C' 以上、GetDriveTypeW == DRIVE_FIXED、かつ物理ディスク実体が存在する
    // （IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS 成功）。最後の条件は Google ドライブ等、
    // DRIVE_FIXED を名乗る仮想ファイルシステムを機械的に除外するために設けている。
    // レター昇順で最大 max_drives 台を返し、それを超えるドライブはログに記録した上で除外する
    // （超過は AlertManager の警告 ID 予約数の制約による）。
    static std::vector<char> enumerate_fixed_drives(int max_drives);

    // enumerate_fixed_drives() の結果を渡して PDH カウンタを初期化する
    bool init(const std::vector<char>& drives);

    // disks は init に渡したドライブ列と同数・同順であること
    void update(std::vector<DiskMetrics>& disks);
    // ディスク空き容量を更新する（GetDiskFreeSpaceExW、5 秒間隔を想定）
    void update_space(std::vector<DiskMetrics>& disks);
    // NVMe S.M.A.R.T. 情報を更新する（1 時間間隔を想定）
    // 同一物理ドライブを共有するドライブ群はクエリを 1 回にまとめ、結果を残りへコピーする
    void update_smart(std::vector<DiskMetrics>& disks);
    void shutdown();

    ~DiskCollector() { shutdown(); }

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
