// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#include "doctest.h"
#include "config.hpp"
#include <filesystem>
#include <fstream>
#include <string>

namespace {

// 一時 TOML ファイルを書き出してパス文字列を返す
// 同一ディレクトリの local オーバーレイは毎回除去し、テスト間の状態漏れを防ぐ。
std::string write_temp_toml(const std::string& content) {
    auto dir   = std::filesystem::temp_directory_path() / "sysmeters-unit-tests";
    std::filesystem::create_directories(dir);
    auto path  = dir / "config.toml";
    auto local = dir / "config.local.toml";
    std::filesystem::remove(local);
    std::ofstream ofs(path, std::ios::binary);
    ofs << content;
    return path.string();
}

}

TEST_CASE("load_config: 存在しないパスはデフォルト値を返す") {
    AppConfig cfg = load_config("__no_such_file_for_sysmeters_tests__.toml");
    CHECK(cfg.warn_cpu_pct  == 95.f);
    CHECK(cfg.reset_cpu_pct == 90.f);
    CHECK(cfg.win_width     == 460);
}

TEST_CASE("load_config: 整数リテラルの float 設定が反映される（issue 修正検証）") {
    // toml11 v4.2.0 の find_or は型不一致で例外を握り潰すため、
    // 整数値の警告閾値を get_float が拾えるかを直接検証する。
    auto path = write_temp_toml(R"(
[threshold]
cpu_pct = 80
gpu_pct = 70
mem_pct = 75
)");
    AppConfig cfg = load_config(path);
    CHECK(cfg.warn_cpu_pct == doctest::Approx(80.f));
    CHECK(cfg.warn_gpu_pct == doctest::Approx(70.f));
    CHECK(cfg.warn_mem_pct == doctest::Approx(75.f));
}

TEST_CASE("load_config: 浮動小数点リテラルの float 設定が反映される") {
    auto path = write_temp_toml(R"(
[threshold]
cpu_pct = 80.5
)");
    AppConfig cfg = load_config(path);
    CHECK(cfg.warn_cpu_pct == doctest::Approx(80.5f));
}

TEST_CASE("load_config: 警告音リセット閾値も整数リテラルで読み込まれる") {
    // 同梱 sysmeters.toml の reset_mem_pct = 70 等が反映される経路を検証する。
    auto path = write_temp_toml(R"(
[threshold]
mem_pct       = 80
reset_mem_pct = 70
)");
    AppConfig cfg = load_config(path);
    CHECK(cfg.warn_mem_pct  == doctest::Approx(80.f));
    CHECK(cfg.reset_mem_pct == doctest::Approx(70.f));
}

TEST_CASE("load_config: reset 閾値が warn 以上のとき clamp_below で補正") {
    auto path = write_temp_toml(R"(
[threshold]
cpu_pct       = 80.0
reset_cpu_pct = 100.0
)");
    AppConfig cfg = load_config(path);
    CHECK(cfg.warn_cpu_pct  == doctest::Approx(80.f));
    CHECK(cfg.reset_cpu_pct == doctest::Approx(75.f));  // warn 80 - margin 5
}

TEST_CASE("load_config: win_width が下限 80 未満なら 80 にクランプ") {
    auto path = write_temp_toml(R"(
[window]
width = 50
)");
    AppConfig cfg = load_config(path);
    CHECK(cfg.win_width == 80);
}

TEST_CASE("load_config: temp_caution >= temp_critical のとき critical を補正") {
    auto path = write_temp_toml(R"(
[threshold]
temp_caution  = 90.0
temp_critical = 80.0
)");
    AppConfig cfg = load_config(path);
    CHECK(cfg.warn_temp_caution  == doctest::Approx(90.f));
    CHECK(cfg.warn_temp_critical == doctest::Approx(100.f));  // caution + 10
}

TEST_CASE("load_config: 16 進整数の色設定が読み込まれる") {
    auto path = write_temp_toml(R"(
[color]
background = 0x123456
)");
    AppConfig cfg = load_config(path);
    CHECK(cfg.col_background == 0x123456u);
}

TEST_CASE("load_config: priority_visible_range_pct が 49 を超えると 49 にクランプ") {
    auto path = write_temp_toml(R"(
[process]
visible_range_pct = 80
hidden_range_pct  = 80
)");
    AppConfig cfg = load_config(path);
    CHECK(cfg.priority_visible_range_pct == 49);
    CHECK(cfg.priority_hidden_range_pct  == 49);
}

TEST_CASE("load_config: [claude_sub] 未指定時はメインのみ有効、サブは無効") {
    auto path = write_temp_toml("");
    AppConfig cfg = load_config(path);
    CHECK(cfg.claude_main.enable == true);
    CHECK(cfg.claude_sub.enable  == false);
}

TEST_CASE("load_config: [claude_sub] enable=false 明示時もサブ無効") {
    auto path = write_temp_toml(R"(
[claude_sub]
enable     = false
config_dir = "C:\\Windows"
)");
    AppConfig cfg = load_config(path);
    CHECK(cfg.claude_sub.enable == false);
}

TEST_CASE("load_config: [claude_sub] enable=true でも config_dir 空ならサブ無効") {
    auto path = write_temp_toml(R"(
[claude_sub]
enable = true
)");
    AppConfig cfg = load_config(path);
    CHECK(cfg.claude_sub.enable == false);
}

TEST_CASE("load_config: [claude_sub] enable=true でも不在パスならサブ無効") {
    auto path = write_temp_toml(R"(
[claude_sub]
enable     = true
config_dir = "Z:\\__no_such_directory_for_sysmeters_tests__"
)");
    AppConfig cfg = load_config(path);
    CHECK(cfg.claude_sub.enable == false);
}

TEST_CASE("load_config: [claude_sub] enable=true かつ実在ディレクトリならサブ有効") {
    // OS の temp ディレクトリは必ず実在する
    auto temp = std::filesystem::temp_directory_path().string();
    // TOML 文字列リテラル用にバックスラッシュをエスケープする
    std::string escaped;
    for (char c : temp) {
        if (c == '\\') escaped += "\\\\";
        else           escaped += c;
    }
    auto toml = std::string("[claude_sub]\nenable = true\nconfig_dir = \"") + escaped + "\"\n";
    auto path = write_temp_toml(toml);
    AppConfig cfg = load_config(path);
    CHECK(cfg.claude_sub.enable == true);
}
