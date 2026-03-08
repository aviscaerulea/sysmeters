// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#include "window.hpp"
#include "resource.h"
#include "config.hpp"
#include "metrics.hpp"
#include "renderer.hpp"
#include "collector_cpu.hpp"
#include "collector_gpu.hpp"
#include "collector_mem.hpp"
#include "collector_disk.hpp"
#include "collector_net.hpp"
#include "collector_claude.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")

static constexpr int TIMER_CPU     = 1;     // CPU 専用タイマー ID
static constexpr int TIMER_FAST    = 2;     // 高速タイマー ID（GPU/Disk/Net）
static constexpr int TIMER_SLOW    = 3;     // 低速タイマー ID（RAM/VRAM）
static constexpr int TIMER_CLAUDE  = 4;     // Claude 専用タイマー ID（5h/7d 更新）
static constexpr int TIMER_CPU_MS    = 1000;   // 1.0 秒
static constexpr int TIMER_FAST_MS   = 1100;   // 1.1 秒
static constexpr int TIMER_SLOW_MS   = 5000;   // 5.0 秒
static constexpr int TIMER_CLAUDE_MS = 60000;  // 60 秒
static constexpr int MIN_CLIENT_W = 450;  // 水平リサイズの最低クライアント幅（px）

// ウィンドウスタイル定数（WM_GETMINMAXINFO でも参照するため定数化）
static constexpr DWORD WND_STYLE    = WS_CAPTION | WS_SYSMENU | WS_THICKFRAME;
static constexpr DWORD WND_EX_STYLE = WS_EX_TOPMOST | WS_EX_TOOLWINDOW;

// DWM 属性 ID（Windows 11 Build 22000 以降。古い SDK でも定義されるようガード）
static constexpr DWORD DWMWA_USE_IMMERSIVE_DARK_MODE_ = 20;
static constexpr DWORD DWMWA_BORDER_COLOR_            = 34;
static constexpr DWORD DWMWA_CAPTION_COLOR_           = 35;

// 0xRRGGBB → COLORREF（0x00BBGGRR）変換
static COLORREF rgb_to_colorref(uint32_t rgb) {
    return ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF);
}

static AppWindow* g_window = nullptr;

bool AppWindow::create(HINSTANCE hinstance, const AppConfig& cfg) {
    hinst_ = hinstance;
    g_window = this;

    // データ構造をヒープに確保
    cfg_      = new AppConfig(cfg);
    metrics_  = new AllMetrics{};
    renderer_ = new Renderer();
    col_cpu_  = new CpuCollector();
    col_gpu_  = new GpuCollector();
    col_mem_  = new MemCollector();
    col_disk_ = new DiskCollector();
    col_net_  = new NetCollector();
    col_claude_ = new ClaudeCollector();

    metrics_->disk_c.drive = 'C';
    metrics_->disk_d.drive = 'D';

    // ウィンドウクラス登録
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = hinstance;
    wc.lpszClassName = L"SystemMetersWnd";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    if (!RegisterClassExW(&wc)) return false;

    // 初期クライアントサイズ(win_width × 600)からウィンドウ全体サイズを計算
    RECT adj = {0, 0, cfg_->win_width, 600};
    AdjustWindowRectEx(&adj, WND_STYLE, FALSE, WND_EX_STYLE);

    // ウィンドウ作成（標準タイトルバー＋閉じるボタン、常前面、タスクバー非表示）
    hwnd_ = CreateWindowExW(
        WND_EX_STYLE,
        L"SystemMetersWnd", L"sysmeters",
        WND_STYLE,
        cfg_->win_x, cfg_->win_y,
        adj.right - adj.left, adj.bottom - adj.top,
        nullptr, nullptr, hinstance, nullptr);

    if (!hwnd_) return false;

    // タイトルバーをダークモードに設定
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE_, &dark, sizeof(dark));

    // タイトルバー・ウィンドウ枠の色を固定（フォーカス/非フォーカスで色が変わらない）
    COLORREF cr = rgb_to_colorref(cfg_->col_border);
    DwmSetWindowAttribute(hwnd_, DWMWA_CAPTION_COLOR_, &cr, sizeof(cr));
    DwmSetWindowAttribute(hwnd_, DWMWA_BORDER_COLOR_,  &cr, sizeof(cr));

    // 描画エンジン初期化
    if (!renderer_->init(hwnd_, *cfg_)) return false;

    // コレクタ初期化
    col_cpu_->init();
    col_gpu_->init();
    col_disk_->init('C', 'D');
    col_net_->init();
    col_claude_->init(hwnd_);

    // タスクトレイアイコン追加
    add_tray_icon();

    // タイマー開始
    SetTimer(hwnd_, TIMER_CPU,    TIMER_CPU_MS,    nullptr);
    SetTimer(hwnd_, TIMER_FAST,   TIMER_FAST_MS,   nullptr);
    SetTimer(hwnd_, TIMER_SLOW,   TIMER_SLOW_MS,   nullptr);
    SetTimer(hwnd_, TIMER_CLAUDE, TIMER_CLAUDE_MS, nullptr);

    // WM_GETMINMAXINFO 用の最小ウィンドウ幅を事前計算
    {
        RECT adj_min = {0, 0, MIN_CLIENT_W, 100};
        AdjustWindowRectEx(&adj_min, WND_STYLE, FALSE, WND_EX_STYLE);
        min_track_x_ = adj_min.right - adj_min.left;
    }

    // 初回描画（全メトリクスを一括取得）
    col_cpu_->update(metrics_->cpu);
    col_gpu_->update_all(metrics_->gpu, metrics_->vram);
    col_mem_->update(metrics_->mem);
    col_disk_->update(metrics_->disk_c, metrics_->disk_d);
    col_net_->update(metrics_->net);
    col_claude_->update(metrics_->claude);
    update_window_size();
    InvalidateRect(hwnd_, nullptr, FALSE);

    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd_);
    return true;
}

