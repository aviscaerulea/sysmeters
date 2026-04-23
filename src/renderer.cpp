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
#include <string>

// 警告色（各セクション共通）
static constexpr uint32_t COL_WARN_RED    = 0xEF5350;  // 赤（危険・閾値超過）
static constexpr uint32_t COL_WARN_ORANGE = 0xFFA726;  // オレンジ（注意・温度中間）
static constexpr uint32_t COL_WARN_YELLOW = 0xd7b437;  // 黄（ペース超過）
static constexpr uint32_t COL_HARD_FAULT  = 0xB05030;  // アンバー寄りの赤（ハードフォールト）
static constexpr uint32_t COL_BAR_BG      = 0x2A2A2A;  // バー背景（横バー・縦バー共通）
static constexpr uint32_t COL_PEAK_BG     = 0x2B1F1F;  // ピーク時間帯バー背景（暗赤）

// 補助色
static constexpr uint32_t COL_TEMP_NORMAL  = 0x888888;  // 正常範囲温度・GPU/VRAM 利用不可（グレー）
static constexpr uint32_t COL_TEMP_UNAVAIL = 0x555555;  // 温度取得不可（暗グレー）
static constexpr uint32_t COL_SUBDUED      = 0x666666;  // 補助情報テキスト（薄グレー）
static constexpr uint32_t COL_WSL_MEM      = 0xC06040;  // WSL メモリオーバーレイ
static constexpr uint32_t COL_PACE_IDEAL   = 0x2E7D32;  // 均等消費ペースマーカー（緑）
static constexpr uint32_t COL_CLAUDE_GRID  = 0xFFFFFF;  // Claude バー内グリッド線（白・半透明で使用）

// Anthropic ピーク時間帯（PT 平日 5:00-10:59）
// トークン消費が速くなるようレートが制限される時間帯
static constexpr int PEAK_HOUR_START = 5;   // 開始時刻（PT、inclusive）
static constexpr int PEAK_HOUR_END   = 11;  // 終了時刻（PT、exclusive）

// 整数値を3桁区切りカンマ付きワイド文字列に変換する
static std::wstring fmt_comma(int v)
{
    std::wstring s = std::to_wstring(v);
    int pos = static_cast<int>(s.size()) - 3;
    while (pos > 0) {
        s.insert(pos, 1, L',');
        pos -= 3;
    }
    return s;
}

// ウィンドウレイアウト定数（クライアント領域内）
static constexpr float PAD        = 11.f;   // 内側パディング
static constexpr float SECTION_H  = 24.f;   // セクションラベル高さ（18pt 対応）
static constexpr float GRAPH_H    = 60.f;   // 面グラフ高さ（Disk/Net）
static constexpr float GRAPH_H_LG = 86.f;  // CPU/GPU 面グラフ高さ
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

