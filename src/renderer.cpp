// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#include "renderer.hpp"
#include "logger.hpp"
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cwchar>

// ウィンドウレイアウト定数（クライアント領域内）
static constexpr float PAD        = 11.f;   // 内側パディング
static constexpr float SECTION_H  = 24.f;   // セクションラベル高さ（18pt 対応）
static constexpr float GRAPH_H    = 72.f;   // 面グラフ高さ（60 * 1.2）
static constexpr float GRAPH_H_LG = 86.f;  // CPU/GPU 面グラフ高さ（72 * 1.2）
static constexpr float BAR_H      = 16.f;   // 横バー高さ（20 * 0.8）
static constexpr float CORE_BAR_H = 40.f;   // コア縦バー高さ
static constexpr float LINE_H     = 30.f;   // 1行テキスト高さ（22pt 対応）
static constexpr float GAP        = 6.f;    // 要素間ギャップ
static constexpr float SECTION_GAP = 2.f;  // セクション間の追加スペース
static constexpr float TOTAL_W    = 50.f;   // RAM/VRAM 総量テキスト幅（"64GB" 相当）
static constexpr float DISK_GAP   = 20.f;  // Disk I/O グラフと Space バーの間ギャップ
static constexpr float INFO_LINE_H = 27.f;  // Space 下テキスト行高さ（容量/GB/h）

// リングバッファの最大値を返す
static float buf_max(const RingBuffer<float, 60>& b) {
    float m = 0.f;
    for (std::size_t i = 0; i < b.size(); ++i) m = max(m, b.at(i));
    return m;
}

// COM インターフェイスポインタを Release して nullptr にする
template<typename T>
static void safe_release(T** p) { if (*p) { (*p)->Release(); *p = nullptr; } }

// 0xRRGGBB → D2D1_COLOR_F 変換
static D2D1_COLOR_F from_rgb(uint32_t rgb, float alpha = 1.f) {
    return D2D1::ColorF(
        ((rgb >> 16) & 0xFF) / 255.f,
        ((rgb >>  8) & 0xFF) / 255.f,
        ( rgb        & 0xFF) / 255.f,
        alpha);
}

uint32_t Renderer::temp_color(float c) {
    if (c >= 90.f) return 0xEF5350;   // 赤
    if (c >= 70.f) return 0xFFA726;   // オレンジ
    return 0x888888;                   // グレー（正常範囲）
}

bool Renderer::init(HWND hwnd, const AppConfig& cfg) {
    hwnd_ = hwnd;

    // D2D ファクトリ作成
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_factory_))) {
        log_error("D2D factory creation failed");
        return false;
    }

    // DirectWrite ファクトリ作成
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&dwrite_factory_)))) {
        log_error("DWrite factory creation failed");
        return false;
    }

    // フォント作成（失敗時は false を返す）
    // CPU/GPU 以外のセクションで使用（要求により 2 倍サイズ）
    if (FAILED(dwrite_factory_->CreateTextFormat(L"Consolas", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        22.f, L"ja-JP", &font_normal_))) return false;

    if (FAILED(dwrite_factory_->CreateTextFormat(L"Consolas", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        18.f, L"ja-JP", &font_small_))) return false;

    if (FAILED(dwrite_factory_->CreateTextFormat(L"Consolas", nullptr,
        DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        22.f, L"ja-JP", &font_large_))) return false;

    // CPU/GPU 使用率オーバーレイ用（33pt × 1.2 ≈ 40pt bold）
    if (FAILED(dwrite_factory_->CreateTextFormat(L"Consolas", nullptr,
        DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        40.f, L"ja-JP", &font_xlarge_))) return false;
    // グラフ内のテキストを縦中央揃えにしてパーセンテージと温度のベースラインを揃える
    font_xlarge_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    create_device_resources(cfg);
    return true;
}

void Renderer::create_device_resources(const AppConfig& cfg) {
    if (!d2d_factory_ || render_target_) return;

    RECT rc;
    GetClientRect(hwnd_, &rc);
    D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
    D2D1_HWND_RENDER_TARGET_PROPERTIES hwnd_props = D2D1::HwndRenderTargetProperties(hwnd_, size);

    d2d_factory_->CreateHwndRenderTarget(props, hwnd_props, &render_target_);
    if (!render_target_) return;

    render_target_->CreateSolidColorBrush(from_rgb(cfg.col_text),       &brush_text_);
    render_target_->CreateSolidColorBrush(from_rgb(cfg.col_graph_fill), &brush_fill_);
}

void Renderer::release_device_resources() {
    safe_release(&brush_text_);
    safe_release(&brush_fill_);
    safe_release(&render_target_);
}

void Renderer::resize(UINT w, UINT h) {
    if (render_target_) render_target_->Resize(D2D1::SizeU(w, h));
}

