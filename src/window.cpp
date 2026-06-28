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
#include "update_check.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")
#include <filesystem>
#include <thread>
#include <cstring>
namespace fs = std::filesystem;

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

// GitHub リポジトリ URL
static constexpr LPCWSTR GITHUB_URL = L"https://github.com/aviscaerulea/sysmeters";
// GitHub リリース一覧 URL（新版が存在するときにバージョン項目から開く）
static constexpr LPCWSTR GITHUB_RELEASES_URL = L"https://github.com/aviscaerulea/sysmeters/releases";

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
static constexpr int MIN_CLIENT_H = 80;   // コンテンツ高さの最低値（px、OS 行 + ドラッグ余白のみ確保）

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

// SYSTEMTIME → FILETIME 形式（100ns 単位）の 64bit 値変換
// UTC 変換ではなく、ローカル時刻同士の前後比較・境界判定にのみ使う。
static ULONGLONG to_100ns(const SYSTEMTIME& st) {
    FILETIME ft{};
    SystemTimeToFileTime(&st, &ft);
    return (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
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
    col_claude_main_ = new ClaudeCollector();
    if (cfg_->claude_sub.enable) col_claude_sub_ = new ClaudeCollector();
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
    // Claude メイン：account_index=0、config_dir 空（~/.claude 既定）
    col_claude_main_->init(hwnd_, 0, L"", "",
                           cfg_->claude_usage_interval_sec,
                           cfg_->claude_main.nudge_enable, cfg_->claude_nudge_cmd);
    // メインの account_label を反映（[claude] name または "Main"）
    wcsncpy_s(metrics_->claude_main.account_label, cfg_->claude_main.name.c_str(), _TRUNCATE);
    metrics_->claude_main.account_enabled = true;

    // Claude サブ：enable=true 時のみ。
    // nudge_cmd は両アカウント共通で渡し、サブの config_dir はコレクタ側で
    // CLAUDE_CONFIG_DIR 環境変数として子プロセスにのみ設定する。
    // （Claude CLI に --config-dir オプションは存在せず、設定ディレクトリの上書きは環境変数経由のみ）
    if (col_claude_sub_) {
        col_claude_sub_->init(hwnd_, 1, cfg_->claude_sub.config_dir, "-sub",
                              cfg_->claude_usage_interval_sec,
                              cfg_->claude_sub.nudge_enable, cfg_->claude_nudge_cmd);
        wcsncpy_s(metrics_->claude_sub.account_label, cfg_->claude_sub.name.c_str(), _TRUNCATE);
        metrics_->claude_sub.account_enabled = true;
    }
    col_ip_->init(hwnd_);

    // 警告音マネージャ初期化
    alert_ = new AlertManager();
    alert_->init(*cfg_);

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

    // 通知境界判定の前回時刻を起動時刻で初期化（初回 tick から境界またぎを検出できるようにする）
    {
        SYSTEMTIME lt;
        GetLocalTime(&lt);
        last_notify_tick_ = to_100ns(lt);
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
    col_gpu_->update_gpu(metrics_->gpu);
    col_gpu_->update_vram(metrics_->vram);
    col_mem_->update(metrics_->mem);
    col_disk_->update(metrics_->disk_c, metrics_->disk_d);
    col_disk_->update_space(metrics_->disk_c, metrics_->disk_d);
    col_disk_->update_smart(metrics_->disk_c, metrics_->disk_d);
    col_net_->update(metrics_->net);
    // セッション数を 1 回でメイン/サブに分けて取得し、両 metrics に書く
    {
        const std::wstring& sub_dir = col_claude_sub_ ? cfg_->claude_sub.config_dir : std::wstring();
        ClaudeSessionCount sc = count_claude_sessions_split(sub_dir);
        metrics_->claude_main.session_count = sc.main_count;
        metrics_->claude_sub.session_count  = sc.sub_count;
    }
    col_claude_main_->update(metrics_->claude_main);
    if (col_claude_sub_) col_claude_sub_->update(metrics_->claude_sub);
    col_ip_->update();
    update_window_size();
    InvalidateRect(hwnd_, nullptr, FALSE);

    topmost_        = load_topmost();
    apply_topmost();
    toast_alert_    = load_toast_alert();
    fullscreen_mute_ = load_fullscreen_mute();
    load_visibility();

    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd_);

    // 起動時点でピーク期間内なら即時通知
    check_peak_limit_on_startup();

    // GitHub の更新チェックを開始（設定有効時のみ、別スレッドで非同期実行）
    start_update_check();

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
        // 各値とも切り捨て時・NUL 終端なし格納時に備えて末尾 NUL を強制する
        siz = sizeof(prod);
        RegQueryValueExW(key, L"ProductName", nullptr, nullptr, reinterpret_cast<BYTE*>(prod), &siz);
        prod[_countof(prod) - 1] = L'\0';
        siz = sizeof(disp);
        RegQueryValueExW(key, L"DisplayVersion", nullptr, nullptr, reinterpret_cast<BYTE*>(disp), &siz);
        disp[_countof(disp) - 1] = L'\0';
        siz = sizeof(build);
        RegQueryValueExW(key, L"CurrentBuildNumber", nullptr, nullptr, reinterpret_cast<BYTE*>(build), &siz);
        build[_countof(build) - 1] = L'\0';

        // ProductName が Windows 11 でも "Windows 10" を返す MS 既知仕様の補正
        // ビルド番号 22000 以降は Windows 11
        if (_wtoi(build) >= 22000) {
            wchar_t* p = wcsstr(prod, L"Windows 10");
            if (p) p[9] = L'1';
        }

        // 入力合計は os_label（64 文字）を超え得るため、あふれ時は切り詰める（swprintf_s は abort する）
        _snwprintf_s(metrics_->os.os_label, _TRUNCATE, L"%s (%s %s)", prod, disp, build);
        RegCloseKey(key);
    }
}

