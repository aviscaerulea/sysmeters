# vim: set ft=ps1 fenc=utf-8 ff=unix sw=4 ts=4 et :
# ==================================================
# 依存ヘッダオンリーライブラリの取得スクリプト
# toml11 と nlohmann/json を include/ に配置する
# ==================================================
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$includeDir = Join-Path $PSScriptRoot "..\include"
if (-not (Test-Path $includeDir)) { New-Item -ItemType Directory -Path $includeDir | Out-Null }

# toml11 シングルヘッダ版
$tomlDest = Join-Path $includeDir "toml.hpp"
if (-not (Test-Path $tomlDest)) {
    Write-Host "Downloading toml11..."
    $tomlUrl = "https://raw.githubusercontent.com/ToruNiina/toml11/v4.2.0/single_include/toml.hpp"
    Invoke-WebRequest -Uri $tomlUrl -OutFile $tomlDest
    Write-Host "  -> $tomlDest"
}
else {
    Write-Host "toml.hpp already exists, skipping."
}

# nlohmann/json シングルヘッダ版
$jsonDest = Join-Path $includeDir "json.hpp"
if (-not (Test-Path $jsonDest)) {
    Write-Host "Downloading nlohmann/json..."
    $jsonUrl = "https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp"
    Invoke-WebRequest -Uri $jsonUrl -OutFile $jsonDest
    Write-Host "  -> $jsonDest"
}
else {
    Write-Host "json.hpp already exists, skipping."
}

# IntelMSR.bin / AMDFamily17.bin（PawnIO.Modules から CPU 温度取得用モジュール）
$dataDir = Join-Path $PSScriptRoot "..\data"
if (-not (Test-Path $dataDir)) { New-Item -ItemType Directory -Path $dataDir | Out-Null }

$msrDest = Join-Path $dataDir "IntelMSR.bin"
$amdDest = Join-Path $dataDir "AMDFamily17.bin"

if (-not (Test-Path $msrDest) -or -not (Test-Path $amdDest)) {
    Write-Host "Downloading PawnIO.Modules..."
    $zipUrl = "https://github.com/namazso/PawnIO.Modules/releases/download/0.2.4/release_0_2_4.zip"
    $zipTemp = [System.IO.Path]::GetTempFileName() + ".zip"
    Invoke-WebRequest -Uri $zipUrl -OutFile $zipTemp
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [System.IO.Compression.ZipFile]::OpenRead($zipTemp)
    foreach ($entry in $zip.Entries) {
        if ($entry.Name -eq "IntelMSR.bin" -and -not (Test-Path $msrDest)) {
            $stream = $entry.Open()
            $fs = [System.IO.File]::Create($msrDest)
            $stream.CopyTo($fs)
            $fs.Close()
            $stream.Close()
            Write-Host "  -> $msrDest"
        }
        if ($entry.Name -eq "AMDFamily17.bin" -and -not (Test-Path $amdDest)) {
            $stream = $entry.Open()
            $fs = [System.IO.File]::Create($amdDest)
            $stream.CopyTo($fs)
            $fs.Close()
            $stream.Close()
            Write-Host "  -> $amdDest"
        }
    }
    $zip.Dispose()
    Remove-Item $zipTemp -Force
}
else {
    Write-Host "IntelMSR.bin and AMDFamily17.bin already exist, skipping."
}

Write-Host "All dependencies ready."
