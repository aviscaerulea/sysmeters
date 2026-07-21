// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#pragma once
#include "metrics.hpp"
#include <windows.h>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

// Claude Code レートリミット情報とセッション数の収集
//
// - セッション数：CreateToolhelp32Snapshot でプロセス列挙（メインスレッド）
// - レートリミット：WinHTTP でバックグラウンドスレッドから非同期取得
//   - credentials.json から OAuth トークン取得
//   - Anthropic Usage API / Account API を呼び出し
//   - $TEMP に JSON キャッシュを保存（Usage: usage_interval_sec 秒、Plan: 3600 秒）
//   - $TEMP に 5h/7d 使用率の時系列履歴を保存（TTL なし、apply_result 実行毎に直接上書き）。
//     アプリ再起動後も init() で読み込み、履歴を復元する。
//     （保持期間は各 delta ウィンドウ幅に従う。7d はデフォルト 12h、5h は数分。
//      7d は加えて停止明け判定用のアンカー 1 個を最大で窓幅 2 倍前まで残す）
//
// アカウントごとに 1 インスタンスを持つ。
// - account_index：WM_CLAUDE_DONE の wParam として送る識別子（0=Main, 1=Sub）
// - config_dir：空ならメイン（~/.claude）、非空ならサブの .claude ディレクトリ絶対パス
// - cache_suffix：キャッシュファイル名末尾に付くサフィックス（メイン=""、サブ="-sub"）
class ClaudeCollector {
public:
    // HWND はバックグラウンドスレッド完了時に WM_CLAUDE_DONE を投げる先
    void init(HWND notify_wnd, int account_index,
              const std::wstring& config_dir, const std::string& cache_suffix,
              int usage_interval_sec = 120,
              bool nudge_enable = false, const std::string& nudge_cmd = {});

    // 60 秒周期のタイマーから呼び出す（非同期 API フェッチのトリガー）。
    // セッション数の取得は行わず、呼び出し側が一括カウントして out に書き込む。
    void update(ClaudeMetrics& out);

    // 取得結果を out へ反映する（WM_CLAUDE_DONE 受信後、メインスレッドから呼ぶ）
    // delta_window_min / delta_window_7d_min は 5h / 7d 履歴の保持期間決定に使う。
    // 履歴は out.five_h_history / out.seven_d_history に push し、
    // (delta_window + 1) × 60 秒より古いサンプルを破棄する。
    // 7d は保持期間より古い最新 1 サンプルをアンカーとして追加保持する（窓幅 2 倍以内かつ
    // 現行 7d ウィンドウ開始以降のもの）。停止明けの実質ペース判定を即時再開するため。
    // （0 で履歴保持が 1 分に縮退し、描画側のオーバーレイ・underuse 判定が事実上無効になる）
    // out 側の履歴が空（アプリ起動後まだ 1 度も push していない）かつ init() が復元した履歴
    // （restored_hist5_ / restored_hist7_）が非空の場合、それを種として一度だけ引き継ぐ。
    // （以後は通常の push_and_trim のみで運用する） avail 時は毎回 cache_hist_path_ へ
    // 直接上書き保存し、次回起動時の復元に備える。（クラッシュ耐性優先。テンポラリ→リネームの
    // 原子的更新はしない。保存に失敗しても現状同様、次回起動時は履歴なしからの復帰に退化する）
    void apply_result(ClaudeMetrics& out, int delta_window_min, int delta_window_7d_min);