void Renderer::shutdown() {
    release_device_resources();
    safe_release(&font_normal_);
    safe_release(&font_small_);
    safe_release(&font_large_);
    safe_release(&font_xlarge_);
    safe_release(&dwrite_factory_);
    safe_release(&d2d_factory_);
}

// 指定色でブラシを使い回す（毎回 SetColor する）
static void set_brush_color(ID2D1SolidColorBrush* b, uint32_t rgb, float alpha = 1.f) {
    b->SetColor(from_rgb(rgb, alpha));
}

void Renderer::draw_section_label_with_model(float x, float y, float ww,
    const wchar_t* prefix, const char* model_name, const AppConfig& cfg) {
    static constexpr float PREFIX_W = 55.f;
    set_brush_color(brush_text_, cfg.col_text);
    render_target_->DrawText(prefix, static_cast<UINT32>(wcslen(prefix)), font_normal_,
        D2D1::RectF(x, y, x + PREFIX_W, y + SECTION_H), brush_text_);
    if (model_name && model_name[0]) {
        wchar_t lbl[48] = {};
        mbstowcs_s(nullptr, lbl, model_name, _TRUNCATE);
        set_brush_color(brush_text_, cfg.col_text, 0.6f);
        render_target_->DrawText(lbl, static_cast<UINT32>(wcslen(lbl)), font_small_,
            D2D1::RectF(x + PREFIX_W, y, x + ww, y + SECTION_H), brush_text_);
    }
}

// グラフ領域にグリッド線を描画する（10 秒間隔の縦線 6 本 + 25% 間隔の横線 4 本）
void Renderer::draw_grid(D2D1_RECT_F rect) {
    float w = rect.right - rect.left;
    float h = rect.bottom - rect.top;

    set_brush_color(brush_fill_, 0x3A3A3A, 0.8f);

    // 縦線（10 秒間隔）
    for (int i = 1; i <= 5; ++i) {
        float x = rect.left + w * (i * 10.f / 60.f);
        render_target_->DrawLine(
            D2D1::Point2F(x, rect.top), D2D1::Point2F(x, rect.bottom),
            brush_fill_, 0.5f);
    }
    // 横線（25% 間隔）
    for (int i = 1; i <= 3; ++i) {
        float y = rect.top + h * (i / 4.f);
        render_target_->DrawLine(
            D2D1::Point2F(rect.left, y), D2D1::Point2F(rect.right, y),
            brush_fill_, 0.5f);
    }
}

// 面グラフ描画
void Renderer::draw_area_graph(const RingBuffer<float, 60>& buf,
                               float max_val, D2D1_RECT_F rect, uint32_t color_rgb) {
    if (max_val <= 0.f) return;

    // グラフ領域にクリッピング（ストロークのはみ出し防止）
    render_target_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    // グラフ背景（ほぼ黒）
    set_brush_color(brush_fill_, 0x0D0D0D);
    render_target_->FillRectangle(rect, brush_fill_);

    // グリッド線
    draw_grid(rect);

    if (buf.empty()) {
        render_target_->PopAxisAlignedClip();
        return;
    }

    float w = rect.right - rect.left;
    float h = rect.bottom - rect.top;
    float dx = w / 60.f;

    ID2D1PathGeometry* path = nullptr;
    if (FAILED(d2d_factory_->CreatePathGeometry(&path))) {
        render_target_->PopAxisAlignedClip();
        return;
    }

    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(path->Open(&sink))) {
        path->Release();
        render_target_->PopAxisAlignedClip();
        return;
    }

    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    sink->BeginFigure(D2D1::Point2F(rect.left, rect.bottom), D2D1_FIGURE_BEGIN_FILLED);

    std::size_t sz = buf.size();
    std::size_t start = 60 - sz;
    for (std::size_t i = 0; i < 60; ++i) {
        float v = (i >= start) ? buf.at(i - start) : 0.f;
        v = min(v, max_val);
        float px = rect.left + static_cast<float>(i) * dx;
        float py = rect.bottom - (v / max_val) * h;
        sink->AddLine(D2D1::Point2F(px, py));
    }

    sink->AddLine(D2D1::Point2F(rect.right, rect.bottom));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) {
        sink->Release();
        path->Release();
        render_target_->PopAxisAlignedClip();
        return;
    }
    sink->Release();

    // 塗りつぶし（半透明）
    set_brush_color(brush_fill_, color_rgb, 0.3f);
    render_target_->FillGeometry(path, brush_fill_);
    // 輪郭線（不透明）
    set_brush_color(brush_fill_, color_rgb, 1.f);
    render_target_->DrawGeometry(path, brush_fill_, 1.5f);
    path->Release();

    render_target_->PopAxisAlignedClip();
}

