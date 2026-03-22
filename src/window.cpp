// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#include "window.hpp"
#include "logger.hpp"
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
#include "collector_ip.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

// GitHub リポジトリ URL
static constexpr LPCWSTR GITHUB_URL = L"https://github.com/aviscaerulea/sysmeters";

static constexpr int TIMER_CPU        = 1;  // CPU 専用タイマー ID
static constexpr int TIMER_FAST       = 2;  // 高速タイマー ID（GPU/Disk/Net）
static constexpr int TIMER_SLOW       = 3;  // 低速タイマー ID（RAM/VRAM、2 秒）
static constexpr int TIMER_CLAUDE     = 4;  // Claude 専用タイマー ID（5h/7d 更新）
static constexpr int TIMER_DISK_SPACE = 5;  // Disk 空き容量タイマー ID（5 秒更新）
static constexpr int TIMER_SMART      = 6;  // NVMe S.M.A.R.T. タイマー ID（1 時間更新）
static constexpr int TIMER_IP         = 7;  // グローバル IP タイマー ID（5 分更新）
static constexpr int TIMER_CPU_MS         = 1000;      // 1.0 秒
static constexpr int TIMER_FAST_MS        = 1000;      // 1.0 秒
static constexpr int TIMER_SLOW_MS        = 2000;      // 2.0 秒
static constexpr int TIMER_CLAUDE_MS      = 60000;     // 60 秒
static constexpr int TIMER_DISK_SPACE_MS  = 5000;      // 5 秒
static constexpr int TIMER_SMART_MS       = 3600000;   // 1 時間
static constexpr int TIMER_IP_MS          = 300000;    // 5 分
static constexpr int MIN_CLIENT_W = 461;  // 水平リサイズの最低クライアント幅（px）
static constexpr int MIN_CLIENT_H = 430;  // コンテンツ高さの最低値（px）

// ウィンドウスタイル定数（WM_GETMINMAXINFO でも参照するため定数化）
static constexpr DWORD WND_STYLE    = WS_CAPTION | WS_SYSMENU | WS_THICKFRAME;
static constexpr DWORD WND_EX_STYLE = WS_EX_TOOLWINDOW;

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
    col_ip_     = new IpCollector();

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
    if (!RegisterClassExW(&wc)) {
        destroy();
        return false;
    }

    // 初期クライアントサイズ(win_width × 750)からウィンドウ全体サイズを計算
    RECT adj = {0, 0, cfg_->win_width, 750};
    AdjustWindowRectEx(&adj, WND_STYLE, FALSE, WND_EX_STYLE);

    // ウィンドウ作成（標準タイトルバー＋閉じるボタン、常前面、タスクバー非表示）
    hwnd_ = CreateWindowExW(
        WND_EX_STYLE,
        L"SystemMetersWnd", L"sysmeters",
        WND_STYLE,
        cfg_->win_x, cfg_->win_y,
        adj.right - adj.left, adj.bottom - adj.top,
        nullptr, nullptr, hinstance, nullptr);

    if (!hwnd_) {
        destroy();
        return false;
    }

    // タイトルバーをダークモードに設定
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE_, &dark, sizeof(dark));

    // タイトルバー・ウィンドウ枠の色を固定（フォーカス/非フォーカスで色が変わらない）
    COLORREF cr = rgb_to_colorref(cfg_->col_border);
    DwmSetWindowAttribute(hwnd_, DWMWA_CAPTION_COLOR_, &cr, sizeof(cr));
    DwmSetWindowAttribute(hwnd_, DWMWA_BORDER_COLOR_,  &cr, sizeof(cr));

    // 描画エンジン初期化
    if (!renderer_->init(hwnd_, *cfg_)) {
        destroy();
        return false;
    }

    // コレクタ初期化
    col_cpu_->init();
    col_gpu_->init();
    col_disk_->init('C', 'D');
    col_net_->init();
    col_claude_->init(hwnd_);
    col_ip_->init(hwnd_);

    // タスクトレイアイコン追加
    add_tray_icon();

    // タイマー開始
    SetTimer(hwnd_, TIMER_CPU,        TIMER_CPU_MS,        nullptr);
    SetTimer(hwnd_, TIMER_FAST,       TIMER_FAST_MS,       nullptr);
    SetTimer(hwnd_, TIMER_SLOW,       TIMER_SLOW_MS,       nullptr);
    SetTimer(hwnd_, TIMER_CLAUDE,     TIMER_CLAUDE_MS,     nullptr);
    SetTimer(hwnd_, TIMER_DISK_SPACE, TIMER_DISK_SPACE_MS, nullptr);
    SetTimer(hwnd_, TIMER_SMART,      TIMER_SMART_MS,      nullptr);
    SetTimer(hwnd_, TIMER_IP,         TIMER_IP_MS,         nullptr);

    // WM_GETMINMAXINFO 用の最小ウィンドウ幅を事前計算
    {
        RECT adj_min = {0, 0, MIN_CLIENT_W, 100};
        AdjustWindowRectEx(&adj_min, WND_STYLE, FALSE, WND_EX_STYLE);
        min_track_x_ = adj_min.right - adj_min.left;
    }

    // OS 情報初期取得（マシン名は不変、OS ラベルは 1 時間ごとに update_os_label で再取得）
    {
        DWORD sz = MAX_COMPUTERNAME_LENGTH + 1;
        GetComputerNameW(metrics_->os.machine_name, &sz);
        update_os_label();
    }

    // 初回描画（全メトリクスを一括取得）
    metrics_->os.uptime_ms = GetTickCount64();
    col_cpu_->update(metrics_->cpu);
    col_gpu_->update_all(metrics_->gpu, metrics_->vram);
    col_mem_->update(metrics_->mem);
    col_disk_->update(metrics_->disk_c, metrics_->disk_d);
    col_disk_->update_space(metrics_->disk_c, metrics_->disk_d);
    col_disk_->update_smart(metrics_->disk_c, metrics_->disk_d);
    col_net_->update(metrics_->net);
    col_claude_->update(metrics_->claude);
    col_ip_->update();
    update_window_size();
    InvalidateRect(hwnd_, nullptr, FALSE);

    topmost_ = load_topmost();
    apply_topmost();

    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd_);
    return true;
}

