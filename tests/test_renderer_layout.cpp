// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#include "doctest.h"
#include "renderer.hpp"
#include "metrics.hpp"
#include "config.hpp"

// Renderer::init() を呼ばずに compute_preferred_height / update_core_animation のみを検証する。
// D2D/DWrite ポインタは nullptr のままだが、これらの 2 関数は描画リソースに触れない純関数。

TEST_CASE("Renderer::compute_preferred_height: 全表示時は正の高さ") {
    Renderer r;
    AllMetrics m;
    Visibility vis;  // 全 true
    int h = r.compute_preferred_height(m, vis);
    CHECK(h > 0);
}

TEST_CASE("Renderer::compute_preferred_height: CPU 非表示で総高さが減少") {
    Renderer r;
    AllMetrics m;
    Visibility vis_all;
    Visibility vis_no_cpu = vis_all;
    vis_no_cpu.cpu = false;
    int h_full = r.compute_preferred_height(m, vis_all);
    int h_no   = r.compute_preferred_height(m, vis_no_cpu);
    CHECK(h_no < h_full);
}

TEST_CASE("Renderer::compute_preferred_height: GPU OFF は GPU + VRAM を一括 OFF") {
    Renderer r;
    AllMetrics m;
    Visibility vis_all;
    Visibility vis_no_gpu = vis_all;
    vis_no_gpu.gpu = false;
    Visibility vis_no_mem = vis_all;
    vis_no_mem.mem = false;
    int h_full   = r.compute_preferred_height(m, vis_all);
    int h_no_gpu = r.compute_preferred_height(m, vis_no_gpu);
    int h_no_mem = r.compute_preferred_height(m, vis_no_mem);
    int diff_gpu = h_full - h_no_gpu;
    int diff_mem = h_full - h_no_mem;
    CHECK(diff_gpu > diff_mem);  // GPU + VRAM 両セクションが抜けるため mem 単独より減少幅が大きい
}

TEST_CASE("Renderer::compute_preferred_height: 全セクション OFF でも OS 行分は残る") {
    Renderer r;
    AllMetrics m;
    Visibility vis_none{false, false, false, false, false, false, false};
    int h_none = r.compute_preferred_height(m, vis_none);
    CHECK(h_none > 0);
    Visibility vis_all;
    int h_full = r.compute_preferred_height(m, vis_all);
    CHECK(h_none < h_full);
}

TEST_CASE("Renderer::compute_preferred_height: Claude Sub 表示時は Main 単独表示より高い") {
    Renderer r;
    AllMetrics m;
    m.claude_main.account_enabled = true;
    m.claude_sub.account_enabled  = true;

    Visibility vis_main_only;
    vis_main_only.claude_main = true;
    vis_main_only.claude_sub  = false;

    Visibility vis_both;
    vis_both.claude_main = true;
    vis_both.claude_sub  = true;

    int h_main = r.compute_preferred_height(m, vis_main_only);
    int h_both = r.compute_preferred_height(m, vis_both);
    CHECK(h_both > h_main);
}

TEST_CASE("Renderer::compute_preferred_height: GPU avail 時のほうが高い") {
    Renderer r;
    AllMetrics m;
    Visibility vis;
    m.gpu.avail  = false;
    m.vram.avail = false;
    int h_unavail = r.compute_preferred_height(m, vis);
    m.gpu.avail  = true;
    m.vram.avail = true;
    int h_avail   = r.compute_preferred_height(m, vis);
    CHECK(h_avail > h_unavail);
}

TEST_CASE("Renderer::update_core_animation: 初回呼び出しは同期するだけで false") {
    Renderer r;
    CpuMetrics m;
    m.core_pct = {25.f, 50.f, 75.f, 100.f};
    bool changed = r.update_core_animation(m);
    CHECK_FALSE(changed);  // 初回は assign で core_disp_ と一致、delta=0
}

TEST_CASE("Renderer::update_core_animation: 値変化で true、収束後は false") {
    Renderer r;
    CpuMetrics m;
    m.core_pct = {0.f, 0.f, 0.f, 0.f};
    r.update_core_animation(m);  // 同期

    m.core_pct = {50.f, 50.f, 50.f, 50.f};
    CHECK(r.update_core_animation(m));  // total_delta = 200 >= 0.5

    // LERP_K=0.33 の指数減衰で残差は急減し、いずれ DONE_THR=0.5 を下回る
    for (int i = 0; i < 50; ++i) r.update_core_animation(m);
    CHECK_FALSE(r.update_core_animation(m));
}