// 横バー描画（背景 + 塗り）
void Renderer::draw_hbar(float val, float max_val, D2D1_RECT_F rect, uint32_t color_rgb) {
    // 背景
    set_brush_color(brush_fill_, 0x2A2A2A);
    render_target_->FillRectangle(rect, brush_fill_);

    // 塗り
    float fill_w = (max_val > 0.f) ? ((val / max_val) * (rect.right - rect.left)) : 0.f;
    fill_w = min(fill_w, rect.right - rect.left);
    if (fill_w > 0.f) {
        D2D1_RECT_F filled = D2D1::RectF(rect.left, rect.top, rect.left + fill_w, rect.bottom);
        set_brush_color(brush_fill_, color_rgb);
        render_target_->FillRectangle(filled, brush_fill_);
    }
}

// 縦バー描画（0-100% の高さ）
void Renderer::draw_vbar(float pct, D2D1_RECT_F rect, uint32_t color_rgb) {
    // 背景
    set_brush_color(brush_fill_, 0x2A2A2A);
    render_target_->FillRectangle(rect, brush_fill_);

    float h = rect.bottom - rect.top;
    float fill_h = min(pct / 100.f, 1.f) * h;
    if (fill_h > 0.f) {
        D2D1_RECT_F filled = D2D1::RectF(rect.left, rect.bottom - fill_h, rect.right, rect.bottom);
        set_brush_color(brush_fill_, color_rgb);
        render_target_->FillRectangle(filled, brush_fill_);
    }
}

// ---- 各セクション描画 ----

