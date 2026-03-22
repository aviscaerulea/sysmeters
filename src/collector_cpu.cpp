// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#include "collector_cpu.hpp"
#include "logger.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#pragma comment(lib, "pdh.lib")
#include <intrin.h>

#include <vector>
#include <cstdio>
#include <cstring>

// CoreTemp 共有メモリ構造体（CoreTempMappingObjectEx）
//
// CoreTemp が公開する共有メモリのレイアウト。
// 参考: https://www.alcpu.com/CoreTemp/developers.html
#pragma pack(push, 1)
struct CoreTempSharedDataEx {
    UINT  uiLoad[256];       // コア別 CPU 使用率 (%)
    UINT  uiTjMax[128];      // CPU 別 Tj Max 温度
    UINT  uiCoreCnt;         // 全 CPU 合計コア数
    UINT  uiCPUCnt;          // 物理 CPU 数
    FLOAT fTemp[256];        // コア別温度（℃ or ℉、ucDeltaToTjMax に応じて解釈が異なる）
    FLOAT fVID;
    FLOAT fCPUSpeed;
    FLOAT fFSBSpeed;
    FLOAT fMultiplier;
    CHAR  sCPUName[100];
    BYTE  ucFahrenheit;      // 1=℉ 表示、0=℃ 表示
    BYTE  ucDeltaToTjMax;    // 1=TjMax までの差分、0=絶対温度
    BYTE  ucTdpSupported;
    BYTE  ucPowerSupported;
    UINT  uiStructVersion;   // 2 以上で以下フィールドが有効
    UINT  uiTdp[128];
    FLOAT fPower[128];
    FLOAT fMultipliers[256];
};
#pragma pack(pop)

// PDH カウンタの実装詳細
struct CpuCollector::Impl {
    PDH_HQUERY   query          = nullptr;
    PDH_HCOUNTER counter_total  = nullptr;
    std::vector<PDH_HCOUNTER> counter_cores;

    // CoreTemp 共有メモリハンドルキャッシュ（起動中は保持し毎秒の OpenFileMapping を省く）
    HANDLE hmap_coretemp = nullptr;

    char cpu_name[48] = {};  // CPUID ブランド文字列
};

bool CpuCollector::init() {
    impl_ = new Impl();

    // --- PDH 初期化 ---
    if (PdhOpenQuery(nullptr, 0, &impl_->query) != ERROR_SUCCESS) {
        log_error("CPU PDH init failed");
        return false;
    }

    // 全体使用率カウンタ
    if (PdhAddEnglishCounterW(impl_->query, L"\\Processor(_Total)\\% Processor Time",
                              0, &impl_->counter_total) != ERROR_SUCCESS) {
        log_error("CPU PDH: Failed to add total counter");
        return false;
    }

    // 論理コア別カウンタ（最大 16 まで試みる）
    for (int i = 0; i < 16; ++i) {
        wchar_t buf[64];
        swprintf_s(buf, L"\\Processor(%d)\\%% Processor Time", i);
        PDH_HCOUNTER hc = nullptr;
        if (PdhAddEnglishCounterW(impl_->query, buf, 0, &hc) == ERROR_SUCCESS) {
            impl_->counter_cores.push_back(hc);
        }
    }

    // 最初のサンプリング（PDH は 2 回目以降が有効）
    PdhCollectQueryData(impl_->query);

    // --- CPUID ブランド文字列取得（管理者権限・COM 不要）---
    {
        int regs[4] = {};
        char brand[49] = {};
        for (int leaf = 0; leaf < 3; ++leaf) {
            __cpuid(regs, 0x80000002 + leaf);
            memcpy(brand + leaf * 16, regs, 16);
        }
        brand[48] = '\0';

        // 先頭空白除去
        const char* p = brand;
        while (*p == ' ') ++p;

        // 末尾の冗長サフィックス除去（" Processor" → " N-Core" の順に除去）
        char trimmed[49] = {};
        strncpy_s(trimmed, sizeof(trimmed), p, _TRUNCATE);

        // 末尾スペース除去（CPUID ブランド文字列は末尾にスペースが入る場合がある）
        {
            size_t tlen = strlen(trimmed);
            while (tlen > 0 && trimmed[tlen - 1] == ' ') trimmed[--tlen] = '\0';
        }

        // " Processor" 除去
        for (const char* suf : {" Processor", " processor"}) {
            size_t tlen = strlen(trimmed);
            size_t slen = strlen(suf);
            if (tlen > slen && strcmp(trimmed + tlen - slen, suf) == 0) {
                trimmed[tlen - slen] = '\0';
            }
        }

        // " N-Core" パターン除去（" 8-Core" や " 12-Core" など）
        {
            const char* last_sp = strrchr(trimmed, ' ');
            if (last_sp) {
                // スペースの後が数字から始まり "-Core" で終わるか判定
                const char* tok = last_sp + 1;
                while (*tok >= '0' && *tok <= '9') ++tok;
                if (tok > last_sp + 1 && strcmp(tok, "-Core") == 0) {
                    trimmed[last_sp - trimmed] = '\0';
                }
            }
        }

        strncpy_s(impl_->cpu_name, sizeof(impl_->cpu_name), trimmed, _TRUNCATE);
    }

    log_info("CPU collector initialized: %s", impl_->cpu_name);
    return true;
}

