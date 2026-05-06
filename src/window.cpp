// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#include "window.hpp"
#include "logger.hpp"
#include "resource.h"
#include "config.hpp"
#include "metrics.hpp"
#include "renderer.hpp"
#include "alert.hpp"
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
#include <filesystem>
namespace fs = std::filesystem;

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

// GitHub リポジトリ URL
static constexpr LPCWSTR GITHUB_URL = L"https://github.com/aviscaerulea/sysmeters";

static constexpr int TIMER_CPU        = 1;  // CPU/GPU タイマー ID（0.9 秒）
static constexpr int TIMER_FAST       = 2;  // 高速タイマー ID（Disk/Net）
static constexpr int TIMER_SLOW       = 3;  // 低速タイマー ID（RAM/VRAM、2 秒）
static constexpr int TIMER_CLAUDE     = 4;  // Claude 専用タイマー ID（5h/7d 更新）
static constexpr int TIMER_DISK_SPACE = 5;  // Disk 空き容量タイマー ID（5 秒更新）
static constexpr int TIMER_SMART      = 6;  // NVMe S.M.A.R.T. タイマー ID（1 時間更新）
static constexpr int TIMER_IP         = 7;  // グローバル IP タイマー ID（5 分更新）
static constexpr int TIMER_ANIM       = 8;  // アニメーションタイマー ID（コアバー補間）
static constexpr int TIMER_PRIORITY   = 9;  // プロセス優先度制御タイマー ID
static constexpr int TIMER_CPU_MS         = 900;        // 0.9 秒
static constexpr int TIMER_FAST_MS        = 1000;      // 1.0 秒
static constexpr int TIMER_SLOW_MS        = 2000;      // 2.0 秒
static constexpr int TIMER_CLAUDE_MS      = 60000;     // 60 秒
static constexpr int TIMER_DISK_SPACE_MS  = 5000;      // 5 秒
static constexpr int TIMER_SMART_MS       = 3600000;   // 1 時間
static constexpr int TIMER_IP_MS          = 300000;    // 5 分
static constexpr int TIMER_ANIM_MS        = 33;        // ≒ 30fps
static constexpr int MIN_CLIENT_H = 430;  // コンテンツ高さの最低値（px）

// ウィンドウスタイル定数
static constexpr DWORD WND_STYLE    = WS_CAPTION | WS_SYSMENU;
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

    // 初期クライアントサイズ(win_width × 880)からウィンドウ全体サイズを計算
    RECT adj = {0, 0, cfg_->win_width, 880};
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
    col_mem_->init();
    col_disk_->init('C', 'D');
    col_net_->init();
    col_claude_->init(hwnd_);
    col_ip_->init(hwnd_);

    // 警告音マネージャ初期化
    alert_ = new AlertManager();
    alert_->init();

    // Explorer 再起動によるタスクバー再生成通知を登録
    WM_TASKBAR_CREATED_ = RegisterWindowMessageW(L"TaskbarCreated");

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
    SetTimer(hwnd_, TIMER_ANIM,       TIMER_ANIM_MS,       nullptr);

    // プロセス優先度制御（設定が有効な場合のみタイマーを立てる）
    if (cfg_->priority_control_enable) {
        SetTimer(hwnd_, TIMER_PRIORITY,
                 static_cast<UINT>(cfg_->priority_check_interval_sec * 1000), nullptr);
        update_process_priority();
    }

    // 制限強化時間 通知チェックタイマー（60 秒周期）
    SetTimer(hwnd_, TIMER_NOTIFY_SCHED, 60'000, nullptr);

    // OS 情報初期取得（マシン名は不変、OS ラベルは 1 時間ごとに update_os_label で再取得）
    {
        DWORD sz = MAX_COMPUTERNAME_LENGTH + 1;
        GetComputerNameW(metrics_->os.machine_name, &sz);
        update_os_label();
    }

    // 初回描画（全メトリクスを一括取得）
    metrics_->os.uptime_ms = GetTickCount64();
    col_cpu_->update(metrics_->cpu);
    col_gpu_->update_gpu(metrics_->gpu);
    col_gpu_->update_vram(metrics_->vram);
    col_mem_->update(metrics_->mem);
    col_disk_->update(metrics_->disk_c, metrics_->disk_d);
    col_disk_->update_space(metrics_->disk_c, metrics_->disk_d);
    col_disk_->update_smart(metrics_->disk_c, metrics_->disk_d);
    col_net_->update(metrics_->net);
    col_claude_->update(metrics_->claude);
    col_ip_->update();
    update_window_size();
    InvalidateRect(hwnd_, nullptr, FALSE);

    topmost_     = load_topmost();
    apply_topmost();
    toast_alert_ = load_toast_alert();

    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd_);

    // 起動時点でピーク期間内なら即時通知
    check_peak_limit_on_startup();

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
        wchar_t prod[128] = {}, disp[16] = {}, build[8] = {};
        DWORD siz;
        siz = sizeof(prod);
        if (RegQueryValueExW(key, L"ProductName", nullptr, nullptr,
                             reinterpret_cast<BYTE*>(prod), &siz) == ERROR_MORE_DATA)
            prod[_countof(prod) - 1] = L'\0';  // 切り捨て時に NUL 終端を保証
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

    // ウィンドウが属するモニタの作業領域を取得してクランプ（マルチモニタ対応）
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR hmon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(hmon, &mi);
    int max_h = mi.rcWork.bottom - mi.rcWork.top;
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