// タイマー経路用の薄いラッパ
//
// renderer の preferred_height は paint() 末尾で確定するため、
// WM_TIMER 駆動でレイアウト変化を後追いする経路として使う。
void AppWindow::update_window_size() {
    apply_window_height(renderer_->preferred_height());
}

// 指定の client 領域高さでウィンドウをリサイズする
//
// MIN_CLIENT_H クランプ・モニタ作業領域クランプ・キャッシュ判定・renderer.resize() を行う。
// 表示トグル経路は呼び出し前に last_pref_h_ = 0 を設定してキャッシュを無効化する。
void AppWindow::apply_window_height(int target_client_h) {
    int client_h = max(target_client_h, MIN_CLIENT_H);
    if (client_h == last_pref_h_) return;
    last_pref_h_ = client_h;

    RECT adj = {0, 0, cfg_->win_width, client_h};
    AdjustWindowRectEx(&adj, WND_STYLE, FALSE, WND_EX_STYLE);
    int full_h = adj.bottom - adj.top;

    // ウィンドウが属するモニタの作業領域を取得してクランプ（マルチモニタ対応）
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR hmon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    if (GetMonitorInfoW(hmon, &mi)) {
        int max_h = mi.rcWork.bottom - mi.rcWork.top;
        if (full_h > max_h) full_h = max_h;
    }

    RECT rc;
    GetWindowRect(hwnd_, &rc);
    if ((rc.bottom - rc.top) != full_h) {
        SetWindowPos(hwnd_, nullptr, 0, 0, adj.right - adj.left, full_h,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        renderer_->resize(cfg_->win_width, client_h);
    }
}

void AppWindow::on_claude_done(int account_index) {
    // WM_DESTROY 後に遅延到着した場合の二重防御
    if (account_index == 0) {
        if (!col_claude_main_) return;
        col_claude_main_->apply_result(metrics_->claude_main);
    }
    else if (account_index == 1) {
        if (!col_claude_sub_) return;
        col_claude_sub_->apply_result(metrics_->claude_sub);
    }
    else {
        return;
    }
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
// 前回 tick と今回 tick の間にローカル平日 21:00 の境界をまたいだとき 1 回だけ発火する。
// 時刻差で判定するため、タイマー遅延やスリープ復帰で分 0 の tick を飛ばしても取りこぼさない。
// 前回時刻は create() で起動時刻に初期化される（20:59 台起動 → 21:00 台初回 tick でも発火する）。
void AppWindow::check_peak_limit_notify() {
    if (!cfg_->notify_peak_limit_enable) return;

    SYSTEMTIME lt;
    GetLocalTime(&lt);
    const ULONGLONG now  = to_100ns(lt);
    const ULONGLONG prev = last_notify_tick_;
    last_notify_tick_ = now;
    if (prev >= now) return;  // 時計の巻き戻し等は発火しない

    // 今日の通知時刻（ローカル 21:00）の境界が (prev, now] に含まれるか
    // 境界は常に今日の日付で計算するため、前回 tick が前日でも当日分以外は発火しない
    SYSTEMTIME bt = lt;
    bt.wHour         = PEAK_NOTIFY_HOUR;
    bt.wMinute       = PEAK_NOTIFY_MIN;
    bt.wSecond       = 0;
    bt.wMilliseconds = 0;
    const ULONGLONG boundary = to_100ns(bt);
    if (boundary <= prev || boundary > now) return;

    // 平日のみ発火（境界の属する日 = 今日）
    if (lt.wDayOfWeek < 1 || lt.wDayOfWeek > 5) return;

    // フルスクリーン中は通知を保留する。tick を巻き戻して次回 tick で境界を再検出させる
    if (fullscreen_mute_ && is_fullscreen_app_running()) {
        last_notify_tick_ = prev;
        return;
    }

    show_notify(cfg_->notify_peak_limit_title.c_str(),
                cfg_->notify_peak_limit_body.c_str());
    if (cfg_->notify_peak_limit_sound && alert_) alert_->play_external();
    log_info("notify: peak limit toast fired");
}

// 起動時にピーク期間内なら即時通知する（create() から 1 度だけ呼ぶ）
//
// ローカル平日 21:00〜翌 03:00 を近似ピーク期間とみなす。
// 夏時間中は PT 5:00〜11:00 と 1 時間ずれるが、時刻固定方針に従う。
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
    if (fullscreen_mute_ && is_fullscreen_app_running()) return;

    show_notify(cfg_->notify_peak_limit_title.c_str(),
                cfg_->notify_peak_limit_body.c_str());
    if (cfg_->notify_peak_limit_sound && alert_) alert_->play_external();
    log_info("notify: startup peak limit toast fired");
}

// GitHub リリースチェックを別スレッドで開始する
//
// 設定無効時は何もしない。取得はバックグラウンドスレッドで行い、新版があれば WM_UPDATE_DONE を post する。
// スレッドは update_thread_ に保持し、destroy() で join する。
// 取得失敗・新版なしの場合は何も通知しない（UI は通常表示のまま）。
void AppWindow::start_update_check() {
    if (!cfg_->update_check_enabled) return;

    HWND hwnd = hwnd_;
    // APP_VERSION を 1 度だけ評価してから範囲を作る
    // 同一行に 2 回展開すると、文字列プーリング無効のデバッグビルドでは
    // 2 つのリテラルが別アドレスに置かれ、不正なポインタ範囲で異常終了する
    const char* ver = APP_VERSION;
    std::wstring current(ver, ver + std::strlen(ver));
    update_thread_ = std::thread([hwnd, current]() {
        UpdateResult r = check_for_updates(current);
        if (!r.available) return;
        // 最新 tag を heap に載せて UI スレッドへ引き渡す（受信側が delete する）
        // PostMessage 失敗時（キューフル等）はポインタをここで解放する
        auto* tag_ptr = new std::wstring(r.latest_tag);
        if (!PostMessage(hwnd, WM_UPDATE_DONE, 0, reinterpret_cast<LPARAM>(tag_ptr)))
            delete tag_ptr;
    });
}

void AppWindow::show_context_menu() {
    // メニュー最上部に「アプリ名 vX.Y.Z」を表示（新版があれば「→ v新版」を併記）
    wchar_t label[96];
    if (update_available_)
        // tag は GitHub 由来で長さ無制限のため、あふれ時は切り詰める（swprintf_s は abort する）
        _snwprintf_s(label, _TRUNCATE, L"sysmeters v%hs → %ls", APP_VERSION, update_latest_tag_.c_str());
    else
        swprintf_s(label, L"sysmeters v%hs", APP_VERSION);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_GITHUB, label);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (topmost_ ? MF_CHECKED : MF_UNCHECKED),
                IDM_TOPMOST, L"常に最前面に表示");
    AppendMenuW(menu, MF_STRING | (toast_alert_ ? MF_CHECKED : MF_UNCHECKED),
                IDM_ALERT_TOAST, L"Toast 通知");
    AppendMenuW(menu, MF_STRING | (fullscreen_mute_ ? MF_CHECKED : MF_UNCHECKED),
                IDM_FULLSCREEN_MUTE, L"フルスクリーン時は通知しない");
    AppendMenuW(menu, MF_STRING | (is_startup_registered() ? MF_CHECKED : MF_UNCHECKED),
                IDM_STARTUP, L"スタートアップ登録");

    // 表示項目サブメニュー（カテゴリ単位の表示/非表示）
    HMENU view_menu = CreatePopupMenu();
    AppendMenuW(view_menu, MF_STRING | (vis_.cpu    ? MF_CHECKED : 0), IDM_VIS_CPU,    L"CPU");
    AppendMenuW(view_menu, MF_STRING | (vis_.gpu    ? MF_CHECKED : 0), IDM_VIS_GPU,    L"GPU / VRAM");
    AppendMenuW(view_menu, MF_STRING | (vis_.mem    ? MF_CHECKED : 0), IDM_VIS_MEM,    L"Memory");
    AppendMenuW(view_menu, MF_STRING | (vis_.disk   ? MF_CHECKED : 0), IDM_VIS_DISK,   L"Disk");
    AppendMenuW(view_menu, MF_STRING | (vis_.net    ? MF_CHECKED : 0), IDM_VIS_NET,    L"Network");
    // Claude メイン/サブの表示トグル。サブはアカウント未構成時 MF_GRAYED で設定誘導する
    {
        wchar_t main_lbl[64], sub_lbl[64];
        _snwprintf_s(main_lbl, _TRUNCATE, L"Claude (%s)", cfg_->claude_main.name.c_str());
        _snwprintf_s(sub_lbl,  _TRUNCATE, L"Claude (%s)", cfg_->claude_sub.name.c_str());
        AppendMenuW(view_menu, MF_STRING | (vis_.claude_main ? MF_CHECKED : 0),
                    IDM_VIS_CLAUDE_MAIN, main_lbl);
        UINT sub_flags = MF_STRING | (vis_.claude_sub ? MF_CHECKED : 0);
        if (!cfg_->claude_sub.enable) sub_flags |= MF_GRAYED;
        AppendMenuW(view_menu, sub_flags, IDM_VIS_CLAUDE_SUB, sub_lbl);
    }
    AppendMenuW(menu, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(view_menu), L"表示項目");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_OPEN_CONFIG, L"設定ファイル");
    AppendMenuW(menu, MF_STRING, IDM_OPEN_LOG, L"ログファイル");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"終了");

    POINT pt;
    GetCursorPos(&pt);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    // 通知領域メニューが閉じない既知の不具合の回避策（KB135788、SetForegroundWindow と対）
    PostMessage(hwnd_, WM_NULL, 0, 0);
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
static constexpr LPCWSTR REG_ALERT_TOAST    = L"AlertToast";         // Toast 通知設定の値名（REG_DWORD、0 or 1）
static constexpr LPCWSTR REG_FULLSCREEN_MUTE = L"FullscreenMute";  // フルスクリーン抑制設定の値名（REG_DWORD、0 or 1）
// セクション表示フラグの値名（REG_DWORD、0 or 1）
static constexpr LPCWSTR REG_VIS_CPU     = L"Visible_CPU";
static constexpr LPCWSTR REG_VIS_GPU     = L"Visible_GPU";
static constexpr LPCWSTR REG_VIS_MEM     = L"Visible_Memory";
static constexpr LPCWSTR REG_VIS_DISK    = L"Visible_Disk";
static constexpr LPCWSTR REG_VIS_NET     = L"Visible_Network";
// 旧仕様（1 アカウント時代）の値名。マイグレーションのみで参照する
static constexpr LPCWSTR REG_VIS_CLAUDE_LEGACY = L"Visible_Claude";
static constexpr LPCWSTR REG_VIS_CLAUDE_MAIN   = L"Visible_Claude_Main";
static constexpr LPCWSTR REG_VIS_CLAUDE_SUB    = L"Visible_Claude_Sub";
// 更新通知済みバージョンの値名（REG_SZ）。同一版の Toast 通知を 1 回に抑えるために保持する
static constexpr LPCWSTR REG_NOTIFIED_VERSION = L"NotifiedUpdateVersion";

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