void CpuCollector::update(CpuMetrics& out) {
    if (!impl_) return;

    // CPU 名はキャッシュから毎回コピー
    memcpy(out.name, impl_->cpu_name, sizeof(out.name));

    // PDH サンプル収集
    PdhCollectQueryData(impl_->query);

    // 全体使用率
    PDH_FMT_COUNTERVALUE val{};
    if (PdhGetFormattedCounterValue(impl_->counter_total, PDH_FMT_DOUBLE, nullptr, &val) == ERROR_SUCCESS) {
        out.total_pct = static_cast<float>(val.doubleValue);
        out.total_history.push(out.total_pct);
    }

    // コア別使用率
    for (int i = 0; i < static_cast<int>(impl_->counter_cores.size()) && i < 16; ++i) {
        PDH_FMT_COUNTERVALUE cv{};
        if (PdhGetFormattedCounterValue(impl_->counter_cores[i], PDH_FMT_DOUBLE, nullptr, &cv) == ERROR_SUCCESS) {
            out.core_pct[i] = static_cast<float>(cv.doubleValue);
        }
    }

    // CoreTemp 共有メモリから CPU 温度を取得する
    //
    // 全コアの最大温度を out.temp_celsius に格納する。
    // CoreTemp が起動していない場合は temp_avail = false のまま（--℃ 表示）。
    // ハンドルは Impl にキャッシュして毎秒の OpenFileMapping コストを回避する。
    if (!impl_->hmap_coretemp) {
        impl_->hmap_coretemp = OpenFileMapping(
            FILE_MAP_READ, FALSE, TEXT("CoreTempMappingObjectEx"));
    }
    if (!impl_->hmap_coretemp) {
        out.temp_avail = false;
        return;
    }

    const auto* p = reinterpret_cast<const CoreTempSharedDataEx*>(
        MapViewOfFile(impl_->hmap_coretemp, FILE_MAP_READ, 0, 0, 0));
    if (!p) {
        // ハンドルが無効（CoreTemp 終了 → 再起動後の可能性）：次回再取得する
        CloseHandle(impl_->hmap_coretemp);
        impl_->hmap_coretemp = nullptr;
        out.temp_avail = false;
        return;
    }

    float max_temp = -1.f;
    const UINT cores   = p->uiCoreCnt < 256u ? p->uiCoreCnt : 256u;
    const UINT cpu_cnt = p->uiCPUCnt  > 0u   ? p->uiCPUCnt  : 1u;

    for (UINT i = 0; i < cores; ++i) {
        float t = p->fTemp[i];

        if (p->ucFahrenheit) {
            t = (t - 32.f) * 5.f / 9.f;
        }

        if (p->ucDeltaToTjMax) {
            // fTemp が TjMax までの差分を表す場合、絶対温度に変換する
            // cores_per_cpu が 0 になる不正値（cores < cpu_cnt）もゼロ除算にならないよう 1 にクランプする
            const UINT cores_per_cpu = (cpu_cnt > 1 && cores >= cpu_cnt) ? (cores / cpu_cnt) : 1u;
            const UINT cpu_idx = i / cores_per_cpu;
            t = static_cast<float>(p->uiTjMax[cpu_idx]) - t;
        }

        if (t > max_temp) max_temp = t;
    }

    UnmapViewOfFile(p);

    if (max_temp >= 0.f) {
        out.temp_celsius = max_temp;
        out.temp_avail   = true;
    }
    else {
        out.temp_avail = false;
    }
}

void CpuCollector::shutdown() {
    if (!impl_) return;
    if (impl_->query)         { PdhCloseQuery(impl_->query); impl_->query = nullptr; }
    if (impl_->hmap_coretemp) { CloseHandle(impl_->hmap_coretemp); impl_->hmap_coretemp = nullptr; }
    delete impl_;
    impl_ = nullptr;
}
