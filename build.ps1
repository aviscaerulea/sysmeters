# vim: set ft=ps1 fenc=utf-8 ff=unix sw=4 ts=4 et :
# ==================================================
# sysmeters ビルドスクリプト
# MSVC cl.exe で src/*.cpp をコンパイルし out/sysmeters.exe を生成する
#
# 引数:
#   -Version  : バージョン文字列（例: 1.0.0）
#   -Config   : Debug | Release（デフォルト: Debug）
# ==================================================
param(
    [string]$Version = "1.0.0",
    [string]$Config  = "Debug"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# VS 開発環境をロード（公式 DLL モジュール方式、Build Tools 単体環境にも対応）
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -products '*' -latest -property installationPath
if (-not $vsPath) { Write-Error "Visual Studio / Build Tools が見つからない"; exit 1 }

$devShellDll = Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
if (-not (Test-Path $devShellDll)) { Write-Error "DevShell.dll が見つからない: $devShellDll"; exit 1 }
Import-Module $devShellDll
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64"

# 出力ディレクトリ作成
if (-not (Test-Path "out")) { New-Item -ItemType Directory -Path "out" | Out-Null }

# コンパイルオプション
$commonFlags = @(
    "/utf-8", "/EHsc", "/std:c++20",
    "/I", "include",
    "/W3",
    "/DAPP_VERSION=`"$Version`""
)

$debugFlags   = @("/Zi", "/Od", "/DDEBUG", "/MTd")
$releaseFlags = @("/O2", "/DNDEBUG", "/MT")

$configFlags = if ($Config -eq "Release") { $releaseFlags } else { $debugFlags }

# リソースコンパイル（app.ico を実行ファイルに埋め込む）
Write-Host "Compiling resources..."
& rc.exe /fo "out\app.res" "src\app.rc"
if ($LASTEXITCODE -ne 0) { Write-Error "リソースコンパイル失敗 (exit $LASTEXITCODE)"; exit $LASTEXITCODE }

# ソースファイル列挙
$sources = Get-ChildItem "src/*.cpp" | ForEach-Object { $_.FullName }

# リンクライブラリ
$libs = @(
    "d2d1.lib", "dwrite.lib", "pdh.lib",
    "winhttp.lib", "windowscodecs.lib",
    "wbemuuid.lib", "ole32.lib", "oleaut32.lib",
    "shell32.lib", "user32.lib", "gdi32.lib",
    "comctl32.lib", "advapi32.lib"
)

# リンクオプション
$linkFlags = @(
    "/SUBSYSTEM:WINDOWS",
    "/ENTRY:mainCRTStartup"
)
if ($Config -eq "Debug") { $linkFlags += "/DEBUG" }

$outExe = "out\sysmeters.exe"

Write-Host "Building $outExe ($Config, v$Version)..."

$clArgs = $commonFlags + $configFlags + $sources + @("/Fe:$outExe", "/Fo:out\\") + `
          @("/link") + $linkFlags + $libs + @("out\app.res")

& cl.exe @clArgs
if ($LASTEXITCODE -ne 0) { Write-Error "ビルド失敗 (exit $LASTEXITCODE)"; exit $LASTEXITCODE }

Write-Host "Build succeeded: $outExe"
