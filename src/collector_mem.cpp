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

struct MemCollector::Impl {
    PDH_HQUERY   query      = nullptr;
    PDH_HCOUNTER counters[2] = {};
    int          n          = 0;
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
    if (p->n == 0) {
        PdhCloseQuery(p->query);
        delete p;
        return;
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
    if (impl_ && PdhCollectQueryData(impl_->query) == ERROR_SUCCESS) {
        LONGLONG wsl_bytes = 0;
        for (int i = 0; i < impl_->n; ++i) {
            PDH_FMT_COUNTERVALUE val{};
            if (PdhGetFormattedCounterValue(impl_->counters[i], PDH_FMT_LARGE, nullptr, &val) == ERROR_SUCCESS)
                wsl_bytes += val.largeValue;
        }
        out.wsl_gb = static_cast<float>(wsl_bytes) / (1024.f * 1024.f * 1024.f);
    }
}

void MemCollector::shutdown() {
    if (!impl_) return;
    PdhCloseQuery(impl_->query);
    delete impl_;
    impl_ = nullptr;
}
