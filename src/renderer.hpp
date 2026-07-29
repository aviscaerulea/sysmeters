// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#pragma once
#include "metrics.hpp"
#include "config.hpp"
#include "ring_buffer.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <array>
#include <d2d1.h>
#include <dwrite.h>

// セクション表示フラグ（カテゴリ単位の表示/非表示制御）
//
// gpu フラグは GPU + VRAM 両方を、net フラグは NIC グラフ + IP 表示を一括制御する。
// OS 行は常時表示でフラグの対象外。
struct Visibility {
    bool cpu         = true;
    bool gpu         = true;
    bool mem         = true;
    bool disk        = true;
    bool net         = true;
    bool claude_main = true;
    bool claude_sub  = true;
    // ドライブ別表示フラグ（レター添字 'A'〜'Z'、デフォルト表示）
    // disk が false の場合はセクション全体が非表示になり、本フラグより優先される。
    // 検出されなかったレターの要素は参照されないため無害
    std::array<bool, 26> disk_drive;
    Visibility() { disk_drive.fill(true); }
};

// Direct2D による描画エンジン
//
// WM_PAINT で Paint() を呼び出すと AllMetrics の内容を描画する。
class Renderer {
public:
    // コンパクト表示の縮小率
    // paint() の SetTransform と物理サイズ算出（preferred 高さ・クライアント幅）の唯一の係数。
    // 描画コードは全て論理座標のままとし、この係数は transform と外形サイズにのみ現れる。
    // 1/2 では文字が小さすぎたため 3/5 とした。（実画面評価による）
    static constexpr float COMPACT_SCALE = 0.6f;

    // コンパクト表示の ON/OFF（window 側のトレイメニュー・レジストリ設定から反映する）
    void  set_compact(bool on) { compact_ = on; }

    // 現在の描画スケール（通常 1.0、コンパクト時 COMPACT_SCALE）
    float scale() const { return compact_ ? COMPACT_SCALE : 1.f; }

    // HWND に紐付けた D2D レンダーターゲットを作成する
    bool init(HWND hwnd, const AppConfig& cfg);

    // メトリクスを描画する（WM_PAINT から呼ぶ）
    void paint(const AllMetrics& m, const AppConfig& cfg, const Visibility& vis);

    // vis に基づき paint() を実行せずに preferred_height を事前計算する
    //
    // WM_COMMAND の表示トグル・コンパクト切替時、SetWindowPos を paint() より先行させるために使う。
    // paint() のセクション加算式と論理座標系で厳密に一致させ、戻り値のみ scale 適用後の物理 px とする。
    int compute_preferred_height(const AllMetrics& m, const Visibility& vis) const;

    // コアバーの補間アニメーションを 1 ステップ進める
    //
    // core_disp_ を m.core_pct に向けて lerp し、合計変化量が閾値を超えれば true を返す。
    // true の場合は呼び出し元が InvalidateRect を行う。
    bool update_core_animation(const CpuMetrics& m);

    // WM_SIZE 時にレンダーターゲットのサイズを変更する。
    // デバイスロスト時のリソース再作成は paint() 側（EndDraw の D2DERR_RECREATE_TARGET 検知）が担う。
    void resize(UINT w, UINT h);

    void shutdown();
    ~Renderer() { shutdown(); }

    // トッププロセスの表示・残像状態を全消去する
    // データ由来でない消失（トレイメニューのトグル OFF 等）で残像を出さないために
    // window 側から呼ぶ。（データ由来の消失は topproc_display が残像として扱う）
    void reset_topproc() { topproc_cpu_ = {}; topproc_gpu_ = {}; }

    // ウィンドウ全体の高さ（コンテンツに合わせて計算する。scale 適用後の物理 px）
    int preferred_height() const { return preferred_h_; }

private:
    HWND hwnd_ = nullptr;

    // D2D リソース
    ID2D1Factory*          d2d_factory_    = nullptr;
    ID2D1HwndRenderTarget* render_target_  = nullptr;
    IDWriteFactory*        dwrite_factory_ = nullptr;
    IDWriteTextFormat*     font_normal_     = nullptr;  // 通常テキスト（22pt）
    IDWriteTextFormat*     font_small_      = nullptr;  // 小テキスト（18pt）
    IDWriteTextFormat*     font_tiny_       = nullptr;  // 極小テキスト（16pt）
    IDWriteTextFormat*     font_large_      = nullptr;  // グラフ内オーバーレイ（25pt bold）
    IDWriteTextFormat*     font_xlarge_     = nullptr;  // CPU/GPU 使用率オーバーレイ（40pt bold）
    // 7d バーの警告解除までの残り時間表示専用（13.5pt、プロポーショナルフォント）。
    // 他フォントは等幅の Consolas だが、バー塗り部分の狭い横幅に収めるため字幅の詰まる
    // プロポーショナルフォント（Segoe UI）を使う
    IDWriteTextFormat*     font_pace_remain_ = nullptr;

    // ブラシキャッシュ（色別に使い回す）
    ID2D1SolidColorBrush*  brush_text_  = nullptr;
    ID2D1SolidColorBrush*  brush_fill_  = nullptr;  // 汎用（後で色を変えて使う）

    int preferred_h_ = 880;

    // コンパクト表示フラグ（true で全描画を COMPACT_SCALE 倍に縮小）
    bool compact_ = false;

    // コアバーのアニメーション補間用表示値（update_core_animation で更新）
    std::vector<float> core_disp_;

