// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#include "doctest.h"
#include "alert.hpp"
#include "metrics.hpp"
#include "config.hpp"

namespace {

// AVG_SAMPLES=10 と同数のサンプルを埋めて total_history.average(10) を確定値にする
void fill_cpu_history(AllMetrics& m, float v) {
    for (int i = 0; i < 10; ++i) m.cpu.total_history.push(v);
}

// AlertManager::Id ビット位置からマスクを構成する
constexpr uint32_t bit(int id) { return 1u << id; }

}

TEST_CASE("AlertManager::check: 閾値未満では発火しない") {
    AlertManager mgr;
    AppConfig cfg;          // warn_cpu_pct = 95, reset_cpu_pct = 90
    AllMetrics m;
    fill_cpu_history(m, 50.f);
    uint32_t r = mgr.check(m, cfg, /*mute=*/true);
    CHECK((r & bit(AlertManager::CPU)) == 0);
}

TEST_CASE("AlertManager::check: 閾値超過で 1 回だけ発火（ヒステリシス）") {
    AlertManager mgr;
    AppConfig cfg;
    AllMetrics m;
    fill_cpu_history(m, 97.f);
    uint32_t r1 = mgr.check(m, cfg, true);
    CHECK((r1 & bit(AlertManager::CPU)) != 0);

    uint32_t r2 = mgr.check(m, cfg, true);
    CHECK((r2 & bit(AlertManager::CPU)) == 0);  // 同一値の連続呼出では再発火しない
}

TEST_CASE("AlertManager::check: リセット閾値を下回ると次回発火可能") {
    AlertManager mgr;
    AppConfig cfg;          // reset_cpu_pct = 90
    AllMetrics m;
    fill_cpu_history(m, 97.f);
    mgr.check(m, cfg, true);   // 1 回目発火

    fill_cpu_history(m, 80.f); // reset 90 を下回らせて解除
    mgr.check(m, cfg, true);

    fill_cpu_history(m, 97.f); // 再上昇
    uint32_t r = mgr.check(m, cfg, true);
    CHECK((r & bit(AlertManager::CPU)) != 0);
}

TEST_CASE("AlertManager::check: UPTIME は一度発火すると値が戻っても再発火しない（one-shot）") {
    AlertManager mgr;
    AppConfig cfg;          // warn_uptime_days = 7
    AllMetrics m;
    fill_cpu_history(m, 0.f);

    m.os.uptime_ms = 8ULL * 86400ULL * 1000ULL;
    uint32_t r1 = mgr.check(m, cfg, true);
    CHECK((r1 & bit(AlertManager::UPTIME)) != 0);

    m.os.uptime_ms = 1ULL * 86400ULL * 1000ULL;
    uint32_t r2 = mgr.check(m, cfg, true);
    CHECK((r2 & bit(AlertManager::UPTIME)) == 0);

    m.os.uptime_ms = 9ULL * 86400ULL * 1000ULL;
    uint32_t r3 = mgr.check(m, cfg, true);
    CHECK((r3 & bit(AlertManager::UPTIME)) == 0);
}

TEST_CASE("AlertManager::check: GPU 不可なら GPU 系項目は発火しない") {
    AlertManager mgr;
    AppConfig cfg;
    AllMetrics m;
    fill_cpu_history(m, 0.f);
    m.gpu.avail = false;
    for (int i = 0; i < 10; ++i) m.gpu.usage_history.push(99.f);
    m.gpu.temp_celsius = 200.f;
    uint32_t r = mgr.check(m, cfg, true);
    CHECK((r & bit(AlertManager::GPU))      == 0);
    CHECK((r & bit(AlertManager::TEMP_GPU)) == 0);
}

TEST_CASE("AlertManager::check: Claude expected_pct=0 のとき判定をスキップ") {
    AlertManager mgr;
    AppConfig cfg;
    AllMetrics m;
    fill_cpu_history(m, 0.f);
    m.claude.avail              = true;
    m.claude.five_h_pct          = 99.f;
    m.claude.five_h_expected_pct = 0.f;  // タイミング不明（リセット時刻未取得）
    uint32_t r = mgr.check(m, cfg, true);
    CHECK((r & bit(AlertManager::CLAUDE_5H)) == 0);
}

TEST_CASE("AlertManager::check: mute=true でも内部の発火状態は更新される") {
    // mute は音の再生だけを抑制し、戻り値のビットマスクとヒステリシス遷移は通常どおり進む。
    AlertManager mgr;
    AppConfig cfg;
    AllMetrics m;
    fill_cpu_history(m, 97.f);
    uint32_t r1 = mgr.check(m, cfg, /*mute=*/true);
    CHECK((r1 & bit(AlertManager::CPU)) != 0);  // mute でも fired_mask は返る

    uint32_t r2 = mgr.check(m, cfg, /*mute=*/true);
    CHECK((r2 & bit(AlertManager::CPU)) == 0);  // 内部 fired_ も立っているため再発火しない
}
