// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#include "collector_disk.hpp"
#include "logger.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>   // IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, IOCTL_STORAGE_QUERY_PROPERTY
#include <pdh.h>
#pragma comment(lib, "pdh.lib")
#include <array>
#include <cstring>  // memcpy

// --- NVMe SMART クエリ用型定義（ntddstor.h / nvme.h のバージョン依存を避けるため自前定義） ---

// クエリ入力バッファ
// STORAGE_PROPERTY_QUERY（PropertyId + QueryType）＋ STORAGE_PROTOCOL_SPECIFIC_DATA（AdditionalParameters）と互換
#pragma pack(push, 4)
struct NvmeSmartQuery {
    DWORD PropertyId;      // 49 = StorageDeviceProtocolSpecificProperty
    DWORD QueryType;       // 0  = PropertyStandardQuery
    DWORD ProtocolType;    // 3  = ProtocolTypeNvme
    DWORD DataType;        // 2  = NVMeDataTypeLogPage
    DWORD ReqValue;        // 2  = NVME_LOG_PAGE_HEALTH_INFO
    DWORD SubValue;        // 0
    DWORD DataOffset;      // 40 = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA)
    DWORD DataLength;      // 512 = sizeof(NvmeHealthLog)
    DWORD FixedReturnData; // 0
    DWORD SubValue2;       // 0
    DWORD SubValue3;       // 0
    DWORD Reserved;        // 0
};  // 48 bytes
#pragma pack(pop)

// NVMe Health Information Log Page（NVMe spec 1.4 section 5.14.1.2）
// STORAGE_PROTOCOL_DATA_DESCRIPTOR（48 bytes）の直後に配置される 512 bytes
#pragma pack(push, 1)
struct NvmeHealthLog {
    UCHAR  critical_warning;
    UCHAR  temperature[2];
    UCHAR  available_spare;
    UCHAR  available_spare_threshold;
    UCHAR  percentage_used;
    UCHAR  reserved0[26];
    UCHAR  data_units_read[16];
    UCHAR  data_units_written[16];  // 128-bit LE、[0..7] = 下位 64bit
    UCHAR  host_read_commands[16];
    UCHAR  host_write_commands[16];
    UCHAR  controller_busy_time[16];
    UCHAR  power_cycles[16];
    UCHAR  power_on_hours[16];      // 128-bit LE、[0..7] = 下位 64bit
    UCHAR  remaining[512 - 144];    // 512 bytes にパディング
};
#pragma pack(pop)

static_assert(sizeof(NvmeSmartQuery) == 48, "NvmeSmartQuery size");
// PropertyId(0), QueryType(4), ProtocolType(8=AdditionalParameters 先頭) のレイアウトを検証
static_assert(offsetof(NvmeSmartQuery, ProtocolType) == 8, "NvmeSmartQuery layout");
static_assert(offsetof(NvmeHealthLog, data_units_written) == 48, "NvmeHealthLog layout");
static_assert(offsetof(NvmeHealthLog, power_on_hours) == 128, "NvmeHealthLog layout");
static_assert(sizeof(NvmeHealthLog) == 512, "NvmeHealthLog size");

// STORAGE_PROTOCOL_DATA_DESCRIPTOR のサイズ（Version DWORD + Size DWORD + STORAGE_PROTOCOL_SPECIFIC_DATA 40 bytes）
static constexpr DWORD kProtoDescSize = 48;

// ドライブレターから物理ドライブ番号を取得する
static int get_phys_drive(char drv) {
    wchar_t path[16];
    swprintf_s(path, L"\\\\.\\%c:", static_cast<wchar_t>(drv));
    HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return -1;

    struct { VOLUME_DISK_EXTENTS vde; DISK_EXTENT extra; } buf{};
    DWORD bytes = 0;
    int result = -1;
    if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                        nullptr, 0, &buf, sizeof(buf), &bytes, nullptr)) {
        result = static_cast<int>(buf.vde.Extents[0].DiskNumber);
    }
    CloseHandle(h);
    return result;
}

