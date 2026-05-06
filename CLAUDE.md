# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 動作環境

- Windows 11（64bit）
- MSVC cl.exe（Visual Studio 2022 または Build Tools 2022）
- 管理者権限不要
- CPU 温度を表示するには PawnIO ドライバのインストールが必要（`winget install namazso.PawnIO`）

## ビルドコマンド

```powershell
# 依存ライブラリの取得（初回のみ）
pwsh.exe scripts/fetch-deps.ps1

# ビルド（デバッグ）
task build

# クリーン
task clean

# リリースビルド（zip パッケージ）
task release
```

ビルド出力先：`out/sysmeters.exe`

## アーキテクチャ

### ファイル構成

```
src/
├── main.cpp                WinMain、メッセージループ、1秒タイマー
├── window.hpp/.cpp         ウィンドウ管理（カスタムタイトルバー、ドラッグ、タスクトレイ）
├── renderer.hpp/.cpp       Direct2D 描画（面グラフ、縦バー、横バー、テキスト）
├── config.hpp/.cpp         TOML 設定読み書き（sysmeters.toml）
├── metrics.hpp             全メトリクス構造体定義
├── ring_buffer.hpp         固定長リングバッファ（テンプレート）
├── collector_cpu.hpp/.cpp  CPU 使用率（PDH）+ 温度（WMI）
├── collector_gpu.hpp/.cpp  GPU 使用率/温度/VRAM（NVML 動的ロード）
├── collector_mem.hpp/.cpp  RAM 使用量（GlobalMemoryStatusEx）
├── collector_disk.hpp/.cpp Disk I/O（PDH、C:/D: パーティション別）
├── collector_net.hpp/.cpp  Network I/O（PDH、全 NIC 合算）
└── collector_claude.hpp/.cpp Claude API 呼び出し（WinHTTP 非同期）+ セッション数
```

### データフロー

```
WM_TIMER (1s) → Collectors → Metrics → Renderer → Direct2D → Window
```

- CPU/Mem/Disk/Net/GPU：メインスレッドで同期取得（軽量）
- Claude API：バックグラウンドスレッドで非同期取得（WinHTTP）

### 主要技術

- **描画**：Direct2D + DirectWrite
- **CPU/Disk/Net**：PDH（Performance Data Helper）
- **GPU/VRAM**：NVML を `LoadLibrary` で動的ロード（GPU なしでも動作）
- **CPU 温度**：WMI `root\WMI` `MSAcpi_ThermalZoneTemperature`（管理者権限必須）
- **Claude API**：WinHTTP + OAuth トークン（`~/.claude/.credentials.json`）
- **設定**：TOML（toml11 シングルヘッダ）
- **JSON**：nlohmann/json シングルヘッダ

## 実装上の注意点

- `docs/screenshot.png` を差し替える際は、グローバル IP アドレスがマスクされていることを必ず確認する。過去に自宅 IP が Git 履歴に残るインシデントがあった

## 参考

@README.md
@spec.md

<!-- code-review-graph MCP tools -->
## MCP Tools: code-review-graph

**IMPORTANT: This project has a knowledge graph. ALWAYS use the
code-review-graph MCP tools BEFORE using Grep/Glob/Read to explore
the codebase.** The graph is faster, cheaper (fewer tokens), and gives
you structural context (callers, dependents, test coverage) that file
scanning cannot.

### When to use graph tools FIRST

- **Exploring code**: `semantic_search_nodes` or `query_graph` instead of Grep
- **Understanding impact**: `get_impact_radius` instead of manually tracing imports
- **Code review**: `detect_changes` + `get_review_context` instead of reading entire files
- **Finding relationships**: `query_graph` with callers_of/callees_of/imports_of/tests_for
- **Architecture questions**: `get_architecture_overview` + `list_communities`

Fall back to Grep/Glob/Read **only** when the graph doesn't cover what you need.

### Key Tools

| Tool | Use when |
|------|----------|
| `detect_changes` | Reviewing code changes — gives risk-scored analysis |
| `get_review_context` | Need source snippets for review — token-efficient |
| `get_impact_radius` | Understanding blast radius of a change |
| `get_affected_flows` | Finding which execution paths are impacted |
| `query_graph` | Tracing callers, callees, imports, tests, dependencies |
| `semantic_search_nodes` | Finding functions/classes by name or keyword |
| `get_architecture_overview` | Understanding high-level codebase structure |
| `refactor_tool` | Planning renames, finding dead code |

### Workflow

1. The graph auto-updates on file changes (via hooks).
2. Use `detect_changes` for code review.
3. Use `get_affected_flows` to understand impact.
4. Use `query_graph` pattern="tests_for" to check coverage.
