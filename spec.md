# sysmeters Specification

## Overview

A system resource monitoring HUD application running on Windows 11.
Displays various metrics in real time on a persistent overlay window rendered with Direct2D.

## Window Specification

| Item | Specification |
|---|---|
| Style | `WS_EX_TOOLWINDOW` (hidden from taskbar); always-on-top toggled via `SetWindowPos` |
| Frame | Thin custom title bar + thin border (self-drawn with Direct2D) |
| Color scheme | Dark theme (deep gray background + orange/green/blue graphs) |
| Interaction | Drag to move |
| Tray | Icon in system tray (Shell_NotifyIcon) |
| Menu | Right-click tray: version display (click to open GitHub), always on top (toggle), config file, log file, exit |
| Always-on-top setting | Persisted in `HKCU\Software\sysmeters\Topmost` (REG_DWORD); default is non-topmost |
| Privileges | Admin required only for CPU temperature (WMI `ROOT\WMI` access) |

## Meter Specification

### OS

Single-line section displayed above CPU:

| Element | Display |
|---|---|
| Section label | `"OS"` (font_normal_, 22pt, left) |
| OS label | `"Windows 11 Pro (24H2 26100)"` format (font_small_, 18pt, alpha 0.6, after label) |
| Uptime | `"N日 HH時間MM分"` or `"HH時間MM分"` (font_small_, 18pt, alpha 0.6, right-aligned) |

Data source: `GetComputerNameW` (machine name), registry `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion` (OS label, refreshed hourly via TIMER_SMART), `GetTickCount64` (uptime, updated every 60 s via TIMER_CLAUDE)

### CPU

| Element | Display |
|---|---|
| Overall usage | Filled area graph (last 60 s) + percentage value |
| Per-core usage | 16 vertical bars side by side (0-100%, logical core count) |
| Temperature | Horizontal bar (0-100°C) + value (°C), 3-level color coding |

Data source: PDH (usage), WMI `MSAcpi_ThermalZoneTemperature` (temperature, admin required)

### GPU

| Element | Display |
|---|---|
| Usage | Filled area graph (last 60 s) + percentage value |
| Temperature | Horizontal bar (0-100°C) + value (°C), 3-level color coding |

Data source: NVML (dynamically loaded; displays "N/A" without crashing when no GPU is present)

### RAM

| Element | Display |
|---|---|
| Usage rate | Horizontal bar + percentage value |
| Used amount | Numeric (e.g., 24/64G) |

Data source: GlobalMemoryStatusEx

### VRAM

| Element | Display |
|---|---|
| Usage rate | Filled area graph (last 60 s) + percentage value |
| Used amount | Numeric (e.g., 4/16G) |

Data source: NVML

### Disk I/O

Displayed per C: and D: partition (left 2/3: I/O graph, right 1/3: Space):

| Element | Display |
|---|---|
| Read throughput | Filled area graph (last 60 s) + MB/s value |
| Write throughput | Filled area graph (last 60 s) + MB/s value |
| NVMe temperature | Top-right overlay on I/O graph (NVMe only, 3-level color coding) |
| Space usage | Horizontal bar + percentage |
| Capacity | Used/total (GB) |
| GB/h | Hourly write amount (NVMe only, updated every hour) |

Data source: PDH (`\LogicalDisk(C:)\Disk Read Bytes/sec`, etc.), NVMe S.M.A.R.T. (`IOCTL_STORAGE_QUERY_PROPERTY`)

### Network

All NICs aggregated (PDH `_Total` instance):

| Element | Display |
|---|---|
| Send throughput | Filled area graph (last 60 s) + KB/s or MB/s value (unit auto-switched by magnitude) |
| Receive throughput | Filled area graph (last 60 s) + KB/s or MB/s value |

Data source: PDH (`\Network Interface(*)\Bytes Sent/sec`, etc.)

### IP

Displayed to the right of the Network title row:

| Element | Display |
|---|---|
| Global IP | Text (e.g., `223.134.59.189`); shows `NO INTERNET📵` on failure |

