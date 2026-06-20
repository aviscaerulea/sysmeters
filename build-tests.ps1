# vim: set ft=ps1 fenc=utf-8 ff=unix sw=4 ts=4 et :
# ==================================================
# sysmeters ユニットテストビルドスクリプト
# doctest 形式の tests/*.cpp を out/test_sysmeters.exe にビルドする。
#
# 引数：
#   -Config : Debug | Release（デフォルト：Debug）
# ==================================================
param(
    [string]$Config = "Debug"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { Write-Error "vswhere.exe が見つからない: $vswhere"; exit 1 }
$vsPath = & $vswhere -products '*' -latest -property installationPath
if (-not $vsPath) { Write-Error "Visual Studio / Build Tools が見つからない"; exit 1 }

$devShellDll = Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Import-Module $devShellDll
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64"

# 出力ディレクトリ作成
if (-not (Test-Path "out"))       { New-Item -ItemType Directory -Path "out"       | Out-Null }
if (-not (Test-Path "out\tests")) { New-Item -ItemType Directory -Path "out\tests" | Out-Null }

# テストソース：test_main.cpp が doctest 本体を生成する
$testSources = @(
    "tests\test_main.cpp",
    "tests\test_ring_buffer.cpp",
    "tests\test_config.cpp",
    "tests\test_alert_check.cpp",
    "tests\test_renderer_layout.cpp"
)

# 本体実装の取り込み
# renderer.cpp は #pragma comment(lib, ...) で d2d1.lib / dwrite.lib を自動リンクする。
# 追加で必要なライブラリ：
#   ole32.lib     : alert.cpp の WASAPI 関連（CoCreateInstance, IMMDeviceEnumerator 等）
#   user32.lib    : renderer.cpp の GetClientRect 等のウィンドウ API
#   advapi32.lib  : renderer.cpp の EnumDynamicTimeZoneInformation（Claude ピーク時間表示）
$prodSources = @(
    "src\config.cpp",
    "src\alert.cpp",
    "src\renderer.cpp",
    "src\logger.cpp"
)
$libs        = @("ole32.lib", "user32.lib", "advapi32.lib")

# コンパイルオプション
$commonFlags = @(
    "/utf-8", "/EHsc", "/std:c++20",
    "/I", "include", "/I", "src",
    "/W3",
    "/D_WIN32_WINNT=0x0A00"
)

$debugFlags   = @("/Zi", "/Od", "/DDEBUG", "/MTd")
$releaseFlags = @("/O2", "/DNDEBUG", "/MT")
$configFlags = if ($Config -eq "Release") { $releaseFlags } else { $debugFlags }

$outExe = "out\test_sysmeters.exe"

Write-Host "Building $outExe ($Config) ..."

# 注：DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN は test_main.cpp 側でのみ定義する。
# /D で全 TU に渡すと多重定義になるためコマンドラインには含めない。
$clArgs = $commonFlags + $configFlags + $testSources + $prodSources +
          @("/Fe:$outExe", "/Fo:out\tests\\") +
          @("/link", "/SUBSYSTEM:CONSOLE") + $libs

& cl.exe @clArgs
if ($LASTEXITCODE -ne 0) { Write-Error "テストビルド失敗 (exit $LASTEXITCODE)"; exit $LASTEXITCODE }

Write-Host "Build succeeded: $outExe"
