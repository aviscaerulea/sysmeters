// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "alert.hpp"
#include "logger.hpp"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
namespace fs = std::filesystem;

// BLE ヘッドフォン対策定数

// 冒頭不可聴トーンの時間（ミリ秒）
static constexpr int    BLE_LEADING_TONE_MS  = 1500;
// 末尾不可聴トーンの時間（ミリ秒）
static constexpr int    BLE_TRAILING_TONE_MS = 2000;
// 不可聴トーンの周波数（Hz）。BLE デバイスの省電力移行を防止する
static constexpr int    TONE_FREQ_HZ         = 19000;
// 不可聴トーンの振幅（int16_t、フルスケール 32767 の約 3%）
static constexpr int    TONE_AMPLITUDE       = 1000;
// 円周率
static constexpr double PI                   = 3.14159265358979323846;

// 再生スレッドに渡すパラメータ
struct SoundParam {
    wchar_t              wav_path[MAX_PATH];
    const volatile bool* shutdown;
};

// 19kHz 不可聴トーンをバッファに書き込む
//
// ナイキスト周波数を超える場合は無音にフォールバックする。
static void fill_tone_buffer(BYTE* buf, UINT32 frames,
                              const WAVEFORMATEX& fmt, double& phase) {
    if (TONE_FREQ_HZ >= static_cast<int>(fmt.nSamplesPerSec / 2)) {
        memset(buf, 0, frames * fmt.nBlockAlign);
        return;
    }
    int16_t* samples = reinterpret_cast<int16_t*>(buf);
    const double step = 2.0 * PI * TONE_FREQ_HZ / fmt.nSamplesPerSec;
    for (UINT32 i = 0; i < frames; i++) {
        int16_t v = static_cast<int16_t>(TONE_AMPLITUDE * std::sin(phase));
        for (int ch = 0; ch < fmt.nChannels; ch++)
            samples[i * fmt.nChannels + ch] = v;
        phase += step;
    }
    phase = std::fmod(phase, 2.0 * PI);
}

