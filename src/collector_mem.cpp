// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#include "collector_mem.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <pdh.h>
#pragma comment(lib, "pdh.lib")

// WSL2 VM Working Set カウンタパス
//
// プロセス名は OS バージョンによって異なるため複数バリアントを登録して合算する。
// vmmem は PPL のため OpenProcess 不可。PDH はハンドル不要で取得できる。
static constexpr const wchar_t* VMMEM_COUNTERS[] = {
    L"\\Process(vmmem)\\Working Set",     // Windows 10 WSL2
    L"\\Process(vmmemWSL)\\Working Set",  // Windows 11 WSL2
};

// ハードフォールトカウンタパス（ディスク I/O 操作回数/秒。リソースモニターの「ハードフォールト」に近い値）
static constexpr const wchar_t* HF_COUNTER = L"\\Memory\\Page Reads/sec";

struct MemCollector::Impl {
    PDH_HQUERY   query      = nullptr;
    PDH_HCOUNTER counters[2] = {};
    int          n          = 0;
    // ハードフォールト専用クエリ（WSL クエリとは更新間隔が異なるため分離）
    PDH_HQUERY   hf_query   = nullptr;
    PDH_HCOUNTER hf_counter = nullptr;
};

void MemCollector::init() {
    if (impl_) return;
    auto* p = new Impl{};
    if (PdhOpenQuery(nullptr, 0, &p->query) != ERROR_SUCCESS) {
        delete p;
        return;
    }
    for (auto path : VMMEM_COUNTERS) {
        if (PdhAddEnglishCounterW(p->query, path, 0, &p->counters[p->n]) == ERROR_SUCCESS)
            ++p->n;
    }
    // WSL 非使用環境ではカウンタ登録ゼロになる。その場合も hf_query は初期化するため早期リターンしない
    if (p->n == 0) {
        PdhCloseQuery(p->query);
        p->query = nullptr;
    }

    // ハードフォールトカウンタ（WSL の有無にかかわらず常に取得する）
    if (PdhOpenQuery(nullptr, 0, &p->hf_query) == ERROR_SUCCESS) {
        if (PdhAddEnglishCounterW(p->hf_query, HF_COUNTER, 0, &p->hf_counter) == ERROR_SUCCESS) {
            // レート型カウンタは最初のサンプルが差分計算のベースになるため捨てる
            PdhCollectQueryData(p->hf_query);
        }
        else {
            PdhCloseQuery(p->hf_query);
            p->hf_query   = nullptr;
            p->hf_counter = nullptr;
        }
    }

    impl_ = p;
}

void MemCollector::update(MemMetrics& out) {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) return;

    out.usage_pct = static_cast<float>(ms.dwMemoryLoad);
    out.total_gb  = static_cast<float>(ms.ullTotalPhys) / (1024.f * 1024.f * 1024.f);
    out.used_gb   = out.total_gb - static_cast<float>(ms.ullAvailPhys) / (1024.f * 1024.f * 1024.f);

    out.wsl_gb = 0.f;
    if (impl_ && impl_->query && PdhCollectQueryData(impl_->query) == ERROR_SUCCESS) {
        LONGLONG wsl_bytes = 0;
        for (int i = 0; i < impl_->n; ++i) {
            PDH_FMT_COUNTERVALUE val{};
            if (PdhGetFormattedCounterValue(impl_->counters[i], PDH_FMT_LARGE, nullptr, &val) == ERROR_SUCCESS)
                wsl_bytes += val.largeValue;
        }
        out.wsl_gb = static_cast<float>(wsl_bytes) / (1024.f * 1024.f * 1024.f);
    }
}

void MemCollector::update_hard_faults(MemMetrics& out) {
    if (!impl_ || !impl_->hf_query) return;
    if (PdhCollectQueryData(impl_->hf_query) != ERROR_SUCCESS) return;

    PDH_FMT_COUNTERVALUE val{};
    if (PdhGetFormattedCounterValue(impl_->hf_counter, PDH_FMT_DOUBLE, nullptr, &val) != ERROR_SUCCESS)
        return;
    // PDH_CSTATUS_VALID_DATA(0x0) または PDH_CSTATUS_NEW_DATA(0x1) 以外は無効値
    if (val.CStatus > 1)
        return;

    out.hard_fault_history.push(static_cast<float>(val.doubleValue));
}

void MemCollector::shutdown() {
    if (!impl_) return;
    if (impl_->hf_query) { PdhCloseQuery(impl_->hf_query); impl_->hf_query = nullptr; }
    if (impl_->query)    { PdhCloseQuery(impl_->query);    impl_->query    = nullptr; }
    delete impl_;
    impl_ = nullptr;
}