// 温度に応じた警告色の取得
// caution 以上でオレンジ、critical 以上で赤、未満はグレーを返す。
uint32_t Renderer::temp_color(float c, float caution, float critical) {
    if (c >= critical) return COL_WARN_RED;
    if (c >= caution)  return COL_WARN_ORANGE;
    return COL_TEMP_NORMAL;  // グレー（正常範囲）
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
        18.f, L"ja-JP", &font_small_bold_))) return false;

    if (FAILED(dwrite_factory_->CreateTextFormat(L"Consolas", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        16.f, L"ja-JP", &font_tiny_))) return false;

    if (FAILED(dwrite_factory_->CreateTextFormat(L"Consolas", nullptr,
        DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        22.f, L"ja-JP", &font_large_))) return false;

    // CPU/GPU 使用率オーバーレイ用（40pt bold）
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
    safe_release(&font_small_bold_);
    safe_release(&font_tiny_);
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
                               float max_val, D2D1_RECT_F rect, uint32_t color_rgb, bool draw_bg) {
    if (!std::isfinite(max_val) || max_val <= 0.f) return;

    // グラフ領域にクリッピング（ストロークのはみ出し防止）
    render_target_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    if (draw_bg) {
        // グラフ背景（ほぼ黒）
        set_brush_color(brush_fill_, 0x0D0D0D);
        render_target_->FillRectangle(rect, brush_fill_);
        // グリッド線
        draw_grid(rect);
    }

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
    set_brush_color(brush_fill_, COL_BAR_BG);
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

// コアバー補間アニメーション（30fps で呼ばれる）
//
// core_disp_ を m.core_pct に向けて lerp し、合計変化量が閾値を超えれば true を返す。
bool Renderer::update_core_animation(const CpuMetrics& m) {
    constexpr float LERP_K   = 0.33f;  // 補間係数（1 フレームで残差の 33% ずつ近づく）
    constexpr float DONE_THR = 0.5f;   // 全コア合計の変化量がこれ未満なら描画不要
    if (core_disp_.size() != m.core_pct.size()) {
        core_disp_.assign(m.core_pct.begin(), m.core_pct.end());
    }
    float total_delta = 0.f;
    for (size_t i = 0; i < m.core_pct.size(); ++i) {
        float delta = m.core_pct[i] - core_disp_[i];
        core_disp_[i] += delta * LERP_K;
        total_delta += (delta < 0.f ? -delta : delta);
    }
    return total_delta >= DONE_THR;
}

// 縦バー描画（0-100% の高さ）
void Renderer::draw_vbar(float pct, D2D1_RECT_F rect, uint32_t color_rgb) {
    // 背景
    set_brush_color(brush_fill_, COL_BAR_BG);
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

        uint32_t uptime_col = (secs > static_cast<ULONGLONG>(cfg.warn_uptime_days) * 86400) ? COL_WARN_RED : cfg.col_text;
        set_brush_color(brush_text_, uptime_col, 0.6f);
        font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        render_target_->DrawText(ubuf, static_cast<UINT32>(wcslen(ubuf)), font_small_,
            D2D1::RectF(x, y, x + ww, y + SECTION_H), brush_text_);
        font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    return y + SECTION_H;
}

float Renderer::draw_cpu(const CpuMetrics& m, const MemMetrics& mem, const AppConfig& cfg, float y) {
    float x  = PAD;
    float ww = static_cast<float>(cfg.win_width) - PAD * 2;

    draw_section_label_with_model(x, y, ww, L"CPU", m.name, cfg);
    y += SECTION_H;

    // ハードフォールト（背面、第 2 Y 軸：0～1000 Page Reads/sec）→ CPU 使用率（前面）の順で描画
    wchar_t buf[32];
    D2D1_RECT_F gr = D2D1::RectF(x, y, x + ww, y + GRAPH_H_LG);
    draw_area_graph(mem.hard_fault_history, 1000.f, gr, COL_HARD_FAULT);
    draw_area_graph(m.total_history, 100.f, gr, cfg.col_graph_fill, false);

    // パーセンテージ（左寄せ、95% 超で赤、font_xlarge_）
    swprintf_s(buf, L"%4.1f%%", m.total_pct);
    uint32_t cpu_text_col = (m.total_pct > cfg.warn_cpu_pct) ? COL_WARN_RED : cfg.col_text;
    set_brush_color(brush_text_, cpu_text_col, 0.9f);
    D2D1_RECT_F ol = D2D1::RectF(x + 4.f, y + 4.f, x + ww - 4.f, y + GRAPH_H_LG - 4.f);
    render_target_->DrawText(buf, static_cast<UINT32>(wcslen(buf)), font_xlarge_, ol, brush_text_);

    // 温度（右寄せ、3 段階色。取得不可時は "--℃"）
    {
        wchar_t tbuf[16];
        if (m.temp_avail) {
            swprintf_s(tbuf, L"%3.0f\u2103", m.temp_celsius);
            set_brush_color(brush_text_, temp_color(m.temp_celsius, cfg.warn_temp_caution, cfg.warn_temp_critical), 0.9f);
        }
        else {
            swprintf_s(tbuf, L"--\u2103");
            set_brush_color(brush_text_, COL_TEMP_UNAVAIL, 0.9f);
        }
        font_large_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        render_target_->DrawText(tbuf, static_cast<UINT32>(wcslen(tbuf)), font_large_, ol, brush_text_);
        font_large_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    // ハードフォールト値（温度直下、右寄せ、目立たないグレー）
    {
        float latest = mem.hard_fault_history.empty() ? 0.f
            : mem.hard_fault_history.at(mem.hard_fault_history.size() - 1);
        wchar_t hf_buf[16];
        swprintf_s(hf_buf, L"HF:%4d", static_cast<int>(latest));
        set_brush_color(brush_text_, COL_SUBDUED);
        D2D1_RECT_F hf_rect = D2D1::RectF(ol.left, ol.top + 26.f, ol.right, ol.bottom);
        font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        render_target_->DrawText(hf_buf, static_cast<UINT32>(wcslen(hf_buf)), font_small_,
            hf_rect, brush_text_);
        font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    y += GRAPH_H_LG + GAP;

    // コア別縦バー（論理コア数本横並び、画面幅に合わせて動的計算）
    const int   N_CORES = static_cast<int>(m.core_pct.size());
    if (N_CORES == 0) {
        y += CORE_BAR_H + GAP;
        return y;
    }
    if (static_cast<int>(core_disp_.size()) != N_CORES) {
        core_disp_.assign(m.core_pct.begin(), m.core_pct.end());
    }
    constexpr float GAP_BAR = 2.f;  // バー間ギャップ
    float bar_w = (ww - GAP_BAR * (N_CORES - 1)) / N_CORES;
    float core_x = x;
    for (int i = 0; i < N_CORES; ++i) {
        D2D1_RECT_F cr = D2D1::RectF(core_x, y, core_x + bar_w, y + CORE_BAR_H);
        uint32_t core_col = (core_disp_[i] > cfg.warn_cpu_pct) ? COL_WARN_RED : cfg.col_cpu_core;
        draw_vbar(core_disp_[i], cr, core_col);
        core_x += bar_w + GAP_BAR;
    }
    y += CORE_BAR_H + GAP;

    // プロセス数/スレッド数/ハンドル数（1 行テキスト、閾値超過で赤文字）
    {
        wchar_t proc_buf[24], thr_buf[28], hdl_buf[28];
        swprintf_s(proc_buf, L"Proc:%5s",     fmt_comma(m.processes).c_str());
        swprintf_s(thr_buf,  L"  Thread:%6s", fmt_comma(m.threads).c_str());
        swprintf_s(hdl_buf,  L"  Handle:%9s", fmt_comma(m.handles).c_str());

        auto measure = [&](const wchar_t* text) -> float {
            IDWriteTextLayout* layout = nullptr;
            DWRITE_TEXT_METRICS tm{};
            if (SUCCEEDED(dwrite_factory_->CreateTextLayout(
                    text, static_cast<UINT32>(wcslen(text)),
                    font_tiny_, 10000.f, SECTION_H, &layout))) {
                layout->GetMetrics(&tm);
                layout->Release();
            }
            return tm.widthIncludingTrailingWhitespace;
        };

        float hw = measure(hdl_buf);
        float tw = measure(thr_buf);
        float pw = measure(proc_buf);

        float right = x + ww;
        struct { const wchar_t* text; float w; int val; int warn; } parts[] = {
            { hdl_buf, hw, m.handles,   cfg.warn_handles   },
            { thr_buf, tw, m.threads,   cfg.warn_threads   },
            { proc_buf, pw, m.processes, cfg.warn_processes },
        };
        for (auto& p : parts) {
            uint32_t col = (p.val > p.warn) ? COL_WARN_RED : cfg.col_text;
            D2D1_RECT_F r = D2D1::RectF(right - p.w, y, right, y + SECTION_H);
            set_brush_color(brush_text_, col, 0.6f);
            render_target_->DrawText(p.text, static_cast<UINT32>(wcslen(p.text)),
                                     font_tiny_, r, brush_text_);
            right -= p.w;
        }

        set_brush_color(brush_text_, cfg.col_text);
        y += SECTION_H;
    }

    return y;
}

float Renderer::draw_gpu(const GpuMetrics& m, const AppConfig& cfg, float y) {
    float x  = PAD;
    float ww = static_cast<float>(cfg.win_width) - PAD * 2;

    draw_section_label_with_model(x, y, ww, L"GPU", m.name, cfg);
    y += SECTION_H;

    if (!m.avail) {
        set_brush_color(brush_text_, COL_TEMP_NORMAL);
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
    uint32_t gpu_text_col = (m.usage_pct > cfg.warn_gpu_pct) ? COL_WARN_RED : cfg.col_text;
    set_brush_color(brush_text_, gpu_text_col, 0.9f);
    D2D1_RECT_F ol = D2D1::RectF(x + 4.f, y + 4.f, x + ww - 4.f, y + GRAPH_H_LG - 4.f);
    render_target_->DrawText(buf, static_cast<UINT32>(wcslen(buf)), font_xlarge_, ol, brush_text_);

    // 温度（右寄せ、3 段階色）
    wchar_t tbuf[16];
    swprintf_s(tbuf, L"%3.0f\u2103", m.temp_celsius);
    set_brush_color(brush_text_, temp_color(m.temp_celsius, cfg.warn_temp_caution, cfg.warn_temp_critical), 0.9f);
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
    uint32_t ram_col = (m.usage_pct > cfg.warn_mem_pct) ? COL_WARN_RED : cfg.col_text;
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
    uint32_t bar_col = (m.usage_pct > cfg.warn_mem_pct) ? COL_WARN_RED : cfg.col_graph_fill;
    draw_hbar(m.usage_pct, 100.f, br, bar_col);

    if (m.wsl_gb > 0.f && m.total_gb > 0.f) {
        float bar_w    = br.right - br.left;
        float wsl_fill = min(m.wsl_gb / m.total_gb, 1.f) * bar_w;
        if (wsl_fill > 0.f) {
            D2D1_RECT_F wr = D2D1::RectF(br.left, br.top, br.left + wsl_fill, br.bottom);
            set_brush_color(brush_fill_, COL_WSL_MEM, 0.9f);
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
        set_brush_color(brush_text_, COL_TEMP_NORMAL);
        D2D1_RECT_F r = D2D1::RectF(x, y, x + ww, y + LINE_H);
        render_target_->DrawText(L"VRAM  N/A", 9, font_normal_, r, brush_text_);
        return y + LINE_H + GAP;
    }

    // VRAM テキスト（使用率は font_normal_ 左寄せ、GB は font_small_ 右寄せ）
    uint32_t vram_col = (m.usage_pct > cfg.warn_mem_pct) ? COL_WARN_RED : cfg.col_text;
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
    draw_hbar(m.usage_pct, 100.f, br, (m.usage_pct > cfg.warn_mem_pct) ? COL_WARN_RED : cfg.col_graph_fill);

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

        float max_val = max(10.f, max(buf_max(dm.read_history), buf_max(dm.write_history)));
        D2D1_RECT_F gr = D2D1::RectF(x, y + LINE_H, x + gw, y + LINE_H + GRAPH_H);
        draw_area_graph(dm.read_history,  max_val, gr, cfg.col_disk_read);
        draw_area_graph(dm.write_history, max_val, gr, cfg.col_disk_write, false);

        // NVMe 温度（グラフ右上オーバーレイ、SMART 有効かつ温度センサー実装済み時のみ）
        if (dm.smart_avail && dm.smart_temp_avail) {
            wchar_t tbuf[16];
            swprintf_s(tbuf, L"%3.0f\u2103", dm.smart_temp_celsius);
            set_brush_color(brush_text_, temp_color(dm.smart_temp_celsius, cfg.warn_temp_caution, cfg.warn_temp_critical), 0.9f);
            font_large_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            D2D1_RECT_F ol = D2D1::RectF(x + 4.f, y + LINE_H + 4.f, x + gw - 4.f, y + LINE_H + GRAPH_H - 4.f);
            render_target_->DrawText(tbuf, static_cast<UINT32>(wcslen(tbuf)), font_large_, ol, brush_text_);
            font_large_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }

        // --- 右 1/3：Space（テキスト + バー + 容量テキスト縦積み）---
        float sx = x + gw + DISK_GAP;
        uint32_t sp_col = (dm.used_pct > cfg.warn_mem_pct) ? COL_WARN_RED : cfg.col_text;

        // テキスト行（"Used" 左寄せ・通常色、パーセンテージ右寄せ・条件付き色）
        D2D1_RECT_F str = D2D1::RectF(sx, y, sx + sw, y + LINE_H);
        set_brush_color(brush_text_, cfg.col_text);
        render_target_->DrawText(L"Used", 4, font_small_, str, brush_text_);
        wchar_t sbuf[16];
        swprintf_s(sbuf, L"%5.1f%%", dm.used_pct);
        set_brush_color(brush_text_, sp_col);
        font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        render_target_->DrawText(sbuf, static_cast<UINT32>(wcslen(sbuf)), font_small_, str, brush_text_);
        font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

        // バー（LINE_H の下から BAR_H 分）
        uint32_t bar_col = (dm.used_pct > cfg.warn_mem_pct) ? COL_WARN_RED : cfg.col_graph_fill;
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
            uint32_t gbh_col = (dm.smart_write_gbh > cfg.warn_disk_gbh) ? COL_WARN_RED : cfg.col_text;
            set_brush_color(brush_text_, gbh_col, 0.45f);
            font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            float st = gt + INFO_LINE_H - 5.f;
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

    float max_val = max(500.f, max(buf_max(m.send_history), buf_max(m.recv_history)));

    D2D1_RECT_F gr = D2D1::RectF(x, y + LINE_H, x + ww, y + LINE_H + GRAPH_H);
    draw_area_graph(m.recv_history, max_val, gr, cfg.col_net_recv);
    draw_area_graph(m.send_history, max_val, gr, cfg.col_net_send, false);

    fmt_kbps(m.send_kbps, buf, 32);
    swprintf_s(labelbuf, L"▲ %s", buf);
    D2D1_RECT_F sr = D2D1::RectF(x + 130.f, y, x + ww, y + LINE_H);
    render_target_->DrawText(labelbuf, static_cast<UINT32>(wcslen(labelbuf)), font_small_, sr, brush_text_);

    y += LINE_H + GRAPH_H + GAP;
    return y;
}


// 太平洋時間（Pacific Standard Time）の DYNAMIC_TIME_ZONE_INFORMATION を返す
//
// Windows TZ DB から "Pacific Standard Time" を検索してキャッシュする。
// 見つからない場合は Bias=480（UTC-8）のフォールバックを返す。
static const DYNAMIC_TIME_ZONE_INFORMATION& get_pacific_tz()
{
    static DYNAMIC_TIME_ZONE_INFORMATION s_tz = []() {
        DYNAMIC_TIME_ZONE_INFORMATION dtzi = {};
        for (DWORD i = 0; i < 256 && EnumDynamicTimeZoneInformation(i, &dtzi) == ERROR_SUCCESS; ++i) {
            if (wcscmp(dtzi.TimeZoneKeyName, L"Pacific Standard Time") == 0)
                return dtzi;
        }
        // フォールバック：US Pacific（PST/PDT）の固定 DST ルール
        dtzi = {};
        dtzi.Bias = 480;  // UTC-8（標準時）
        dtzi.StandardDate = {0, 11, 0, 1, 2, 0, 0, 0};  // 11月第1日曜 2:00
        dtzi.DaylightDate = {0,  3, 0, 2, 2, 0, 0, 0};  //  3月第2日曜 2:00
        dtzi.DaylightBias = -60;                          // PDT = UTC-7
        return dtzi;
    }();
    return s_tz;
}

// UTC の time_t をバー割合（0.0-1.0）に変換するユーティリティ
//
// 時刻 t（UTC time_t）が 5h ウィンドウ [window_start, window_end] 内で占める割合を返す。
static float utc_to_bar_frac(time_t t, time_t window_start, double window_secs)
{
    return static_cast<float>((static_cast<double>(t) - static_cast<double>(window_start)) / window_secs);
}

// UTC の SYSTEMTIME を time_t に変換するユーティリティ
//
// st は UTC 前提。_mkgmtime は MSVC 固有関数。
static time_t systemtime_to_timet(const SYSTEMTIME& st)
{
    struct tm t = {};
    t.tm_year = st.wYear - 1900;
    t.tm_mon  = st.wMonth - 1;
    t.tm_mday = st.wDay;
    t.tm_hour = st.wHour;
    t.tm_min  = st.wMinute;
    t.tm_sec  = st.wSecond;
    return _mkgmtime(&t);
}

// 5h ウィンドウ内でのピーク時間帯（PT 平日 5:00-10:59）の占有区間を返す
//
// 戻り値は [left_frac, right_frac]（バー上の 0.0-1.0 割合）。
// ピーク区間がない場合は {0, 0} を返す。
// resets_ts は 5h ごとにしか変わらないため結果をキャッシュする。
static std::pair<float, float> calc_peak_overlap(time_t resets_ts)
{
    static time_t s_cached_ts = -1;
    static std::pair<float, float> s_cached_result = {0.f, 0.f};
    if (resets_ts <= 0)
        return {0.f, 0.f};
    if (resets_ts == s_cached_ts)
        return s_cached_result;

    const auto& tz = get_pacific_tz();
    constexpr double WINDOW_SECS = 5.0 * 3600.0;
    const time_t window_start = resets_ts - static_cast<time_t>(WINDOW_SECS);
    const time_t window_end   = resets_ts;

    // ウィンドウが跨ぐ PT 暦日を最大 2 つチェックする
    // window_start と window_end-1 を代表点として使うことで
    // 5h ウィンドウが日付変更線を跨ぐ場合も両方の暦日を確認できる
    const time_t probes[2] = {window_start, window_end - 1};
    std::pair<float, float> result = {0.f, 0.f};

    for (const time_t probe_t : probes) {
        struct tm probe_tm = {};
        gmtime_s(&probe_tm, &probe_t);
        SYSTEMTIME utc_st = {};
        utc_st.wYear   = static_cast<WORD>(probe_tm.tm_year + 1900);
        utc_st.wMonth  = static_cast<WORD>(probe_tm.tm_mon + 1);
        utc_st.wDay    = static_cast<WORD>(probe_tm.tm_mday);
        utc_st.wHour   = static_cast<WORD>(probe_tm.tm_hour);
        utc_st.wMinute = static_cast<WORD>(probe_tm.tm_min);
        utc_st.wSecond = static_cast<WORD>(probe_tm.tm_sec);

        SYSTEMTIME pt_st = {};
        if (!SystemTimeToTzSpecificLocalTimeEx(&tz, &utc_st, &pt_st))
            continue;

        // 週末（日曜=0, 土曜=6）はスキップ
        if (pt_st.wDayOfWeek == 0 || pt_st.wDayOfWeek == 6)
            continue;

        SYSTEMTIME pt_peak_start = {pt_st.wYear, pt_st.wMonth, 0, pt_st.wDay,
                                    static_cast<WORD>(PEAK_HOUR_START), 0, 0, 0};
        SYSTEMTIME pt_peak_end   = {pt_st.wYear, pt_st.wMonth, 0, pt_st.wDay,
                                    static_cast<WORD>(PEAK_HOUR_END),   0, 0, 0};
        SYSTEMTIME utc_peak_start = {}, utc_peak_end = {};
        if (!TzSpecificLocalTimeToSystemTimeEx(&tz, &pt_peak_start, &utc_peak_start) ||
            !TzSpecificLocalTimeToSystemTimeEx(&tz, &pt_peak_end,   &utc_peak_end))
            continue;

        const time_t peak_start_t = systemtime_to_timet(utc_peak_start);
        const time_t peak_end_t   = systemtime_to_timet(utc_peak_end);

        const time_t overlap_start = max(peak_start_t, window_start);
        const time_t overlap_end   = min(peak_end_t,   window_end);
        if (overlap_start >= overlap_end)
            continue;

        float lf = max(0.f, min(utc_to_bar_frac(overlap_start, window_start, WINDOW_SECS), 1.f));
        float rf = max(0.f, min(utc_to_bar_frac(overlap_end,   window_start, WINDOW_SECS), 1.f));
        if (lf < rf) {
            result = {lf, rf};
            break;
        }
    }

    s_cached_ts     = resets_ts;
    s_cached_result = result;
    return result;
}

float Renderer::draw_claude(const ClaudeMetrics& m, const AppConfig& cfg, float y) {
    float x  = PAD;
    float ww = static_cast<float>(cfg.win_width) - PAD * 2;

    // ヘッダ行：「Claude」は font_normal_（RAM と同サイズ）、残りは font_small_（Disk I/O と同サイズ）
    static constexpr float CLAUDE_LBL_W = 90.f;  // "Claude" の描画幅
    set_brush_color(brush_text_, cfg.col_text);
    D2D1_RECT_F hlr = D2D1::RectF(x, y, x + CLAUDE_LBL_W, y + LINE_H);
    render_target_->DrawText(L"Claude", 6, font_normal_, hlr, brush_text_);

    D2D1_RECT_F hsr = D2D1::RectF(x + CLAUDE_LBL_W, y + 4.f, x + ww, y + LINE_H);

    // プラン名（常に通常色）
    wchar_t plan_name[24];
    swprintf_s(plan_name, L"%.15hs", m.plan_label);
    set_brush_color(brush_text_, cfg.col_text);
    render_target_->DrawText(plan_name, static_cast<UINT32>(wcslen(plan_name)), font_small_, hsr, brush_text_);

    // 超過料金テキスト（閾値超で赤）
    // Consolas はモノスペースのためプラン名文字数分スペースを先頭に積むことで横位置を合わせる
    if (m.extra_enabled) {
        wchar_t over_buf[80];
        int pad = static_cast<int>(wcslen(plan_name));
        wmemset(over_buf, L' ', pad);
        // _countof は size_t なので int 演算のため明示キャスト。pad は wcslen 起源で常に _countof 以下
        swprintf_s(over_buf + pad, static_cast<int>(_countof(over_buf)) - pad, L"  over $%.1f", m.extra_used_dollars);
        uint32_t over_col = (m.extra_used_dollars > cfg.warn_claude_over) ? COL_WARN_RED : cfg.col_text;
        set_brush_color(brush_text_, over_col);
        render_target_->DrawText(over_buf, static_cast<UINT32>(wcslen(over_buf)), font_small_, hsr, brush_text_);
    }
    wchar_t sess_buf[24];
    swprintf_s(sess_buf, L"Sessions:%3d", m.session_count);
    set_brush_color(brush_text_, cfg.col_text);
    font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    render_target_->DrawText(sess_buf, static_cast<UINT32>(wcslen(sess_buf)), font_small_, hsr, brush_text_);
    font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    y += LINE_H;

    // 5h / 7d バー：ラベル+パーセンテージ（左）、バー（中）、リセット時刻（右）の同一行レイアウト
    // テキストは Disk I/O と同じ font_small_（18pt）
    static constexpr float LBL_W   = 72.f;   // "5h 100%" が収まる幅（font_small_）
    static constexpr float RESET_W = 138.f;  // リセット時刻テキスト幅（"12/31 月 23:59" が収まる幅）
    // 現在時刻からリアルタイムに均等消費ペースを算出
    auto calc_expected_now = [](time_t resets_ts, double window_secs) -> float {
        if (resets_ts <= 0) return 0.f;  // 未取得（-1）または epoch（0）は無効
        double remaining = static_cast<double>(resets_ts) - static_cast<double>(time(nullptr));
        if (remaining < 0.0) remaining = 0.0;
        if (remaining > window_secs) return 0.f;
        return std::clamp(static_cast<float>((window_secs - remaining) / window_secs * 100.0), 0.f, 100.f);
    };

    // Claude レートリミット横バーを 1 本描画する
    //
    // lbl:          ラベル文字列（"5h"/"7d"）
    // pct:          現在使用率（0〜100%）
    // reset:        リセット時刻文字列（avail=false のとき nullptr 可）
    // avail:        データ取得済みなら true（false のときグレー表示）
    // expected_pct: 均等消費ペースの理想位置（%）。0 のとき計算不可
    // tick_count:   ペースマーカーの縦線本数（0 のとき縦線なし）
    // warn_pct:     理想ペースからの超過率の警告閾値（%）
    // peak_frac:    ピーク時間帯の開始・終了位置（バー幅に対する比率）。{0,0} のとき非表示
    auto draw_bar = [&](const wchar_t* lbl, float pct, const wchar_t* reset, bool avail,
                         float expected_pct, int tick_count, float warn_pct,
                         std::pair<float, float> peak_frac = {0.f, 0.f}) {
        static constexpr float CLAUDE_BAR_H = BAR_H * 1.2f;  // Claude 専用バー高さ（1.2 倍）

        // ラベル（"5h"/"7d"）は常に通常色で左寄せ、パーセンテージは条件付き色・フォントで右寄せ
        font_small_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        D2D1_RECT_F lr = D2D1::RectF(x, y, x + LBL_W, y + SECTION_H);
        set_brush_color(brush_text_, avail ? cfg.col_text : COL_TEMP_NORMAL);
        render_target_->DrawText(lbl, static_cast<UINT32>(wcslen(lbl)), font_small_, lr, brush_text_);

        // パーセンテージ：理想ペース超過が閾値以上→太字赤、ペースマーカー超→黄、それ以外→通常色
        if (avail) {
            wchar_t pct_buf[16];
            swprintf_s(pct_buf, L"%3.0f%%", pct);
            uint32_t pct_col;
            IDWriteTextFormat* pct_font;
            if (expected_pct > 0.f && (pct - expected_pct) >= warn_pct) {
                pct_col  = COL_WARN_RED;
                pct_font = font_small_bold_;
            }
            else if (expected_pct > 0.f && pct > expected_pct) {
                pct_col  = COL_WARN_YELLOW;
                pct_font = font_small_;
            }
            else {
                pct_col  = cfg.col_text;
                pct_font = font_small_;
            }
            set_brush_color(brush_text_, pct_col);
            pct_font->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            pct_font->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            render_target_->DrawText(pct_buf, static_cast<UINT32>(wcslen(pct_buf)), pct_font, lr, brush_text_);
            pct_font->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            pct_font->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }

        // バー（ラベル右端からリセット時刻左端まで）
        float bar_right = avail ? (x + ww - RESET_W) : (x + ww);
        float bar_top   = y + (SECTION_H - CLAUDE_BAR_H) / 2.f;  // 行内で縦中央揃え
        D2D1_RECT_F br  = D2D1::RectF(x + LBL_W + 4.f, bar_top, bar_right, bar_top + CLAUDE_BAR_H);
        set_brush_color(brush_fill_, COL_BAR_BG);
        render_target_->FillRectangle(br, brush_fill_);

        // ピーク時間帯背景（塗りバーより先に描画し、塗りバーの下に隠れる）
        if (peak_frac.first < peak_frac.second) {
            float bw = br.right - br.left;
            D2D1_RECT_F pr = D2D1::RectF(
                br.left + bw * peak_frac.first,  br.top,
                br.left + bw * peak_frac.second, br.bottom);
            set_brush_color(brush_fill_, COL_PEAK_BG);
            render_target_->FillRectangle(pr, brush_fill_);
        }

        float fill_pct = avail ? pct : 0.f;
        float fill_w = (fill_pct / 100.f) * (br.right - br.left);
        fill_w = min(fill_w, br.right - br.left);
        if (fill_w > 0.f) {
            D2D1_RECT_F filled = D2D1::RectF(br.left, br.top, br.left + fill_w, br.bottom);
            set_brush_color(brush_fill_, cfg.col_claude_bar);
            render_target_->FillRectangle(filled, brush_fill_);
        }

        // 等分グリッド線（消費ペースの目安：5h は 1h 間隔、7d は 1d 間隔）
        float bar_w = br.right - br.left;
        set_brush_color(brush_fill_, COL_CLAUDE_GRID, 0.25f);
        for (int i = 1; i < tick_count; ++i) {
            float gx = br.left + bar_w * (static_cast<float>(i) / tick_count);
            render_target_->DrawLine(
                D2D1::Point2F(gx, br.top), D2D1::Point2F(gx, br.bottom),
                brush_fill_, 1.0f);
        }

        // 現在時刻の均等消費ペース線（緑）
        if (avail && expected_pct > 0.f) {
            float ex = br.left + bar_w * (expected_pct / 100.f);
            set_brush_color(brush_fill_, COL_PACE_IDEAL);
            render_target_->DrawLine(
                D2D1::Point2F(ex, br.top), D2D1::Point2F(ex, br.bottom),
                brush_fill_, 3.5f);
        }

        // リセット時刻（右端、未取得時は非表示、font_small_）
        // 7d 形式（"M/D 曜 HH:MM"）はスペース 2 つで 3 分割し曜日前後を圧縮描画する
        if (avail) {
            static constexpr float TIME_W = 54.f;  // "HH:MM" 描画幅
            static constexpr float DAY_W  = 22.f;  // 曜日文字（全角 1 文字）描画幅
            static constexpr float DAY_GAP = 4.f;  // 曜日前後ギャップ（通常スペースの 70%）
            wchar_t rtbuf[40];
            swprintf_s(rtbuf, L"%.38s", reset);
            set_brush_color(brush_text_, cfg.col_text, 1.0f);
            font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            wchar_t tmp[40];
            wcscpy_s(tmp, rtbuf);
            wchar_t* ctx = nullptr;
            wchar_t* p0 = wcstok_s(tmp,     L" ", &ctx);  // "M/D"
            wchar_t* p1 = wcstok_s(nullptr, L" ", &ctx);  // "曜"
            wchar_t* p2 = wcstok_s(nullptr, L" ", &ctx);  // "HH:MM"
            if (p0 && p1 && p2) {
                // 右から：時刻 → 曜日 → 日付 の順に描画
                float rx = x + ww;
                render_target_->DrawText(p2, static_cast<UINT32>(wcslen(p2)), font_small_,
                    D2D1::RectF(rx - TIME_W, y, rx, y + SECTION_H), brush_text_);
                rx -= TIME_W + DAY_GAP;
                render_target_->DrawText(p1, static_cast<UINT32>(wcslen(p1)), font_small_,
                    D2D1::RectF(rx - DAY_W, y, rx, y + SECTION_H), brush_text_);
                rx -= DAY_W + DAY_GAP;
                render_target_->DrawText(p0, static_cast<UINT32>(wcslen(p0)), font_small_,
                    D2D1::RectF(x + ww - RESET_W, y, rx, y + SECTION_H), brush_text_);
            }
            else {
                // 5h 形式（"HH:MM"）：通常描画
                render_target_->DrawText(rtbuf, static_cast<UINT32>(wcslen(rtbuf)), font_small_,
                    D2D1::RectF(x + ww - RESET_W, y, x + ww, y + SECTION_H), brush_text_);
            }
            font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
        font_small_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        y += SECTION_H + GAP;
    };

    auto peak = calc_peak_overlap(m.five_h_resets_ts);
    draw_bar(L"5h", m.five_h_pct,  m.five_h_reset,  m.avail,
             calc_expected_now(m.five_h_resets_ts,  5.0 * 3600), 5, cfg.warn_claude_5h_pct, peak);
    draw_bar(L"7d", m.seven_d_pct, m.seven_d_reset, m.avail,
             calc_expected_now(m.seven_d_resets_ts, 7.0 * 24 * 3600), 7, cfg.warn_claude_7d_pct);

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
    y = draw_cpu(m.cpu, m.mem, cfg, y); y += SECTION_GAP;
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
