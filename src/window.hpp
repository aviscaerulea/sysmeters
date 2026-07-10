// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <thread>

#include "renderer.hpp"

struct AppConfig;
struct AllMetrics;
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
// 標準タイトルバー＋閉じるボタンを持つオーバーレイウィンドウ。
// タスクトレイアイコンを表示し、右クリックメニューで操作する。
class AppWindow {
public:
    bool create(HINSTANCE hinstance, const AppConfig& cfg);
    void run();
    void destroy();

    // WM_CLAUDE_DONE 受信時に呼ぶ。
    // account_index は wParam に載せたアカウント識別子（0=Main, 1=Sub）
    void on_claude_done(int account_index);

private:
    // レジストリ未設定時のデフォルト値
    static constexpr bool DEF_TOPMOST        = false;
    static constexpr bool DEF_TOAST_ALERT    = true;
    static constexpr bool DEF_FULLSCREEN_MUTE = true;
    // 「常に警告通知を有効にする」のデフォルト。
    // PC にダメージを与え得る温度系のみ ON とし、使用率系は OFF とする
    static constexpr bool DEF_ALWAYS_ALERT_CPU       = false;
    static constexpr bool DEF_ALWAYS_ALERT_TEMP_CPU  = true;
    static constexpr bool DEF_ALWAYS_ALERT_GPU       = false;
    static constexpr bool DEF_ALWAYS_ALERT_TEMP_GPU  = true;
    static constexpr bool DEF_ALWAYS_ALERT_TEMP_DISK = true;

    // Claude Code 制限強化時間 通知の発火時刻（ローカル時刻でハードコード固定）
    static constexpr int PEAK_NOTIFY_HOUR = 21;
    static constexpr int PEAK_NOTIFY_MIN  = 0;
    // 60 秒周期の通知チェックタイマー ID
    static constexpr int TIMER_NOTIFY_SCHED = 110;

    HWND hwnd_         = nullptr;
    HINSTANCE hinst_   = nullptr;
    int  last_pref_h_  = 0;            // update_window_size 早期リターン用キャッシュ
    bool topmost_        = DEF_TOPMOST;
    bool toast_alert_    = DEF_TOAST_ALERT;
    bool fullscreen_mute_ = DEF_FULLSCREEN_MUTE;  // フルスクリーンアプリ実行中は通知・警告音を抑制
    // フルスクリーン抑制中でも Toast＋警告音を通す例外項目（レジストリ保存）
    bool always_alert_cpu_       = DEF_ALWAYS_ALERT_CPU;        // CPU 使用率
    bool always_alert_temp_cpu_  = DEF_ALWAYS_ALERT_TEMP_CPU;   // CPU 温度
    bool always_alert_gpu_       = DEF_ALWAYS_ALERT_GPU;        // GPU 使用率
    bool always_alert_temp_gpu_  = DEF_ALWAYS_ALERT_TEMP_GPU;   // GPU 温度
    bool always_alert_temp_disk_ = DEF_ALWAYS_ALERT_TEMP_DISK;  // ディスク温度（全ドライブ一括）
    // 現在フルスクリーン抑制が働いているか（タイトルバー表示の差分検知用。WM_TIMER で更新）
    bool fullscreen_silent_ = false;
    Visibility vis_;                   // セクション表示フラグ（カテゴリ単位の表示/非表示）
    UINT WM_TASKBAR_CREATED_ = 0;      // Explorer 再起動によるタスクバー再生成通知
    ULONGLONG last_notify_tick_ = 0;   // 前回通知チェック時のローカル時刻（FILETIME 形式 100ns 単位、境界またぎ検出用）

    // 更新チェック状態（UI スレッドのみが参照する。WM_UPDATE_DONE 受信時に確定）
    bool         update_available_ = false;  // GitHub に新版ありなら true
    std::wstring update_latest_tag_;         // 最新リリースの tag_name（新版あり時のみ）

    // 起動時の GitHub リリースチェックを実行するバックグラウンドスレッド
    //
    // start_update_check() で起動し、destroy() で join する。
    // 他のコレクタと同様に終了処理で待ち合わせ、ウィンドウ破棄後の通知投函を防ぐ。
    std::thread  update_thread_;