// OS バージョンラベルの再取得
// レジストリから ProductName / DisplayVersion / CurrentBuildNumber を読み取って os_label を更新する。
// Windows Update 後のバージョン変更を反映するため、TIMER_SMART（1 時間）で定期実行する。
void AppWindow::update_os_label() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
            0, KEY_READ, &key) == ERROR_SUCCESS) {
        wchar_t prod[64] = {}, disp[16] = {}, build[8] = {};
        DWORD siz;
        siz = sizeof(prod);
        RegQueryValueExW(key, L"ProductName", nullptr, nullptr, reinterpret_cast<BYTE*>(prod), &siz);
        siz = sizeof(disp);
        RegQueryValueExW(key, L"DisplayVersion", nullptr, nullptr, reinterpret_cast<BYTE*>(disp), &siz);
        siz = sizeof(build);
        RegQueryValueExW(key, L"CurrentBuildNumber", nullptr, nullptr, reinterpret_cast<BYTE*>(build), &siz);

        // ProductName が Windows 11 でも "Windows 10" を返す MS 既知仕様の補正
        // ビルド番号 22000 以降は Windows 11
        if (_wtoi(build) >= 22000) {
            wchar_t* p = wcsstr(prod, L"Windows 10");
            if (p) p[9] = L'1';
        }

        swprintf_s(metrics_->os.os_label, L"%s (%s %s)", prod, disp, build);
        RegCloseKey(key);
    }
}

// ウィンドウ高さをコンテンツに合わせて調整する
void AppWindow::update_window_size() {
    int client_h = max(renderer_->preferred_height(), MIN_CLIENT_H);
    if (client_h <= 10 || client_h == last_pref_h_) return;
    last_pref_h_ = client_h;

    RECT adj = {0, 0, cfg_->win_width, client_h};
    AdjustWindowRectEx(&adj, WND_STYLE, FALSE, WND_EX_STYLE);
    int full_h = adj.bottom - adj.top;

    // 作業領域を超えないようクランプ
    RECT work;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0);
    int max_h = work.bottom - work.top;
    if (full_h > max_h) full_h = max_h;

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
    // メニュー最上部に「アプリ名 vX.Y.Z」を表示
    wchar_t label[64];
    swprintf_s(label, L"sysmeters v%hs", APP_VERSION);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_GITHUB, label);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (topmost_ ? MF_CHECKED : MF_UNCHECKED),
                IDM_TOPMOST, L"常に最前面に表示");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_OPEN_CONFIG, L"設定ファイル");
    AppendMenuW(menu, MF_STRING, IDM_OPEN_LOG, L"ログファイル");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"終了");

    POINT pt;
    GetCursorPos(&pt);
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

// 当日のログファイルをエディタで開く
//
// log_get_dir() で解決済みのログディレクトリを取得し、
// 当日の sysmeters_YYYYMMDD.log を ShellExecuteW で開く。
// ファイルが存在しない場合（まだ何も記録されていない等）はディレクトリを開く。
void AppWindow::open_log_file() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\sysmeters_%04d%02d%02d.log",
               log_get_dir(), st.wYear, st.wMonth, st.wDay);

    // ファイルが存在しない場合はディレクトリを開く
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        ShellExecuteW(nullptr, L"open", log_get_dir(), nullptr, nullptr, SW_SHOW);
        return;
    }

    ShellExecuteW(nullptr, L"open", path, nullptr, nullptr, SW_SHOW);
}