// 発火した項目のビットマスクからバルーン（Toast）通知を表示する
//
// fired_mask の各ビットが AlertManager::Id に対応する。
// 1 件発火：Toast の 3 行表示領域の真ん中（2 行目）に配置するため前後に改行を挿入する。
// 2〜3 件：上から順に詰めて表示する。
// 4 件以上：上 2 行を項目名、3 行目を「ほか N 件」に集約する。
void AppWindow::show_balloon(uint32_t fired_mask) {
    NOTIFYICONDATAW nid{};
    nid.cbSize      = sizeof(nid);
    nid.hWnd        = hwnd_;
    nid.uID         = IDI_TRAY_ICON;
    nid.uFlags      = NIF_INFO;
    nid.dwInfoFlags = NIIF_WARNING;

    const wchar_t* labels[AlertManager::COUNT_] = {};
    int n = 0;
    for (int i = 0; i < AlertManager::COUNT_; i++) {
        if (!(fired_mask & (1u << i))) continue;
        labels[n++] = AlertManager::label(static_cast<AlertManager::Id>(i));
    }
    if (n == 0) return;

    // ラベル最大長は AlertManager::label の最長文字列で約 17 wchar、
    // 4 行で 70 wchar 程度と szInfo 容量（256 wchar）に十分収まる
    // ※ Win11 新通知 UI（XAML）では szInfo 先頭の改行がトリムされる OS バージョンがある
    const size_t cap  = std::size(nid.szInfo);
    // 表示行数の調整：4 件以上は先頭 2 件 + 「ほか N 件」にまとめる
    const int    show = (n <= 3) ? n : 2;
    size_t       off  = 0;
    // 1 件のみのときは Toast 3 行領域の中央（2 行目）に寄せるため先頭に空行を入れる
    if (n == 1) {
        int r = swprintf_s(nid.szInfo, cap, L"\n");
        if (r > 0) off += static_cast<size_t>(r);
    }
    // 項目テキスト書き込み（need_lf：次の行または「ほか」テキストへの改行が必要なとき true）
    for (int i = 0; i < show; i++) {
        if (off >= cap) break;
        const bool need_lf = (i + 1 < show) || (n > 3) || (n == 1);
        int r = swprintf_s(nid.szInfo + off, cap - off, L"　%s%s", labels[i], need_lf ? L"\n" : L"");
        if (r > 0) off += static_cast<size_t>(r);
    }
    // 件数ヘッダ：4 件以上のとき省略分を追記
    if (n > 3 && off < cap)
        swprintf_s(nid.szInfo + off, cap - off, L"　ほか %d 件", n - 2);

    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// 指定タイトル・本文で情報レベルの Toast 通知を表示する
void AppWindow::show_notify(const wchar_t* title, const wchar_t* body) {
    NOTIFYICONDATAW nid{};
    nid.cbSize      = sizeof(nid);
    nid.hWnd        = hwnd_;
    nid.uID         = IDI_TRAY_ICON;
    nid.uFlags      = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    wcsncpy_s(nid.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(nid.szInfo,      body,  _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// Claude Code 制限強化時間 通知の発火判定（60 秒周期タイマーから呼ばれる）
//
// ローカル平日 21:00 の分境界をまたいだタイミングで 1 回だけ発火する。
// 初回呼び出しは分を記録するのみで発火しない（起動直後の誤発火抑止）。
void AppWindow::check_peak_limit_notify() {
    if (!cfg_->notify_peak_limit_enable) {
        last_check_min_ = -1;
        return;
    }

    SYSTEMTIME lt;
    GetLocalTime(&lt);
    int cur_min = lt.wMinute;

    if (last_check_min_ < 0) {
        last_check_min_ = cur_min;
        return;
    }

    int prev = last_check_min_;
    last_check_min_ = cur_min;

    // 発火条件：平日、21:00、分境界をまたいだ
    if (lt.wDayOfWeek < 1 || lt.wDayOfWeek > 5) return;
    if (lt.wHour != PEAK_NOTIFY_HOUR)             return;
    if (cur_min  != PEAK_NOTIFY_MIN)               return;
    if (prev == cur_min)                           return;

    show_notify(cfg_->notify_peak_limit_title.c_str(),
                cfg_->notify_peak_limit_body.c_str());
    if (cfg_->notify_peak_limit_sound && alert_) alert_->play_external();
    log_info("notify: peak limit toast fired");
}

// 起動時にピーク期間内なら即時通知する（create() から 1 度だけ呼ぶ）
//
// ローカル平日 21:00-翌 03:00 を近似ピーク期間とみなす。
// 夏時間中は PT 5:00-11:00 と 1 時間ずれるが、時刻固定方針に従う。
void AppWindow::check_peak_limit_on_startup() {
    if (!cfg_->notify_peak_limit_enable) return;

    SYSTEMTIME lt;
    GetLocalTime(&lt);

    bool in_peak = false;
    if (lt.wHour >= 21 && lt.wDayOfWeek >= 1 && lt.wDayOfWeek <= 5) {
        in_peak = true;  // 平日夜 21-23 時台
    }
    else if (lt.wHour < 3 && lt.wDayOfWeek >= 2 && lt.wDayOfWeek <= 6) {
        in_peak = true;  // 翌日早朝 00-02 時台（前日が平日 = 今日は火〜土）
    }
    if (!in_peak) return;

    show_notify(cfg_->notify_peak_limit_title.c_str(),
                cfg_->notify_peak_limit_body.c_str());
    if (cfg_->notify_peak_limit_sound && alert_) alert_->play_external();
    log_info("notify: startup peak limit toast fired");
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
    AppendMenuW(menu, MF_STRING | (toast_alert_ ? MF_CHECKED : MF_UNCHECKED),
                IDM_ALERT_TOAST, L"Toast 通知");
    AppendMenuW(menu, MF_STRING | (is_startup_registered() ? MF_CHECKED : MF_UNCHECKED),
                IDM_STARTUP, L"スタートアップ登録");
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
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(hinst_, exe, MAX_PATH);
    auto path = fs::path(exe).parent_path() / L"sysmeters.toml";
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOW);
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
static constexpr LPCWSTR REG_KEY         = L"Software\\sysmeters";  // HKCU 以下のキーパス
static constexpr LPCWSTR REG_TOPMOST     = L"Topmost";              // 最前面設定の値名（REG_DWORD、0 or 1）
static constexpr LPCWSTR REG_ALERT_TOAST = L"AlertToast";           // Toast 通知設定の値名（REG_DWORD、0 or 1）

// HKCU\Software\sysmeters の DWORD 値を bool として読む
//
// キーや値が存在しないか型が不正な場合は default_val を返す。
static bool load_reg_bool(LPCWSTR name, bool default_val) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return default_val;

    DWORD val = default_val ? 1 : 0, size = sizeof(val), type = 0;
    LONG result = RegQueryValueExW(key, name, nullptr, &type,
                                   reinterpret_cast<BYTE*>(&val), &size);
    RegCloseKey(key);

    if (result != ERROR_SUCCESS || type != REG_DWORD) return default_val;
    return val != 0;
}

// HKCU\Software\sysmeters に DWORD 値（bool）を書く
static void save_reg_bool(LPCWSTR name, bool value) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, nullptr,
                        0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;

    DWORD val = value ? 1 : 0;
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(val));
    RegCloseKey(key);
}

bool AppWindow::load_topmost()      { return load_reg_bool(REG_TOPMOST,     DEF_TOPMOST);     }
void AppWindow::save_topmost()      { save_reg_bool(REG_TOPMOST,     topmost_);               }
bool AppWindow::load_toast_alert()  { return load_reg_bool(REG_ALERT_TOAST, DEF_TOAST_ALERT); }
void AppWindow::save_toast_alert()  { save_reg_bool(REG_ALERT_TOAST, toast_alert_);           }

// Windows スタートアップ用レジストリ（HKCU\Software\Microsoft\Windows\CurrentVersion\Run キー配下の sysmeters 値）
static constexpr LPCWSTR STARTUP_KEY   = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static constexpr LPCWSTR STARTUP_VALUE = L"sysmeters";

// スタートアップ登録の有無を返す
bool AppWindow::is_startup_registered() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, STARTUP_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;

    LONG result = RegQueryValueExW(key, STARTUP_VALUE, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

// スタートアップへ現在の実行ファイルを登録 / 解除する
//
// 登録時はパスに空白が含まれる環境でも Explorer が正しく解釈できるようダブルクォートで括って書き込む。
void AppWindow::set_startup(bool enable) {
    if (enable) {
        wchar_t exe[MAX_PATH] = {};
        // 戻り値 == MAX_PATH は切り詰められた可能性あり（Windows 10 未満では NUL 終端保証なし）
        DWORD len = GetModuleFileNameW(hinst_, exe, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) return;
        wchar_t command[MAX_PATH + 4];
        swprintf_s(command, L"\"%s\"", exe);

        HKEY key;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, STARTUP_KEY, 0, KEY_WRITE, &key) != ERROR_SUCCESS)
            return;
        DWORD size = static_cast<DWORD>((wcslen(command) + 1) * sizeof(wchar_t));
        RegSetValueExW(key, STARTUP_VALUE, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(command), size);
        RegCloseKey(key);
    }
    else {
        HKEY key;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, STARTUP_KEY, 0, KEY_WRITE, &key) != ERROR_SUCCESS)
            return;
        RegDeleteValueW(key, STARTUP_VALUE);
        RegCloseKey(key);
    }
}

