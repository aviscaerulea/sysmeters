// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#pragma once
#include "metrics.hpp"

// CPU 使用率（PDH）と温度（PawnIO ドライバ経由の MSR 読み取り）の収集
//
// PDH カウンタで全体使用率と論理コア別使用率を取得する。
// PawnIO ドライバ（\\.\PawnIO）経由で MSR を直接読み取り CPU パッケージ温度を取得する。
// PawnIO ドライバが未インストールの場合は temp_avail = false になる。
class CpuCollector {
public:
    // PDH カウンタを初期化する。
    // 成功しない項目は avail フラグを false のままにする。
    bool init();

    // メトリクスを更新する（WM_TIMER から 1 秒ごとに呼び出す）
    void update(CpuMetrics& out);

    void shutdown();

    ~CpuCollector() { shutdown(); }

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