    // 2 段階終了：複数コレクタを並行停止できるよう、フラグ立てと join 待ちを分離
    // 両方のコレクタに request_shutdown() を先に呼んでから wait_shutdown() を順に呼ぶことで、
    // 順次 shutdown() 呼び出しの 15 秒 * 2 待機を並列化して合計 15 秒に抑える
    void request_shutdown();
    void wait_shutdown();
    void shutdown() { request_shutdown(); wait_shutdown(); }
    ~ClaudeCollector() { shutdown(); }

private:
    std::atomic<HWND>  notify_wnd_ = nullptr;
    int                account_index_ = 0;     // 0=Main, 1=Sub。WM_CLAUDE_DONE の wParam として送る
    double             usage_ttl_  = 120.0;    // Usage API のキャッシュ TTL（秒）
    bool               nudge_enable_ = false;  // 5h リセット通過後の未消費間隙で claude.exe を起動するか
    std::string        nudge_cmd_;             // 起動コマンドライン
    // サブアカウント用の .claude ディレクトリ。
    // 非空のとき nudge 実行時に CLAUDE_CONFIG_DIR 環境変数を一時設定して claude.exe を起動する。
    // Claude CLI には --config-dir コマンドオプションが存在せず、設定ディレクトリの上書きは
    // CLAUDE_CONFIG_DIR 環境変数経由でのみ可能。設定は子プロセスに限定するためレジストリやプロセス
    // 親環境は変更しない。
    std::wstring       config_dir_;
    // 最後に観測したアクティブ 5h ウィンドウの resets_ts（-1 = 未観測）。
    // init 時のキャッシュ復元値でも種付けされ、リセット通過後の未消費間隙検知の基準になる。
    // fetch スレッドと init（スレッド起動前）からのみ触るため排他は不要
    time_t             watched_5h_resets_ts_ = -1;
    // nudge 発火済みウィンドウの resets_ts（重複抑制キー）。
    // 値 1 は「終了ウィンドウの識別子が不明な間隙で発火済み」を示す番兵
    time_t             last_nudge_resets_ts_ = -1;
    std::atomic<bool>  fetching_   = false;
    // fetch スレッドの世代番号。update() が新スレッド起動または watchdog リセットを行うたびに
    // インクリメントする。do_fetch は起動時に自世代を捕獲し、終端で「自世代のときのみ
    // pending_ / fetching_ / WM_CLAUDE_DONE 通知を実行する」ことで、遅延復帰した旧スレッドが
    // 新世代の状態を破壊するのを防ぐ
    std::atomic<uint32_t> fetch_gen_ = 0;
    // 直近 fetch スレッド起動時刻（GetTickCount64 の ms、単調時計）。update() のハング検知に使う。
    // system_clock を避けるのは NTP 補正等の壁掛け時計飛びで誤発火や見逃しが起きるため
    std::atomic<uint64_t> fetch_start_tick_ = 0;
    std::atomic<bool>  shutdown_   = false;    // shutdown 要求フラグ（fetch スレッドの早期中断用）
    HANDLE             fetch_thread_ = nullptr;
    std::mutex         result_mutex_;
    bool               first_fetch_ = true;   // 初回フェッチフラグ（ネガティブキャッシュ無視に使用）

    // インスタンスごとに固定されるパス。init() で構築する
    // 取得失敗時は空 path のままにし、関連 I/O は静かに失敗させる（既存挙動互換）
    std::filesystem::path creds_path_;
    std::filesystem::path cache_usage_path_;
    std::filesystem::path cache_plan_path_;
    // 5h/7d 履歴の永続化キャッシュ（claude-history-cache{cache_suffix}.json）。
    // apply_result が avail 時に毎回上書き保存し、init() が起動時に読み込んで
    // restored_hist5_ / restored_hist7_ の種にする
    std::filesystem::path cache_hist_path_;

    // バックグラウンドで取得した結果（仮置き）
    ClaudeMetrics pending_{};

    // init() でキャッシュファイルから復元した 5h/7d 履歴（起動直後の一度きりの種）。
    // apply_result 初回呼び出しで out 側の履歴（空）へ移し替えたら空にする。
    // pending_ 経由にしない理由：pending_ は do_fetch 完了ごとに丸ごと上書きされ、
    // フェッチ結果 JSON には履歴フィールドが存在しないため、pending_ 経由だと復元値が消えるため
    std::vector<ClaudeHistorySample> restored_hist5_;
    std::vector<ClaudeHistorySample> restored_hist7_;

    static DWORD WINAPI fetch_thread(LPVOID param);
    void do_fetch();
    // presumed = true は ERR 経路（フェッチ失敗中の時計ベース推定発火）を示し、ログ文言を区別する
    void run_nudge(bool presumed);
    std::string get_token();
};

// claude.exe のセッション数をメイン/サブで分けて数える
// `sub_config_dir` が空またはディレクトリ正規化に失敗した場合は全件をメインとして数える。
// 各 claude.exe プロセスの PEB から環境変数ブロックを読み、CLAUDE_CONFIG_DIR の値を
// GetFullPathNameW で正規化して sub_config_dir と一致するものをサブとしてカウントする。
// OpenProcess 拒否や PEB 読み取り失敗、WoW64（32bit）プロセスは main 扱いにフォールバックする。
struct ClaudeSessionCount {
    int main_count = 0;
    int sub_count  = 0;
};
ClaudeSessionCount count_claude_sessions_split(const std::wstring& sub_config_dir);