// WAV ファイルを WASAPI 共有モードで再生する
//
// path: 16bit PCM WAV ファイルのフルパス
// shutdown: true になると再生を中断して終了する
// BLE 対策として冒頭 BLE_LEADING_TONE_MS + 末尾 BLE_TRAILING_TONE_MS の 19kHz トーンを挿入する。
// WAV ファイル自体は変更しない（オンメモリで構成する）。
static bool play_wav_wasapi(const wchar_t* path, const volatile bool* shutdown) {
    HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        log_error("alert: CreateFileW failed");
        return false;
    }

    bool             ok          = false;
    HANDLE           hEvent      = nullptr;
    WAVEFORMATEX     fmt         = {};
    DWORD            dataStart   = 0;
    DWORD            totalFrames = 0;
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice*           device     = nullptr;
    IAudioClient*        client     = nullptr;
    IAudioRenderClient*  render     = nullptr;

    // RIFF/WAVE ヘッダ検証
    {
        char  buf[12] = {};
        DWORD nRead   = 0;
        ReadFile(hFile, buf, 12, &nRead, nullptr);
        if (nRead != 12 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
            log_error("alert: invalid RIFF/WAVE header");
            goto cleanup;
        }
    }

    // チャンク走査：fmt と data を探す
    {
        bool hasFmt = false, hasData = false;
        while (!hasData) {
            char  id[4] = {};
            DWORD chunkSize = 0, nRead = 0;
            if (!ReadFile(hFile, id, 4, &nRead, nullptr) || nRead != 4) break;
            if (!ReadFile(hFile, &chunkSize, 4, &nRead, nullptr) || nRead != 4) break;

            if (memcmp(id, "fmt ", 4) == 0) {
                DWORD readSize = std::min(chunkSize, (DWORD)sizeof(WAVEFORMATEX));
                ReadFile(hFile, &fmt, readSize, &nRead, nullptr);
                if (chunkSize > readSize)
                    SetFilePointer(hFile, (LONG)(chunkSize - readSize), nullptr, FILE_CURRENT);
                if (fmt.wFormatTag != WAVE_FORMAT_PCM || fmt.wBitsPerSample != 16
                        || fmt.nSamplesPerSec == 0 || fmt.nBlockAlign == 0) {
                    log_error("alert: unsupported format (16bit PCM WAV only)");
                    goto cleanup;
                }
                hasFmt = true;
            }
            else if (memcmp(id, "data", 4) == 0) {
                if (!hasFmt) { log_error("alert: data chunk before fmt chunk"); goto cleanup; }
                totalFrames = chunkSize / fmt.nBlockAlign;
                dataStart   = SetFilePointer(hFile, 0, nullptr, FILE_CURRENT);
                hasData     = true;
            }
            else {
                // 不明チャンクをスキップ（偶数バイト境界）
                SetFilePointer(hFile, (LONG)((chunkSize + 1) & ~1u), nullptr, FILE_CURRENT);
            }
        }
        if (!hasFmt || !hasData) {
            log_error("alert: fmt or data chunk not found");
            goto cleanup;
        }
    }

    // WASAPI デバイス取得
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator)))) {
        log_error("alert: CoCreateInstance IMMDeviceEnumerator failed");
        goto cleanup;
    }
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
        log_error("alert: GetDefaultAudioEndpoint failed");
        goto cleanup;
    }
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(&client)))) {
        log_error("alert: Activate IAudioClient failed");
        goto cleanup;
    }

    hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!hEvent) goto cleanup;

    {
        constexpr REFERENCE_TIME bufDuration = 500'000;  // 50ms = 500,000 * 100ns
        // AUTOCONVERTPCM + SRC_DEFAULT_QUALITY で BLE 等フォーマット差異のあるデバイスにも対応する
        constexpr DWORD initFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                                  | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                                  | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, initFlags,
                bufDuration, 0, &fmt, nullptr))) {
            log_error("alert: IAudioClient::Initialize failed");
            goto cleanup;
        }
    }
    if (FAILED(client->SetEventHandle(hEvent))) {
        log_error("alert: SetEventHandle failed");
        goto cleanup;
    }

    if (FAILED(client->GetService(__uuidof(IAudioRenderClient),
                reinterpret_cast<void**>(&render)))) {
        log_error("alert: GetService IAudioRenderClient failed");
        goto cleanup;
    }

    {
        UINT32 bufFrames = 0;
        if (FAILED(client->GetBufferSize(&bufFrames)) || bufFrames == 0) {
            log_error("alert: GetBufferSize failed");
            goto cleanup;
        }

        // 冒頭不可聴トーン挿入（BLE ヘッドフォン省電力移行防止）
        {
            const UINT32 toneFrames = fmt.nSamplesPerSec * BLE_LEADING_TONE_MS / 1000;
            UINT32 written = 0;
            double phase   = 0.0;
            client->Start();
            while (written < toneFrames && !*shutdown) {
                WaitForSingleObject(hEvent, 200);
                UINT32 padding = 0;
                client->GetCurrentPadding(&padding);
                UINT32 avail  = bufFrames - padding;
                UINT32 frames = std::min(avail, toneFrames - written);
                if (frames == 0) continue;
                BYTE* buf = nullptr;
                if (SUCCEEDED(render->GetBuffer(frames, &buf))) {
                    fill_tone_buffer(buf, frames, fmt, phase);
                    render->ReleaseBuffer(frames, 0);
                }
                written += frames;
            }
            client->Stop();
            client->Reset();
        }

        // data チャンク先頭にシーク
        SetFilePointer(hFile, (LONG)dataStart, nullptr, FILE_BEGIN);

        // WAV PCM 供給ループ
        bool eof = false;
        {
            DWORD sentFrames = 0;
            client->Start();
            while (!eof && !*shutdown) {
                WaitForSingleObject(hEvent, 200);
                UINT32 padding = 0;
                client->GetCurrentPadding(&padding);
                UINT32 avail = bufFrames - padding;
                if (avail == 0) continue;
                UINT32 frames = std::min(avail, static_cast<UINT32>(totalFrames - sentFrames));
                if (frames == 0) {
                    // 全フレーム送信済み。残りバッファが再生されるまで待機（最大約 1 秒）
                    for (int i = 0; i < 100 && !*shutdown; i++) {
                        UINT32 rem = 0;
                        client->GetCurrentPadding(&rem);
                        if (rem == 0) break;
                        Sleep(10);
                    }
                    eof = true;
                    break;
                }
                BYTE* buf = nullptr;
                if (SUCCEEDED(render->GetBuffer(frames, &buf))) {
                    DWORD nActual = 0;
                    ReadFile(hFile, buf, frames * fmt.nBlockAlign, &nActual, nullptr);
                    UINT32 framesRead = nActual / fmt.nBlockAlign;
                    render->ReleaseBuffer(framesRead, 0);
                    sentFrames += framesRead;
                }
            }
            client->Stop();
        }

        // 末尾不可聴トーン挿入（BLE ヘッドフォン省電力移行防止）
        if (eof) {
            const UINT32 trailFrames = fmt.nSamplesPerSec * BLE_TRAILING_TONE_MS / 1000;
            UINT32 written = 0;
            double phase   = 0.0;
            client->Start();
            while (written < trailFrames && !*shutdown) {
                WaitForSingleObject(hEvent, 200);
                UINT32 padding = 0;
                client->GetCurrentPadding(&padding);
                UINT32 avail  = bufFrames - padding;
                UINT32 frames = std::min(avail, trailFrames - written);
                if (frames == 0) continue;
                BYTE* buf = nullptr;
                if (SUCCEEDED(render->GetBuffer(frames, &buf))) {
                    fill_tone_buffer(buf, frames, fmt, phase);
                    render->ReleaseBuffer(frames, 0);
                }
                written += frames;
            }
            client->Stop();
        }

        ok = eof;
    }

