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

// 補助色
static constexpr uint32_t COL_TEMP_NORMAL  = 0x888888;  // 正常範囲温度・GPU/VRAM 利用不可（グレー）
static constexpr uint32_t COL_TEMP_UNAVAIL = 0x555555;  // 温度取得不可（暗グレー）
static constexpr uint32_t COL_SUBDUED      = 0x666666;  // 補助情報テキスト（薄グレー）
static constexpr uint32_t COL_WSL_MEM      = 0xC06040;  // WSL メモリオーバーレイ
static constexpr uint32_t COL_PACE_IDEAL   = 0x2E7D32;  // 均等消費ペースマーカー（緑）
static constexpr uint32_t COL_CLAUDE_GRID  = 0xFFFFFF;  // Claude バー内グリッド線（白・半透明で使用）

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
static constexpr float GRAPH_H    = 54.f;   // 面グラフ高さ（Disk/Net）。60 の 90%
static constexpr float GRAPH_H_LG = 86.f;  // CPU/GPU 面グラフ高さ
static constexpr float BAR_H      = 16.f;   // 横バー高さ（20 * 0.8）
static constexpr float CORE_BAR_H = 40.f;   // コア縦バー高さ
static constexpr float LINE_H     = 30.f;   // 1行テキスト高さ（22pt 対応）
static constexpr float GAP        = 6.f;    // 要素間ギャップ
static constexpr float SECTION_GAP = 2.f;  // セクション間の追加スペース
static constexpr float TOTAL_W    = 50.f;   // RAM/VRAM 総量テキスト幅（"64GB" 相当）
static constexpr float DISK_GAP   = 20.f;  // Disk I/O グラフと Space バーの間ギャップ
static constexpr float INFO_LINE_H = 27.f;  // Space 下テキスト行高さ（容量/GB/h）

// セクションごとの縦幅ヘルパー
//
// compute_preferred_height() が paint() を実行せず縦幅を見積るために使う。
// paint() 側は draw_*() の戻り値で y を累積する設計を維持しており、
// 各式は対応する draw_*() 内の y 累積と数値上一致させる必要がある。
// レイアウト定数（SECTION_H 等）を変更した場合は両側を必ず揃える。
namespace {
inline float section_h_os()             { return SECTION_H; }
inline float section_h_cpu()            { return SECTION_H + GRAPH_H_LG + GAP + CORE_BAR_H + GAP + SECTION_H; }
inline float section_h_gpu(bool avail)  { return avail ? (SECTION_H + GRAPH_H_LG + GAP)
                                                       : (SECTION_H + LINE_H + GAP); }
inline float section_h_mem()            { return (LINE_H - 1.f) + BAR_H + GAP; }
inline float section_h_vram(bool avail) { return avail ? ((LINE_H - 1.f) + BAR_H + GAP)
                                                       : (LINE_H + GAP); }
inline float section_h_disk(int n)      { return (LINE_H + GRAPH_H + GAP) * static_cast<float>(n); }
inline float section_h_net()            { return LINE_H + LINE_H + GRAPH_H + GAP; }
inline float section_h_claude()         { return (LINE_H - 3.f) + SECTION_H * 2.f + GAP; }
}

// 可視ドライブ数を数える（paint() と compute_preferred_height() の高さ契約の単一ソース）
// vis.disk が false の場合はドライブ別フラグに関わらず 0 を返し、両系統のセクション
// スキップ条件（「可視台数 > 0」）を一致させる
static int visible_disk_count(const std::vector<DiskMetrics>& disks, const Visibility& vis) {
    if (!vis.disk) return 0;
    int n = 0;
    for (const auto& dm : disks)
        if (vis.disk_drive[dm.drive - 'A']) ++n;
    return n;
}

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
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        16.f, L"ja-JP", &font_tiny_))) return false;

    if (FAILED(dwrite_factory_->CreateTextFormat(L"Consolas", nullptr,
        DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        25.f, L"ja-JP", &font_large_))) return false;

    // CPU/GPU 使用率オーバーレイ用（40pt bold）
    if (FAILED(dwrite_factory_->CreateTextFormat(L"Consolas", nullptr,
        DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        40.f, L"ja-JP", &font_xlarge_))) return false;
    // グラフ内のテキストを縦中央揃えにしてパーセンテージと温度のベースラインを揃える
    font_xlarge_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    // 7d バーの警告解除までの残り時間表示用（プロポーショナルフォント）
    if (FAILED(dwrite_factory_->CreateTextFormat(L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        13.5f, L"ja-JP", &font_pace_remain_))) return false;

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

    HRESULT hr = d2d_factory_->CreateHwndRenderTarget(props, hwnd_props, &render_target_);
    if (FAILED(hr) || !render_target_) {
        safe_release(&render_target_);
        return;
    }

    // ブラシ生成失敗時は次回 paint() でやり直せるようリソースを巻き戻す
    hr = render_target_->CreateSolidColorBrush(from_rgb(cfg.col_text), &brush_text_);
    if (FAILED(hr) || !brush_text_) { release_device_resources(); return; }
    hr = render_target_->CreateSolidColorBrush(from_rgb(cfg.col_graph_fill), &brush_fill_);
    if (FAILED(hr) || !brush_fill_) { release_device_resources(); return; }
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
    safe_release(&font_tiny_);
    safe_release(&font_large_);
    safe_release(&font_xlarge_);
    safe_release(&font_pace_remain_);
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
    // "CPU"/"GPU" の見た目の微調整として、他セクションのタイトルに比べ詰まって見える
    // タイトル〜グラフ間の余白を広げるため、プレフィックス文字だけ 2px 上にずらす
    // （モデル名テキストの位置は変えない）
    render_target_->DrawText(prefix, static_cast<UINT32>(wcslen(prefix)), font_normal_,
        D2D1::RectF(x, y - 2.f, x + PREFIX_W, y + SECTION_H - 2.f), brush_text_);
    if (model_name && model_name[0]) {
        wchar_t lbl[48] = {};
        mbstowcs_s(nullptr, lbl, model_name, _TRUNCATE);
        set_brush_color(brush_text_, cfg.col_text, 0.6f);
        render_target_->DrawText(lbl, static_cast<UINT32>(wcslen(lbl)), font_small_,
            D2D1::RectF(x + PREFIX_W, y, x + ww, y + SECTION_H), brush_text_);
    }
}