// HKCU\Software\sysmeters の REG_SZ 値を読む
//
// キーや値が存在しないか型が不正な場合は空文字を返す。
static std::wstring load_reg_string(LPCWSTR name) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return {};

    DWORD type = 0, bytes = 0;
    LONG r = RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes);
    if (r != ERROR_SUCCESS || type != REG_SZ || bytes < sizeof(wchar_t)) {
        RegCloseKey(key);
        return {};
    }

    std::wstring buf(bytes / sizeof(wchar_t), L'\0');
    r = RegQueryValueExW(key, name, nullptr, nullptr,
                         reinterpret_cast<BYTE*>(buf.data()), &bytes);
    RegCloseKey(key);
    if (r != ERROR_SUCCESS) return {};
    // 末尾の NUL 終端を取り除く
    if (!buf.empty() && buf.back() == L'\0') buf.pop_back();
    return buf;
}

// HKCU\Software\sysmeters に REG_SZ 値を書く
static void save_reg_string(LPCWSTR name, const std::wstring& value) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, nullptr,
                        0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;

    DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    RegSetValueExW(key, name, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(value.c_str()), bytes);
    RegCloseKey(key);
}

// 新版状態を確定し、未通知版なら Toast 通知する（WM_UPDATE_DONE 受信時に UI スレッドで呼ぶ）
//
// 通知済みバージョンをレジストリに記録し、前回値と異なるときだけ通知する。
// フルスクリーン中は通知済みフラグを書かず保留し、次回起動で再試行される。
void AppWindow::on_update_available(const std::wstring& latest_tag) {
    update_available_  = true;
    update_latest_tag_ = latest_tag;

    std::wstring notified = load_reg_string(REG_NOTIFIED_VERSION);
    if (notified == latest_tag) return;  // この版は通知済み
    if (fullscreen_mute_ && is_fullscreen_app_running()) return;

    save_reg_string(REG_NOTIFIED_VERSION, latest_tag);
    wchar_t body[128];
    // tag は GitHub 由来で長さ無制限のため、あふれ時は切り詰める（swprintf_s は abort する）
    _snwprintf_s(body, _TRUNCATE, L"sysmeters v%hs → %ls", APP_VERSION, latest_tag.c_str());
    show_notify(L"新しいバージョンがあります", body);
    log_info("update available: notified");
}

