// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#include "collector_net.hpp"
#include "logger.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <vector>
#pragma comment(lib, "pdh.lib")
struct NetCollector::Impl {
    PDH_HQUERY   query        = nullptr;
    PDH_HCOUNTER counter_send = nullptr;  // Bytes Sent/sec
    PDH_HCOUNTER counter_recv = nullptr;  // Bytes Received/sec
};

bool NetCollector::init() {
    impl_ = new Impl();

    if (PdhOpenQuery(nullptr, 0, &impl_->query) != ERROR_SUCCESS) {
        log_error("Net PDH init failed");
        return false;
    }

    // ワイルドカードで全 NIC のインスタンスを取得する
    // Network Interface オブジェクトには _Total インスタンスが存在しないため、
    // 個別 NIC を列挙して update 側で合算する
    PDH_STATUS st = PdhAddEnglishCounterW(impl_->query,
        L"\\Network Interface(*)\\Bytes Sent/sec",
        0, &impl_->counter_send);
    if (st != ERROR_SUCCESS) log_error("Net PDH add counter (send) failed (0x%08lX)", st);
    st = PdhAddEnglishCounterW(impl_->query,
        L"\\Network Interface(*)\\Bytes Received/sec",
        0, &impl_->counter_recv);
    if (st != ERROR_SUCCESS) log_error("Net PDH add counter (recv) failed (0x%08lX)", st);

    PdhCollectQueryData(impl_->query);
    log_info("Net collector initialized");
    return true;
}

static float bytes_to_kb(double bytes) {
    return static_cast<float>(bytes / 1024.0);
}

void NetCollector::update(NetMetrics& out) {
    if (!impl_) return;

    PdhCollectQueryData(impl_->query);

    // ワイルドカードカウンタの全インスタンス値を合算する
    // 1 回目の呼び出しで必要バッファサイズを得て確保し、2 回目で値を取得する
    auto sum_all = [](PDH_HCOUNTER hc) -> float {
        DWORD buf_size = 0;
        DWORD item_count = 0;
        if (PdhGetFormattedCounterArrayW(hc, PDH_FMT_DOUBLE, &buf_size, &item_count, nullptr)
                != PDH_MORE_DATA)
            return 0.f;

        std::vector<BYTE> buffer(buf_size);
        auto* items = reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(buffer.data());
        if (PdhGetFormattedCounterArrayW(hc, PDH_FMT_DOUBLE, &buf_size, &item_count, items)
                != ERROR_SUCCESS)
            return 0.f;

        double total = 0.0;
        for (DWORD i = 0; i < item_count; ++i) {
            // NIC 新規出現直後などは要素単位で無効データが混入する（doubleValue は未定義値）ためスキップする
            DWORD cs = items[i].FmtValue.CStatus;
            if (cs != PDH_CSTATUS_VALID_DATA && cs != PDH_CSTATUS_NEW_DATA) continue;
            total += items[i].FmtValue.doubleValue;
        }
        return bytes_to_kb(total);
    };

    out.send_kbps = sum_all(impl_->counter_send);
    out.recv_kbps = sum_all(impl_->counter_recv);
    out.send_history.push(out.send_kbps);
    out.recv_history.push(out.recv_kbps);

}

void NetCollector::shutdown() {
    if (!impl_) return;
    if (impl_->query) { PdhCloseQuery(impl_->query); impl_->query = nullptr; }
    delete impl_;
    impl_ = nullptr;
}
