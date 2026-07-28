// vim: set ft=cpp fenc=utf-8 ff=unix sw=4 ts=4 et :
#include "collector_topproc.hpp"
#include "logger.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#pragma comment(lib, "pdh.lib")

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

// ---- NtQuerySystemInformation(SystemProcessInformation) ----
//
// winternl.h の SYSTEM_PROCESS_INFORMATION は SDK バージョンにより KernelTime/UserTime が
// Reserved 配列に隠れるため使わず、必要なフィールドだけを実レイアウト（x64）で自前定義する。
// オフセットは static_assert で固定し、SDK やコンパイラの変化で静かに壊れないようにする。
static_assert(sizeof(void*) == 8, "x64 前提のレイアウト定義（build.ps1 は x64 固定）");

static constexpr ULONG kSystemProcessInformation = 5;
static constexpr LONG  kStatusInfoLengthMismatch = static_cast<LONG>(0xC0000004L);

struct NtUnicodeString {
    USHORT Length;          // バイト長（NUL 終端を含まない）
    USHORT MaximumLength;
    PWSTR  Buffer;          // Idle プロセスでは nullptr
};

struct NtSystemProcessInfo {
    ULONG           NextEntryOffset;  // 0 で列挙終端。次エントリまでのバイト数
    ULONG           NumberOfThreads;
    BYTE            Reserved1[24];    // WorkingSetPrivateSize / HardFaultCount / 高水位 / CycleTime
    LARGE_INTEGER   CreateTime;
    LARGE_INTEGER   UserTime;         // 累積ユーザ時間（100ns 単位）
    LARGE_INTEGER   KernelTime;       // 累積カーネル時間（100ns 単位）
    NtUnicodeString ImageName;        // exe 名のみ（パスなし）
    LONG            BasePriority;
    BYTE            Reserved2[4];     // x64 のアライメントパディング
    HANDLE          UniqueProcessId;  // PID
};
static_assert(offsetof(NtSystemProcessInfo, CreateTime)      == 0x20, "layout");
static_assert(offsetof(NtSystemProcessInfo, UserTime)        == 0x28, "layout");
static_assert(offsetof(NtSystemProcessInfo, KernelTime)      == 0x30, "layout");
static_assert(offsetof(NtSystemProcessInfo, ImageName)       == 0x38, "layout");
static_assert(offsetof(NtSystemProcessInfo, UniqueProcessId) == 0x50, "layout");

using NtQuerySystemInformationFn = LONG (NTAPI*)(ULONG, PVOID, ULONG, PULONG);

// ntdll!NtQuerySystemInformation を 1 度だけ解決する
//
// 公式には未文書だが Windows NT 以降すべてでシグネチャが安定している。
// ntdll は全プロセスに必ずロード済みのため GetModuleHandleW で足りる。
// 解決失敗時は nullptr を返し、CPU 側トップは恒久的に非表示となる。
static NtQuerySystemInformationFn get_nt_query_sysinfo() {
    static const NtQuerySystemInformationFn fn = []() -> NtQuerySystemInformationFn {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) return nullptr;
        return reinterpret_cast<NtQuerySystemInformationFn>(
            GetProcAddress(ntdll, "NtQuerySystemInformation"));
    }();
    return fn;
}

// ---- 定数 ----

// GPU 収集の間引き係数（TIMER_CPU 0.9 秒 × 3 ≒ 2.7 秒間隔）
// PDH のレート型カウンタは直前の PdhCollectQueryData との差分で値を出すため、
// この間隔がそのまま平均窓になる。毎 tick 収集すると窓が短すぎて値が暴れるうえ負荷も増す。
static constexpr int GPU_TICK_DIV = 3;

// GPU エンジン別使用率のワイルドカードカウンタ
// インスタンス名は "pid_1234_luid_0x00000000_0x0000ABCD_phys_0_eng_1_engtype_3D" 形式。
// タスクマネージャの GPU 列と同一ソースで、NVIDIA 以外の GPU でも取得できる。
static constexpr wchar_t GPU_ENGINE_COUNTER[] = L"\\GPU Engine(*)\\Utilization Percentage";

// 表示下限（%）。丸めて 0% になる値は「トップ」として意味がないため非表示にする
static constexpr float MIN_SHOW_PCT = 0.5f;

// スナップショット間隔の上限（ms）。これを超えた差分はスリープ復帰等で信頼できず、基準を取り直す
static constexpr ULONGLONG MAX_ELAPSED_MS = 10000;

// ---- 内部状態 ----

