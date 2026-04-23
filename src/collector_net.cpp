// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#include "collector_net.hpp"
#include "logger.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <pdh.h>
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

    // _Total で OS 側が重複排除した全 NIC 合算値を取得する
    PdhAddEnglishCounterW(impl_->query,
        L"\\Network Interface(_Total)\\Bytes Sent/sec",
        0, &impl_->counter_send);
    PdhAddEnglishCounterW(impl_->query,
        L"\\Network Interface(_Total)\\Bytes Received/sec",
        0, &impl_->counter_recv);

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

    auto get = [](PDH_HCOUNTER hc) -> float {
        PDH_FMT_COUNTERVALUE v{};
        if (PdhGetFormattedCounterValue(hc, PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS)
            return bytes_to_kb(v.doubleValue);
        return 0.f;
    };

    out.send_kbps = get(impl_->counter_send);
    out.recv_kbps = get(impl_->counter_recv);
    out.send_history.push(out.send_kbps);
    out.recv_history.push(out.recv_kbps);

}

void NetCollector::shutdown() {
    if (!impl_) return;
    if (impl_->query) { PdhCloseQuery(impl_->query); impl_->query = nullptr; }
    delete impl_;
    impl_ = nullptr;
}
