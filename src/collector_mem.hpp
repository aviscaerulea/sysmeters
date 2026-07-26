// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#pragma once
#include "metrics.hpp"

// 物理メモリ使用量の収集（GlobalMemoryStatusEx + PDH WSL カウンタ）
class MemCollector {
public:
    // PDH カウンタを初期化する。
    // WSL2 未起動の環境ではカウンタ取得を省略して初期化する。
    void init();

    // 物理メモリ使用量を更新する（起動時の初回取得と、TIMER_SLOW から 2.0 秒ごとに呼び出す）
    // usage_pct を usage_history にも push する（警告音の平均判定用）。
    // GlobalMemoryStatusEx が失敗した場合は out を一切更新せず、履歴も据え置く
    void update(MemMetrics& out);

    // ハードフォールトカウンタを更新する（TIMER_CPU から 0.9 秒ごとに呼び出す）
    void update_hard_faults(MemMetrics& out);

    void shutdown();

    ~MemCollector() { shutdown(); }

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
