// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
// std::min を使うため、windows.h の min/max マクロを無効化する。
// collector_disk.hpp が metrics.hpp 経由で windows.h を include するより前に定義する必要がある。
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "collector_disk.hpp"
#include "logger.hpp"
#include <windows.h>
#include <winioctl.h>   // IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, IOCTL_STORAGE_QUERY_PROPERTY
#include <pdh.h>
#pragma comment(lib, "pdh.lib")
#include <algorithm>  // std::min
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
//
// 戻り値 -1 は「物理ディスク実体が存在しない」ことを意味し、Google ドライブ等
// DRIVE_FIXED を名乗る仮想ファイルシステムの判別にも使う（enumerate_fixed_drives 参照）。
// 実機検証で判明した事実：Google ドライブの仮想ドライブは NumberOfDiskExtents こそ 1 を
// 返すが、Extents[0].ExtentLength が 0（開始オフセットも 0）のダミー値であり、
// 実パーティション（長さが実容量分ある）と明確に区別できる。DiskNumber だけでは
// 裏側の物理ディスク（他の実ドライブと同一）が返ってしまうため判別に使えない。
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
                        nullptr, 0, &buf, sizeof(buf), &bytes, nullptr) &&
        buf.vde.NumberOfDiskExtents > 0 &&
        buf.vde.Extents[0].ExtentLength.QuadPart > 0) {
        result = static_cast<int>(buf.vde.Extents[0].DiskNumber);
    }
    CloseHandle(h);
    return result;
}

std::vector<char> DiskCollector::enumerate_fixed_drives(int max_drives) {
    std::vector<char> out;
    DWORD mask = GetLogicalDrives();
    for (char c = 'C'; c <= 'Z'; ++c) {
        if (!(mask & (1u << (c - 'A')))) continue;

        wchar_t root[4] = {static_cast<wchar_t>(c), L':', L'\\', L'\0'};
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        if (get_phys_drive(c) < 0) continue;  // 物理実体なし＝仮想 FS として除外

        if (static_cast<int>(out.size()) >= max_drives) {
            log_info("Disk: drive %c: exceeds monitor limit (%d), excluded", c, max_drives);
            continue;
        }
        out.push_back(c);
    }
    return out;
}

struct DiskCollector::Impl {
    PDH_HQUERY query = nullptr;
    // ドライブごとに [0]=Read, [1]=Write の 2 カウンタを持つ
    std::vector<std::array<PDH_HCOUNTER, 2>> counters;
    std::vector<char> drives;
    std::vector<int>  phys_drives;  // 各ドライブの物理ドライブ番号（init 時に解決）
};

bool DiskCollector::init(const std::vector<char>& drives) {
    impl_ = new Impl();
    impl_->drives = drives;
    impl_->counters.resize(drives.size());
    impl_->phys_drives.resize(drives.size());

    if (PdhOpenQuery(nullptr, 0, &impl_->query) != ERROR_SUCCESS) {
        log_error("Disk PDH init failed");
        return false;
    }

    for (size_t i = 0; i < drives.size(); ++i) {
        wchar_t buf[128];
        swprintf_s(buf, L"\\LogicalDisk(%c:)\\Disk Read Bytes/sec", static_cast<wchar_t>(drives[i]));
        PdhAddEnglishCounterW(impl_->query, buf, 0, &impl_->counters[i][0]);
        swprintf_s(buf, L"\\LogicalDisk(%c:)\\Disk Write Bytes/sec", static_cast<wchar_t>(drives[i]));
        PdhAddEnglishCounterW(impl_->query, buf, 0, &impl_->counters[i][1]);
    }

    PdhCollectQueryData(impl_->query);  // 初回サンプリング（レート系カウンタは 2 サンプル必要）

    for (size_t i = 0; i < drives.size(); ++i)
        impl_->phys_drives[i] = get_phys_drive(drives[i]);

    log_info("Disk collector initialized (%zu drives)", drives.size());
    return true;
}