bool AppWindow::load_topmost()      { return load_reg_bool(REG_TOPMOST,     DEF_TOPMOST);     }
void AppWindow::save_topmost()      { save_reg_bool(REG_TOPMOST,     topmost_);               }
bool AppWindow::load_toast_alert()  { return load_reg_bool(REG_ALERT_TOAST, DEF_TOAST_ALERT); }
void AppWindow::save_toast_alert()  { save_reg_bool(REG_ALERT_TOAST, toast_alert_);           }
bool AppWindow::load_fullscreen_mute()  { return load_reg_bool(REG_FULLSCREEN_MUTE, DEF_FULLSCREEN_MUTE); }
void AppWindow::save_fullscreen_mute()  { save_reg_bool(REG_FULLSCREEN_MUTE, fullscreen_mute_);           }

// フルスクリーンアプリケーション実行中の判定
// SHQueryUserNotificationState で OS の通知状態を問い合わせ、
// D3D フルスクリーン・プレゼンテーションモード・ユーザ応答不可状態を検出する。
bool AppWindow::is_fullscreen_app_running() {
    QUERY_USER_NOTIFICATION_STATE state = QUNS_ACCEPTS_NOTIFICATIONS;
    if (FAILED(SHQueryUserNotificationState(&state))) return false;
    return state == QUNS_RUNNING_D3D_FULL_SCREEN
        || state == QUNS_PRESENTATION_MODE
        || state == QUNS_BUSY;
}

