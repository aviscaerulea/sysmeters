// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#pragma once
#include <string>

// ログモジュール初期化（アプリ起動時に 1 回呼ぶ）
//
// dir: ログ出力先ディレクトリ（UTF-8）。相対パスの場合は実行ファイルと同ディレクトリを基準に解決する。
// ディレクトリが存在しなければ自動作成する。30 日超の古いログファイルを削除する。
void log_init(const std::string& dir);

// ログモジュール終了
void log_shutdown();

// 解決済みのログ出力先ディレクトリを返す（log_init の後に有効）
const wchar_t* log_get_dir();

// INFO レベルのログ出力
void log_info(const char* fmt, ...);

// ERROR レベルのログ出力
void log_error(const char* fmt, ...);