// グラフ領域にグリッド線を描画する（10 秒間隔の縦線 5 本 + 25% 間隔の横線 3 本）
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
    float dx = w / 59.f;  // 点数−1 で割り、最後の点（i=59）を右端に到達させる

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
    // "Windows 11 Pro (24H2 26100)" 形式の "(" 以降は font_tiny_ サイズ（16pt）で控えめに描く
    // ベースラインは IDWriteTextLayout が同一行内で自動整合させるため Y オフセット調整は不要
    if (m.os_label[0]) {
        set_brush_color(brush_text_, cfg.col_text, 0.6f);
        UINT32 len = static_cast<UINT32>(wcslen(m.os_label));
        IDWriteTextLayout* layout = nullptr;
        if (SUCCEEDED(dwrite_factory_->CreateTextLayout(
                m.os_label, len, font_small_,
                x + ww - UPTIME_W - x, SECTION_H, &layout))) {
            const wchar_t* paren = wcschr(m.os_label, L'(');
            if (paren) {
                DWRITE_TEXT_RANGE r;
                r.startPosition = static_cast<UINT32>(paren - m.os_label);
                r.length        = len - r.startPosition;
                layout->SetFontSize(16.f, r);
            }
            render_target_->DrawTextLayout(D2D1::Point2F(x, y), layout, brush_text_);
            layout->Release();
        }
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

    // ハードフォールト（背面、第 2 Y 軸：0〜1000 Page Reads/sec）→ CPU 使用率（前面）の順で描画
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
    if (N_CORES > 0) {
        if (static_cast<int>(core_disp_.size()) != N_CORES) {
            core_disp_.assign(m.core_pct.begin(), m.core_pct.end());
        }
        constexpr float GAP_BAR = 2.f;  // バー間ギャップ
        float bar_w = (ww - GAP_BAR * (N_CORES - 1)) / N_CORES;
        bar_w = max(bar_w, 1.f);  // 高コア数×狭幅で負値にならないようクランプ（右端のはみ出しはクリップ許容）
        float core_x = x;
        for (int i = 0; i < N_CORES; ++i) {
            D2D1_RECT_F cr = D2D1::RectF(core_x, y, core_x + bar_w, y + CORE_BAR_H);
            uint32_t core_col = (core_disp_[i] > cfg.warn_cpu_pct) ? COL_WARN_RED : cfg.col_cpu_core;
            draw_vbar(core_disp_[i], cr, core_col);
            core_x += bar_w + GAP_BAR;
        }
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
    // font_normal_（22pt、使用率%）と font_small_（18pt）はフォントサイズ差でベースラインが
    // 揃わないため、Claude ヘッダ（プラン名テキスト）と同じ +4px 補正でベースラインを揃える
    wchar_t gbuf[16];
    swprintf_s(gbuf, L"%5.1fGB", m.used_gb);
    set_brush_color(brush_text_, ram_col, 0.8f);
    D2D1_RECT_F gb_r = D2D1::RectF(x, y + 4.f, x + ww, y + LINE_H + 4.f);
    font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    render_target_->DrawText(gbuf, static_cast<UINT32>(wcslen(gbuf)), font_small_, gb_r, brush_text_);

    // WSL 使用量（RAM GB 表示と重ならないよう右端 80px を除いた矩形で右寄せ）
    if (m.wsl_gb > 0.f) {
        wchar_t wslbuf[24];
        swprintf_s(wslbuf, L"WSL %4.1fGB", m.wsl_gb);
        D2D1_RECT_F wsl_r = D2D1::RectF(x, y + 4.f, x + ww - 80.f, y + LINE_H + 4.f);
        font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        render_target_->DrawText(wslbuf, static_cast<UINT32>(wcslen(wslbuf)), font_small_, wsl_r, brush_text_);
    }

    font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    // ヘッダ行テキストのベースラインから RAM バーまでの余白を詰めるため、
    // 行送りを LINE_H + 2.f より 3px 減らす
    y += LINE_H - 1.f;

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

    // 総量テキストをバー右側に表示。
    // DirectWrite の PARAGRAPH_ALIGNMENT_FAR はラインボックス下端を矩形下端に揃える。
    // Consolas のディセントの分だけ視覚的文字底辺はラインボックス下端より
    // 上にずれるため、矩形下端を BAR_H + 3px（視覚調整値）に拡張してベースラインをバー下端に合わせる。
    wchar_t totbuf[16];
    swprintf_s(totbuf, L"%2.0fGB", m.total_gb);
    set_brush_color(brush_text_, cfg.col_text);
    D2D1_RECT_F tr2 = D2D1::RectF(x + ww - TOTAL_W, y, x + ww, y + BAR_H + 3.f);
    font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    font_small_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_FAR);
    render_target_->DrawText(totbuf, static_cast<UINT32>(wcslen(totbuf)), font_small_, tr2, brush_text_);
    font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    font_small_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

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
    // font_normal_（22pt、使用率%）と font_small_（18pt）はフォントサイズ差でベースラインが
    // 揃わないため、Claude ヘッダ（プラン名テキスト）と同じ +4px 補正でベースラインを揃える
    wchar_t gbuf[16];
    swprintf_s(gbuf, L"%5.1fGB", m.used_gb);
    set_brush_color(brush_text_, vram_col, 0.8f);
    D2D1_RECT_F gb_r = D2D1::RectF(x, y + 4.f, x + ww, y + LINE_H + 4.f);
    font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    render_target_->DrawText(gbuf, static_cast<UINT32>(wcslen(gbuf)), font_small_, gb_r, brush_text_);
    font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

    // ヘッダ行テキストのベースラインから VRAM バーまでの余白を詰めるため、
    // 行送りを LINE_H + 2.f より 3px 減らす
    y += LINE_H - 1.f;

    D2D1_RECT_F br = D2D1::RectF(x, y, x + ww - TOTAL_W - 4.f, y + BAR_H);
    draw_hbar(m.usage_pct, 100.f, br, (m.usage_pct > cfg.warn_mem_pct) ? COL_WARN_RED : cfg.col_graph_fill);

    // 総量テキストをバー右側に表示。
    // DirectWrite の PARAGRAPH_ALIGNMENT_FAR はラインボックス下端を矩形下端に揃える。
    // Consolas のディセントの分だけ視覚的文字底辺はラインボックス下端より
    // 上にずれるため、矩形下端を BAR_H + 3px（視覚調整値）に拡張してベースラインをバー下端に合わせる。
    wchar_t totbuf[16];
    swprintf_s(totbuf, L"%2.0fGB", m.total_gb);
    set_brush_color(brush_text_, cfg.col_text);
    D2D1_RECT_F tr2 = D2D1::RectF(x + ww - TOTAL_W, y, x + ww, y + BAR_H + 3.f);
    font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    font_small_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_FAR);
    render_target_->DrawText(totbuf, static_cast<UINT32>(wcslen(totbuf)), font_small_, tr2, brush_text_);
    font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    font_small_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    y += BAR_H + GAP;

    return y;
}