// レジストリキー定数
static constexpr LPCWSTR REG_KEY   = L"Software\\sysmeters";  // HKCU 以下のキーパス
static constexpr LPCWSTR REG_TOPMOST = L"Topmost";            // 最前面設定の値名（REG_DWORD、0 or 1）

// レジストリから最前面設定を読む
//
// HKCU\Software\sysmeters\Topmost (REG_DWORD) を読み、値が存在しないか型が不正な場合は false を返す。
bool AppWindow::load_topmost() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;

    DWORD val = 0, size = sizeof(val), type = 0;
    LONG result = RegQueryValueExW(key, REG_TOPMOST, nullptr, &type,
                                   reinterpret_cast<BYTE*>(&val), &size);
    RegCloseKey(key);

    return result == ERROR_SUCCESS && type == REG_DWORD && val != 0;
}

// レジストリに最前面設定を書く
void AppWindow::save_topmost() {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, nullptr,
                        0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;

    DWORD val = topmost_ ? 1 : 0;
    RegSetValueExW(key, REG_TOPMOST, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(val));
    RegCloseKey(key);
}

// SetWindowPos で最前面状態を反映する
void AppWindow::apply_topmost() {
    SetWindowPos(hwnd_, topmost_ ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
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
    KillTimer(hwnd_, TIMER_DISK_SPACE);
    KillTimer(hwnd_, TIMER_SMART);
    KillTimer(hwnd_, TIMER_IP);
    remove_tray_icon();

    if (col_cpu_)    { col_cpu_->shutdown();    delete col_cpu_;    }
    if (col_gpu_)    { col_gpu_->shutdown();    delete col_gpu_;    }
    if (col_disk_)   { col_disk_->shutdown();   delete col_disk_;   }
    if (col_net_)    { col_net_->shutdown();    delete col_net_;    }
    if (col_claude_) { col_claude_->shutdown(); delete col_claude_; }
    if (col_ip_)     { col_ip_->shutdown();     delete col_ip_; }
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
            // 高速更新（1.0 秒）：GPU/Disk/Net
            col_gpu_->update_gpu(metrics_->gpu);
            col_disk_->update(metrics_->disk_c, metrics_->disk_d);
            col_net_->update(metrics_->net);
        }
        else if (wp == TIMER_SLOW) {
            // 低速更新（2.0 秒）：RAM/VRAM
            col_mem_->update(metrics_->mem);
            col_gpu_->update_all(metrics_->gpu, metrics_->vram);
        }
        else if (wp == TIMER_CLAUDE) {
            // Claude + OS 更新（60 秒）：5h/7d レートリミット + アップタイム
            col_claude_->update(metrics_->claude);
            metrics_->os.uptime_ms = GetTickCount64();
        }
        else if (wp == TIMER_DISK_SPACE) {
            // Disk 空き容量更新（5 秒）
            col_disk_->update_space(metrics_->disk_c, metrics_->disk_d);
        }
        else if (wp == TIMER_SMART) {
            // NVMe S.M.A.R.T. + OS バージョン更新（1 時間）
            col_disk_->update_smart(metrics_->disk_c, metrics_->disk_d);
            update_os_label();
        }
        else if (wp == TIMER_IP) {
            // グローバル IP 更新（5 分）
            col_ip_->update();
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
    case WM_TRAY: {
        const UINT notif = LOWORD(lp);
        if (notif == WM_LBUTTONUP || notif == WM_RBUTTONUP) SetForegroundWindow(hwnd_);
        if (notif == WM_RBUTTONUP) show_context_menu();
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_TOPMOST:
            topmost_ = !topmost_;
            apply_topmost();
            save_topmost();
            break;
        case IDM_GITHUB:       ShellExecuteW(nullptr, L"open", GITHUB_URL, nullptr, nullptr, SW_SHOW); break;
        case IDM_OPEN_CONFIG: open_config_file(); break;
        case IDM_OPEN_LOG:    open_log_file(); break;
        case IDM_EXIT:        DestroyWindow(hwnd); break;
        }
        return 0;

    case WM_CLAUDE_DONE:
        on_claude_done();
        return 0;

    case WM_IP_DONE:
        if (col_ip_) {
            col_ip_->apply_result(metrics_->net);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    // WS_POPUP ウィンドウで DefWindowProc が SC_CLOSE を無視する場合の保険
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_CLOSE) { DestroyWindow(hwnd); return 0; }
        break;

    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) return 0;
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