// 直近スナップショットの 1 プロセス分
struct ProcEntry {
    std::wstring name;       // ".exe" を除いた exe 名
    ULONGLONG    cpu_100ns;  // KernelTime + UserTime の累積
};

struct TopProcCollector::Impl {
    // NtQuerySystemInformation の出力バッファ（tick 間で再利用し毎秒の再確保を避ける）
    // 初期 512KB はプロセス 300〜500 台で概ね収まるサイズ
    std::vector<BYTE> buf = std::vector<BYTE>(512 * 1024);

    // PID → 直近スナップショット。
    // 「CPU 時間差分の基準」と「GPU 側の PID → exe 名解決表」を 1 本で兼ねる。
    // cur は毎 tick 作り直し、末尾で prev と swap して再確保を避ける。
    std::unordered_map<DWORD, ProcEntry> prev, cur;

    ULONGLONG prev_tick_ms = 0;  // 直近スナップショット時刻（GetTickCount64。単調で NTP 巻き戻りなし。0 = 未取得）
    DWORD     core_count   = 1;  // 論理コア数（全プロセッサグループ合計。% の分母）

    // GPU Engine ワイルドカードカウンタ。
    // GPU 非搭載・カウンタ不在環境では query を nullptr のままにし、以後 GPU 側トップは
    // 恒久的に非表示になる。再試行はしない（環境要因は起動中に変わらない）。
    PDH_HQUERY        gpu_query   = nullptr;
    PDH_HCOUNTER      gpu_counter = nullptr;
    std::vector<BYTE> gpu_buf;      // PdhGetFormattedCounterArrayW の出力（tick 間で再利用）
    int  gpu_tick   = 0;            // TIMER_CPU tick カウンタ（GPU_TICK_DIV で 1 周）
    bool gpu_primed = false;        // 2 サンプル目に到達したか（レート型カウンタの初回無効値対策）

    // 直前に選出したトッププロセス名。（空 = 直前選出なし。表示閾値による非表示中も選出は続く）
    // 四捨五入した整数 % が同値の候補が並ぶ間は直前の選出を優先し、同率トップで
    // 名前だけが入れ替わり続けるのを防ぐための状態。
    // 候補なし、表示下限未満で選出を見送った tick、および差分基準の取り直し
    // （スリープ復帰等）で消去する。一時的な取得失敗の tick では保持する。
    std::wstring last_cpu_top;
    std::wstring last_gpu_top;

    // Impl は private ネスト型のためファイルスコープの静的関数からは参照できず、
    // 収集ヘルパーは Impl 自身のメンバ関数として持つ（定義は本ファイル後方）
    bool take_snapshot();
    void pick_cpu_top(ULONGLONG elapsed_ms, CpuMetrics& out);
    void pick_gpu_top(GpuMetrics& out);
};

// ---- 内部関数 ----

// 末尾の ".exe" を大文字小文字無視で取り除く
static void strip_exe_suffix(std::wstring& name) {
    if (name.size() > 4 && _wcsnicmp(name.c_str() + name.size() - 4, L".exe", 4) == 0) {
        name.resize(name.size() - 4);
    }
}

// プロセス一覧を取得して cur を作る
//
// 成功時 true。バッファ不足は ReturnLength + 余裕分に拡張して再試行する。
// リトライ上限に達した場合は false を返し、その tick はスキップする（差分基準は更新しない）。
// Idle（PID 0、ImageName なし）は「使用率」の意味を持たないため除外する。
bool TopProcCollector::Impl::take_snapshot() {
    const auto fn = get_nt_query_sysinfo();
    if (!fn) return false;

    ULONG needed = 0;
    LONG  st     = 0;
    for (int attempt = 0; attempt < 4; ++attempt) {
        st = fn(kSystemProcessInformation, buf.data(),
                static_cast<ULONG>(buf.size()), &needed);
        if (st >= 0) break;
        if (st != kStatusInfoLengthMismatch) return false;
        // 取得中のプロセス増加に備えて 64KB の余裕を足す
        buf.resize(static_cast<size_t>(needed) + 64 * 1024);
    }
    if (st < 0) return false;

    cur.clear();
    const BYTE* p = buf.data();
    for (;;) {
        const auto* e   = reinterpret_cast<const NtSystemProcessInfo*>(p);
        const DWORD pid = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(e->UniqueProcessId));
        if (pid != 0 && e->ImageName.Buffer && e->ImageName.Length > 0) {
            // ImageName.Buffer は buf 内を指すため、buf 再利用前にこの場でコピーする
            std::wstring name(e->ImageName.Buffer, e->ImageName.Length / sizeof(wchar_t));
            strip_exe_suffix(name);
            cur[pid] = ProcEntry{
                std::move(name),
                static_cast<ULONGLONG>(e->KernelTime.QuadPart + e->UserTime.QuadPart) };
        }
        if (e->NextEntryOffset == 0) break;
        p += e->NextEntryOffset;
    }
    return true;
}