void AppWindow::apply_topmost() {
    SetWindowPos(hwnd_, topmost_ ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}


int AppWindow::compute_occlusion_percent() {
    RECT rc;
    if (!hwnd_ || IsIconic(hwnd_) || !IsWindowVisible(hwnd_) || !GetWindowRect(hwnd_, &rc))
        return 100;

    // 仮想デスクトップ非表示（クローク）時は完全隠蔽扱い
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(hwnd_, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return 100;
    const int w = rc.right  - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return 100;

    // グリッドセルの中心座標をサンプリングする（枠端の影響を避けるため中心を使う）
    //
    // 直前のヒット HWND をキャッシュし、同一 HWND なら GetAncestor 呼び出しを省く。
    // 重なり領域は空間相関が強いため有効打率は高い。
    // 制約：DPI スケーリング・マルチモニタ・WS_EX_LAYERED（透過）は考慮しない。
    // オフスクリーン配置時は全点が NULL → 完全隠蔽（100%）として扱われる。
    constexpr int GRID = 10;
    int  visible  = 0;
    HWND prev_top = nullptr;
    bool prev_own = false;
    for (int iy = 0; iy < GRID; ++iy) {
        for (int ix = 0; ix < GRID; ++ix) {
            POINT p = {
                rc.left + (w * (ix * 2 + 1)) / (GRID * 2),
                rc.top  + (h * (iy * 2 + 1)) / (GRID * 2),
            };
            // NULL は画面外（ユーザから不可視）なので隠蔽として扱う
            HWND top = WindowFromPoint(p);
            bool own;
            if (top == prev_top) own = prev_own;
            else                 own = (top == hwnd_) || (top && GetAncestor(top, GA_ROOT) == hwnd_);
            if (own) ++visible;
            prev_top = top;
            prev_own = own;
        }
    }
    // GRID * GRID で正規化する（GRID 変更時に数値が壊れないようにする）
    return (GRID * GRID - visible) * 100 / (GRID * GRID);
}

void AppWindow::update_process_priority() {
    const int hidden = compute_occlusion_percent();
    DWORD target;
    if      (hidden <       cfg_->priority_visible_range_pct) target = ABOVE_NORMAL_PRIORITY_CLASS;
    else if (hidden < 100 - cfg_->priority_hidden_range_pct)  target = NORMAL_PRIORITY_CLASS;
    else                                                      target = BELOW_NORMAL_PRIORITY_CLASS;

    if (target == current_priority_class_) return;
    if (!SetPriorityClass(GetCurrentProcess(), target)) {
        log_error("priority: SetPriorityClass failed (err=%lu)", GetLastError());
        return;  // キャッシュ更新スキップ（次周期で再試行される）
    }
    current_priority_class_ = target;
}

void AppWindow::restore_process_priority() {
    if (current_priority_class_ == NORMAL_PRIORITY_CLASS) return;
    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
    current_priority_class_ = NORMAL_PRIORITY_CLASS;
}

void AppWindow::run() {
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void AppWindow::destroy() {
    // wnd_proc 経由の遅延メッセージが解放済みメンバを参照しないようにする
    g_window = nullptr;

    if (hwnd_) {
        KillTimer(hwnd_, TIMER_CPU);
        KillTimer(hwnd_, TIMER_FAST);
        KillTimer(hwnd_, TIMER_SLOW);
        KillTimer(hwnd_, TIMER_CLAUDE);
        KillTimer(hwnd_, TIMER_DISK_SPACE);
        KillTimer(hwnd_, TIMER_SMART);
        KillTimer(hwnd_, TIMER_IP);
        KillTimer(hwnd_, TIMER_ANIM);
        KillTimer(hwnd_, TIMER_PRIORITY);
    }
    restore_process_priority();
    remove_tray_icon();

    auto destroy_obj = [](auto*& p) {
        if (p) { p->shutdown(); delete p; p = nullptr; }
    };
    destroy_obj(alert_);
    destroy_obj(col_cpu_);
    destroy_obj(col_gpu_);
    destroy_obj(col_disk_);
    destroy_obj(col_net_);
    destroy_obj(col_claude_);
    destroy_obj(col_ip_);
    destroy_obj(col_mem_);
    destroy_obj(renderer_);
    delete metrics_;  metrics_ = nullptr;
    delete cfg_;      cfg_     = nullptr;
}

LRESULT CALLBACK AppWindow::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_window) return g_window->handle_message(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT AppWindow::handle_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Explorer 再起動後のトレイ復帰（動的メッセージはcase に書けないため先行チェック）
    if (WM_TASKBAR_CREATED_ && msg == WM_TASKBAR_CREATED_) {
        add_tray_icon();
        apply_topmost();
        return 0;
    }

    switch (msg) {
    case WM_TIMER:
        if (wp == TIMER_CPU) {
            // CPU/GPU/ハードフォールト更新（0.9 秒）：CPU グラフ描画タイミングを同期
            col_cpu_->update(metrics_->cpu);
            col_gpu_->update_gpu(metrics_->gpu);
            col_mem_->update_hard_faults(metrics_->mem);
        }
        else if (wp == TIMER_FAST) {
            // 高速更新（1.0 秒）：Disk/Net
            col_disk_->update(metrics_->disk_c, metrics_->disk_d);
            col_net_->update(metrics_->net);
        }
        else if (wp == TIMER_SLOW) {
            // 低速更新（2.0 秒）：RAM/VRAM
            col_mem_->update(metrics_->mem);
            col_gpu_->update_vram(metrics_->vram);
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
        else if (wp == TIMER_PRIORITY) {
            // 優先度制御は再描画や警告チェック対象外（バックグラウンド処理のみ）
            update_process_priority();
            return 0;
        }
        else if (wp == TIMER_ANIM) {
            // コアバー補間アニメーション（30fps）：変化があれば再描画。警告チェック・ウィンドウリサイズは不要
            if (renderer_->update_core_animation(metrics_->cpu))
                InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        else if (wp == TIMER_NOTIFY_SCHED) {
            // 制限強化時間 通知チェック（60 秒周期）：再描画・警告チェック対象外
            check_peak_limit_notify();
            return 0;
        }
        if (alert_) {
            uint32_t fired = alert_->check(*metrics_, *cfg_);
            if (fired && toast_alert_) show_balloon(fired);
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
        case IDM_ALERT_TOAST:
            toast_alert_ = !toast_alert_;
            save_toast_alert();
            break;
        case IDM_STARTUP:
            set_startup(!is_startup_registered());
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
        renderer_->resize(LOWORD(lp), HIWORD(lp));
        return 0;

    case WM_DESTROY:
        destroy();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}
