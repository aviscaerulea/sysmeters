// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#pragma once
#include <string>

// GitHub リリースチェックの結果
//
// available: 現在版より新しいリリースが存在するとき true。取得失敗・新版なしは false。
// latest_tag: 最新リリースの tag_name（例: "v1.15.0"）。available が false のときは空。
struct UpdateResult {
    bool         available = false;
    std::wstring latest_tag;
};

// GitHub の最新リリースを確認し、現在版と比較する
//
// current_version: 比較元のバージョン文字列（例: "1.14.2" / "v1.14.2" / "dev"）。
// api.github.com の releases/latest を同期 GET し、tag_name を semver 比較する。
// 通信失敗・JSON 不正・バージョンパース失敗時はすべて available=false を返す。
// 起動時に detach したスレッドから 1 回だけ呼ぶ想定。
UpdateResult check_for_updates(const std::wstring& current_version);