// 同名 exe を合算して CPU トップ 1 件を out に書く
//
// 分母は「経過実時間 × 論理コア数」。システム全体を 100% とする尺度になり、
// 面グラフ左端の大パーセンテージと一致する。
// 同名合算はパスを見ない exe 名一致で行う（chrome の多プロセスをまとめるのが目的）。
// MIN_SHOW_PCT 未満のときは out に書かず、name は空のまま = 非表示となる。
// 四捨五入した整数 % が直前選出（last_cpu_top）と同値の間は直前選出を優先して表示を安定させる。
// このため生値の厳密な最大でない候補を書くことがある。
// 選出結果は last_cpu_top に記録する。（副作用。このため const メンバではない）
void TopProcCollector::Impl::pick_cpu_top(ULONGLONG elapsed_ms, CpuMetrics& out) {
    std::unordered_map<std::wstring, ULONGLONG> by_name;
    for (const auto& [pid, e] : cur) {
        auto it = prev.find(pid);
        if (it == prev.end()) continue;                    // 新規プロセスは次 tick から計上
        if (e.cpu_100ns <= it->second.cpu_100ns) continue; // PID 再利用等での累積逆行を無視
        by_name[e.name] += e.cpu_100ns - it->second.cpu_100ns;
    }
    if (by_name.empty()) {
        last_cpu_top.clear();
        return;
    }

    auto top = std::max_element(by_name.begin(), by_name.end(),
        [](const auto& a, const auto& b) { return a.second < b.second; });

    // elapsed_ms(ms) × 10000 = 100ns 単位の経過時間
    const double denom = static_cast<double>(elapsed_ms) * 10000.0 * core_count;
    const auto pct_of = [&](ULONGLONG v) {
        return std::clamp(static_cast<float>(static_cast<double>(v) / denom * 100.0), 0.f, 100.f);
    };
    float pct = pct_of(top->second);

    // 表示の安定化：四捨五入した整数 % が直前選出プロセスと同値の間は直前を維持する。
    // 同率トップで名前だけが毎 tick 入れ替わるのを防ぐ。（unordered_map の走査順は
    // 不定のため、完全同値が並ぶと max_element の選択自体も tick ごとに揺れる）
    if (!last_cpu_top.empty() && last_cpu_top != top->first) {
        const auto last = by_name.find(last_cpu_top);
        if (last != by_name.end()) {
            const float last_pct = pct_of(last->second);
            if (std::lroundf(last_pct) == std::lroundf(pct)) {
                top = last;
                pct = last_pct;
            }
        }
    }
    if (pct < MIN_SHOW_PCT) {
        last_cpu_top.clear();
        return;
    }

    last_cpu_top = top->first;
    wcsncpy_s(out.top_proc_name, top->first.c_str(), _TRUNCATE);
    out.top_proc_pct = pct;
}

// インスタンス名 "pid_1234_luid_..." から PID を取り出す。書式が違えば 0 を返す
static DWORD parse_engine_pid(const wchar_t* inst) {
    if (!inst || wcsncmp(inst, L"pid_", 4) != 0) return 0;
    return static_cast<DWORD>(wcstoul(inst + 4, nullptr, 10));
}

// インスタンス名の "engtype_" 以降（最初の一致箇所のエンジン種別名）を返す。
// nullptr または "engtype_" 不在なら空文字を返す
static const wchar_t* parse_engine_type(const wchar_t* inst) {
    if (!inst) return L"";
    const wchar_t* t = wcsstr(inst, L"engtype_");
    return t ? t + 8 : L"";  // 8 = "engtype_" の文字数
}

// 大文字小文字を無視した部分一致判定
static bool contains_i(const wchar_t* s, const wchar_t* sub) {
    const size_t n = wcslen(sub);
    for (; *s; ++s) {
        if (_wcsnicmp(s, sub, n) == 0) return true;
    }
    return false;
}

// 大パーセンテージ（NVML の 3D/Compute 稼働率）が数える系統のエンジン種別か判定する
// 種別名はドライバ世代で揺れるため（Compute/Cuda 等）、部分一致で寛容に判定する
static bool is_gpu_core_engine(const wchar_t* engtype) {
    return contains_i(engtype, L"3D") || contains_i(engtype, L"Compute") ||
           contains_i(engtype, L"Cuda");
}

