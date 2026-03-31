// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#pragma once
#include "metrics.hpp"

// 物理メモリ使用量の収集（GlobalMemoryStatusEx + PDH WSL カウンタ）
class MemCollector {
public:
    // PDH カウンタを初期化する。
    // WSL2 未起動の環境ではカウンタ取得を省略して初期化する。
    void init();
    void update(MemMetrics& out);
    void shutdown();

    ~MemCollector() { shutdown(); }

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
