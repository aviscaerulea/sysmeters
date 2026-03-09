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
    HWND hwnd_         = nullptr;
    HINSTANCE hinst_   = nullptr;
    int  min_track_x_  = 0;     // WM_GETMINMAXINFO 用：最小ウィンドウ幅（create で計算）
    int  last_pref_h_  = 0;     // update_window_size 早期リターン用キャッシュ
    bool topmost_      = false;  // 最前面表示フラグ（レジストリから読み込み）

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

    void update_window_size();
    void add_tray_icon();
    void remove_tray_icon();
    void show_context_menu();
    void open_config_file();
    bool load_topmost();   // レジストリから最前面設定を読む
    void save_topmost();   // レジストリに最前面設定を書く
    void apply_topmost();  // SetWindowPos で最前面状態を反映

    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle_message(HWND, UINT, WPARAM, LPARAM);
};