// exe 名に解決 → 3D/Compute 系エンジンに限定 → 同名 exe × アダプタ × 種別内で合算
// → バケット間は最大値 → トップ 1 件
//
// PdhGetFormattedCounterArrayW は 1 回目を nullptr 呼び出しで必要サイズを得る 2 段構え。
// ワイルドカードインスタンスは PdhCollectQueryData のたびに再列挙されるため、
// 起動後に現れたプロセスも追加登録なしで拾える。
// 対象を 3D/Compute 系に限定するのは、大パーセンテージが数えない Decode/Copy 等の
// 値が最大に選ばれると超過表示（大 % より高いトップ %）が再発するため。
// アダプタ識別部（luid〜phys）をキーに含めるのは、iGPU + dGPU 構成で別アダプタの
// 負荷が NVML 対象 GPU のグラフに上乗せされるのを防ぐため。
// 既知の制約：第 2 段の最大値はアダプタも跨いで選ぶため、別アダプタ上で 3D を回す
// プロセスがトップに選ばれ得る。（NVML 対象アダプタへの限定は NVML の PCI 情報と
// DXGI 列挙の LUID 突き合わせが必要になり、コストに見合わないため実装しない）
// バケットを跨いで加算しない方針はタスクマネージャの GPU 列と同じ。（同一バケット内の
// 合算はタスクマネージャより大きい値になり得る点が異なる）
// 同種エンジンが複数あるアダプタでは合算が 100% を超え得るためクランプは維持する。
// 四捨五入した整数 % が直前選出（last_gpu_top）と同値の間は直前選出を優先して表示を安定させる。
// このため生値の厳密な最大でない候補を書くことがある。
void TopProcCollector::Impl::pick_gpu_top(GpuMetrics& out) {
    DWORD size = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(gpu_counter, PDH_FMT_DOUBLE, &size, &count, nullptr)
            != PDH_MORE_DATA) {
        return;
    }
    gpu_buf.resize(size);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(gpu_buf.data());
    if (PdhGetFormattedCounterArrayW(gpu_counter, PDH_FMT_DOUBLE, &size, &count, items)
            != ERROR_SUCCESS) {
        return;
    }

    // 第 1 段：exe 名 × アダプタ × エンジン種別のバケットで合算する。
    // キーは「名前 + 単位分離子（U+001F） + アダプタ識別部 + 単位分離子 + 種別名」の連結。
    // （Win32 のファイル名に制御文字は使えないため実用上衝突しない）
    std::unordered_map<std::wstring, double> by_bucket;
    for (DWORD i = 0; i < count; ++i) {
        // PDH_CSTATUS_VALID_DATA(0) / PDH_CSTATUS_NEW_DATA(1) 以外は無効値
        if (items[i].FmtValue.CStatus > 1) continue;
        const DWORD pid = parse_engine_pid(items[i].szName);
        if (pid == 0) continue;
        const wchar_t* engtype = parse_engine_type(items[i].szName);
        if (!is_gpu_core_engine(engtype)) continue;
        // PID → exe 名は CPU 側の直近スナップショットを流用する。
        // 直後に終了したプロセス等、スナップショットにない PID は捨てる
        auto it = prev.find(pid);
        if (it == prev.end()) continue;
        // アダプタ識別部は "luid_..." から "_eng_" 手前まで（"luid_0x..._0x..._phys_N"）。
        // 抽出できない書式のインスタンスは捨てる。（空のアダプタ部で全アダプタが
        // 同一バケットへ静かに統合される劣化を防ぐため）
        const wchar_t* luid = wcsstr(items[i].szName, L"luid_");
        const wchar_t* eng  = luid ? wcsstr(luid, L"_eng_") : nullptr;
        if (!luid || !eng) continue;
        std::wstring key = it->second.name;
        key += L'\x1F';
        key.append(luid, static_cast<size_t>(eng - luid));
        key += L'\x1F';
        key += engtype;
        by_bucket[key] += items[i].FmtValue.doubleValue;
    }

    // 第 2 段：名前ごとにバケット（アダプタ × 種別）最大を採る
    std::unordered_map<std::wstring, double> by_name;
    for (const auto& [key, v] : by_bucket) {
        std::wstring name = key.substr(0, key.find(L'\x1F'));
        double& slot = by_name[name];
        if (v > slot) slot = v;
    }
    if (by_name.empty()) {
        last_gpu_top.clear();
        return;
    }

    auto top = std::max_element(by_name.begin(), by_name.end(),
        [](const auto& a, const auto& b) { return a.second < b.second; });

    float pct = std::clamp(static_cast<float>(top->second), 0.f, 100.f);

    // 表示の安定化（pick_cpu_top と同旨）：四捨五入した整数 % が同値の間は直前選出を維持する
    if (!last_gpu_top.empty() && last_gpu_top != top->first) {
        const auto last = by_name.find(last_gpu_top);
        if (last != by_name.end()) {
            const float last_pct = std::clamp(static_cast<float>(last->second), 0.f, 100.f);
            if (std::lroundf(last_pct) == std::lroundf(pct)) {
                top = last;
                pct = last_pct;
            }
        }
    }
    if (pct < MIN_SHOW_PCT) {
        last_gpu_top.clear();
        return;
    }

    last_gpu_top = top->first;
    wcsncpy_s(out.top_proc_name, top->first.c_str(), _TRUNCATE);
    out.top_proc_pct = pct;
}