float Renderer::draw_disk(const std::vector<DiskMetrics>& disks, const Visibility& vis,
                           const AppConfig& cfg, float y) {
    float x  = PAD;
    float ww = static_cast<float>(cfg.win_width) - PAD * 2;
    float sw = (ww - DISK_GAP) / 3.f;  // 右：Space 列幅（1/3）
    float gw = ww - DISK_GAP - sw;     // 左：I/O グラフ幅（2/3）

    // ドライブ 1 行分（I/O 面グラフ＋Space 横バー＋S.M.A.R.T. を横並びで描画）
    // 「Disk」セクション見出し行は廃止し、各ドライブの IO 行先頭に "Disk:X" として埋め込むことで縦スペースを 1 行節約
    // show_smart: このドライブの GB/h 行を描画するか（呼び出し側が同一物理ドライブの重複を判定して渡す）
    auto draw_drive = [&](const DiskMetrics& dm, bool show_smart) {
        // --- 左 2/3：I/O ---
        // プレフィックス "Disk:X" は他セクション見出し（CPU/RAM/GPU 等）と同じ font_normal_（22pt）で描画
        // IO 数値部 "R 0.0  W 0.0 MB/s" は font_tiny_ で描画し、プレフィックス幅 DISK_PREFIX_W 分右にオフセット
        // Network セクションの D の書き出し位置（NET_LBL_W = 105.f）と揃える
        static constexpr float DISK_PREFIX_W = 105.f;  // "Disk:X" 描画幅 + 右余白（IO 数値部の開始位置）
        wchar_t pbuf[16];
        swprintf_s(pbuf, L"Disk:%c", dm.drive);
        set_brush_color(brush_text_, cfg.col_text);
        D2D1_RECT_F pr = D2D1::RectF(x, y, x + DISK_PREFIX_W, y + LINE_H);
        render_target_->DrawText(pbuf, static_cast<UINT32>(wcslen(pbuf)), font_normal_, pr, brush_text_);

        // Used:・パーセンテージ用の Y オフセット（font_normal_ プレフィックスとベースラインを揃える）
        static constexpr float DISK_TEXT_DROP    = 3.f;
        // R/W 数値部用の Y オフセット（視覚調整で Used: 群より 1px 下げる）
        static constexpr float DISK_IO_TEXT_DROP = DISK_TEXT_DROP + 1.f;
        wchar_t buf[64];
        swprintf_s(buf, L"R %.1f  W %.1f MB/s", dm.read_mbps, dm.write_mbps);
        D2D1_RECT_F tr = D2D1::RectF(x + DISK_PREFIX_W, y + DISK_IO_TEXT_DROP, x + gw, y + LINE_H + DISK_IO_TEXT_DROP);
        render_target_->DrawText(buf, static_cast<UINT32>(wcslen(buf)), font_tiny_, tr, brush_text_);

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
        uint32_t sp_col = (dm.used_pct > cfg.warn_disk_space_pct) ? COL_WARN_RED : cfg.col_text;

        // テキスト行（"Used:" は font_tiny_・補助情報トーン・右寄せ、パーセンテージは font_small_・条件付き色・右寄せ）
        // "Used:" は Sessions: 行とフォントサイズ・色を揃え、"100.0%" 分のスペースを空けて右に詰める
        // IO 行と同様に、font_normal_ プレフィックス（Disk[X]）とベースラインを揃えるため DISK_TEXT_DROP 分下げる
        // さらに font_small_ (18pt) と font_tiny_ (16pt) のベースライン差を相殺するためラベル矩形を 2px 追加で下げる
        static constexpr float PCT_RESERVE_W = 62.f;  // "100.0%" が font_small_ で占める想定幅
        D2D1_RECT_F str = D2D1::RectF(sx, y + DISK_TEXT_DROP, sx + sw, y + LINE_H + DISK_TEXT_DROP);
        D2D1_RECT_F lbr = D2D1::RectF(sx, y + DISK_TEXT_DROP + 2.f, sx + sw - PCT_RESERVE_W, y + LINE_H + DISK_TEXT_DROP + 2.f);
        set_brush_color(brush_text_, cfg.col_text, 0.6f);
        font_tiny_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        render_target_->DrawText(L"Used:", 5, font_tiny_, lbr, brush_text_);
        font_tiny_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        wchar_t sbuf[16];
        swprintf_s(sbuf, L"%5.1f%%", dm.used_pct);
        set_brush_color(brush_text_, sp_col);
        font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        render_target_->DrawText(sbuf, static_cast<UINT32>(wcslen(sbuf)), font_small_, str, brush_text_);
        font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

        // バー（LINE_H の下から BAR_H 分）
        uint32_t bar_col = (dm.used_pct > cfg.warn_disk_space_pct) ? COL_WARN_RED : cfg.col_graph_fill;
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
        // 容量テキストと GB/h 行は補助情報のため、font_small_ よりひと回り小さい font_tiny_ で控えめに
        set_brush_color(brush_text_, cfg.col_text, 0.5f);
        font_tiny_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        float gt = y + LINE_H + 2.f + BAR_H;
        // 容量テキストはフォント縮小に合わせてベースラインを 2px 下げる（GB/h 行は gt 基準のまま据え置き）
        D2D1_RECT_F gbr = D2D1::RectF(sx, gt + 2.f, sx + sw, gt + 2.f + INFO_LINE_H);
        render_target_->DrawText(gbuf, static_cast<UINT32>(wcslen(gbuf)), font_tiny_, gbr, brush_text_);
        font_tiny_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

        // GB/h 行（容量テキストの下、同一物理ドライブの後続は省略）
        if (show_smart) {
            wchar_t smuf[32];
            swprintf_s(smuf, L"%.1f GB/h", dm.smart_write_gbh);
            uint32_t gbh_col = (dm.smart_write_gbh > cfg.warn_disk_gbh) ? COL_WARN_RED : cfg.col_text;
            set_brush_color(brush_text_, gbh_col, 0.45f);
            font_tiny_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            float st = gt + INFO_LINE_H - 6.f;
            D2D1_RECT_F smr = D2D1::RectF(sx, st, sx + sw, st + INFO_LINE_H);
            render_target_->DrawText(smuf, static_cast<UINT32>(wcslen(smuf)), font_tiny_, smr, brush_text_);
            font_tiny_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
    };

    // GB/h を描画済みの物理ドライブ番号。同一物理ドライブを共有する後続の可視ドライブは
    // GB/h を省略する（「描画済み基準」。先行ドライブが非表示なら後続の可視ドライブ側に出る）
    std::vector<int> gbh_drawn;
    for (const auto& dm : disks) {
        if (!vis.disk_drive[dm.drive - 'A']) continue;
        const bool show_smart = dm.smart_avail
            && std::find(gbh_drawn.begin(), gbh_drawn.end(), dm.phys_drive) == gbh_drawn.end();
        draw_drive(dm, show_smart);
        if (show_smart) gbh_drawn.push_back(dm.phys_drive);
        y += LINE_H + GRAPH_H + GAP;
    }

    return y;
}

float Renderer::draw_net(const NetMetrics& m, const AppConfig& cfg, float y) {
    float x  = PAD;
    float ww = static_cast<float>(cfg.win_width) - PAD * 2;

    // "Network" タイトル + DL/UL + グローバル IP を同一行に並べる
    // 旧 ▼/▲ 専用行は廃止し、Disk セクションと同じくタイトル行に数値を埋め込む
    static constexpr float NET_LBL_W     = 105.f;  // "Network" 描画幅 + 右余白（D の開始位置）
    static constexpr float NET_TEXT_DROP = 3.f;    // font_normal_ プレフィックスとベースラインを合わせるため小フォントを下げる
    set_brush_color(brush_text_, cfg.col_text);
    render_target_->DrawText(L"Network", 7, font_normal_,
        D2D1::RectF(x, y, x + NET_LBL_W, y + LINE_H), brush_text_);

    // 3 桁左スペースパディングで表示のガタつきを防ぐ
    auto fmt_kbps = [](float kbps, wchar_t* buf, int len) {
        if (kbps >= 1024.f) swprintf_s(buf, len, L"%5.1f MB/s", kbps / 1024.f);
        else                swprintf_s(buf, len, L"%3.0f KB/s", kbps);
    };

    // D / U を Network ラベルの右側に font_tiny_ で並べる（Disk セクションの "R %s W %s" と同じ並べ方）
    // D と U の間は値の右パディング込みで 1 スペースのみ。スペースを増やすと UL 開始位置が右に寄りすぎる
    wchar_t dlbuf[16], ulbuf[16], labelbuf[48];
    fmt_kbps(m.recv_kbps, dlbuf, 16);
    fmt_kbps(m.send_kbps, ulbuf, 16);
    swprintf_s(labelbuf, L"D %s U %s", dlbuf, ulbuf);
    D2D1_RECT_F dur = D2D1::RectF(x + NET_LBL_W, y + NET_TEXT_DROP, x + ww, y + LINE_H + NET_TEXT_DROP);
    render_target_->DrawText(labelbuf, static_cast<UINT32>(wcslen(labelbuf)), font_tiny_, dur, brush_text_);

    // グローバル IP は行末右寄せ、フォントとトーンは Handle: 等の補助情報と同じ font_tiny_ + アルファ 0.6
    set_brush_color(brush_text_, cfg.col_text, 0.6f);
    font_tiny_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    const wchar_t* ip_txt = m.ip_avail ? m.global_ip : L"NO INTERNET\U0001F4F5";
    render_target_->DrawText(ip_txt, static_cast<UINT32>(wcslen(ip_txt)), font_tiny_,
        D2D1::RectF(x + NET_LBL_W, y + NET_TEXT_DROP, x + ww, y + LINE_H + NET_TEXT_DROP), brush_text_);
    font_tiny_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    y += LINE_H;

    // グラフ
    float max_val = max(500.f, max(buf_max(m.send_history), buf_max(m.recv_history)));
    D2D1_RECT_F gr = D2D1::RectF(x, y, x + ww, y + GRAPH_H);
    draw_area_graph(m.recv_history, max_val, gr, cfg.col_net_recv);
    draw_area_graph(m.send_history, max_val, gr, cfg.col_net_send, false);

    y += GRAPH_H + GAP;
    return y;
}


float Renderer::draw_claude(const ClaudeMetrics& m, const AppConfig& cfg, float y) {
    float x  = PAD;
    float ww = static_cast<float>(cfg.win_width) - PAD * 2;

    // ヘッダ行：「Claude」は font_normal_（RAM と同サイズ）、残りは font_small_（Disk I/O と同サイズ）
    static constexpr float CLAUDE_LBL_W = 90.f;  // "Claude" の描画幅
    set_brush_color(brush_text_, cfg.col_text);
    D2D1_RECT_F hlr = D2D1::RectF(x, y, x + CLAUDE_LBL_W, y + LINE_H);
    // ヘッダ表示名は TOML `name` から来る account_label。長すぎる名前で枠を越えないよう 6 wchar で切り詰める
    wchar_t hdr_buf[8] = {};
    _snwprintf_s(hdr_buf, _TRUNCATE, L"%.6s",
                 m.account_label[0] != L'\0' ? m.account_label : L"Claude");
    render_target_->DrawText(hdr_buf, static_cast<UINT32>(wcsnlen_s(hdr_buf, _countof(hdr_buf))),
                             font_normal_, hlr, brush_text_);

    D2D1_RECT_F hsr = D2D1::RectF(x + CLAUDE_LBL_W, y + 4.f, x + ww, y + LINE_H);

    // プラン名（常に通常色）
    wchar_t plan_name[24];
    swprintf_s(plan_name, L"%.15hs", m.plan_label);
    set_brush_color(brush_text_, cfg.col_text);
    render_target_->DrawText(plan_name, static_cast<UINT32>(wcslen(plan_name)), font_small_, hsr, brush_text_);

    // Usage API 取得失敗インジケータ（プラン名の右に赤で "ERR"）
    int text_cursor = static_cast<int>(wcslen(plan_name));
    if (m.fetch_error) {
        wchar_t err_buf[24] = {};
        wmemset(err_buf, L' ', text_cursor);
        swprintf_s(err_buf + text_cursor, static_cast<int>(_countof(err_buf)) - text_cursor, L" ERR");
        set_brush_color(brush_text_, COL_WARN_RED);
        render_target_->DrawText(err_buf, static_cast<UINT32>(wcslen(err_buf)), font_small_, hsr, brush_text_);
        text_cursor = static_cast<int>(wcslen(err_buf));
    }

    // 超過料金テキスト（閾値超で赤）
    // Consolas はモノスペースのためプラン名文字数分スペースを先頭に積むことで横位置を合わせる
    if (m.extra_enabled) {
        wchar_t over_buf[80];
        int pad = text_cursor;
        wmemset(over_buf, L' ', pad);
        // _countof は size_t なので int 演算のため明示キャスト。pad は wcslen 起源で常に _countof 以下
        swprintf_s(over_buf + pad, static_cast<int>(_countof(over_buf)) - pad, L"  over $%.1f", m.extra_used_dollars);
        uint32_t over_col = (m.extra_used_dollars > cfg.warn_claude_over) ? COL_WARN_RED : cfg.col_text;
        set_brush_color(brush_text_, over_col);
        render_target_->DrawText(over_buf, static_cast<UINT32>(wcslen(over_buf)), font_small_, hsr, brush_text_);
    }
    wchar_t sess_buf[32];
    if (m.fetched_at[0] != L'\0')
        swprintf_s(sess_buf, L"%s  Sessions:%3d", m.fetched_at, m.session_count);
    else
        swprintf_s(sess_buf, L"Sessions:%3d", m.session_count);
    // CPU セクションの Proc/Thread/Handle 行と同じ補助情報トーン（16pt、アルファ 0.6）に揃え、視覚的な主張を抑える
    // 先頭の HH:MM は Usage API の直近取得時刻。（鮮度インジケータ）未取得時は空文字で Sessions のみ表示
    // プラン名 etc とフォントサイズが違うため、ベースラインを揃えるために専用矩形を 4px 下げる
    D2D1_RECT_F ssr = D2D1::RectF(hsr.left, hsr.top + 4.f, hsr.right, hsr.bottom + 4.f);
    set_brush_color(brush_text_, cfg.col_text, 0.6f);
    font_tiny_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    render_target_->DrawText(sess_buf, static_cast<UINT32>(wcslen(sess_buf)), font_tiny_, ssr, brush_text_);
    font_tiny_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    // ヘッダ行テキスト（"Claude"/プラン名/取得時刻・Sessions）のベースラインから
    // 5h バーまでの余白を詰めるため、行送りを LINE_H より 3px 減らす
    y += LINE_H - 3.f;

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

    // 追い上げ可能な最大到達率（%）を実測ペースから外挿する
    // rate は 7d 履歴の端点差分から算出した平均消費レート（%/秒、calc_hist_rate が算出）。
    // rate 0（推定不可：起動直後・ウィンドウ切替直後・増加実績なし）や resets_ts 無効時は
    // -1 を返し、呼び出し側は使い切り不能の判定をしない（警告なしの安全側）
    auto calc_reach_pct = [](time_t resets_ts, float pct, float rate) -> float {
        if (rate <= 0.f || resets_ts <= 0) return -1.f;
        double remaining = static_cast<double>(resets_ts) - static_cast<double>(time(nullptr));
        if (remaining < 0.0) remaining = 0.0;
        return pct + rate * static_cast<float>(remaining);
    };

    // 履歴から「N 分前の使用率」を求める（5h/7d オーバーレイ共用）
    // 新しい順に走査し、ts <= now - N 分 の最初のサンプルを返す。
    // N 分前のサンプルが無い場合、最古サンプルの経過時間が 60 秒以上なら最古サンプルを返す
    // （起動直後でもおおむねペースが見える効果。1 分未満は誤差が大きいため抑制）
    // 長時間停止明けは collector が残したアンカー（停止前最後のサンプル）が「N 分以上前の
    // 最新サンプル」に該当するため、起点が N 分前より古くなる。（停止中の増分も濃色に含まれる）
    auto calc_delta_start_pct = [&](const std::vector<ClaudeHistorySample>& hist, int win_min) -> float {
        if (win_min <= 0 || hist.empty()) return 0.f;
        time_t now = time(nullptr);
        time_t target = now - static_cast<time_t>(win_min * 60);
        for (auto it = hist.rbegin(); it != hist.rend(); ++it) {
            if (it->ts <= target) return it->pct;
        }
        if (now - hist.front().ts >= 60) return hist.front().pct;
        return 0.f;
    };

    // 短スパン外挿の抑制下限（秒）
    // コールドスタート直後（win_min 分前の基準サンプルが無い）に最古サンプルで代用する際、
    // これ未満の観測時間では傾きが暴れるため推定不可扱いにする
    constexpr time_t UNDERUSE_MIN_SPAN_SECS = 30 * 60;
    // 7d 履歴から直近の実質平均消費レート（%/秒）を求める
    // 基準点は「win_min 分以上前」の最新サンプル。（アプリ停止明けは collector が残した
    // アンカー＝停止前最後のサンプルが該当し、停止期間も分母に含んだ正味ペースになる）
    // 基準サンプルが無い場合（コールドスタート直後）は最古サンプルで代用するが、
    // 観測時間が UNDERUSE_MIN_SPAN_SECS 未満なら 0（推定不可）を返す。
    // 増加 0 以下（リセット跨ぎで旧ウィンドウの高値が基準になった場合を含む）も 0 を返す
    auto calc_hist_rate = [](const std::vector<ClaudeHistorySample>& hist, int win_min, float pct_now) -> float {
        if (win_min <= 0 || hist.empty()) return 0.f;
        time_t now = time(nullptr);
        time_t target = now - static_cast<time_t>(win_min) * 60;
        const ClaudeHistorySample* base = nullptr;
        for (auto it = hist.rbegin(); it != hist.rend(); ++it) {
            if (it->ts <= target) {
                base = &*it;
                break;
            }
        }
        if (!base && now - hist.front().ts >= UNDERUSE_MIN_SPAN_SECS) base = &hist.front();
        if (!base) return 0.f;
        time_t span = now - base->ts;
        if (span <= 0) return 0.f;
        float rate = (pct_now - base->pct) / static_cast<float>(span);
        return rate > 0.f ? rate : 0.f;
    };

    // Claude レートリミット横バーを 1 本描画する
    //
    // lbl:              ラベル文字列（"5h"/"7d"）
    // pct:              現在使用率（0〜100%）
    // reset:            リセット時刻文字列（avail=false のとき nullptr 可）
    // avail:            データ取得済みなら true（false のときグレー表示）
    // expected_pct:     均等消費ペースの理想位置（%）。0 のとき計算不可
    // tick_count:       ペースマーカーの縦線本数（0 のとき縦線なし）
    // warn_pct:         理想ペースからの超過率の警告閾値（%）
    // underuse:         使い切り不能検知の発火フラグ（判定は呼び出し側で行う。7d バー専用、5h は常に false）。
    //                   true のときバー未使用部分の背景を暗青（col_claude_underuse_bg）で塗る
    // delta_start_pct:  直近ウィンドウ開始時点の使用率（%）。0 のとき増加分オーバーレイ非表示
    // window_secs:      ウィンドウ長（秒）。0 より大きいとき、パーセンテージが警告色（黄・赤）の間
    //                    バー左端に警告解除までの残り時間を黒字で表示する（7d のみで使用、5h は 0 のまま非表示）
    auto draw_bar = [&](const wchar_t* lbl, float pct, const wchar_t* reset, bool avail,
                         float expected_pct, int tick_count, float warn_pct,
                         bool underuse = false,
                         float delta_start_pct = 0.f,
                         double window_secs = 0.0) {
        static constexpr float CLAUDE_BAR_H = BAR_H;  // VRAM 等と同じバー高さに揃える

        // ラベル（"5h"/"7d"）は常に通常色で左寄せ、パーセンテージは条件付き色・フォントで右寄せ
        font_small_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        D2D1_RECT_F lr = D2D1::RectF(x, y, x + LBL_W, y + SECTION_H);
        set_brush_color(brush_text_, avail ? cfg.col_text : COL_TEMP_NORMAL);
        render_target_->DrawText(lbl, static_cast<UINT32>(wcslen(lbl)), font_small_, lr, brush_text_);

        // パーセンテージ：100% 到達、または理想ペース超過が閾値以上→赤、ペースマーカー超→黄、それ以外→通常色
        // 他の警告項目（CPU/GPU/RAM 等）と同じ慣例で太字にせず色のみで警告する。
        // 100% 由来の赤を最優先で判定する（ペース超過由来の黄分岐に埋もれて発火しなくなるのを防ぐ）。
        // ペース超過由来の黄・赤の解除条件は pct <= expected_pct、100% 由来の赤の解除条件は pct < 100
        // if (avail) の外側で宣言し、後段の残り時間表示からも参照できるようにする
        bool pace_warning = avail && expected_pct > 0.f && pct > expected_pct;
        if (avail) {
            // API 異常値対策：pct を [0, 999999] にクランプしてから書式化する。
            // pct は API JSON の utilization をクランプなしで格納した値であり、
            // 巨大値が来ると swprintf_s（_TRUNCATE 無指定）が invalid parameter handler を
            // 起動してプロセス即終了に至る。表示上は 999999% 以上を「999999%」で頭打ちにする
            wchar_t pct_buf[16];
            swprintf_s(pct_buf, L"%3.0f%%", std::clamp(pct, 0.f, 999999.f));
            uint32_t pct_col;
            if (pct >= 100.f || (pace_warning && (pct - expected_pct) >= warn_pct)) {
                pct_col = COL_WARN_RED;
            }
            else if (pace_warning) {
                pct_col = COL_WARN_YELLOW;
            }
            else {
                pct_col = cfg.col_text;
            }
            set_brush_color(brush_text_, pct_col);
            font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            font_small_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            render_target_->DrawText(pct_buf, static_cast<UINT32>(wcslen(pct_buf)), font_small_, lr, brush_text_);
            font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }

        // バー（ラベル右端からリセット時刻左端まで）
        // 未取得時もプレースホルダ "--:--" を右側に描画するため、常に RESET_W 分の余白を確保する
        float bar_right = x + ww - RESET_W;
        float bar_top   = y + (SECTION_H - CLAUDE_BAR_H) / 2.f;  // 行内で縦中央揃え
        D2D1_RECT_F br  = D2D1::RectF(x + LBL_W + 4.f, bar_top, bar_right, bar_top + CLAUDE_BAR_H);
        // 使い切り不能検知（7d 専用、判定は呼び出し側）：未使用部分の背景を暗青にして
        // 容量を余らせる見込みが確定的であることを知らせる
        set_brush_color(brush_fill_, underuse ? cfg.col_claude_underuse_bg : COL_BAR_BG);
        render_target_->FillRectangle(br, brush_fill_);

        float fill_pct = avail ? pct : 0.f;
        float fill_w = (fill_pct / 100.f) * (br.right - br.left);
        fill_w = min(fill_w, br.right - br.left);
        if (fill_w > 0.f) {
            D2D1_RECT_F filled = D2D1::RectF(br.left, br.top, br.left + fill_w, br.bottom);
            set_brush_color(brush_fill_, cfg.col_claude_bar);
            render_target_->FillRectangle(filled, brush_fill_);
        }

        // 直近 N 分間の増加分を濃色（COL_WSL_MEM）で重ね塗りする
        // ペース把握用の補助表示。減少時（リセット直後など）は描画しない
        if (avail && delta_start_pct > 0.f && pct > delta_start_pct) {
            float bw = br.right - br.left;
            float xs = br.left + bw * (delta_start_pct / 100.f);
            float xe = br.left + bw * (pct             / 100.f);
            xe = min(xe, br.right);
            if (xe > xs) {
                D2D1_RECT_F dr = D2D1::RectF(xs, br.top, xe, br.bottom);
                set_brush_color(brush_fill_, COL_WSL_MEM, 0.9f);
                render_target_->FillRectangle(dr, brush_fill_);
            }
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

        // 警告解除までの残り時間（黒字、バー左端＝塗り部分の上に描画する）
        // 理想ペース線は時間経過で pct に追いつくため、pace_warning 中は
        // (pct - expected_pct) / 100 * window_secs 秒後に解除される。window_secs が
        // 0（5h 呼び出しではデフォルトのまま）のときは非表示。
        // 負号を付けて「あと -20m」のようなカウントダウン値であることを明示する。
        // pct は API JSON の utilization をクランプなしで格納した値（renderer.cpp 上部の
        // pct_buf 生成と同じ注意点）なので、巨大な異常値で swprintf_s がバッファをはみ出し
        // invalid parameter handler を起動しないよう pct_buf と同じ上限でクランプする。
        // 黒字は塗り部分（明色）の上でしか読めないため、警告点灯直後など塗り幅が狭い間は
        // 暗い未塗り部分に文字がはみ出し判読不能になる。塗り幅がテキスト全体
        // （左オフセット 3px + 実測幅 + 右余白 2px）を収容できるときのみ描画する
        if (avail && pace_warning && window_secs > 0.0) {
            float pct_clamped = std::clamp(pct, 0.f, 999999.f);
            double remain_secs = (static_cast<double>(pct_clamped) - static_cast<double>(expected_pct))
                                / 100.0 * window_secs;
            wchar_t remain_buf[16];
            if (remain_secs <= 3600.0) {
                int remain_min = static_cast<int>(std::ceil(remain_secs / 60.0));
                swprintf_s(remain_buf, L"-%dm", remain_min);
            }
            else {
                swprintf_s(remain_buf, L"-%.1fh", remain_secs / 3600.0);
            }
            IDWriteTextLayout* remain_layout = nullptr;
            DWRITE_TEXT_METRICS remain_tm{};
            if (SUCCEEDED(dwrite_factory_->CreateTextLayout(
                    remain_buf, static_cast<UINT32>(wcslen(remain_buf)),
                    font_pace_remain_, 10000.f, SECTION_H, &remain_layout))) {
                remain_layout->GetMetrics(&remain_tm);
                remain_layout->Release();
            }
            float remain_text_w = remain_tm.widthIncludingTrailingWhitespace;
            if (fill_w >= 3.f + remain_text_w + 2.f) {
                font_pace_remain_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                set_brush_color(brush_text_, 0x000000);
                // 見た目の微調整として基準位置から左に 1px、上に 1px ずらす
                render_target_->DrawText(remain_buf, static_cast<UINT32>(wcslen(remain_buf)), font_pace_remain_,
                    D2D1::RectF(br.left + 3.f, y - 1.f, br.right, y + SECTION_H - 1.f), brush_text_);
                font_pace_remain_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            }
        }

        // リセット時刻（右端、font_small_）
        // 7d 形式（"M/D 曜 HH:MM"）はスペース 2 つで 3 分割し曜日前後を圧縮描画する
        // 未取得時はグレーのプレースホルダ "--:--" を表示する
        font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        if (avail) {
            static constexpr float TIME_W = 54.f;  // "HH:MM" 描画幅
            static constexpr float DAY_W  = 22.f;  // 曜日文字（全角 1 文字）描画幅
            static constexpr float DAY_GAP = 4.f;  // 曜日前後ギャップ（通常スペースの 70%）
            wchar_t rtbuf[40];
            swprintf_s(rtbuf, L"%.38s", reset);
            set_brush_color(brush_text_, cfg.col_text, 1.0f);
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
        }
        else {
            // 未取得時のプレースホルダ（API 取得完了で本来の時刻に置き換わる）
            set_brush_color(brush_text_, COL_TEMP_NORMAL, 1.0f);
            render_target_->DrawText(L"--:--", 5, font_small_,
                D2D1::RectF(x + ww - RESET_W, y, x + ww, y + SECTION_H), brush_text_);
        }
        font_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        // 段落整列の復元はリセット時刻描画後に行う（途中で戻すと警告状態で縦位置が変動する）
        font_small_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        // 5h と 7d の行送りは SECTION_H のみ。視覚的な隙間を詰めるため GAP を含めない。
        // セクション末尾のギャップは draw_bar 呼び出し後に 1 度だけ加算する。
        y += SECTION_H;
    };

    float five_h_delta_start  = calc_delta_start_pct(m.five_h_history,  cfg.claude_delta_window_min);
    float seven_d_delta_start = calc_delta_start_pct(m.seven_d_history, cfg.claude_delta_window_7d_min);
    // 7d 使い切り不能検知：条件は次の 2 つのみ。（5h には検知を行わない）
    // (1) ウィンドウ開始（リセット）から underuse_grace_hours 時間以上経過している
    // (2) 直近の実質平均消費ペース（calc_hist_rate。停止期間も分母に含む正味レート）で
    //     残り時間を外挿した予測到達率が underuse_warn_pct 未満
    //     （レート推定不可のときは reach_pct が -1 になり判定しない）
    // リセット直後は (1) が 48h（デフォルト）の間判定を止める。旧ウィンドウのサンプルは
    // 保持期間切れの破棄と、アンカーのウィンドウ開始チェック（collector 側）で基準にならない
    bool underuse_7d = false;
    if (m.avail && cfg.claude_underuse_enable && m.seven_d_resets_ts > 0) {
        constexpr double WIN_7D_SECS = 7.0 * 24 * 3600;
        double elapsed = WIN_7D_SECS - (static_cast<double>(m.seven_d_resets_ts)
                                        - static_cast<double>(time(nullptr)));
        float rate  = calc_hist_rate(m.seven_d_history, cfg.claude_delta_window_7d_min, m.seven_d_pct);
        float reach = calc_reach_pct(m.seven_d_resets_ts, m.seven_d_pct, rate);
        underuse_7d = elapsed >= static_cast<double>(cfg.claude_underuse_grace_hours) * 3600.0
                   && reach >= 0.f && reach < cfg.claude_underuse_warn_pct;
    }
    draw_bar(L"5h", m.five_h_pct,  m.five_h_reset,  m.avail,
             calc_expected_now(m.five_h_resets_ts,  5.0 * 3600), 5, cfg.warn_claude_5h_pct,
             false, five_h_delta_start);
    draw_bar(L"7d", m.seven_d_pct, m.seven_d_reset, m.avail,
             calc_expected_now(m.seven_d_resets_ts, 7.0 * 24 * 3600), 7, cfg.warn_claude_7d_pct,
             underuse_7d, seven_d_delta_start, 7.0 * 24 * 3600);
    // モデルスコープ（Fable 等）7d 専用ミニバー
    // 7d バー下端に隙間なく密着する塗り矩形のみ（縦幅は cfg.claude_scoped_bar_px、0 = 非表示）。
    // バー全幅 = スコープ枠の 100%。
    // テキスト・背景トラック・警告色は持たず、行高（section_h_claude）にも影響しない。
    // 縦幅は config 側で 0〜4 にクランプ済みで、行内のバー下余白 (SECTION_H - BAR_H) / 2 = 4px に
    // 必ず収まるためレイアウト調整は不要。
    // weekly_scoped を返さないアカウントでは非表示（seven_d_scoped_pct < 0）
    if (m.avail && m.seven_d_scoped_pct >= 0.f && cfg.claude_scoped_bar_px > 0) {
        float bar_left  = x + LBL_W + 4.f;
        float bar_right = x + ww - RESET_W;
        float fill_w = std::clamp(m.seven_d_scoped_pct, 0.f, 100.f) / 100.f * (bar_right - bar_left);
        if (fill_w > 0.f) {
            float top = y - (SECTION_H - BAR_H) / 2.f;  // draw_bar が y を進めた後なので、これが 7d バー下端
            // 100% 到達時は警告色（col_claude_scoped_bar_warn）で塗り、それ以外は通常色。
            // 判定は AlertManager の CLAUDE_*_SCOPED と同一条件（100% 到達）で同時に発火する
            uint32_t bar_col = (m.seven_d_scoped_pct >= 100.f)
                ? cfg.col_claude_scoped_bar_warn : cfg.col_claude_scoped_bar;
            set_brush_color(brush_fill_, bar_col);
            render_target_->FillRectangle(
                D2D1::RectF(bar_left, top, bar_left + fill_w,
                            top + static_cast<float>(cfg.claude_scoped_bar_px)), brush_fill_);
        }
    }
    y += GAP;  // セクション末尾の通常ギャップ（後続セクションとの間隔を維持）

    return y;
}

// ---- メイン描画 ----

void Renderer::paint(const AllMetrics& m, const AppConfig& cfg, const Visibility& vis) {
    if (!render_target_) create_device_resources(cfg);
    if (!render_target_) return;

    render_target_->BeginDraw();
    render_target_->Clear(from_rgb(cfg.col_background));

    float y = PAD;
    y = draw_os(m.os, cfg, y);                            y += SECTION_GAP;
    if (vis.cpu)    { y = draw_cpu(m.cpu, m.mem, cfg, y); y += SECTION_GAP; }
    if (vis.gpu)    { y = draw_gpu(m.gpu, cfg, y);        y += SECTION_GAP;
                      y = draw_vram(m.vram, cfg, y);      y += SECTION_GAP; }
    if (vis.mem)    { y = draw_mem(m.mem, cfg, y);        y += SECTION_GAP; }
    if (visible_disk_count(m.disks, vis) > 0) { y = draw_disk(m.disks, vis, cfg, y); y += SECTION_GAP; }
    if (vis.net)    { y = draw_net(m.net, cfg, y);        y += SECTION_GAP; }
    // Claude メイン/サブの 2 アカウント分。サブはアカウント有効化時のみ描画する。
    // サブ未構成時は Visibility がオンでも account_enabled で抑止して領域を消費しない
    if (vis.claude_main && m.claude_main.account_enabled) {
        y = draw_claude(m.claude_main, cfg, y);
        if (vis.claude_sub && m.claude_sub.account_enabled) y += SECTION_GAP;
    }
    if (vis.claude_sub && m.claude_sub.account_enabled) {
        y = draw_claude(m.claude_sub, cfg, y);
    }

    preferred_h_ = static_cast<int>(y + PAD);

    HRESULT hr = render_target_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        release_device_resources();
    }
}

int Renderer::compute_preferred_height(const AllMetrics& m, const Visibility& vis) const {
    float y = PAD;
    y += section_h_os();                                       y += SECTION_GAP;
    if (vis.cpu)    { y += section_h_cpu();                    y += SECTION_GAP; }
    if (vis.gpu)    { y += section_h_gpu(m.gpu.avail);         y += SECTION_GAP;
                      y += section_h_vram(m.vram.avail);       y += SECTION_GAP; }
    if (vis.mem)    { y += section_h_mem();                    y += SECTION_GAP; }
    if (int n = visible_disk_count(m.disks, vis); n > 0) { y += section_h_disk(n); y += SECTION_GAP; }
    if (vis.net)    { y += section_h_net();                    y += SECTION_GAP; }
    // Claude メイン/サブ：paint() の加算式に厳密一致させる
    if (vis.claude_main && m.claude_main.account_enabled) {
        y += section_h_claude();
        if (vis.claude_sub && m.claude_sub.account_enabled) y += SECTION_GAP;
    }
    if (vis.claude_sub && m.claude_sub.account_enabled) {
        y += section_h_claude();
    }
    return static_cast<int>(y + PAD);
}