    AppConfig*       cfg_     = nullptr;
    AllMetrics*      metrics_ = nullptr;
    Renderer*        renderer_ = nullptr;
    CpuCollector*    col_cpu_  = nullptr;
    GpuCollector*    col_gpu_  = nullptr;
    MemCollector*    col_mem_  = nullptr;
    DiskCollector*   col_disk_   = nullptr;
    NetCollector*    col_net_    = nullptr;
    // Claude コレクタはアカウント別に独立インスタンスを持つ。
    // サブは [claude_sub] enable=true かつ config_dir 検証が成功したときのみ生成される
    ClaudeCollector* col_claude_main_ = nullptr;
    ClaudeCollector* col_claude_sub_  = nullptr;
    IpCollector*     col_ip_     = nullptr;
    AlertManager*    alert_       = nullptr;

    void update_window_size();
    // 指定の client 領域高さでウィンドウをリサイズする
    void apply_window_height(int target_client_h);
    void update_os_label();  // OS バージョンラベルをレジストリから再取得する
    void add_tray_icon();
    void remove_tray_icon();
    // バルーン（Toast）通知表示
    // fired_mask の各ビットが AlertManager::Id に対応する。
    void show_balloon(uint32_t fired_mask);
    // 指定タイトル・本文で Toast 通知（情報レベル）を表示する
    void show_notify(const wchar_t* title, const wchar_t* body);
    // 制限強化時間 通知の発火判定と実行（TIMER_NOTIFY_SCHED から呼ばれる）
    void check_peak_limit_notify();
    // 起動時にピーク期間内なら即時通知する（create() から 1 度だけ呼ぶ）
    void check_peak_limit_on_startup();
    // 起動時に GitHub リリースチェックをバックグラウンドスレッドで開始する（create() から 1 度だけ呼ぶ）
    void start_update_check();
    // WM_UPDATE_DONE 受信時に新版状態を確定し、未通知版なら Toast 通知する
    void on_update_available(const std::wstring& latest_tag);
    void show_context_menu();
    void open_config_file();
    void open_log_file();
    bool load_topmost();        // レジストリから最前面設定を読む
    void save_topmost();        // レジストリに最前面設定を書く
    void apply_topmost();       // SetWindowPos で最前面状態を反映
    bool load_toast_alert();    // レジストリから Toast 通知設定を読む（未設定時は true）
    void save_toast_alert();    // レジストリに Toast 通知設定を書く
    bool load_fullscreen_mute();  // レジストリからフルスクリーン抑制設定を読む（未設定時は true）
    void save_fullscreen_mute();  // レジストリにフルスクリーン抑制設定を書く
    void load_always_alert();  // レジストリから「常に警告通知」5 フラグを一括読み込み
    void save_always_alert();  // レジストリに「常に警告通知」5 フラグを一括保存
    // 「常に警告通知」フラグを AlertManager::Id のビットマスクに変換する
    // （ディスク温度は TEMP_NVME_0..7 の全添字に展開する）
    uint32_t always_alert_mask() const;
    // フルスクリーンアプリが実行中か判定する（SHQueryUserNotificationState 使用）
    bool is_fullscreen_app_running();
    void load_visibility();     // レジストリからセクション表示フラグ全項目を一括読み込み（未設定時は true）
    void save_visibility();     // レジストリにセクション表示フラグ全項目を一括保存
    // 表示トグル共通の後処理：永続化 → 高さ事前計算 → 先行リサイズ → 同期再描画
    // （二段表示防止のため SetWindowPos を paint() より先行させる。IDM_VIS_* とドライブ別トグルが共用）
    void on_visibility_toggled();
    bool is_startup_registered(); // Windows スタートアップ（HKCU\...\Run）の登録有無を返す
    void set_startup(bool enable);// Windows スタートアップに現在の実行ファイルを登録 / 解除する

    // プロセス優先度自動制御
    DWORD current_priority_class_  = NORMAL_PRIORITY_CLASS;      // 直前に適用した優先度クラス（差分検知用キャッシュ）
    int   compute_occlusion_percent();
    void  update_process_priority();
    void  restore_process_priority();

    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle_message(HWND, UINT, WPARAM, LPARAM);
};