void AppWindow::load_visibility() {
    vis_.cpu    = load_reg_bool(REG_VIS_CPU,    true);
    vis_.gpu    = load_reg_bool(REG_VIS_GPU,    true);
    vis_.mem    = load_reg_bool(REG_VIS_MEM,    true);
    vis_.disk   = load_reg_bool(REG_VIS_DISK,   true);
    vis_.net    = load_reg_bool(REG_VIS_NET,    true);

    // 旧 "Visible_Claude" 値（v1.x の単一アカウント時代）が残っていれば、
    // その値を新 Visible_Claude_Main に引き継いでから削除する。1 回限りのマイグレーション
    // Main への書き込みが失敗した状態で Legacy を削除すると設定が消失するため、
    // Set が成功したときに限り Legacy を削除する
    {
        HKEY key;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ | KEY_WRITE, &key) == ERROR_SUCCESS) {
            DWORD dv = 0, sz = sizeof(dv), tp = 0;
            if (RegQueryValueExW(key, REG_VIS_CLAUDE_LEGACY, nullptr, &tp,
                                 reinterpret_cast<BYTE*>(&dv), &sz) == ERROR_SUCCESS) {
                LONG set_ret = RegSetValueExW(key, REG_VIS_CLAUDE_MAIN, 0, REG_DWORD,
                                              reinterpret_cast<const BYTE*>(&dv), sizeof(dv));
                if (set_ret == ERROR_SUCCESS) {
                    RegDeleteValueW(key, REG_VIS_CLAUDE_LEGACY);
                }
            }
            RegCloseKey(key);
        }
    }
    vis_.claude_main = load_reg_bool(REG_VIS_CLAUDE_MAIN, true);
    vis_.claude_sub  = load_reg_bool(REG_VIS_CLAUDE_SUB,  true);
}

