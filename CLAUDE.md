# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 動作環境

- Windows 11（64bit）
- MSVC cl.exe（Visual Studio 2022 または Build Tools 2022）
- **管理者権限必須**（WMI による CPU 温度取得のため）

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

## 参考

@README.md
@spec.md
