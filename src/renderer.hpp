// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#pragma once
#include "metrics.hpp"
#include "config.hpp"
#include "ring_buffer.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

// Direct2D による描画エンジン
//
// WM_PAINT で Paint() を呼び出すと AllMetrics の内容を描画する。
class Renderer {
public:
    // HWND に紐付けた D2D レンダーターゲットを作成する
    bool init(HWND hwnd, const AppConfig& cfg);

    // メトリクスを描画する（WM_PAINT から呼ぶ）
    void paint(const AllMetrics& m, const AppConfig& cfg);

    // デバイスロスト時（WM_SIZE 等）にリソースを再作成する
    void resize(UINT w, UINT h);

    void shutdown();
    ~Renderer() { shutdown(); }

    // ウィンドウ全体の高さ（コンテンツに合わせて計算する）
    int preferred_height() const { return preferred_h_; }

private:
    HWND hwnd_ = nullptr;

    // D2D リソース
    ID2D1Factory*          d2d_factory_    = nullptr;
    ID2D1HwndRenderTarget* render_target_  = nullptr;
    IDWriteFactory*        dwrite_factory_ = nullptr;
    IDWriteTextFormat*     font_normal_    = nullptr;  // 通常テキスト（22pt）
    IDWriteTextFormat*     font_small_     = nullptr;  // 小テキスト（18pt）
    IDWriteTextFormat*     font_large_     = nullptr;  // グラフ内オーバーレイ（22pt bold）
    IDWriteTextFormat*     font_xlarge_    = nullptr;  // CPU/GPU 使用率オーバーレイ（33pt bold）

    // ブラシキャッシュ（色別に使い回す）
    ID2D1SolidColorBrush*  brush_text_  = nullptr;
    ID2D1SolidColorBrush*  brush_fill_  = nullptr;  // 汎用（後で色を変えて使う）

    int preferred_h_ = 750;

    void create_device_resources(const AppConfig& cfg);
    void release_device_resources();

    // 描画プリミティブ

    // グリッド線を描画する（面グラフ領域に重ねる）
    void draw_grid(D2D1_RECT_F rect);

    // 面グラフを描画する（指定 rect に収める、color は塗りつぶし色）
    void draw_area_graph(const RingBuffer<float, 60>& buf,
                         float max_val, D2D1_RECT_F rect, uint32_t color_rgb);

    // 横バー（0-max_val のレンジ）を描画する
    void draw_hbar(float val, float max_val, D2D1_RECT_F rect, uint32_t color_rgb);

    // 縦バー（0-100%）を描画する
    void draw_vbar(float pct, D2D1_RECT_F rect, uint32_t color_rgb);

    // 温度色（3段階）を返す
    static uint32_t temp_color(float celsius);

    // セクション名（"CPU"/"GPU"）+ モデル名の 2 段階ラベル描画
    void draw_section_label_with_model(float x, float y, float ww,
        const wchar_t* prefix, const char* model_name, const AppConfig& cfg);

    // メーター各セクションの描画
    float draw_os(const OsMetrics& m, const AppConfig& cfg, float y);
    float draw_cpu(const CpuMetrics& m, const AppConfig& cfg, float y);
    float draw_gpu(const GpuMetrics& m, const AppConfig& cfg, float y);
    float draw_mem(const MemMetrics& m,  const AppConfig& cfg, float y);
    float draw_vram(const VramMetrics& m, const AppConfig& cfg, float y);
    float draw_disk(const DiskMetrics& c, const DiskMetrics& d,
                    const AppConfig& cfg, float y);
    float draw_net(const NetMetrics& m,  const AppConfig& cfg, float y);
    float draw_claude(const ClaudeMetrics& m, const AppConfig& cfg, float y);
};
