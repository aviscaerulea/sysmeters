// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#include "collector_net.hpp"
#include "logger.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <pdh.h>
#pragma comment(lib, "pdh.lib")
#include <vector>

struct NetCollector::Impl {
    PDH_HQUERY   query        = nullptr;
    PDH_HCOUNTER counter_send = nullptr;  // Bytes Sent/sec
    PDH_HCOUNTER counter_recv = nullptr;  // Bytes Received/sec
    PDH_HCOUNTER counter_bw   = nullptr;  // Current Bandwidth（bps）
};

bool NetCollector::init() {
    impl_ = new Impl();

    if (PdhOpenQuery(nullptr, 0, &impl_->query) != ERROR_SUCCESS) {
        log_error("Net PDH init failed");
        return false;
    }

    // _Total で全 NIC 合算
    PdhAddEnglishCounterW(impl_->query,
        L"\\Network Interface(*)\\Bytes Sent/sec",
        0, &impl_->counter_send);
    PdhAddEnglishCounterW(impl_->query,
        L"\\Network Interface(*)\\Bytes Received/sec",
        0, &impl_->counter_recv);
    PdhAddEnglishCounterW(impl_->query,
        L"\\Network Interface(*)\\Current Bandwidth",
        0, &impl_->counter_bw);

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

    // PDH_FMT_DOUBLE で各 NIC の合計を取得（* ワイルドカードの場合は配列になる）
    // 全 NIC 合算のためスカラー形式で取得を試みる
    auto get_sum = [](PDH_HCOUNTER hc) -> float {
        // ARRAY 形式で取得して合算する
        DWORD buf_size = 0, item_count = 0;
        PdhGetFormattedCounterArray(hc, PDH_FMT_DOUBLE, &buf_size, &item_count, nullptr);
        if (buf_size == 0) return 0.f;

        // buf_size はバイト数（文字列データを含む）なので raw バッファで確保してキャストする
        std::vector<char> raw(buf_size);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM*>(raw.data());
        if (PdhGetFormattedCounterArray(hc, PDH_FMT_DOUBLE,
            &buf_size, &item_count, items) != ERROR_SUCCESS) {
            return 0.f;
        }
        double sum = 0.0;
        for (DWORD i = 0; i < item_count; ++i) sum += items[i].FmtValue.doubleValue;
        return bytes_to_kb(sum);
    };

    out.send_kbps = get_sum(impl_->counter_send);
    out.recv_kbps = get_sum(impl_->counter_recv);
    out.send_history.push(out.send_kbps);
    out.recv_history.push(out.recv_kbps);

    // NIC リンク速度（bps → KB/s 変換）
    // ワイルドカードカウンタは ARRAY 形式で取得して最大値を採用する
    {
        DWORD buf_size = 0, item_count = 0;
        PdhGetFormattedCounterArray(impl_->counter_bw, PDH_FMT_DOUBLE, &buf_size, &item_count, nullptr);
        if (buf_size > 0) {
            std::vector<char> raw(buf_size);
            auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM*>(raw.data());
            if (PdhGetFormattedCounterArray(impl_->counter_bw, PDH_FMT_DOUBLE,
                &buf_size, &item_count, items) == ERROR_SUCCESS) {
                double max_bps = 0.0;
                for (DWORD i = 0; i < item_count; ++i)
                    max_bps = max(max_bps, items[i].FmtValue.doubleValue);
                out.link_kbps = static_cast<float>(max_bps / 8.0 / 1024.0);
            }
        }
    }
}

void NetCollector::shutdown() {
    if (!impl_) return;
    if (impl_->query) { PdhCloseQuery(impl_->query); impl_->query = nullptr; }
    delete impl_;
    impl_ = nullptr;
}