// ウィンドウ高さをコンテンツに合わせて調整する
void AppWindow::update_window_size() {
    int client_h = renderer_->preferred_height();
    if (client_h <= 10 || client_h == last_pref_h_) return;
    last_pref_h_ = client_h;

    RECT adj = {0, 0, cfg_->win_width, client_h};
    AdjustWindowRectEx(&adj, WND_STYLE, FALSE, WND_EX_STYLE);
    int full_h = adj.bottom - adj.top;

    RECT rc;
    GetWindowRect(hwnd_, &rc);
    if ((rc.bottom - rc.top) != full_h) {
        SetWindowPos(hwnd_, nullptr, 0, 0, adj.right - adj.left, full_h,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        renderer_->resize(cfg_->win_width, client_h);
    }
}

void AppWindow::on_claude_done() {
    if (!col_claude_) return;  // WM_DESTROY 後に遅延到着した場合の二重防御
    col_claude_->apply_result(metrics_->claude);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void AppWindow::add_tray_icon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = hwnd_;
    nid.uID              = IDI_TRAY_ICON;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon            = LoadIcon(hinst_, MAKEINTRESOURCE(IDI_APP_ICON));
    wcscpy_s(nid.szTip, L"sysmeters");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void AppWindow::remove_tray_icon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = hwnd_;
    nid.uID    = IDI_TRAY_ICON;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void AppWindow::show_context_menu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_OPEN_CONFIG, L"設定ファイルを開く");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"終了");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
}

void AppWindow::open_config_file() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(hinst_, path, MAX_PATH);
    // 実行ファイルのディレクトリに sysmeters.toml がある
    wchar_t* last_sep = wcsrchr(path, L'\\');
    if (last_sep) { *(last_sep + 1) = L'\0'; }
    wcscat_s(path, L"sysmeters.toml");

    ShellExecuteW(nullptr, L"open", path, nullptr, nullptr, SW_SHOW);
}

void AppWindow::run() {
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void AppWindow::destroy() {
    KillTimer(hwnd_, TIMER_CPU);
    KillTimer(hwnd_, TIMER_FAST);
    KillTimer(hwnd_, TIMER_SLOW);
    KillTimer(hwnd_, TIMER_CLAUDE);
    remove_tray_icon();

    if (col_cpu_)    { col_cpu_->shutdown();    delete col_cpu_;    }
    if (col_gpu_)    { col_gpu_->shutdown();    delete col_gpu_;    }
    if (col_disk_)   { col_disk_->shutdown();   delete col_disk_;   }
    if (col_net_)    { col_net_->shutdown();    delete col_net_;    }
    if (col_claude_) { col_claude_->shutdown(); delete col_claude_; col_claude_ = nullptr; }
    if (col_mem_)    delete col_mem_;  // MemCollector は shutdown() 不要
    if (renderer_)   { renderer_->shutdown(); delete renderer_; }
    delete metrics_;
    delete cfg_;
}

LRESULT CALLBACK AppWindow::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_window) return g_window->handle_message(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT AppWindow::handle_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        if (wp == TIMER_CPU) {
            // CPU 専用更新（0.8 秒）
            col_cpu_->update(metrics_->cpu);
        }
        else if (wp == TIMER_FAST) {
            // 高速更新（1.1 秒）：GPU/Disk/Net
            col_gpu_->update_gpu(metrics_->gpu);
            col_disk_->update(metrics_->disk_c, metrics_->disk_d);
            col_net_->update(metrics_->net);
        }
        else if (wp == TIMER_SLOW) {
            // 低速更新（5.0 秒）：RAM/VRAM
            col_mem_->update(metrics_->mem);
            col_gpu_->update_all(metrics_->gpu, metrics_->vram);
        }
        else if (wp == TIMER_CLAUDE) {
            // Claude 更新（60 秒）：5h/7d レートリミット
            col_claude_->update(metrics_->claude);
        }
        update_window_size();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        renderer_->paint(*metrics_, *cfg_);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;  // 背景消去を抑制（ちらつき防止）

    // 上下リサイズを無効化（左右のみ許可）
    case WM_NCHITTEST: {
        LRESULT hit = DefWindowProcW(hwnd, msg, wp, lp);
        switch (hit) {
        case HTBOTTOM:      return HTCLIENT;
        case HTBOTTOMLEFT:  return HTLEFT;
        case HTBOTTOMRIGHT: return HTRIGHT;
        }
        return hit;
    }

    // 最低クライアント幅 MIN_CLIENT_W を強制（値は create で事前計算済み）
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize.x = min_track_x_;
        return 0;

    // タスクトレイイベント
    case WM_TRAY:
        if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) show_context_menu();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_OPEN_CONFIG: open_config_file(); break;
        case IDM_EXIT:        DestroyWindow(hwnd); break;
        }
        return 0;

    case WM_CLAUDE_DONE:
        on_claude_done();
        return 0;

    // WS_POPUP ウィンドウで DefWindowProc が SC_CLOSE を無視する場合の保険
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_CLOSE) { DestroyWindow(hwnd); return 0; }
        break;

    case WM_SIZE:
        // ユーザリサイズ時にクライアント幅を同期（グラフ幅はこれを参照）
        cfg_->win_width = static_cast<int>(LOWORD(lp));
        renderer_->resize(LOWORD(lp), HIWORD(lp));
        return 0;

    case WM_DESTROY:
        destroy();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}
