// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#include "collector_cpu.hpp"
#include "logger.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <pdh.h>
#include <pdhmsg.h>
#pragma comment(lib, "pdh.lib")
#include <intrin.h>

#include <vector>
#include <cstdio>
#include <cstring>

// PawnIO IOCTL 定義（pawnio_um.h より）
//
// PawnIO ドライバ（\\.\PawnIO）経由で MSR を読み取るための IOCTL コードと定数。
// デバイスタイプ 41394 は PawnIO が使用する固有の値（k_device_type）。
static constexpr ULONG k_pawnio_device_type = 41394;
static constexpr ULONG IOCTL_PIO_LOAD_BINARY =
    CTL_CODE(k_pawnio_device_type, 0x821, METHOD_BUFFERED, FILE_ANY_ACCESS);
static constexpr ULONG IOCTL_PIO_EXECUTE_FN =
    CTL_CODE(k_pawnio_device_type, 0x841, METHOD_BUFFERED, FILE_ANY_ACCESS);

// Intel MSR アドレス（MSR_IA32_PACKAGE_THERM_STATUS, MSR_IA32_TEMPERATURE_TARGET）
static constexpr ULONG64 MSR_IA32_TEMPERATURE_TARGET   = 0x1A2;
static constexpr ULONG64 MSR_IA32_PACKAGE_THERM_STATUS = 0x1B1;

// AMD SMN アドレス（F17H_M01H_THM_TCON_CUR_TMP - Zen 1〜5 共通）
static constexpr ULONG64 SMN_THM_TCON_CUR_TMP = 0x00059800;

// システム統計 PDH カウンタパス
static constexpr wchar_t PDH_SYSTEM_PROCESSES[] = L"\\System\\Processes";
static constexpr wchar_t PDH_SYSTEM_THREADS[]   = L"\\System\\Threads";
static constexpr wchar_t PDH_PROCESS_HANDLES[]  = L"\\Process(_Total)\\Handle Count";

// IOCTL_PIO_EXECUTE_FN 入力バッファ（PawnIOLib.cpp の pawnio_execute_nt に準拠）
//
// 先頭 32 バイトが関数名（ゼロ埋め）、以降が ULONG64 の入力パラメータ配列。
static constexpr size_t FN_NAME_LENGTH = 32;
struct PawnIOExecuteInput {
    char    fn_name[FN_NAME_LENGTH];  // 関数名（最大 31 文字 + ヌル終端）
    ULONG64 param;                    // 入力パラメータ（MSR/SMN アドレス）
};

enum class CpuVendor { Unknown, Intel, Amd };

// PDH カウンタの実装詳細
struct CpuCollector::Impl {
    PDH_HQUERY   query          = nullptr;
    PDH_HCOUNTER counter_total  = nullptr;
    std::vector<PDH_HCOUNTER> counter_cores;
    int core_count = 0;  // 登録済みコアカウンタ数（論理コア数）

    PDH_HCOUNTER counter_processes = nullptr;
    PDH_HCOUNTER counter_threads   = nullptr;
    PDH_HCOUNTER counter_handles   = nullptr;

    // PawnIO デバイスハンドルと状態
    HANDLE    hdev_pawnio  = INVALID_HANDLE_VALUE;
    bool      pawnio_avail = false;        // init() 成功フラグ
    CpuVendor vendor       = CpuVendor::Unknown;
    UINT32    tjmax        = 100;          // TjMax キャッシュ（Intel のみ、MSR 0x1A2 から取得）
    HANDLE    hmutex_pci   = nullptr;      // AMD SMN 読み取り時の PCI アクセス排他ミューテックス

    char cpu_name[48] = {};  // CPUID ブランド文字列
};