static float bytes_to_mb(double bytes) {
    return static_cast<float>(bytes / (1024.0 * 1024.0));
}

void DiskCollector::update(std::vector<DiskMetrics>& disks) {
    if (!impl_) return;

    PdhCollectQueryData(impl_->query);

    auto get = [&](PDH_HCOUNTER hc) -> float {
        PDH_FMT_COUNTERVALUE v{};
        if (PdhGetFormattedCounterValue(hc, PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS) {
            return bytes_to_mb(v.doubleValue);
        }
        return 0.f;
    };

    // disks は init に渡したドライブ列と同数・同順が契約だが、不整合時も暴走しないよう防御的に切り詰める
    const size_t n = std::min(impl_->drives.size(), disks.size());
    for (size_t i = 0; i < n; ++i) {
        disks[i].read_mbps  = get(impl_->counters[i][0]);
        disks[i].write_mbps = get(impl_->counters[i][1]);
        disks[i].read_history.push(disks[i].read_mbps);
        disks[i].write_history.push(disks[i].write_mbps);
    }
}

void DiskCollector::update_space(std::vector<DiskMetrics>& disks) {
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

    const size_t n = std::min(impl_->drives.size(), disks.size());
    for (size_t i = 0; i < n; ++i) fetch(impl_->drives[i], disks[i]);
}

// NVMe S.M.A.R.T. データ取得
// 指定した物理ドライブから S.M.A.R.T. データを取得して dm に書き込む。取得成功時は dm.smart_avail = true をセットする
static void query_nvme_smart(int phys_drive, DiskMetrics& dm) {
    dm.smart_avail = false;
    dm.smart_temp_avail = false;
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
    // NVMe 仕様上 kelvin==0 は温度センサー未実装を意味するため除外する
    uint16_t kelvin = static_cast<uint16_t>(log.temperature[0])
                    | (static_cast<uint16_t>(log.temperature[1]) << 8);
    if (kelvin > 0) {
        dm.smart_temp_celsius = static_cast<float>(kelvin) - 273.15f;
        dm.smart_temp_avail   = true;
    }

    // TBW（bytes）= DataUnitsWritten × 512,000 bytes
    if (poh > 0) {
        double tbw_gb = static_cast<double>(written) * 512000.0
                        / (1024.0 * 1024.0 * 1024.0);
        dm.smart_write_gbh = static_cast<float>(tbw_gb / static_cast<double>(poh));
    }

    dm.smart_avail = true;
}

void DiskCollector::update_smart(std::vector<DiskMetrics>& disks) {
    if (!impl_) return;

    const size_t n = std::min(impl_->drives.size(), disks.size());
    for (size_t i = 0; i < n; ++i) {
        disks[i].phys_drive = impl_->phys_drives[i];

        // 先行ドライブに同一物理ドライブがあればクエリを省略しコピーする（物理 1 台につき 1 回のみ）。
        // phys_drive < 0（解決失敗）同士は「別ドライブ」として扱う（誤って同一視しない）。
        int src = -1;
        for (size_t j = 0; j < i; ++j) {
            if (impl_->phys_drives[j] >= 0 && impl_->phys_drives[j] == impl_->phys_drives[i]) {
                src = static_cast<int>(j);
                break;
            }
        }

        if (src >= 0) {
            // 同一物理ドライブのコピーブロック。DiskMetrics に S.M.A.R.T. フィールドを追加した際は必ずここも更新すること
            disks[i].smart_write_gbh    = disks[src].smart_write_gbh;
            disks[i].smart_temp_celsius = disks[src].smart_temp_celsius;
            disks[i].smart_avail        = disks[src].smart_avail;
            disks[i].smart_temp_avail   = disks[src].smart_temp_avail;
        }
        else {
            query_nvme_smart(impl_->phys_drives[i], disks[i]);
        }
    }
}

void DiskCollector::shutdown() {
    if (!impl_) return;
    if (impl_->query) { PdhCloseQuery(impl_->query); impl_->query = nullptr; }
    delete impl_;
    impl_ = nullptr;
}