struct DiskCollector::Impl {
    PDH_HQUERY query = nullptr;
    // [0]=C:Read, [1]=C:Write, [2]=D:Read, [3]=D:Write
    std::array<PDH_HCOUNTER, 4> counters = {};
    char drives[2] = {'C', 'D'};
    int  phys_drives[2] = {-1, -1};  // 各ドライブの物理ドライブ番号（init 時に解決）
};

bool DiskCollector::init(char drive_c, char drive_d) {
    impl_ = new Impl();
    impl_->drives[0] = drive_c;
    impl_->drives[1] = drive_d;

    if (PdhOpenQuery(nullptr, 0, &impl_->query) != ERROR_SUCCESS) {
        log_error("Disk PDH init failed");
        return false;
    }

    const wchar_t* templates[4] = {
        L"\\LogicalDisk(%c:)\\Disk Read Bytes/sec",
        L"\\LogicalDisk(%c:)\\Disk Write Bytes/sec",
        L"\\LogicalDisk(%c:)\\Disk Read Bytes/sec",
        L"\\LogicalDisk(%c:)\\Disk Write Bytes/sec",
    };
    char drv_for[4] = {drive_c, drive_c, drive_d, drive_d};

    for (int i = 0; i < 4; ++i) {
        wchar_t buf[128];
        swprintf_s(buf, templates[i], static_cast<wchar_t>(drv_for[i]));
        PdhAddEnglishCounterW(impl_->query, buf, 0, &impl_->counters[i]);
    }

    PdhCollectQueryData(impl_->query);  // 初回サンプリング
    impl_->phys_drives[0] = get_phys_drive(drive_c);
    impl_->phys_drives[1] = get_phys_drive(drive_d);
    log_info("Disk collector initialized (C: phys=%d, D: phys=%d)",
             impl_->phys_drives[0], impl_->phys_drives[1]);
    return true;
}

static float bytes_to_mb(double bytes) {
    return static_cast<float>(bytes / (1024.0 * 1024.0));
}

