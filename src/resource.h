// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#pragma once

// アプリケーションアイコン ID
#define IDI_APP_ICON    100
// タスクトレイアイコン用 ID
#define IDI_TRAY_ICON   1
// タスクトレイ通知メッセージ
#define WM_TRAY         (WM_USER + 1)
// コンテキストメニュー項目 ID
#define IDM_OPEN_CONFIG 100
#define IDM_EXIT        101
#define IDM_TOPMOST     102
#define IDM_OPEN_LOG    103
#define IDM_GITHUB       104
#define IDM_ALERT_TOAST  105
// Claude API バックグラウンド完了通知
#define WM_CLAUDE_DONE  (WM_USER + 2)
// IP 取得バックグラウンド完了通知
#define WM_IP_DONE      (WM_USER + 3)