// ---- 公開 API ----

void TopProcCollector::ensure_impl() {
    impl_ = new Impl();
    // 全プロセッサグループ合計。CpuMetrics::core_count に依存させないのは、
    // このコレクタが CpuCollector の初期化順序と無関係に単体で正しく動くようにするため
    impl_->core_count = max(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), 1u);

    if (PdhOpenQuery(nullptr, 0, &impl_->gpu_query) != ERROR_SUCCESS) {
        impl_->gpu_query = nullptr;
        log_error("TopProc: PdhOpenQuery failed");
        return;
    }
    if (PdhAddEnglishCounterW(impl_->gpu_query, GPU_ENGINE_COUNTER, 0, &impl_->gpu_counter)
            != ERROR_SUCCESS) {
        // GPU 非搭載機や GPU Engine カウンタ未提供環境。GPU 側だけ静かに諦める
        PdhCloseQuery(impl_->gpu_query);
        impl_->gpu_query   = nullptr;
        impl_->gpu_counter = nullptr;
        log_info("TopProc: GPU Engine counter unavailable");
    }
}

void TopProcCollector::update(CpuMetrics& cpu, GpuMetrics& gpu, bool enabled) {
    if (!enabled) {
        // OFF：表示を即座に消し、スナップショット・PDH クエリを解放して負荷ゼロにする。
        // 再度 ON になれば ensure_impl() が再構築し、差分基準の取得からやり直す
        cpu.top_proc_name[0] = L'\0';
        gpu.top_proc_name[0] = L'\0';
        shutdown();
        return;
    }
    if (!impl_) ensure_impl();

    // --- CPU（毎 tick）---
    cpu.top_proc_name[0] = L'\0';
    if (impl_->take_snapshot()) {
        const ULONGLONG now        = GetTickCount64();
        const ULONGLONG elapsed_ms = now - impl_->prev_tick_ms;
        // 初回と長時間停止明け（スリープ復帰等）は差分が信頼できないため基準の取り直しに留める
        if (impl_->prev_tick_ms != 0 && elapsed_ms > 0 && elapsed_ms <= MAX_ELAPSED_MS) {
            impl_->pick_cpu_top(elapsed_ms, cpu);
        }
        else {
            // 基準の取り直し（初回・スリープ復帰等）では選出の連続性も切れているため、
            // 同率優先の対象となる直前選出名を消去して白紙から選び直す
            impl_->last_cpu_top.clear();
            impl_->last_gpu_top.clear();
        }
        impl_->prev.swap(impl_->cur);
        impl_->prev_tick_ms = now;
    }

    // --- GPU（GPU_TICK_DIV tick に 1 回。未到達 tick では前回値を保持）---
    if (++impl_->gpu_tick < GPU_TICK_DIV) return;
    impl_->gpu_tick = 0;

    gpu.top_proc_name[0] = L'\0';
    // NVML 不可（GPU セクションが N/A 表示）の間は面グラフ自体が描かれないため収集しない
    if (!gpu.avail || !impl_->gpu_query) return;
    if (PdhCollectQueryData(impl_->gpu_query) != ERROR_SUCCESS) return;
    // レート型カウンタは 2 サンプル目から有効値になる（1 サンプル目は差分の基準）
    if (!impl_->gpu_primed) {
        impl_->gpu_primed = true;
        return;
    }
    impl_->pick_gpu_top(gpu);
}

void TopProcCollector::shutdown() {
    if (!impl_) return;
    if (impl_->gpu_query) PdhCloseQuery(impl_->gpu_query);
    delete impl_;
    impl_ = nullptr;
}