cleanup:
    if (render)     render->Release();
    if (client)     client->Release();
    if (device)     device->Release();
    if (enumerator) enumerator->Release();
    if (hEvent)     CloseHandle(hEvent);
    CloseHandle(hFile);
    return ok;
}

// WASAPI 再生スレッド関数
//
// STA で COM 初期化し、play_wav_wasapi を実行して終了する。
DWORD WINAPI AlertManager::sound_thread_func(LPVOID param) {
    auto* p  = static_cast<SoundParam*>(param);
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (hr == S_OK || hr == S_FALSE) {
        play_wav_wasapi(p->wav_path, p->shutdown);
        CoUninitialize();
    }
    else {
        log_error("alert: CoInitializeEx STA failed");
    }
    delete p;
    return 0;
}

void AlertManager::init() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    auto wav = fs::path(exe).parent_path() / L"alert.wav";
    wcscpy_s(wav_path_, wav.wstring().c_str());
    wav_avail_ = (GetFileAttributesW(wav_path_) != INVALID_FILE_ATTRIBUTES);
    if (!wav_avail_)
        log_info("alert: alert.wav not found, sound alerts disabled");
}

void AlertManager::shutdown() {
    shutdown_ = true;
    if (sound_thread_) {
        WaitForSingleObject(sound_thread_, 5000);
        CloseHandle(sound_thread_);
        sound_thread_ = nullptr;
    }
}

// バックグラウンドスレッドで alert.wav を再生する
//
// 前の再生スレッドがまだ動いている場合はスキップする（連続再生防止）。
void AlertManager::play() {
    if (sound_thread_) {
        if (WaitForSingleObject(sound_thread_, 0) != WAIT_OBJECT_0)
            return;  // 再生中のためスキップ
        CloseHandle(sound_thread_);
        sound_thread_ = nullptr;
    }
    auto* p = new SoundParam{};
    wcscpy_s(p->wav_path, wav_path_);
    p->shutdown = &shutdown_;
    HANDLE h = CreateThread(nullptr, 0, sound_thread_func, p, 0, nullptr);
    if (h) {
        sound_thread_ = h;
    }
    else {
        log_error("alert: CreateThread failed");
        delete p;
    }
}