Data source: `https://checkip.amazonaws.com` (fetched asynchronously every 5 minutes, IPv4/IPv6 supported)
Change detection: `NotifyIpInterfaceChange` (IP Helper API) triggers immediate re-fetch on network change

### Claude Code

| Element | Display |
|---|---|
| 5h rate limit | Horizontal bar + percentage + reset time (HH:MM JST) |
| 7d rate limit | Horizontal bar + percentage + reset time (M/D ddd HH:MM JST) |
| Plan name | Text (e.g., Max5, Max20, Pro) |
| Session count | Numeric (number of running claude.exe processes) |

Data source: Anthropic Usage API / Account API (OAuth token)

## Temperature Color Coding

| State | Color |
|---|---|
| 0-69°C (normal) | Gray |
| 70-89°C (caution) | Orange |
| 90°C and above (critical) | Red |
| No data (unavailable) | Dark gray |

Applied to CPU, GPU, and Disk (NVMe) temperatures.

## Update Specification

| Item | Value |
|---|---|
| CPU polling interval | 1.0 s |
| Fast polling interval | 1.0 s (GPU/Disk/Net) |
| Slow polling interval | 5.0 s (RAM/VRAM) |
| Claude polling interval | 60 s |
| OS uptime interval | 60 s |
| OS version interval | 3600 s (1 hour) |
| Graph history | Last 60 points (ring buffer) |
| Claude API cache (Usage) | 360 s |
| Claude API cache (Plan) | 3600 s |
| S.M.A.R.T. update interval | 3600 s (1 hour) |
| Disk space update interval | 300 s (5 minutes) |
| Global IP update interval | 300 s (5 minutes) |

## Claude API Specification

- **Usage API**: `https://api.anthropic.com/api/oauth/usage`
- **Account API**: `https://api.anthropic.com/api/oauth/account`
- **Auth**: `Authorization: Bearer {token}` / `anthropic-beta: oauth-2025-04-20`
- **Token source**: `claudeAiOauth.accessToken` in `~/.claude/.credentials.json`
- **Cache location**: `$TEMP\claude-usage-cache.json` (Usage), `$TEMP\claude-plan-cache.json` (Plan)
- **API calls**: Executed asynchronously on a background thread (WinHTTP)

## Log Specification

File-based logging that records startup information and critical errors.

| Item | Specification |
|---|---|
| File name | `sysmeters_YYYYMMDD.log` (daily rotation) |
| Output directory | `[log] dir` in `sysmeters.toml` (default: `logs/`, relative to exe) |
| Log levels | `INFO` and `ERROR` (2 levels) |
| Format | `YYYY-MM-DD HH:mm:ss [LEVEL] message` + CRLF |
| Thread safety | Mutual exclusion via `CRITICAL_SECTION` |
| File sharing | Opened with `FILE_SHARE_READ`, viewable in editors while running |
| Auto-deletion | Deletes `sysmeters_*.log` files older than 30 days on startup |
| Tray menu | "Open Log" → opens today's log in the default editor (opens directory if file not yet created) |

Logged events: startup/shutdown, window creation failure, collector initialization success/failure, Claude API HTTP/JSON errors, config file parse errors.
Per-second polling failures (PDH queries, WMI queries, etc.) are not logged.

## Build Specification

| Item | Value |
|---|---|
| Compiler | MSVC cl.exe (Visual Studio 2022 / Build Tools 2022) |
| Language standard | C++20 |
| Compile options | `/utf-8 /EHsc /std:c++20 /I include` |
| Link libraries | d2d1.lib, dwrite.lib, pdh.lib, winhttp.lib, windowscodecs.lib, wbemuuid.lib, ole32.lib, oleaut32.lib, shell32.lib, advapi32.lib |
| Subsystem | `/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup` |
| Version injection | `/DAPP_VERSION=\"x.y.z\"` |
| Output | `out/sysmeters.exe` |

## External Dependencies

| Library | Purpose | Source |
|---|---|---|
| toml11 (single header) | TOML config file parsing | GitHub Releases |
| nlohmann/json (single header) | JSON parsing for Claude API responses | GitHub Releases |
| NVML (nvml.dll) | GPU/VRAM/temperature acquisition | Bundled with NVIDIA driver (dynamically loaded) |