void AppWindow::save_visibility() {
    save_reg_bool(REG_VIS_CPU,    vis_.cpu);
    save_reg_bool(REG_VIS_GPU,    vis_.gpu);
    save_reg_bool(REG_VIS_MEM,    vis_.mem);
    save_reg_bool(REG_VIS_DISK,   vis_.disk);
    save_reg_bool(REG_VIS_NET,    vis_.net);
    save_reg_bool(REG_VIS_CLAUDE_MAIN, vis_.claude_main);
    save_reg_bool(REG_VIS_CLAUDE_SUB,  vis_.claude_sub);
}

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
    // 更新チェックスレッドの終了待ち
    //
    // detach せず join することで、ウィンドウ破棄後・最悪 CRT 破棄後にスレッドが
    // 通知を投函する未定義動作を防ぐ。取得が進行中なら HTTP タイムアウトまで待つ。
    if (update_thread_.joinable()) update_thread_.join();

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
        KillTimer(hwnd_, TIMER_NOTIFY_SCHED);
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
    // Claude コレクタは 2 本を並行停止する。
    // 順次 shutdown() を呼ぶと wait の 15 秒 * 2 が直列化するため、
    // 先に両方の request_shutdown を立ててから個別 destroy_obj に進む
    if (col_claude_main_) col_claude_main_->request_shutdown();
    if (col_claude_sub_)  col_claude_sub_->request_shutdown();
    destroy_obj(col_claude_main_);
    destroy_obj(col_claude_sub_);
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
            {
                const std::wstring& sub_dir = col_claude_sub_ ? cfg_->claude_sub.config_dir : std::wstring();
                ClaudeSessionCount sc = count_claude_sessions_split(sub_dir);
                metrics_->claude_main.session_count = sc.main_count;
                metrics_->claude_sub.session_count  = sc.sub_count;
            }
            col_claude_main_->update(metrics_->claude_main);
            if (col_claude_sub_) col_claude_sub_->update(metrics_->claude_sub);
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
            const bool muted = fullscreen_mute_ && is_fullscreen_app_running();
            uint32_t fired = alert_->check(*metrics_, *cfg_, muted);
            if (fired && toast_alert_ && !muted) show_balloon(fired);
        }
        update_window_size();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        renderer_->paint(*metrics_, *cfg_, vis_);
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
        case IDM_FULLSCREEN_MUTE:
            fullscreen_mute_ = !fullscreen_mute_;
            save_fullscreen_mute();
            break;
        case IDM_STARTUP:
            set_startup(!is_startup_registered());
            break;
        // 表示項目トグル：フラグ反転 → 永続化 → 高さ事前計算 → 先行リサイズ → 同期再描画
        case IDM_VIS_CPU:
        case IDM_VIS_GPU:
        case IDM_VIS_MEM:
        case IDM_VIS_DISK:
        case IDM_VIS_NET:
        case IDM_VIS_CLAUDE_MAIN:
        case IDM_VIS_CLAUDE_SUB: {
            switch (LOWORD(wp)) {
            case IDM_VIS_CPU:         vis_.cpu         = !vis_.cpu;         break;
            case IDM_VIS_GPU:         vis_.gpu         = !vis_.gpu;         break;
            case IDM_VIS_MEM:         vis_.mem         = !vis_.mem;         break;
            case IDM_VIS_DISK:        vis_.disk        = !vis_.disk;        break;
            case IDM_VIS_NET:         vis_.net         = !vis_.net;         break;
            case IDM_VIS_CLAUDE_MAIN: vis_.claude_main = !vis_.claude_main; break;
            case IDM_VIS_CLAUDE_SUB:  vis_.claude_sub  = !vis_.claude_sub;  break;
            }
            save_visibility();

            // 先にリサイズして最初のフレームで「新サイズ + 新コンテンツ」を成立させ、
            // 縦幅が遅れて追従する二段表示を防ぐ。
            int new_pref_h = renderer_->compute_preferred_height(*metrics_, vis_);
            last_pref_h_ = 0;  // apply_window_height のキャッシュを無効化
            apply_window_height(new_pref_h);
            InvalidateRect(hwnd_, nullptr, FALSE);
            UpdateWindow(hwnd_);
            break;
        }
        case IDM_GITHUB: {
            // 新版あり時はリリース一覧、なし時はリポジトリトップを開く
            LPCWSTR url = update_available_ ? GITHUB_RELEASES_URL : GITHUB_URL;
            ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOW);
            break;
        }
        case IDM_OPEN_CONFIG: open_config_file(); break;
        case IDM_OPEN_LOG:    open_log_file(); break;
        case IDM_EXIT:        DestroyWindow(hwnd); break;
        }
        return 0;

    case WM_CLAUDE_DONE:
        // wParam にアカウント識別子（0=Main, 1=Sub）が載っている
        on_claude_done(static_cast<int>(wp));
        return 0;

    case WM_IP_DONE:
        if (col_ip_) {
            col_ip_->apply_result(metrics_->net);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_UPDATE_DONE: {
        // lParam は new した最新 tag。所有権を受け取り、必ず delete する
        std::wstring* tag = reinterpret_cast<std::wstring*>(lp);
        if (tag) {
            if (cfg_) on_update_available(*tag);  // 破棄後の遅延到着に対する二重防御
            delete tag;
        }
        return 0;
    }

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
