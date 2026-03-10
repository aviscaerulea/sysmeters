# sysmeters 仕様書

## 概要

Windows 11 上で動作するシステムリソース監視 HUD アプリケーション。
Direct2D で描画した常駐オーバーレイウィンドウに各種メトリクスをリアルタイム表示する。

## ウィンドウ仕様

| 項目 | 仕様 |
|---|---|
| スタイル | `WS_EX_TOOLWINDOW`（タスクバー非表示）、最前面は `SetWindowPos` でトグル制御 |
| フレーム | 細いカスタムタイトルバー + 細いボーダー（Direct2D で自前描画） |
| 配色 | ダーク系（濃いグレー背景 + オレンジ/緑/青系グラフ） |
| 操作 | ドラッグ移動可能 |
| トレイ | タスクトレイにアイコン表示（Shell_NotifyIcon） |
| メニュー | タスクトレイ右クリック：常に最前面に表示（トグル）、設定ファイルを開く、ログを開く、終了 |
| 最前面設定 | `HKCU\Software\sysmeters\Topmost`（REG_DWORD）に永続化、初期値は非最前面 |
| 権限 | **管理者権限必須**（CPU 温度 WMI アクセスに必要） |

## メーター仕様

### CPU

| 要素 | 表示形式 |
|---|---|
| 全体使用率 | 塗りつぶし面グラフ（直近 60 秒）+ パーセンテージ数値 |
| コア別使用率 | 16 本の縦バー横並び（0-100%、論理コア数） |
| 温度 | 横バー（0-100°C）+ 数値（°C）、3段階色分け |

データソース：PDH（使用率）、WMI `MSAcpi_ThermalZoneTemperature`（温度、管理者権限必須）

### GPU

| 要素 | 表示形式 |
|---|---|
| 使用率 | 塗りつぶし面グラフ（直近 60 秒）+ パーセンテージ数値 |
| 温度 | 横バー（0-100°C）+ 数値（°C）、3段階色分け |

データソース：NVML（動的ロード、GPU なし時は "N/A" 表示でクラッシュしない）

### RAM

| 要素 | 表示形式 |
|---|---|
| 使用率 | 横バー + パーセンテージ数値 |
| 使用量 | 数値（例：24/64G） |

データソース：GlobalMemoryStatusEx

### VRAM

| 要素 | 表示形式 |
|---|---|
| 使用率 | 塗りつぶし面グラフ（直近 60 秒）+ パーセンテージ数値 |
| 使用量 | 数値（例：4/16G） |

データソース：NVML

### Disk I/O

C: と D: パーティション別に以下を表示（左 2/3：I/O グラフ、右 1/3：Space）：

| 要素 | 表示形式 |
|---|---|
| Read スループット | 塗りつぶし面グラフ（直近 60 秒）+ MB/s 数値 |
| Write スループット | 塗りつぶし面グラフ（直近 60 秒）+ MB/s 数値 |
| NVMe 温度 | I/O グラフ右上オーバーレイ（NVMe のみ、3段階色分け） |
| Space 使用率 | 横バー + パーセンテージ |
| 容量 | 使用量/総容量（GB） |
| GB/h | 時間あたり書き込み量（NVMe のみ、1 時間間隔で更新） |

データソース：PDH（`\LogicalDisk(C:)\Disk Read Bytes/sec` 等）、NVMe S.M.A.R.T.（`IOCTL_STORAGE_QUERY_PROPERTY`）

### Network

全 NIC 合算（PDH の `_Total` インスタンス）：

| 要素 | 表示形式 |
|---|---|
| 送信スループット | 塗りつぶし面グラフ（直近 60 秒）+ KB/s or MB/s 数値（値に応じて単位自動切替） |
| 受信スループット | 塗りつぶし面グラフ（直近 60 秒）+ KB/s or MB/s 数値 |

データソース：PDH（`\Network Interface(*)\Bytes Sent/sec` 等）

### IP

Network タイトル行の右側に表示：

| 要素 | 表示形式 |
|---|---|
| グローバル IP | テキスト（例：`223.134.59.189`）、取得失敗時は `NO INTERNET📵` |

データソース：`https://checkip.amazonaws.com`（5 分間隔で非同期取得、IPv4/IPv6 対応）
変化検出：`NotifyIpInterfaceChange`（IP Helper API）でネットワーク変化を即時検出して再取得

### Claude Code