void DiskCollector::update(DiskMetrics& c, DiskMetrics& d) {
    if (!impl_) return;

    PdhCollectQueryData(impl_->query);

    auto get = [&](PDH_HCOUNTER hc) -> float {
        PDH_FMT_COUNTERVALUE v{};
        if (PdhGetFormattedCounterValue(hc, PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS) {
            return bytes_to_mb(v.doubleValue);
        }
        return 0.f;
    };

    c.read_mbps  = get(impl_->counters[0]);
    c.write_mbps = get(impl_->counters[1]);
    c.read_history.push(c.read_mbps);
    c.write_history.push(c.write_mbps);

    d.read_mbps  = get(impl_->counters[2]);
    d.write_mbps = get(impl_->counters[3]);
    d.read_history.push(d.read_mbps);
    d.write_history.push(d.write_mbps);
}

void DiskCollector::update_space(DiskMetrics& c, DiskMetrics& d) {
    if (!impl_) return;

    // GetDiskFreeSpaceExW: 第3引数=総容量、第4引数=ドライブ全体の空き容量
    auto fetch = [](char drive, DiskMetrics& dm) {
        wchar_t path[4] = {static_cast<wchar_t>(drive), L':', L'\\', L'\0'};
        ULARGE_INTEGER free_bytes{}, total_bytes{};
        if (!GetDiskFreeSpaceExW(path, nullptr, &total_bytes, &free_bytes)) {
            dm.total_gb = dm.used_gb = dm.used_pct = 0.f;
            return;
        }
        double total = static_cast<double>(total_bytes.QuadPart) / (1024.0 * 1024.0 * 1024.0);
        double free_ = static_cast<double>(free_bytes.QuadPart)  / (1024.0 * 1024.0 * 1024.0);
        double used  = total - free_;
        dm.total_gb  = static_cast<float>(total);
        dm.used_gb   = static_cast<float>(used);
        dm.used_pct  = (total > 0.0) ? static_cast<float>((used / total) * 100.0) : 0.f;
    };
    fetch(impl_->drives[0], c);
    fetch(impl_->drives[1], d);
}

// NVMe S.M.A.R.T. データを指定した物理ドライブから取得して dm に書き込む
// 取得成功時は dm.smart_avail = true をセットする
static void query_nvme_smart(int phys_drive, DiskMetrics& dm) {
    dm.smart_avail = false;
    if (phys_drive < 0) return;

    wchar_t drv_path[32];
    swprintf_s(drv_path, L"\\\\.\\PhysicalDrive%d", phys_drive);
    HANDLE h = CreateFileW(drv_path, 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    NvmeSmartQuery req{};
    req.PropertyId  = 49;    // StorageDeviceProtocolSpecificProperty
    req.QueryType   = 0;     // PropertyStandardQuery
    req.ProtocolType = 3;    // ProtocolTypeNvme
    req.DataType    = 2;     // NVMeDataTypeLogPage
    req.ReqValue    = 0x02;  // NVME_LOG_PAGE_HEALTH_INFO
    req.DataOffset  = 40;    // sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA)
    req.DataLength  = sizeof(NvmeHealthLog);

    BYTE rsp[kProtoDescSize + sizeof(NvmeHealthLog)]{};
    DWORD bytes = 0;
    bool ok = !!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
                                &req, sizeof(req),
                                rsp, sizeof(rsp), &bytes, nullptr);
    CloseHandle(h);
    if (!ok || bytes < kProtoDescSize + sizeof(NvmeHealthLog)) return;

    auto& log = *reinterpret_cast<NvmeHealthLog*>(rsp + kProtoDescSize);

    // 128-bit LE 値の下位 64bit を取得（memcpy でアライメント・strict aliasing 安全）
    ULONGLONG poh = 0, written = 0;
    std::memcpy(&poh,     log.power_on_hours,    sizeof(poh));
    std::memcpy(&written, log.data_units_written, sizeof(written));

    // コンポジット温度（Kelvin 16-bit LE → Celsius、poh に依存しない）
    uint16_t kelvin = static_cast<uint16_t>(log.temperature[0])
                    | (static_cast<uint16_t>(log.temperature[1]) << 8);
    dm.smart_temp_celsius = (kelvin > 0) ? static_cast<float>(kelvin) - 273.15f : 0.f;

    // TBW（bytes）= DataUnitsWritten × 512,000 bytes
    if (poh > 0) {
        double tbw_gb = static_cast<double>(written) * 512000.0
                        / (1024.0 * 1024.0 * 1024.0);
        dm.smart_write_gbh = static_cast<float>(tbw_gb / static_cast<double>(poh));
    }

    dm.smart_avail = true;
}

void DiskCollector::update_smart(DiskMetrics& c, DiskMetrics& d) {
    if (!impl_) return;

    c.phys_drive = impl_->phys_drives[0];
    d.phys_drive = impl_->phys_drives[1];

    query_nvme_smart(impl_->phys_drives[0], c);

    // 同一物理ドライブのコピーブロック。DiskMetrics に S.M.A.R.T. フィールドを追加した際は必ずここも更新すること
    if (impl_->phys_drives[1] == impl_->phys_drives[0]) {
        d.smart_write_gbh   = c.smart_write_gbh;
        d.smart_temp_celsius = c.smart_temp_celsius;
        d.smart_avail       = c.smart_avail;
    }
    else {
        query_nvme_smart(impl_->phys_drives[1], d);
    }
}

void DiskCollector::shutdown() {
    if (!impl_) return;
    if (impl_->query) { PdhCloseQuery(impl_->query); impl_->query = nullptr; }
    delete impl_;
    impl_ = nullptr;
}
