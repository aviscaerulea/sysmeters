// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#pragma once
#include "metrics.hpp"
#include <windows.h>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>

// Claude Code レートリミット情報とセッション数の収集
//
// - セッション数：CreateToolhelp32Snapshot でプロセス列挙（メインスレッド）
// - レートリミット：WinHTTP でバックグラウンドスレッドから非同期取得
//   - credentials.json から OAuth トークン取得
//   - Anthropic Usage API / Account API を呼び出し
//   - $TEMP に JSON キャッシュを保存（Usage: usage_interval_sec 秒、Plan: 3600 秒）
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
    // delta_window_min は 5h 履歴の保持期間決定に使用する。（0 で履歴記録自体は行うが描画側で無効化される）
    // 履歴は out.five_h_history に push し、(delta_window_min + 1) × 60 秒より古いサンプルを破棄する
    void apply_result(ClaudeMetrics& out, int delta_window_min);

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
    bool               nudge_enable_ = false;  // 5h リセット後に claude.exe を起動するか
    std::string        nudge_cmd_;             // 起動コマンドライン
    // サブアカウント用の .claude ディレクトリ。
    // 非空のとき nudge 実行時に CLAUDE_CONFIG_DIR 環境変数を一時設定して claude.exe を起動する。
    // Claude CLI には --config-dir コマンドオプションが存在せず、設定ディレクトリの上書きは
    // CLAUDE_CONFIG_DIR 環境変数経由でのみ可能。設定は子プロセスに限定するためレジストリやプロセス
    // 親環境は変更しない。
    std::wstring       config_dir_;
    time_t             last_nudge_resets_ts_ = -1;  // 前回 nudge 実行時の 5h resets_ts（重複抑制）
    std::atomic<bool>  fetching_   = false;
    std::atomic<bool>  shutdown_   = false;    // shutdown 要求フラグ（fetch スレッドの早期中断用）
    HANDLE             fetch_thread_ = nullptr;
    std::mutex         result_mutex_;
    bool               first_fetch_ = true;   // 初回フェッチフラグ（ネガティブキャッシュ無視に使用）

    // インスタンスごとに固定されるパス。init() で構築する
    // 取得失敗時は空 path のままにし、関連 I/O は静かに失敗させる（既存挙動互換）
    std::filesystem::path creds_path_;
    std::filesystem::path cache_usage_path_;
    std::filesystem::path cache_plan_path_;

    // バックグラウンドで取得した結果（仮置き）
    ClaudeMetrics pending_{};

    // 使い切り不能検知用のウィンドウ内ペース追跡
    //
    // 現在ウィンドウをバケット（一定時間の区切り）に分け、バケット完了ごとの増加レート
    // （Δpct ÷ 実経過秒）の上位 3 件を保持する。TOP3 の平均が「実証済みの追い上げ可能ペース」
    // となり、描画側が残り時間への外挿に使う。
    // ウィンドウ切替（resets_ts の大幅な変化）で全状態をクリアする。起動直後・切替直後は
    // 最初のバケット完了まで平均 0（推定不可）を返し、警告を出さない安全側に倒す。
    // 一時的な異常応答への耐性として、同一ウィンドウ内での使用率逆行を検知したら
    // TOP3 を全クリアする。（詳細は update の実装コメントを参照）
    // apply_result（メインスレッド）からのみ触るため排他は不要
    struct PaceTracker {
        time_t window_ts = -1;   // 追跡中ウィンドウの resets_ts（切替検知用）
        time_t bucket_ts  = 0;   // 現在バケット開始サンプルの時刻
        float  bucket_pct = 0.f; // 現在バケット開始サンプルの使用率（%）
        float  last_pct   = 0.f; // 直前サンプルの使用率（%、逆行 = 異常応答の検知用）
        // 逆行直後は基準点（bucket_pct）が異常低値の可能性があり、そこからの増加レートは
        // 正常値への復帰ジャンプを含んで過大になり得る。true の間は次のバケット完了時に
        // レートを記録せず基準を取り直す。
        bool   baseline_suspect = false;
        float  top[3]     = {};  // 観測増加レート上位 3 件（%/秒、0 = 空きスロット）

        // 新サンプルを反映し、TOP3 平均レート（%/秒、0 = 推定不可）を返す
        float update(time_t ts, float pct, time_t resets_ts, time_t bucket_secs);

        // 追跡状態を現サンプル起点で初期化する（ウィンドウ切替・使用率逆行時の共通処理）
        // suspect_baseline は現サンプルが異常低値である可能性を示し、初回バケットのレート採否を制御する
        void restart(time_t ts, float pct, time_t resets_ts, bool suspect_baseline);
    };
    PaceTracker pace_5h_;
    PaceTracker pace_7d_;

    static DWORD WINAPI fetch_thread(LPVOID param);
    void do_fetch();
    void run_nudge();
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