| 要素 | 表示形式 |
|---|---|
| 5h レートリミット | 横バー + パーセンテージ + リセット時刻（HH:MM JST） |
| 7d レートリミット | 横バー + パーセンテージ + リセット時刻（M/D 曜 HH:MM JST） |
| プラン名 | テキスト表示（例：Max5, Max20, Pro） |
| セッション数 | 数値表示（実行中の claude.exe プロセス数） |

データソース：Anthropic Usage API / Account API（OAuth トークン使用）

## 温度の色分け

| 状態 | 色 |
|---|---|
| 0〜69°C（正常） | グレー系 |
| 70〜89°C（注意） | オレンジ系 |
| 90°C 以上（危険） | 赤系 |
| データなし（取得不可） | 暗いグレー系 |

CPU・GPU・Disk（NVMe）温度の全てに適用。

## 更新仕様

| 項目 | 値 |
|---|---|
| 高速ポーリング間隔 | 1.1 秒（CPU/GPU/Disk/Net/Claude） |
| 低速ポーリング間隔 | 3.0 秒（RAM/VRAM） |
| グラフ履歴 | 直近 60 ポイント（リングバッファ） |
| Claude API キャッシュ（Usage） | 360 秒 |
| Claude API キャッシュ（Plan） | 3600 秒 |
| S.M.A.R.T. 更新間隔 | 3600 秒（1 時間） |
| グローバル IP 更新間隔 | 300 秒（5 分） |

## Claude API 仕様

- **Usage API**：`https://api.anthropic.com/api/oauth/usage`
- **Account API**：`https://api.anthropic.com/api/oauth/account`
- **認証**：`Authorization: Bearer {token}` / `anthropic-beta: oauth-2025-04-20`
- **トークン取得元**：`~/.claude/.credentials.json` の `claudeAiOauth.accessToken`
- **キャッシュ保存先**：`$TEMP\claude-usage-cache.json`（Usage）、`$TEMP\claude-plan-cache.json`（Plan）
- **API 呼び出し**：バックグラウンドスレッドで非同期実行（WinHTTP）

## ログ仕様

ファイルベースのログ機能。起動情報と重要エラーを記録する。

| 項目 | 仕様 |
|---|---|
| ファイル名 | `sysmeters_YYYYMMDD.log`（日次ローテーション） |
| 出力先 | `sysmeters.toml` の `[log] dir`（デフォルト：`logs/`、exe 相対パス） |
| ログレベル | `INFO`、`ERROR` の 2 段階 |
| フォーマット | `YYYY-MM-DD HH:mm:ss [LEVEL] message` + CRLF |
| スレッドセーフ | `CRITICAL_SECTION` による排他制御 |
| ファイル共有 | `FILE_SHARE_READ` で開くため起動中でもエディタから閲覧可能 |
| 自動削除 | 30 日超の `sysmeters_*.log` を起動時に削除 |
| トレイメニュー | 「ログを開く」→当日ログを既定エディタで開く（ファイル未作成時はディレクトリを開く） |

ログ出力対象：起動・終了・ウィンドウ作成失敗、各コレクタの初期化成功/失敗、Claude API の HTTP/JSON エラー。
毎秒のポーリング失敗（PDH クエリ、WMI クエリ等）は出力しない。

## ビルド仕様

| 項目 | 値 |
|---|---|
| コンパイラ | MSVC cl.exe（Visual Studio 2022 / Build Tools 2022） |
| 言語標準 | C++20 |
| コンパイルオプション | `/utf-8 /EHsc /std:c++20 /I include` |
| リンクライブラリ | d2d1.lib, dwrite.lib, pdh.lib, winhttp.lib, windowscodecs.lib, wbemuuid.lib, ole32.lib, oleaut32.lib, shell32.lib, advapi32.lib |
| サブシステム | `/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup` |
| バージョン注入 | `/DAPP_VERSION=\"x.y.z\"` |
| 出力先 | `out/sysmeters.exe` |

## 外部依存ライブラリ

| ライブラリ | 用途 | 取得方法 |
|---|---|---|
| toml11（シングルヘッダ） | TOML 設定ファイルのパース | GitHub Releases |
| nlohmann/json（シングルヘッダ） | Claude API レスポンスの JSON パース | GitHub Releases |
| NVML（nvml.dll） | GPU/VRAM/温度取得 | NVIDIA ドライバ同梱（動的ロード） |