float Renderer::draw_os(const OsMetrics& m, const AppConfig& cfg, float y) {
    static constexpr float UPTIME_W  = 150.f;  // "99日 23時間59分" が収まる幅
    float x  = PAD;
    float ww = static_cast<float>(cfg.win_width) - PAD * 2;

    // OS ラベル（左端、アップタイム幅を除いた範囲、alpha 0.6）
    if (m.os_label[0]) {
        set_brush_color(brush_text_, cfg.col_text, 0.6f);
        render_target_->DrawText(m.os_label, static_cast<UINT32>(wcslen(m.os_label)), font_small_,
            D2D1::RectF(x, y, x + ww - UPTIME_W, y + SECTION_H), brush_text_);
    }

    // アップタイム（右寄せ、alpha 0.6）
    if (m.uptime_ms > 0) {
        ULONGLONG secs  = m.uptime_ms / 1000;
        ULONGLONG mins  = (secs / 60) % 60;
        ULONGLONG hours = (secs / 3600) % 24;
        ULONGLONG days  = secs / 86400;

        wchar_t ubuf[32];
        if (days > 0)
            swprintf_s(ubuf, L"%llu日 %02llu時間%02llu分", days, hours, mins);
        else
            swprintf_s(ubuf, L"%02llu時間%02llu分", hours, mins);

        set_brush_color(brush_text_, cfg.col_text, 0.6f);
        font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        render_target_->DrawText(ubuf, static_cast<UINT32>(wcslen(ubuf)), font_small_,
            D2D1::RectF(x, y, x + ww, y + SECTION_H), brush_text_);
        font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    return y + SECTION_H;
}

float Renderer::draw_cpu(const CpuMetrics& m, const AppConfig& cfg, float y) {
    float x  = PAD;
    float ww = static_cast<float>(cfg.win_width) - PAD * 2;

    draw_section_label_with_model(x, y, ww, L"CPU", m.name, cfg);
    y += SECTION_H;

    // 全体使用率 面グラフ（全幅）+ オーバーレイテキスト
    wchar_t buf[32];
    D2D1_RECT_F gr = D2D1::RectF(x, y, x + ww, y + GRAPH_H_LG);
    draw_area_graph(m.total_history, 100.f, gr, cfg.col_graph_fill);

    // パーセンテージ（左寄せ、95% 超で赤、font_xlarge_）
    swprintf_s(buf, L"%4.1f%%", m.total_pct);
    uint32_t cpu_text_col = (m.total_pct > 95.f) ? 0xEF5350 : cfg.col_text;
    set_brush_color(brush_text_, cpu_text_col, 0.9f);
    D2D1_RECT_F ol = D2D1::RectF(x + 4.f, y + 4.f, x + ww - 4.f, y + GRAPH_H_LG - 4.f);
    render_target_->DrawText(buf, static_cast<UINT32>(wcslen(buf)), font_xlarge_, ol, brush_text_);

    // 温度（右寄せ、3 段階色。取得不可時は "--℃"）
    wchar_t tbuf[16];
    if (m.temp_avail) {
        swprintf_s(tbuf, L"%3.0f\u2103", m.temp_celsius);
        set_brush_color(brush_text_, temp_color(m.temp_celsius), 0.9f);
    }
    else {
        swprintf_s(tbuf, L"--\u2103");
        set_brush_color(brush_text_, 0x555555, 0.9f);
    }
    font_large_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    render_target_->DrawText(tbuf, static_cast<UINT32>(wcslen(tbuf)), font_large_, ol, brush_text_);
    font_large_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

    y += GRAPH_H_LG + GAP;

    // コア別縦バー（論理コア数本横並び、画面幅に合わせて動的計算）
    const int   N_CORES = static_cast<int>(std::size(m.core_pct));
    constexpr float GAP_BAR = 2.f;  // バー間ギャップ
    float bar_w = (ww - GAP_BAR * (N_CORES - 1)) / N_CORES;
    float core_x = x;
    for (int i = 0; i < N_CORES; ++i) {
        D2D1_RECT_F cr = D2D1::RectF(core_x, y, core_x + bar_w, y + CORE_BAR_H);
        uint32_t core_col = (m.core_pct[i] > 95.f) ? 0xEF5350 : cfg.col_cpu_core;
        draw_vbar(m.core_pct[i], cr, core_col);
        core_x += bar_w + GAP_BAR;
    }
    y += CORE_BAR_H + GAP;

    return y;
}

float Renderer::draw_gpu(const GpuMetrics& m, const AppConfig& cfg, float y) {
    float x  = PAD;
    float ww = static_cast<float>(cfg.win_width) - PAD * 2;

    draw_section_label_with_model(x, y, ww, L"GPU", m.name, cfg);
    y += SECTION_H;

    if (!m.avail) {
        set_brush_color(brush_text_, 0x888888);
        D2D1_RECT_F r = D2D1::RectF(x, y, x + ww, y + LINE_H);
        render_target_->DrawText(L"N/A", 3, font_normal_, r, brush_text_);
        return y + LINE_H + GAP;
    }

    // 使用率 面グラフ（全幅）+ オーバーレイテキスト
    wchar_t buf[32];
    D2D1_RECT_F gr = D2D1::RectF(x, y, x + ww, y + GRAPH_H_LG);
    draw_area_graph(m.usage_history, 100.f, gr, cfg.col_graph_fill);

    // パーセンテージ（左寄せ、95% 超で赤、font_xlarge_）
    swprintf_s(buf, L"%4.1f%%", m.usage_pct);
    uint32_t gpu_text_col = (m.usage_pct > 95.f) ? 0xEF5350 : cfg.col_text;
    set_brush_color(brush_text_, gpu_text_col, 0.9f);
    D2D1_RECT_F ol = D2D1::RectF(x + 4.f, y + 4.f, x + ww - 4.f, y + GRAPH_H_LG - 4.f);
    render_target_->DrawText(buf, static_cast<UINT32>(wcslen(buf)), font_xlarge_, ol, brush_text_);

    // 温度（右寄せ、3 段階色）
    wchar_t tbuf[16];
    swprintf_s(tbuf, L"%3.0f\u2103", m.temp_celsius);
    set_brush_color(brush_text_, temp_color(m.temp_celsius), 0.9f);
    font_large_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    render_target_->DrawText(tbuf, static_cast<UINT32>(wcslen(tbuf)), font_large_, ol, brush_text_);
    font_large_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

    y += GRAPH_H_LG + GAP;

    return y;
}

float Renderer::draw_mem(const MemMetrics& m, const AppConfig& cfg, float y) {
    float x  = PAD;
    float ww = static_cast<float>(cfg.win_width) - PAD * 2;

    // RAM テキスト（使用率は font_normal_ 左寄せ、GB は font_small_ 右寄せ）
    uint32_t ram_col = (m.usage_pct > 90.f) ? 0xEF5350 : cfg.col_text;
    wchar_t buf[64];
    swprintf_s(buf, L"RAM   %5.1f%%", m.usage_pct);
    set_brush_color(brush_text_, ram_col);
    D2D1_RECT_F tr = D2D1::RectF(x, y, x + ww, y + LINE_H);
    render_target_->DrawText(buf, static_cast<UINT32>(wcslen(buf)), font_normal_, tr, brush_text_);

    // 使用 GB（右寄せ）
    wchar_t gbuf[16];
    swprintf_s(gbuf, L"%5.1fGB", m.used_gb);
    set_brush_color(brush_text_, ram_col, 0.8f);
    font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    render_target_->DrawText(gbuf, static_cast<UINT32>(wcslen(gbuf)), font_small_, tr, brush_text_);

    // WSL 使用量（RAM GB 表示と重ならないよう右端 80px を除いた矩形で右寄せ）
    if (m.wsl_gb > 0.f) {
        wchar_t wslbuf[24];
        swprintf_s(wslbuf, L"WSL %4.1fGB", m.wsl_gb);
        D2D1_RECT_F wlr = D2D1::RectF(x, y, x + ww - 80.f, y + LINE_H);
        font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        render_target_->DrawText(wslbuf, static_cast<UINT32>(wcslen(wslbuf)), font_small_, wlr, brush_text_);
    }

    font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    y += LINE_H + 2.f;

    // RAM バー：全体使用量を通常色で描画し、WSL 分を同系色の濃いオーバーレイで重ねる
    D2D1_RECT_F br = D2D1::RectF(x, y, x + ww - TOTAL_W - 4.f, y + BAR_H);
    uint32_t bar_col = (m.usage_pct > 90.f) ? 0xEF5350 : cfg.col_graph_fill;
    draw_hbar(m.usage_pct, 100.f, br, bar_col);

    if (m.wsl_gb > 0.f && m.total_gb > 0.f) {
        float bar_w    = br.right - br.left;
        float wsl_fill = min(m.wsl_gb / m.total_gb, 1.f) * bar_w;
        if (wsl_fill > 0.f) {
            D2D1_RECT_F wr = D2D1::RectF(br.left, br.top, br.left + wsl_fill, br.bottom);
            set_brush_color(brush_fill_, 0xCC6B3A, 0.9f);
            render_target_->FillRectangle(wr, brush_fill_);
        }
    }

    // 総量テキストをバー右側に表示（棒グラフと縦位置を合わせるため上にシフト）
    wchar_t totbuf[16];
    swprintf_s(totbuf, L"%2.0fGB", m.total_gb);
    set_brush_color(brush_text_, cfg.col_text);
    D2D1_RECT_F tr2 = D2D1::RectF(x + ww - TOTAL_W, y - 5.f, x + ww, y + BAR_H);
    font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    render_target_->DrawText(totbuf, static_cast<UINT32>(wcslen(totbuf)), font_small_, tr2, brush_text_);
    font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

    y += BAR_H + GAP;

    return y;
}

float Renderer::draw_vram(const VramMetrics& m, const AppConfig& cfg, float y) {
    float x  = PAD;
    float ww = static_cast<float>(cfg.win_width) - PAD * 2;

    if (!m.avail) {
        set_brush_color(brush_text_, 0x888888);
        D2D1_RECT_F r = D2D1::RectF(x, y, x + ww, y + LINE_H);
        render_target_->DrawText(L"VRAM  N/A", 9, font_normal_, r, brush_text_);
        return y + LINE_H + GAP;
    }

    // VRAM テキスト（使用率は font_normal_ 左寄せ、GB は font_small_ 右寄せ）
    uint32_t vram_col = (m.usage_pct > 90.f) ? 0xEF5350 : cfg.col_text;
    wchar_t buf[64];
    swprintf_s(buf, L"VRAM  %5.1f%%", m.usage_pct);
    set_brush_color(brush_text_, vram_col);
    D2D1_RECT_F tr = D2D1::RectF(x, y, x + ww, y + LINE_H);
    render_target_->DrawText(buf, static_cast<UINT32>(wcslen(buf)), font_normal_, tr, brush_text_);

    // GB 表示（右寄せ、font_small_）
    wchar_t gbuf[16];
    swprintf_s(gbuf, L"%5.1fGB", m.used_gb);
    set_brush_color(brush_text_, vram_col, 0.8f);
    font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    render_target_->DrawText(gbuf, static_cast<UINT32>(wcslen(gbuf)), font_small_, tr, brush_text_);
    font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

    y += LINE_H + 2.f;

    D2D1_RECT_F br = D2D1::RectF(x, y, x + ww - TOTAL_W - 4.f, y + BAR_H);
    draw_hbar(m.usage_pct, 100.f, br, (m.usage_pct > 90.f) ? 0xEF5350 : cfg.col_graph_fill);

    // 総量テキストをバー右側に表示（棒グラフと縦位置を合わせるため上にシフト）
    wchar_t totbuf[16];
    swprintf_s(totbuf, L"%2.0fGB", m.total_gb);
    set_brush_color(brush_text_, cfg.col_text);
    D2D1_RECT_F tr2 = D2D1::RectF(x + ww - TOTAL_W, y - 5.f, x + ww, y + BAR_H);
    font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    render_target_->DrawText(totbuf, static_cast<UINT32>(wcslen(totbuf)), font_small_, tr2, brush_text_);
    font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

    y += BAR_H + GAP;

    return y;
}

float Renderer::draw_disk(const DiskMetrics& c, const DiskMetrics& d,
                           const AppConfig& cfg, float y) {
    float x  = PAD;
    float ww = static_cast<float>(cfg.win_width) - PAD * 2;
    float sw = (ww - DISK_GAP) / 3.f;  // 右：Space 列幅（1/3）
    float gw = ww - DISK_GAP - sw;     // 左：I/O グラフ幅（2/3）

    set_brush_color(brush_text_, cfg.col_text);
    render_target_->DrawText(L"Disk", 4, font_normal_,
        D2D1::RectF(x, y, x + ww, y + LINE_H), brush_text_);
    y += LINE_H;

    // ドライブ 1 行分（I/O 面グラフ＋Space 横バー＋S.M.A.R.T. を横並びで描画）
    // prev: 前のドライブ（C: は nullptr）。同一物理ドライブなら SMART 行を省略する
    auto draw_drive = [&](const DiskMetrics& dm, const DiskMetrics* prev) {
        // --- 左 2/3：I/O ---
        wchar_t buf[64];
        swprintf_s(buf, L"%c: R %.1f  W %.1f MB/s", dm.drive, dm.read_mbps, dm.write_mbps);
        set_brush_color(brush_text_, cfg.col_text);
        D2D1_RECT_F tr = D2D1::RectF(x, y, x + gw, y + LINE_H);
        render_target_->DrawText(buf, static_cast<UINT32>(wcslen(buf)), font_small_, tr, brush_text_);

        float max_val = max(1.f, dm.peak_mbps);
        D2D1_RECT_F gr = D2D1::RectF(x, y + LINE_H, x + gw, y + LINE_H + GRAPH_H);
        draw_area_graph(dm.read_history,  max_val, gr, cfg.col_disk_read);
        draw_area_graph(dm.write_history, max_val, gr, cfg.col_disk_write);

        // NVMe 温度（グラフ右上オーバーレイ、SMART 有効時のみ）
        if (dm.smart_avail) {
            wchar_t tbuf[16];
            swprintf_s(tbuf, L"%3.0f\u2103", dm.smart_temp_celsius);
            set_brush_color(brush_text_, temp_color(dm.smart_temp_celsius), 0.9f);
            font_large_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            D2D1_RECT_F ol = D2D1::RectF(x + 4.f, y + LINE_H + 4.f, x + gw - 4.f, y + LINE_H + GRAPH_H - 4.f);
            render_target_->DrawText(tbuf, static_cast<UINT32>(wcslen(tbuf)), font_large_, ol, brush_text_);
            font_large_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }

        // --- 右 1/3：Space（テキスト + バー + 容量テキスト縦積み）---
        float sx = x + gw + DISK_GAP;
        uint32_t sp_col = (dm.used_pct > 90.f) ? 0xEF5350 : cfg.col_text;

        // テキスト行（"Used" 左寄せ + パーセンテージ右寄せ）
        set_brush_color(brush_text_, sp_col);
        D2D1_RECT_F str = D2D1::RectF(sx, y, sx + sw, y + LINE_H);
        render_target_->DrawText(L"Used", 4, font_small_, str, brush_text_);
        wchar_t sbuf[16];
        swprintf_s(sbuf, L"%5.1f%%", dm.used_pct);
        font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        render_target_->DrawText(sbuf, static_cast<UINT32>(wcslen(sbuf)), font_small_, str, brush_text_);
        font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

        // バー（LINE_H の下から BAR_H 分）
        uint32_t bar_col = (dm.used_pct > 90.f) ? 0xEF5350 : cfg.col_graph_fill;
        D2D1_RECT_F br = D2D1::RectF(sx, y + LINE_H + 2.f, sx + sw, y + LINE_H + 2.f + BAR_H);
        draw_hbar(dm.used_pct, 100.f, br, bar_col);

        // 容量テキスト（バーの下、右寄せ）
        wchar_t gbuf[32];
        if (dm.total_gb > 0.f) {
            swprintf_s(gbuf, L"%.0f/%.0fGB", dm.used_gb, dm.total_gb);
        }
        else {
            wcscpy_s(gbuf, L"-");
        }
        set_brush_color(brush_text_, cfg.col_text, 0.5f);
        font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        float gt = y + LINE_H + 2.f + BAR_H;
        D2D1_RECT_F gbr = D2D1::RectF(sx, gt, sx + sw, gt + INFO_LINE_H);
        render_target_->DrawText(gbuf, static_cast<UINT32>(wcslen(gbuf)), font_small_, gbr, brush_text_);
        font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

        // GB/h 行（容量テキストの下、同一物理ドライブの後続は省略）
        bool show_smart = dm.smart_avail
                       && (!prev || prev->phys_drive != dm.phys_drive);
        if (show_smart) {
            wchar_t smuf[32];
            swprintf_s(smuf, L"%.1f GB/h", dm.smart_write_gbh);
            set_brush_color(brush_text_, cfg.col_text, 0.45f);
            font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            float st = gt + INFO_LINE_H;
            D2D1_RECT_F smr = D2D1::RectF(sx, st, sx + sw, st + INFO_LINE_H);
            render_target_->DrawText(smuf, static_cast<UINT32>(wcslen(smuf)), font_small_, smr, brush_text_);
            font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
    };

    draw_drive(c, nullptr);
    y += LINE_H + GRAPH_H + GAP;

    draw_drive(d, &c);
    y += LINE_H + GRAPH_H + GAP;

    return y;
}

float Renderer::draw_net(const NetMetrics& m, const AppConfig& cfg, float y) {
    float x  = PAD;
    float ww = static_cast<float>(cfg.win_width) - PAD * 2;

    // "Network" ラベルと グローバル IP を同一行に並べる
    static constexpr float NET_LBL_W = 140.f;  // Consolas 22pt "Network" ≈ 110px + IP との余白
    set_brush_color(brush_text_, cfg.col_text);
    render_target_->DrawText(L"Network", 7, font_normal_,
        D2D1::RectF(x, y, x + NET_LBL_W, y + LINE_H), brush_text_);

    // IP アドレスは行末右寄せで描画する
    font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    const wchar_t* ip_txt = m.ip_avail ? m.global_ip : L"NO INTERNET\U0001F4F5";
    render_target_->DrawText(ip_txt, static_cast<UINT32>(wcslen(ip_txt)), font_small_,
        D2D1::RectF(x + NET_LBL_W, y + 4.f, x + ww, y + LINE_H), brush_text_);
    font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    y += LINE_H;

    // 3 桁左スペースパディングで表示のガタつきを防ぐ
    auto fmt_kbps = [](float kbps, wchar_t* buf, int len) {
        if (kbps >= 1024.f) swprintf_s(buf, len, L"%5.1f MB/s", kbps / 1024.f);
        else                swprintf_s(buf, len, L"%3.0f KB/s", kbps);
    };

    wchar_t buf[32];
    fmt_kbps(m.recv_kbps, buf, 32);
    wchar_t labelbuf[48];
    swprintf_s(labelbuf, L"▼ %s", buf);
    set_brush_color(brush_text_, cfg.col_text);
    D2D1_RECT_F tr = D2D1::RectF(x, y, x + 120.f, y + LINE_H);
    render_target_->DrawText(labelbuf, static_cast<UINT32>(wcslen(labelbuf)), font_small_, tr, brush_text_);

    float max_val = (m.link_kbps > 0.f)
        ? m.link_kbps
        : max(1.f, max(buf_max(m.send_history), buf_max(m.recv_history)));

    D2D1_RECT_F gr = D2D1::RectF(x, y + LINE_H, x + ww, y + LINE_H + GRAPH_H);
    draw_area_graph(m.recv_history, max_val, gr, cfg.col_net_recv);
    draw_area_graph(m.send_history, max_val, gr, cfg.col_net_send);

    fmt_kbps(m.send_kbps, buf, 32);
    swprintf_s(labelbuf, L"▲ %s", buf);
    D2D1_RECT_F sr = D2D1::RectF(x + 130.f, y, x + ww, y + LINE_H);
    render_target_->DrawText(labelbuf, static_cast<UINT32>(wcslen(labelbuf)), font_small_, sr, brush_text_);

    y += LINE_H + GRAPH_H + GAP;
    return y;
}


float Renderer::draw_claude(const ClaudeMetrics& m, const AppConfig& cfg, float y) {
    float x  = PAD;
    float ww = static_cast<float>(cfg.win_width) - PAD * 2;

    // ヘッダ行：「Claude」は font_normal_（RAM と同サイズ）、残りは font_small_（Disk I/O と同サイズ）
    static constexpr float CLAUDE_LBL_W = 90.f;  // "Claude" の描画幅
    set_brush_color(brush_text_, cfg.col_text);
    D2D1_RECT_F hlr = D2D1::RectF(x, y, x + CLAUDE_LBL_W, y + LINE_H);
    render_target_->DrawText(L"Claude", 6, font_normal_, hlr, brush_text_);

    wchar_t hdr[48];
    swprintf_s(hdr, L"%.15hs  Sessions: %d", m.plan_label, m.session_count);
    D2D1_RECT_F hsr = D2D1::RectF(x + CLAUDE_LBL_W, y + 4.f, x + ww, y + LINE_H);
    render_target_->DrawText(hdr, static_cast<UINT32>(wcslen(hdr)), font_small_, hsr, brush_text_);
    y += LINE_H;

    // 5h / 7d バー：ラベル+パーセンテージ（左）、バー（中）、リセット時刻（右）の同一行レイアウト
    // テキストは Disk I/O と同じ font_small_（18pt）
    static constexpr float LBL_W   = 72.f;   // "5h 100%" が収まる幅（font_small_）
    static constexpr float RESET_W = 130.f;  // リセット時刻テキスト幅（"12/31 月 23:59" が収まる幅）
    // 現在時刻からリアルタイムに均等消費ペースを算出
    auto calc_expected_now = [](time_t resets_ts, double window_secs) -> float {
        if (resets_ts <= 0) return 0.f;  // 未取得（-1）または epoch（0）は無効
        double remaining = static_cast<double>(resets_ts) - static_cast<double>(time(nullptr));
        if (remaining < 0.0) remaining = 0.0;
        if (remaining > window_secs) return 0.f;
        return std::clamp(static_cast<float>((window_secs - remaining) / window_secs * 100.0), 0.f, 100.f);
    };

    auto draw_bar = [&](const wchar_t* lbl, float pct, const wchar_t* reset, bool avail,
                         float expected_pct, int tick_count) {
        // ラベル + パーセンテージ（左寄せ、font_small_ = Disk I/O と同サイズ）
        wchar_t buf[64];
        if (avail) swprintf_s(buf, L"%s %3.0f%%", lbl, pct);
        else       swprintf_s(buf, L"%s",         lbl);
        uint32_t text_col = (avail && pct > expected_pct * 1.1f) ? 0xEF5350 : (avail ? cfg.col_text : 0x888888);
        set_brush_color(brush_text_, text_col);
        D2D1_RECT_F lr = D2D1::RectF(x, y, x + LBL_W, y + SECTION_H);
        render_target_->DrawText(buf, static_cast<UINT32>(wcslen(buf)), font_small_, lr, brush_text_);

        // バー（ラベル右端からリセット時刻左端まで）
        float bar_right = avail ? (x + ww - RESET_W) : (x + ww);
        float bar_top   = y + (SECTION_H - BAR_H) / 2.f;  // 行内で縦中央揃え
        D2D1_RECT_F br  = D2D1::RectF(x + LBL_W + 4.f, bar_top, bar_right, bar_top + BAR_H);
        draw_hbar(avail ? pct : 0.f, 100.f, br, cfg.col_claude_bar);

        // 等分グリッド線（消費ペースの目安：5h は 1h 間隔、7d は 1d 間隔）
        float bar_w = br.right - br.left;
        set_brush_color(brush_fill_, 0xFFFFFF, 0.25f);
        for (int i = 1; i < tick_count; ++i) {
            float gx = br.left + bar_w * (static_cast<float>(i) / tick_count);
            render_target_->DrawLine(
                D2D1::Point2F(gx, br.top), D2D1::Point2F(gx, br.bottom),
                brush_fill_, 1.0f);
        }

        // 現在時刻の均等消費ペース線（緑）
        if (avail && expected_pct > 0.f) {
            float ex = br.left + bar_w * (expected_pct / 100.f);
            set_brush_color(brush_fill_, 0x2E7D32);
            render_target_->DrawLine(
                D2D1::Point2F(ex, br.top), D2D1::Point2F(ex, br.bottom),
                brush_fill_, 3.5f);
        }

        // リセット時刻（右端、未取得時は非表示、font_small_）
        if (avail) {
            wchar_t rtbuf[40];
            swprintf_s(rtbuf, L"%.38s", reset);
            set_brush_color(brush_text_, cfg.col_text, 1.0f);
            D2D1_RECT_F rr = D2D1::RectF(x + ww - RESET_W, y, x + ww, y + SECTION_H);
            font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            render_target_->DrawText(rtbuf, static_cast<UINT32>(wcslen(rtbuf)), font_small_, rr, brush_text_);
            font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
        y += SECTION_H + GAP;
    };

    draw_bar(L"5h", m.five_h_pct,  m.five_h_reset,  m.avail,
             calc_expected_now(m.five_h_resets_ts,  5.0 * 3600), 5);
    draw_bar(L"7d", m.seven_d_pct, m.seven_d_reset, m.avail,
             calc_expected_now(m.seven_d_resets_ts, 7.0 * 24 * 3600), 7);

    return y;
}

// ---- メイン描画 ----

void Renderer::paint(const AllMetrics& m, const AppConfig& cfg) {
    if (!render_target_) create_device_resources(cfg);
    if (!render_target_) return;

    render_target_->BeginDraw();
    render_target_->Clear(from_rgb(cfg.col_background));

    float y = PAD;
    y = draw_os(m.os, cfg, y);          y += SECTION_GAP;
    y = draw_cpu(m.cpu, cfg, y);        y += SECTION_GAP;
    y = draw_gpu(m.gpu, cfg, y);        y += SECTION_GAP;
    y = draw_mem(m.mem, cfg, y);        y += SECTION_GAP;
    y = draw_vram(m.vram, cfg, y);      y += SECTION_GAP;
    y = draw_disk(m.disk_c, m.disk_d, cfg, y); y += SECTION_GAP;
    y = draw_net(m.net, cfg, y);        y += SECTION_GAP;
    y = draw_claude(m.claude, cfg, y);

    preferred_h_ = static_cast<int>(y + PAD);

    HRESULT hr = render_target_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        release_device_resources();
    }
}