// PawnIO モジュール関数を呼び出すヘルパー
//
// DeviceIoControl で IOCTL_PIO_EXECUTE_FN を呼び出し、指定した fn 関数を実行する。
// 成功時は true を返し out_value に結果を格納する。
static bool pawnio_call(HANDLE hdev, const char* fn, ULONG64 param, ULONG64& out_value) {
    PawnIOExecuteInput in_buf{};
    lstrcpynA(in_buf.fn_name, fn, static_cast<int>(FN_NAME_LENGTH));
    in_buf.param = param;

    ULONG64 out_val = 0;
    DWORD bytes_ret = 0;
    BOOL ok = DeviceIoControl(
        hdev,
        IOCTL_PIO_EXECUTE_FN,
        &in_buf, static_cast<DWORD>(sizeof(in_buf)),
        &out_val, static_cast<DWORD>(sizeof(out_val)),
        &bytes_ret,
        nullptr);

    if (!ok || bytes_ret < sizeof(ULONG64)) return false;
    out_value = out_val;
    return true;
}

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

    // 論理コア別カウンタ（GetSystemInfo で取得した論理コア数分を登録）
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const int n_cores = static_cast<int>(si.dwNumberOfProcessors);
    for (int i = 0; i < n_cores; ++i) {
        wchar_t buf[64];
        swprintf_s(buf, L"\\Processor(%d)\\%% Processor Time", i);
        PDH_HCOUNTER hc = nullptr;
        if (PdhAddEnglishCounterW(impl_->query, buf, 0, &hc) == ERROR_SUCCESS) {
            impl_->counter_cores.push_back(hc);
        }
    }
    impl_->core_count = static_cast<int>(impl_->counter_cores.size());

    PdhAddEnglishCounterW(impl_->query, PDH_SYSTEM_PROCESSES, 0, &impl_->counter_processes);
    PdhAddEnglishCounterW(impl_->query, PDH_SYSTEM_THREADS,   0, &impl_->counter_threads);
    PdhAddEnglishCounterW(impl_->query, PDH_PROCESS_HANDLES,  0, &impl_->counter_handles);

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

        const char* p = brand;
        while (*p == ' ') ++p;

        // 末尾の冗長サフィックス除去（" Processor" → " N-Core" の順に除去）
        char trimmed[49] = {};
        strncpy_s(trimmed, sizeof(trimmed), p, _TRUNCATE);

        // CPUID ブランド文字列は末尾にスペースが入る場合があるため除去する
        {
            size_t tlen = strlen(trimmed);
            while (tlen > 0 && trimmed[tlen - 1] == ' ') trimmed[--tlen] = '\0';
        }

        for (const char* suf : {" Processor", " processor"}) {
            size_t tlen = strlen(trimmed);
            size_t slen = strlen(suf);
            if (tlen > slen && strcmp(trimmed + tlen - slen, suf) == 0) {
                trimmed[tlen - slen] = '\0';
            }
        }

        // " 8-Core" や " 12-Core" など末尾の N-Core パターン除去
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

    // CPU ベンダ検出（CPUID leaf 0）
    {
        int v[4] = {};
        __cpuid(v, 0);
        char vendor_id[13] = {};
        memcpy(vendor_id + 0, &v[1], 4);  // EBX
        memcpy(vendor_id + 4, &v[3], 4);  // EDX
        memcpy(vendor_id + 8, &v[2], 4);  // ECX
        if      (strcmp(vendor_id, "GenuineIntel") == 0) impl_->vendor = CpuVendor::Intel;
        else if (strcmp(vendor_id, "AuthenticAMD") == 0) impl_->vendor = CpuVendor::Amd;
    }
    if (impl_->vendor == CpuVendor::Unknown) {
        log_error("PawnIO: unsupported CPU vendor, temperature unavailable");
        return true;
    }

    // --- PawnIO ドライバ経由の CPU 温度取得初期化 ---
    //
    // PawnIO ドライバが未インストールの場合は temp_avail = false のまま（N/A 表示）。
    // NVML パターンと同様に、ドライバが利用可能な場合のみ温度を取得する。

    // デバイスオープン（\\.\PawnIO は PawnIO ドライバが作成する DosDevices シンボリックリンク）
    impl_->hdev_pawnio = CreateFileW(
        L"\\\\.\\PawnIO",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (impl_->hdev_pawnio == INVALID_HANDLE_VALUE) {
        log_error("PawnIO: device not found (install PawnIO driver for CPU temp)");
        return true;  // 温度なしで継続
    }

    // exe と同ディレクトリの IntelMSR.bin を読み込む
    wchar_t exe_path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    // exe パスの末尾ファイル名を IntelMSR.bin に置換
    wchar_t* last_sep = wcsrchr(exe_path, L'\\');
    if (!last_sep) {
        log_error("PawnIO: failed to resolve exe path");
        CloseHandle(impl_->hdev_pawnio);
        impl_->hdev_pawnio = INVALID_HANDLE_VALUE;
        return true;
    }
    const wchar_t* bin_name = (impl_->vendor == CpuVendor::Amd) ? L"AMDFamily17.bin" : L"IntelMSR.bin";
    wcscpy_s(last_sep + 1, MAX_PATH - (last_sep + 1 - exe_path), bin_name);

    HANDLE hfile = CreateFileW(exe_path, GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hfile == INVALID_HANDLE_VALUE) {
        log_error("PawnIO: %ls not found", bin_name);
        CloseHandle(impl_->hdev_pawnio);
        impl_->hdev_pawnio = INVALID_HANDLE_VALUE;
        return true;
    }

    DWORD bin_size = GetFileSize(hfile, nullptr);
    if (bin_size == INVALID_FILE_SIZE) {
        log_error("PawnIO: failed to get %ls size", bin_name);
        CloseHandle(hfile);
        CloseHandle(impl_->hdev_pawnio);
        impl_->hdev_pawnio = INVALID_HANDLE_VALUE;
        return true;
    }
    std::vector<BYTE> bin_data(bin_size);
    DWORD read_bytes = 0;
    BOOL read_ok = ReadFile(hfile, bin_data.data(), bin_size, &read_bytes, nullptr);
    CloseHandle(hfile);

    if (!read_ok || read_bytes != bin_size) {
        log_error("PawnIO: failed to read %ls", bin_name);
        CloseHandle(impl_->hdev_pawnio);
        impl_->hdev_pawnio = INVALID_HANDLE_VALUE;
        return true;
    }

    // モジュールをロード
    DWORD bytes_ret = 0;
    BOOL load_ok = DeviceIoControl(
        impl_->hdev_pawnio,
        IOCTL_PIO_LOAD_BINARY,
        bin_data.data(), bin_size,
        nullptr, 0,
        &bytes_ret, nullptr);

    if (!load_ok) {
        log_error("PawnIO: IOCTL_PIO_LOAD_BINARY failed (err=%lu)", GetLastError());
        CloseHandle(impl_->hdev_pawnio);
        impl_->hdev_pawnio = INVALID_HANDLE_VALUE;
        return true;
    }

    if (impl_->vendor == CpuVendor::Intel) {
        // TjMax を取得してキャッシュ（MSR_IA32_TEMPERATURE_TARGET ビット [23:16]）
        ULONG64 tgt_val = 0;
        if (pawnio_call(impl_->hdev_pawnio, "ioctl_read_msr", MSR_IA32_TEMPERATURE_TARGET, tgt_val)) {
            impl_->tjmax = static_cast<UINT32>((tgt_val >> 16) & 0xFF);
            if (impl_->tjmax == 0) impl_->tjmax = 100;  // 不正値はデフォルトにフォールバック
        }
        impl_->pawnio_avail = true;
        log_info("PawnIO: CPU temp init OK (Intel TjMax=%u)", impl_->tjmax);
    }
    else {
        // AMD: SMN 読み取りの PCI アクセス排他ミューテックスを作成/取得（LibreHardwareMonitor 互換）
        impl_->hmutex_pci  = CreateMutexW(nullptr, FALSE, L"Global\\Access_PCI");
        impl_->pawnio_avail = true;
        log_info("PawnIO: CPU temp init OK (AMD SMN)");
    }
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

    // コア別使用率（初回のみサイズ確保）
    if (out.core_count == 0) {
        out.core_count = impl_->core_count;
        out.core_pct.resize(impl_->core_count, 0.f);
    }
    for (int i = 0; i < static_cast<int>(impl_->counter_cores.size()); ++i) {
        PDH_FMT_COUNTERVALUE cv{};
        if (PdhGetFormattedCounterValue(impl_->counter_cores[i], PDH_FMT_DOUBLE, nullptr, &cv) == ERROR_SUCCESS) {
            out.core_pct[i] = static_cast<float>(cv.doubleValue);
        }
    }

    {
        PDH_FMT_COUNTERVALUE sv{};
        if (impl_->counter_processes
                && PdhGetFormattedCounterValue(impl_->counter_processes, PDH_FMT_LARGE, nullptr, &sv) == ERROR_SUCCESS) {
            out.processes = static_cast<int>(sv.largeValue);
        }
        if (impl_->counter_threads
                && PdhGetFormattedCounterValue(impl_->counter_threads, PDH_FMT_LARGE, nullptr, &sv) == ERROR_SUCCESS) {
            out.threads = static_cast<int>(sv.largeValue);
        }
        if (impl_->counter_handles
                && PdhGetFormattedCounterValue(impl_->counter_handles, PDH_FMT_LARGE, nullptr, &sv) == ERROR_SUCCESS) {
            out.handles = static_cast<int>(sv.largeValue);
        }
    }

    // PawnIO ドライバ経由で CPU パッケージ温度を取得する（Intel: MSR, AMD: SMN）
    if (!impl_->pawnio_avail) {
        out.temp_avail = false;
        return;
    }

    if (impl_->vendor == CpuVendor::Intel) {
        // MSR_IA32_PACKAGE_THERM_STATUS（0x1B1）ビット [22:16] = Digital Readout、ビット 31 = Reading Valid
        // 温度 = TjMax - Digital Readout
        ULONG64 therm_val = 0;
        if (!pawnio_call(impl_->hdev_pawnio, "ioctl_read_msr", MSR_IA32_PACKAGE_THERM_STATUS, therm_val)
                || (therm_val >> 31) == 0) {
            out.temp_avail = false;
            return;
        }
        const UINT32 readout = static_cast<UINT32>((therm_val >> 16) & 0x7F);
        out.temp_celsius = static_cast<float>(impl_->tjmax) - static_cast<float>(readout);
        out.temp_avail   = true;
    }
    else {
        // AMD: SMN レジスタ THM_TCON_CUR_TMP（0x59800）から Tctl 温度を取得
        // 温度 = (raw >> 21) * 125 / 1000、ビット 19 = オフセットフラグ（-49°C）
        if (impl_->hmutex_pci) {
            if (WaitForSingleObject(impl_->hmutex_pci, 100) != WAIT_OBJECT_0) {
                out.temp_avail = false;
                return;
            }
        }
        ULONG64 raw = 0;
        const bool ok = pawnio_call(impl_->hdev_pawnio, "ioctl_read_smn", SMN_THM_TCON_CUR_TMP, raw);
        if (impl_->hmutex_pci) ReleaseMutex(impl_->hmutex_pci);
        if (!ok) {
            out.temp_avail = false;
            return;
        }
        float temp = static_cast<float>(raw >> 21) * 125.0f / 1000.0f;
        if (raw & 0x80000) temp -= 49.0f;
        out.temp_celsius = temp;
        out.temp_avail   = true;
    }
}

void CpuCollector::shutdown() {
    if (!impl_) return;
    if (impl_->query)                               { PdhCloseQuery(impl_->query); impl_->query = nullptr; }
    if (impl_->hdev_pawnio != INVALID_HANDLE_VALUE) { CloseHandle(impl_->hdev_pawnio); impl_->hdev_pawnio = INVALID_HANDLE_VALUE; }
    if (impl_->hmutex_pci)                          { CloseHandle(impl_->hmutex_pci); impl_->hmutex_pci = nullptr; }
    delete impl_;
    impl_ = nullptr;
}