    // トッププロセス表示の状態（CPU/GPU 別）
    // shown は既存ヒステリシス用の前回表示状態。（表示中は閾値を 0.8 倍に緩めて点滅を防ぐ）
    // name / pct は最後に表示した凍結値、seen_ms は最後に表示条件を満たして描画した時刻。
    // （GetTickCount64 の単調時計、0 = 表示実績なし）条件喪失後も seen_ms から
    // linger_sec 秒以内は凍結値を薄く残像表示し、一瞬のスパイクで表示を見逃すのを防ぐ。
    // データ由来でない消失（トグル OFF・セクション非表示・GPU N/A）では全体を
    // 既定値へ戻して残像を出さない
    struct TopProcState {
        bool      shown    = false;
        wchar_t   name[64] = {};
        float     pct      = 0.f;
        ULONGLONG seen_ms  = 0;
    };
    TopProcState topproc_cpu_, topproc_gpu_;

    void create_device_resources(const AppConfig& cfg);
    void release_device_resources();

    // 描画プリミティブ

    // グリッド線を描画する（面グラフ領域に重ねる）
    void draw_grid(D2D1_RECT_F rect);

    // 面グラフを描画する（指定 rect に収める、color は塗りつぶし色）
    // draw_bg が false の場合は背景・グリッド描画をスキップする（重ね描画時の上書き防止）
    void draw_area_graph(const RingBuffer<float, 60>& buf,
                         float max_val, D2D1_RECT_F rect, uint32_t color_rgb, bool draw_bg = true);

    // 横バー（0〜max_val のレンジ）を描画する
    void draw_hbar(float val, float max_val, D2D1_RECT_F rect, uint32_t color_rgb);

    // 縦バー（0〜100%）を描画する
    void draw_vbar(float pct, D2D1_RECT_F rect, uint32_t color_rgb);

    // 温度色（3段階）を返す
    static uint32_t temp_color(float celsius, float caution, float critical);

    // セクション名 + モデル名の 2 段ラベル描画
    // ww はレイアウト幅。model_name が空のときはモデル名行を省略する
    void draw_section_label_with_model(float x, float y, float ww,
        const wchar_t* prefix, const char* model_name, const AppConfig& cfg);

    // 面グラフ内のトッププロセス表示（CPU/GPU 共通）
    // ol は大パーセンテージの描画に使うオーバーレイ矩形。その左端から TOPPROC_X 右に寄せ、
    // 右端の温度・HF 表示に届かないよう TOPPROC_R の余白を残した帯に "名前 NN%" を描く。
    // alpha はテキストの不透明度。（通常表示 0.6、残像表示はさらに落とす）
    // name が空文字のときは呼び出さないこと（収集 OFF・値未確定を表す）
    void draw_top_proc(const wchar_t* name, float pct, D2D1_RECT_F ol, const AppConfig& cfg,
                       float alpha);

    // トッププロセスの表示判定と残像（linger）管理（CPU/GPU 共通）
    // 表示条件を満たす間は実測値を out_* へ返して凍結値を更新し、残像を解除する。
    // 条件を失ったら喪失時刻を記録し、linger_sec 秒以内は凍結値を out_* へ返す。
    // （residual = true。呼び出し側は薄い alpha で描画する）表示実績が無いまま・
    // linger_sec = 0・残像期限切れでは false を返し、凍結値を消去する。
    // 戻り値は描画の要否
    bool topproc_display(const wchar_t* name, float pct, float overall_pct,
                         const AppConfig& cfg, TopProcState& st,
                         const wchar_t*& out_name, float& out_pct, bool& residual);

    // メーター各セクションの描画
    float draw_os(const OsMetrics& m, const AppConfig& cfg, float y);
    float draw_cpu(const CpuMetrics& m, const MemMetrics& mem, const AppConfig& cfg, float y);
    float draw_gpu(const GpuMetrics& m, const AppConfig& cfg, float y);
    float draw_mem(const MemMetrics& m,  const AppConfig& cfg, float y);
    float draw_vram(const VramMetrics& m, const AppConfig& cfg, float y);
    // RAM / VRAM 共通のヘッダ行 + 使用率横バー描画（wsl_gb > 0 で WSL オーバーレイ付き）
    float draw_mem_bar_section(const wchar_t* label, float usage_pct, float used_gb,
                               float total_gb, float wsl_gb, const AppConfig& cfg, float y);

    // 整列指定付きテキスト描画
    // フォントの整列状態を一時変更して text を 1 回描画し、既定（LEADING / NEAR）へ戻す。
    // IDWriteTextFormat の整列は共有可変状態のため、呼び出し側の手動復元（戻し忘れで
    // 以降の描画全体が崩れる事故の温床）を排除する目的で変更と復元をここへ閉じ込める。
    // 既定が NEAR でないフォント（font_xlarge_ は CENTER）には使わないこと
    void draw_text_aligned(const wchar_t* text, IDWriteTextFormat* font,
                           const D2D1_RECT_F& rect, ID2D1SolidColorBrush* brush,
                           DWRITE_TEXT_ALIGNMENT ta,
                           DWRITE_PARAGRAPH_ALIGNMENT pa = DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    float draw_disk(const std::vector<DiskMetrics>& disks, const Visibility& vis,
                    const AppConfig& cfg, float y);
    float draw_net(const NetMetrics& m,  const AppConfig& cfg, float y);
    float draw_claude(const ClaudeMetrics& m, const AppConfig& cfg, float y);
};
