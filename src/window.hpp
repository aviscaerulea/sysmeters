// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

struct AppConfig;
struct AllMetrics;
class Renderer;
class CpuCollector;
class GpuCollector;
class MemCollector;
class DiskCollector;
class NetCollector;
class ClaudeCollector;
class IpCollector;
class AlertManager;

// アプリケーションウィンドウの管理
//
// カスタムタイトルバー + ボーダーを持つオーバーレイウィンドウ。
// タスクトレイアイコンを表示し、右クリックメニューで操作する。
class AppWindow {
public:
    bool create(HINSTANCE hinstance, const AppConfig& cfg);
    void run();
    void destroy();

    // WM_CLAUDE_DONE 受信時に呼ぶ
    void on_claude_done();

private:
    // レジストリ未設定時のデフォルト値（load_topmost / load_toast_alert の fallback）
    static constexpr bool DEF_TOPMOST     = false;
    static constexpr bool DEF_TOAST_ALERT = true;

    HWND hwnd_         = nullptr;
    HINSTANCE hinst_   = nullptr;
    int  last_pref_h_  = 0;            // update_window_size 早期リターン用キャッシュ
    bool topmost_      = DEF_TOPMOST;
    bool toast_alert_  = DEF_TOAST_ALERT;

    AppConfig*       cfg_     = nullptr;
    AllMetrics*      metrics_ = nullptr;
    Renderer*        renderer_ = nullptr;
    CpuCollector*    col_cpu_  = nullptr;
    GpuCollector*    col_gpu_  = nullptr;
    MemCollector*    col_mem_  = nullptr;
    DiskCollector*   col_disk_   = nullptr;
    NetCollector*    col_net_    = nullptr;
    ClaudeCollector* col_claude_ = nullptr;
    IpCollector*     col_ip_     = nullptr;
    AlertManager*    alert_       = nullptr;

    void update_window_size();
    void update_os_label();  // OS バージョンラベルをレジストリから再取得する
    void add_tray_icon();
    void remove_tray_icon();
    // バルーン（Toast）通知表示
    // fired_mask の各ビットが AlertManager::Id に対応する。
    void show_balloon(uint32_t fired_mask);
    void show_context_menu();
    void open_config_file();
    void open_log_file();
    bool load_topmost();        // レジストリから最前面設定を読む
    void save_topmost();        // レジストリに最前面設定を書く
    void apply_topmost();       // SetWindowPos で最前面状態を反映
    bool load_toast_alert();    // レジストリから Toast 通知設定を読む（未設定時は true）
    void save_toast_alert();    // レジストリに Toast 通知設定を書く

    // プロセス優先度自動制御
    DWORD current_priority_class_  = NORMAL_PRIORITY_CLASS;      // 直前に適用した優先度クラス（差分検知用キャッシュ）
    int   compute_occlusion_percent();
    void  update_process_priority();
    void  restore_process_priority();

    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle_message(HWND, UINT, WPARAM, LPARAM);
};
