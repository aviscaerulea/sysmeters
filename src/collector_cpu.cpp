// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#include "collector_cpu.hpp"
#include "logger.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
// WMI
#define _WIN32_DCOM
#include <comdef.h>
#include <wbemidl.h>
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "pdh.lib")
#include <intrin.h>

#include <vector>
#include <string>
#include <cstdio>
#include <cstring>

// PDH カウンタ + WMI 接続の実装詳細
struct CpuCollector::Impl {
    // PDH
    PDH_HQUERY   query          = nullptr;
    PDH_HCOUNTER counter_total  = nullptr;
    std::vector<PDH_HCOUNTER> counter_cores;

    // WMI
    IWbemLocator*  wbem_locator  = nullptr;
    IWbemServices* wbem_services = nullptr;
    bool           wmi_ok        = false;

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

    // --- WMI 初期化 ---
    // CoInitializeEx は main で呼ばれている前提
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IWbemLocator, reinterpret_cast<void**>(&impl_->wbem_locator));
    if (FAILED(hr)) {
        log_error("CPU WMI CoCreateInstance failed (hr=0x%08x)", static_cast<unsigned>(hr));
        return true;  // PDH は OK なので true を返す
    }

    hr = impl_->wbem_locator->ConnectServer(
        _bstr_t(L"ROOT\\WMI"), nullptr, nullptr, nullptr, 0, nullptr, nullptr,
        &impl_->wbem_services);
    if (FAILED(hr)) {
        log_error("CPU WMI ConnectServer failed (hr=0x%08x)", static_cast<unsigned>(hr));
        impl_->wbem_locator->Release();
        impl_->wbem_locator = nullptr;
        return true;
    }

    hr = CoSetProxyBlanket(impl_->wbem_services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
                           nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                           nullptr, EOAC_NONE);
    if (FAILED(hr)) {
        impl_->wbem_services->Release();
        impl_->wbem_services = nullptr;
        impl_->wbem_locator->Release();
        impl_->wbem_locator = nullptr;
        return true;
    }

    impl_->wmi_ok = true;
    log_info("CPU collector initialized: %s (WMI: OK)", impl_->cpu_name);
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

    // WMI 温度取得
    if (!impl_->wmi_ok || !impl_->wbem_services) {
        out.temp_avail = false;
        return;
    }

    IEnumWbemClassObject* enumerator = nullptr;
    HRESULT hr = impl_->wbem_services->ExecQuery(
        _bstr_t(L"WQL"),
        _bstr_t(L"SELECT CurrentTemperature FROM MSAcpi_ThermalZoneTemperature"),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &enumerator);
    if (FAILED(hr) || !enumerator) { out.temp_avail = false; return; }

    IWbemClassObject* obj = nullptr;
    ULONG returned = 0;
    float sum = 0.f;
    int   cnt = 0;

    // タイムアウト 500ms（WBEM_INFINITE だと WMI 無応答時に UI スレッドがブロックされる）
    while (enumerator->Next(500, 1, &obj, &returned) == WBEM_S_NO_ERROR) {
        VARIANT vtProp;
        VariantInit(&vtProp);
        if (SUCCEEDED(obj->Get(L"CurrentTemperature", 0, &vtProp, nullptr, nullptr))) {
            // WMI 温度は 10 分の 1 ケルビン（デシケルビン）
            float kelvin = static_cast<float>(vtProp.uintVal) / 10.f;
            sum += kelvin - 273.15f;
            ++cnt;
        }
        VariantClear(&vtProp);
        obj->Release();
    }
    enumerator->Release();

    if (cnt > 0) {
        out.temp_celsius = sum / static_cast<float>(cnt);
        out.temp_avail   = true;
    }
    else {
        out.temp_avail = false;
    }
}

void CpuCollector::shutdown() {
    if (!impl_) return;
    if (impl_->query) { PdhCloseQuery(impl_->query); impl_->query = nullptr; }
    if (impl_->wbem_services) { impl_->wbem_services->Release(); impl_->wbem_services = nullptr; }
    if (impl_->wbem_locator)  { impl_->wbem_locator->Release();  impl_->wbem_locator  = nullptr; }
    delete impl_;
    impl_ = nullptr;
}