// 監視項目 ID に対応する表示ラベルを返す
const wchar_t* AlertManager::label(Id id) {
    switch (id) {
    case CPU:        return L"CPU 使用率";
    case GPU:        return L"GPU 使用率";
    case RAM:        return L"RAM 使用率";
    case VRAM:       return L"VRAM 使用率";
    case DISK_C:     return L"ディスク C: 使用率";
    case DISK_D:     return L"ディスク D: 使用率";
    case TEMP_CPU:   return L"CPU 温度";
    case TEMP_GPU:   return L"GPU 温度";
    case TEMP_NVME_C: return L"NVMe C: 温度";
    case TEMP_NVME_D: return L"NVMe D: 温度";
    case DISK_GBH:   return L"ディスク書き込み量";
    case UPTIME:     return L"OS 稼働時間";
    case CLAUDE_5H:  return L"Claude 5h レートリミット";
    case CLAUDE_7D:  return L"Claude 7d レートリミット";
    case CLAUDE_OVER: return L"Claude 超過料金";
    default:         return L"不明";
    }
}

uint32_t AlertManager::check(const AllMetrics& m, const AppConfig& cfg) {
    uint32_t fired_mask = 0;

    // 通常の閾値チェック：超えたら発火、リセット閾値を下回ったら解除
    auto check_item = [&](Id id, float value, float warn, float reset) {
        if (!fired_[id] && value >= warn) {
            fired_[id] = true;
            fired_mask |= (1u << id);
        }
        else if (fired_[id] && value < reset) {
            fired_[id] = false;
        }
    };

    // ヒステリシスなし：1 回のみ発火（再起動またはセッション終了までリセットしない）
    auto check_once = [&](Id id, float value, float warn) {
        if (!fired_[id] && value > warn) {
            fired_[id] = true;
            fired_mask |= (1u << id);
        }
    };

    check_item(CPU,    m.cpu.total_history.average(AVG_SAMPLES),  cfg.warn_cpu_pct, cfg.reset_cpu_pct);
    if (m.gpu.avail)
        check_item(GPU, m.gpu.usage_history.average(AVG_SAMPLES), cfg.warn_gpu_pct, cfg.reset_gpu_pct);
    check_item(RAM,    m.mem.usage_pct,  cfg.warn_mem_pct, cfg.reset_mem_pct);
    if (m.vram.avail)
        check_item(VRAM, m.vram.usage_pct, cfg.warn_mem_pct, cfg.reset_mem_pct);
    check_item(DISK_C, m.disk_c.used_pct, cfg.warn_mem_pct, cfg.reset_mem_pct);
    check_item(DISK_D, m.disk_d.used_pct, cfg.warn_mem_pct, cfg.reset_mem_pct);
    if (m.cpu.temp_avail)
        check_item(TEMP_CPU, m.cpu.temp_celsius, cfg.warn_temp_critical, cfg.reset_temp);
    if (m.gpu.avail)
        check_item(TEMP_GPU, m.gpu.temp_celsius, cfg.warn_temp_critical, cfg.reset_temp);
    if (m.disk_c.smart_avail)
        check_item(TEMP_NVME_C, m.disk_c.smart_temp_celsius, cfg.warn_temp_critical, cfg.reset_temp);
    if (m.disk_d.smart_avail)
        check_item(TEMP_NVME_D, m.disk_d.smart_temp_celsius, cfg.warn_temp_critical, cfg.reset_temp);
    {
        float gbh = std::max(m.disk_c.smart_write_gbh, m.disk_d.smart_write_gbh);
        check_item(DISK_GBH, gbh, cfg.warn_disk_gbh, cfg.reset_disk_gbh);
    }
    {
        float uptime_days = static_cast<float>(m.os.uptime_ms) / (1000.f * 86400.f);
        check_once(UPTIME, uptime_days, static_cast<float>(cfg.warn_uptime_days));
    }
    if (m.claude.avail) {
        check_item(CLAUDE_5H, m.claude.five_h_pct,  cfg.warn_claude_5h_pct, cfg.reset_claude_5h_pct);
        check_item(CLAUDE_7D, m.claude.seven_d_pct, cfg.warn_claude_7d_pct, cfg.reset_claude_7d_pct);
        if (m.claude.extra_enabled)
            check_once(CLAUDE_OVER, m.claude.extra_used_dollars, cfg.warn_claude_over);
    }

    if (fired_mask && cfg.alert_sound && wav_avail_) play();
    return fired_mask;
}
