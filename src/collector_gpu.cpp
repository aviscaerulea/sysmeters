// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#include "collector_gpu.hpp"
#include "logger.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstring>

// NVML の最小限の型定義（nvml.h 不要）
typedef int nvmlReturn_t;
typedef void* nvmlDevice_t;

struct nvmlUtilization_t {
    unsigned int gpu;    // GPU コア使用率（%）
    unsigned int memory; // VRAM 使用率（%）
};

struct nvmlMemory_t {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
};

// NVML 関数ポインタ型
typedef nvmlReturn_t (*PFN_nvmlInit)();
typedef nvmlReturn_t (*PFN_nvmlShutdown)();
typedef nvmlReturn_t (*PFN_nvmlDeviceGetHandleByIndex)(unsigned int, nvmlDevice_t*);
typedef nvmlReturn_t (*PFN_nvmlDeviceGetUtilizationRates)(nvmlDevice_t, nvmlUtilization_t*);
typedef nvmlReturn_t (*PFN_nvmlDeviceGetTemperature)(nvmlDevice_t, unsigned int, unsigned int*);
typedef nvmlReturn_t (*PFN_nvmlDeviceGetMemoryInfo)(nvmlDevice_t, nvmlMemory_t*);
typedef nvmlReturn_t (*PFN_nvmlDeviceGetName)(nvmlDevice_t, char*, unsigned int);

// NVML_TEMPERATURE_GPU = 0
static constexpr unsigned int NVML_TEMPERATURE_GPU = 0;
// NVML_SUCCESS = 0
static constexpr nvmlReturn_t NVML_SUCCESS = 0;

struct GpuCollector::Impl {
    HMODULE dll = nullptr;
    nvmlDevice_t device = nullptr;
    bool nvml_initialized = false;  // nvmlInit() 成功済みフラグ

    PFN_nvmlInit                       fn_init           = nullptr;
    PFN_nvmlShutdown                   fn_shutdown        = nullptr;
    PFN_nvmlDeviceGetHandleByIndex     fn_get_handle      = nullptr;
    PFN_nvmlDeviceGetUtilizationRates  fn_get_util        = nullptr;
    PFN_nvmlDeviceGetTemperature       fn_get_temp        = nullptr;
    PFN_nvmlDeviceGetMemoryInfo        fn_get_mem         = nullptr;
    PFN_nvmlDeviceGetName              fn_get_name        = nullptr;

    char gpu_name[48] = {};  // NVML 取得 GPU 名
};

bool GpuCollector::init() {
    impl_ = new Impl();

    // nvml.dll を NVIDIA ドライバディレクトリから検索
    impl_->dll = LoadLibraryW(L"nvml.dll");
    if (!impl_->dll) {
        // 別パスも試みる
        impl_->dll = LoadLibraryW(L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
    }
    if (!impl_->dll) {
        log_error("nvml.dll not found");
        shutdown();
        return false;
    }

#define LOAD_FN(field, sym) \
    impl_->field = reinterpret_cast<decltype(impl_->field)>(GetProcAddress(impl_->dll, "nvml" #sym)); \
    if (!impl_->field) { log_error("NVML: failed to load nvml" #sym); shutdown(); return false; }

    LOAD_FN(fn_init,       Init)
    LOAD_FN(fn_shutdown,   Shutdown)
    LOAD_FN(fn_get_handle, DeviceGetHandleByIndex)
    LOAD_FN(fn_get_util,   DeviceGetUtilizationRates)
    LOAD_FN(fn_get_temp,   DeviceGetTemperature)
    LOAD_FN(fn_get_mem,    DeviceGetMemoryInfo)
    LOAD_FN(fn_get_name,   DeviceGetName)
#undef LOAD_FN

    if (impl_->fn_init() != NVML_SUCCESS) {
        log_error("NVML init failed");
        shutdown();
        return false;
    }
    impl_->nvml_initialized = true;

    if (impl_->fn_get_handle(0, &impl_->device) != NVML_SUCCESS) {
        log_error("NVML: device handle not found");
        shutdown();
        return false;
    }

    // GPU 名取得
    char name_buf[96] = {};
    if (impl_->fn_get_name(impl_->device, name_buf, sizeof(name_buf)) == NVML_SUCCESS) {
        strncpy_s(impl_->gpu_name, sizeof(impl_->gpu_name), name_buf, _TRUNCATE);
    }

    log_info("GPU collector initialized: %s", impl_->gpu_name);
    return true;
}

// GPU 使用率・温度のみ更新する（NVML の VRAM クエリをスキップ）
void GpuCollector::update_gpu(GpuMetrics& gpu) {
    if (!impl_ || !impl_->device) {
        gpu.avail = false;
        return;
    }

    nvmlUtilization_t util{};
    if (impl_->fn_get_util(impl_->device, &util) == NVML_SUCCESS) {
        gpu.usage_pct = static_cast<float>(util.gpu);
        gpu.usage_history.push(gpu.usage_pct);
        gpu.avail = true;
    }

    unsigned int temp = 0;
    if (impl_->fn_get_temp(impl_->device, NVML_TEMPERATURE_GPU, &temp) == NVML_SUCCESS) {
        gpu.temp_celsius = static_cast<float>(temp);
    }
}

// GPU 使用率・温度 + VRAM を更新する
// TIMER_FAST の update_gpu() と二重 push しないよう、GPU 使用率は push せず値のみ更新する
void GpuCollector::update_all(GpuMetrics& gpu, VramMetrics& vram) {
    if (!impl_ || !impl_->device) {
        gpu.avail  = false;
        vram.avail = false;
        return;
    }

    // GPU 使用率（値のみ更新、履歴 push は update_gpu に委ねる）
    nvmlUtilization_t util{};
    if (impl_->fn_get_util(impl_->device, &util) == NVML_SUCCESS) {
        gpu.usage_pct = static_cast<float>(util.gpu);
        gpu.avail = true;
    }

    // GPU 温度
    unsigned int temp = 0;
    if (impl_->fn_get_temp(impl_->device, NVML_TEMPERATURE_GPU, &temp) == NVML_SUCCESS) {
        gpu.temp_celsius = static_cast<float>(temp);
    }

    // GPU 名をメトリクスにコピー
    memcpy(gpu.name, impl_->gpu_name, sizeof(gpu.name));

    nvmlMemory_t mem{};
    if (impl_->fn_get_mem(impl_->device, &mem) == NVML_SUCCESS) {
        constexpr float GB = 1024.f * 1024.f * 1024.f;
        vram.total_gb  = static_cast<float>(mem.total) / GB;
        vram.used_gb   = static_cast<float>(mem.used)  / GB;
        vram.usage_pct = (vram.total_gb > 0.f) ? (vram.used_gb / vram.total_gb * 100.f) : 0.f;
        vram.usage_history.push(vram.usage_pct);
        vram.avail = true;
    }
}

void GpuCollector::shutdown() {
    if (!impl_) return;
    if (impl_->nvml_initialized && impl_->fn_shutdown) { impl_->fn_shutdown(); }
    if (impl_->dll) { FreeLibrary(impl_->dll); impl_->dll = nullptr; }
    impl_->device = nullptr;
    delete impl_;
    impl_ = nullptr;
}
