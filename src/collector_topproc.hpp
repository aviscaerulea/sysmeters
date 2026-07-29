// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#pragma once
#include "metrics.hpp"

// CPU / GPU を最も消費しているプロセス名の収集
//
// CPU 側は NtQuerySystemInformation(SystemProcessInformation) のスナップショット差分から、
// 同名 exe を合算した最上位 1 件を CpuMetrics に書く。
// GPU 側は PDH の \GPU Engine(*)\Utilization Percentage の 3D/Compute 系エンジンを
// 同名 exe × アダプタ × エンジン種別のバケット内で合算し、バケット間は最大値を採った
// 最上位 1 件を GpuMetrics に書く。（集計方式の根拠は pick_gpu_top のコメントを参照）
// CPU/GPU とも、四捨五入した整数 % が直前選出プロセスと同値の間は直前を優先して維持し、
// 同率トップによる表示の入れ替わりを抑える。
//
// NVML の per-process API（nvmlDeviceGetProcessUtilization 等）は WDDM ドライバモデルで
// グラフィックスプロセスの値が返らない事例が多く採用しない。PDH の GPU Engine は
// タスクマネージャの GPU 列と同一ソースで、ベンダ非依存に動く。
class TopProcCollector {
public:
    // トッププロセスを更新する（TIMER_CPU から 0.9 秒ごとに呼ぶ）
    //
    // enabled=false のときは出力を空にして内部状態（スナップショット・PDH クエリ）を
    // 解放するだけで返る。表示 OFF 中に追加負荷を一切発生させないための契約。
    // CPU は呼び出しごと、GPU は 3 回に 1 回（≒2.7 秒間隔）だけ更新する。
    // 有効化直後の 1 回目は差分の基準を取るだけで、値は 2 回目以降に確定する。
    // 値を確定できない間（初回・取得失敗・丸めて 0%）は top_proc_name を空にし、
    // 描画側は空文字を「実測値なし」として扱う。（残像期間内は描画側が直前の凍結値を
    // 薄く表示し得るため、空にしても即座に画面から消えるとは限らない）
    void update(CpuMetrics& cpu, GpuMetrics& gpu, bool enabled);

    void shutdown();  // 冪等。update(enabled=false) からも呼ばれる

    ~TopProcCollector() { shutdown(); }

private:
    struct Impl;
    Impl* impl_ = nullptr;

    // 初回 update() 時に内部状態を確保する（表示 OFF で起動した場合に何も確保しないため遅延生成）
    void ensure_impl();
};
