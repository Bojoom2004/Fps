// afx_hook.cpp - внедряемая в игру DLL (x64): перехват IDXGISwapChain::Present / Present1.
//
// Что делает (параметры приходят из лаунчера через общую память "AdrenalinLite_Shared_v1"):
//   * FRTC / Chill  - ограничитель кадров: измеряет время с предыдущего кадра и засыпает
//                     (высокоточный ждущий таймер + короткий спин), если кадр готов раньше цели;
//   * Anti-Lag      - DirectX 11: IDXGIDevice1::SetMaximumFrameLatency(1) + ожидание завершения
//                     предыдущего кадра на GPU (event-запрос), т.е. очередь кадров не больше одного;
//   * Enhanced Sync - SyncInterval = 0 (+ ALLOW_TEARING, если своп-чейн создан с таким флагом);
//   * счётчик FPS   - FPS, время кадра, 1% low пишутся обратно в общую память для оверлея.
//
// Сборка (x64 Native Tools Command Prompt for VS):
//   cl /nologo /std:c++17 /EHsc /O2 /MT /LD afx_hook.cpp /Fe:afx_hook.dll
// Только 64-разрядные игры. Не использовать с играми под античитом.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3d12.h>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <functional>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "winmm.lib")

// ---------------------------------------------------------------- общая память
// Структура должна совпадать с hook_ctl.SharedBlock в Python (96 байт).
struct SharedBlock {
    uint32_t magic;          //  0  'ADLT'
    uint32_t version;        //  4
    uint32_t limiter_on;     //  8  лаунчер -> хук
    float    fps_cap;        // 12
    uint32_t antilag_on;     // 16
    uint32_t esync_on;       // 20
    uint32_t reserved0;      // 24
    uint32_t hooked_pid;     // 28  хук -> лаунчер
    uint32_t heartbeat;      // 32
    uint32_t api;            // 36  11 / 12 / 0
    uint32_t width;          // 40
    uint32_t height;         // 44
    float    fps;            // 48
    float    frametime_ms;   // 52
    float    fps_1pct_low;   // 56
    float    fps_avg;        // 60
    uint32_t reserved1[8];   // 64
};
static_assert(sizeof(SharedBlock) == 96, "SharedBlock layout must match Python");

static const uint32_t kMagic = 0x544C4441;
static const wchar_t* kMapName = L"AdrenalinLite_Shared_v1";

// ---------------------------------------------------------------------- состояние
typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* Present1Fn)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

static HMODULE g_self = nullptr;
static HANDLE g_map = nullptr;
static SharedBlock* g_sh = nullptr;
static PresentFn g_origPresent = nullptr;
static Present1Fn g_origPresent1 = nullptr;
static CRITICAL_SECTION g_cs;
static LARGE_INTEGER g_qpf;
static HANDLE g_timer = nullptr;
static bool g_hiRes = false;

// какие своп-чейны видим; "основной" - самый большой из недавно показывавших кадры
struct SeenSwap {
    IDXGISwapChain* sc;
    uint32_t pixels;
    uint32_t w, h;
    DWORD lastTick;
    DWORD descTick;
    bool tearing;
    uint32_t api;
};
static SeenSwap g_seen[8];
static int g_seenCount = 0;

// статистика кадров
static const int kRing = 600;
static float g_ft[kRing];
static int g_ftCount = 0;
static int g_ftHead = 0;
static LONGLONG g_lastPresent = 0;
static LONGLONG g_lastPublish = 0;
static LONGLONG g_firstFrame = 0;
static uint64_t g_totalFrames = 0;

// ограничитель
static LONGLONG g_nextTarget = 0;

// DX11 Anti-Lag
struct Dx11State {
    IDXGISwapChain* owner = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11Query* query = nullptr;
    bool pending = false;
    bool latencySet = false;
    DWORD acquireTick = 0;
};
static Dx11State g_dx;

static inline LONGLONG Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

// ------------------------------------------------------------------- ожидание
static void InitTimer() {
    // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION = 0x2 (Windows 10 1803+)
    g_timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
    if (g_timer) {
        g_hiRes = true;
    } else {
        g_timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
        g_hiRes = false;
        timeBeginPeriod(1);
    }
}

static void SleepUntil(LONGLONG target) {
    for (;;) {
        LONGLONG remain = target - Now();
        if (remain <= 0) return;
        double ms = (double)remain * 1000.0 / (double)g_qpf.QuadPart;
        if (g_hiRes && g_timer && ms > 2.5) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((ms - 1.5) * 10000.0);   // 100-нс единицы, отрицательное = относительное
            if (SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(g_timer, 200);
            } else {
                Sleep(1);
            }
        } else if (!g_hiRes && ms > 2.5) {
            Sleep(1);
        } else {
            YieldProcessor();   // последние ~1-2 мс - спин для точности
        }
    }
}

static void LimitFrame(float cap) {
    LONGLONG now = Now();
    LONGLONG interval = (LONGLONG)((double)g_qpf.QuadPart / (double)cap);
    if (g_nextTarget == 0 || now - g_nextTarget > g_qpf.QuadPart) g_nextTarget = now;   // старт либо долгий простой
    LONGLONG target = g_nextTarget + interval;
    if (now >= target) {
        // опаздываем: если сильно - не догоняем пачкой кадров
        g_nextTarget = (now - target > 2 * interval) ? now : target;
    } else {
        SleepUntil(target);
        g_nextTarget = target;
    }
}

// ------------------------------------------------------------------ статистика
static void Publish(LONGLONG now) {
    // средний FPS за последнюю секунду
    double sum = 0.0;
    int cnt = 0;
    for (int i = 0; i < g_ftCount; ++i) {
        float v = g_ft[(g_ftHead - 1 - i + kRing) % kRing];
        sum += v;
        ++cnt;
        if (sum >= 1000.0) break;
    }
    if (cnt > 0 && sum > 0.0) {
        double avgMs = sum / cnt;
        g_sh->frametime_ms = (float)avgMs;
        g_sh->fps = (float)(1000.0 / avgMs);
    }
    // 1% low: среднее по худшим 1% кадров из последних 600
    if (g_ftCount >= 20) {
        static float tmp[kRing];
        int n = g_ftCount;
        for (int i = 0; i < n; ++i) tmp[i] = g_ft[(g_ftHead - 1 - i + kRing) % kRing];
        std::sort(tmp, tmp + n, std::greater<float>());
        int k = std::max(1, n / 100);
        double s = 0.0;
        for (int i = 0; i < k; ++i) s += tmp[i];
        double worst = s / k;
        g_sh->fps_1pct_low = worst > 0.0 ? (float)(1000.0 / worst) : 0.0f;
    }
    double elapsed = (double)(now - g_firstFrame) / (double)g_qpf.QuadPart;
    if (elapsed > 0.5) g_sh->fps_avg = (float)((double)g_totalFrames / elapsed);
}

static void RecordFrame(LONGLONG now) {
    if (g_lastPresent != 0) {
        double ms = (double)(now - g_lastPresent) * 1000.0 / (double)g_qpf.QuadPart;
        if (ms > 0.0 && ms < 2000.0) {
            g_ft[g_ftHead] = (float)ms;
            g_ftHead = (g_ftHead + 1) % kRing;
            if (g_ftCount < kRing) ++g_ftCount;
        }
    } else {
        g_firstFrame = now;
    }
    g_lastPresent = now;
    ++g_totalFrames;
    g_sh->heartbeat++;
    if (now - g_lastPublish >= g_qpf.QuadPart / 4) {
        Publish(now);
        g_lastPublish = now;
    }
}

// ------------------------------------------------------------- выбор основного
static bool UpdatePrimary(IDXGISwapChain* sc, uint32_t* outW, uint32_t* outH, bool* outTearing, uint32_t* outApi) {
    DWORD now = GetTickCount();
    int idx = -1;
    for (int i = 0; i < g_seenCount; ++i) {
        if (g_seen[i].sc == sc) { idx = i; break; }
    }
    if (idx < 0) {
        if (g_seenCount < 8) {
            idx = g_seenCount++;
        } else {
            DWORD oldest = 0xFFFFFFFF;
            for (int i = 0; i < 8; ++i) {
                if (g_seen[i].lastTick < oldest) { oldest = g_seen[i].lastTick; idx = i; }
            }
        }
        g_seen[idx] = SeenSwap{ sc, 0, 0, 0, now, 0, false, 0 };
        g_seen[idx].descTick = now - 10000;   // принудительно прочитать описание
    }
    SeenSwap& e = g_seen[idx];
    e.lastTick = now;
    if (now - e.descTick > 2000) {
        DXGI_SWAP_CHAIN_DESC d;
        if (SUCCEEDED(sc->GetDesc(&d))) {
            e.w = d.BufferDesc.Width;
            e.h = d.BufferDesc.Height;
            e.pixels = e.w * e.h;
            e.tearing = (d.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;
        }
        void* p = nullptr;   // какое API: DX11 (устройство) или DX12 (очередь команд); ссылку сразу отпускаем
        if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), &p)) && p) {
            ((IUnknown*)p)->Release();
            e.api = 11;
        } else if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D12CommandQueue), &p)) && p) {
            ((IUnknown*)p)->Release();
            e.api = 12;
        } else {
            e.api = 0;
        }
        e.descTick = now;
    }
    uint32_t best = 0;
    IDXGISwapChain* bestSc = nullptr;
    for (int i = 0; i < g_seenCount; ++i) {
        if (now - g_seen[i].lastTick < 3000 && g_seen[i].pixels >= best) {
            best = g_seen[i].pixels;
            bestSc = g_seen[i].sc;
        }
    }
    *outW = e.w;
    *outH = e.h;
    *outTearing = e.tearing;
    *outApi = e.api;
    return bestSc == sc;
}

// ------------------------------------------------------------------- Anti-Lag
static void Dx11Release() {
    if (g_dx.query) { g_dx.query->Release(); g_dx.query = nullptr; }
    if (g_dx.ctx) { g_dx.ctx->Release(); g_dx.ctx = nullptr; }
    if (g_dx.dev) { g_dx.dev->Release(); g_dx.dev = nullptr; }
    g_dx.pending = false;
    g_dx.latencySet = false;
    g_dx.owner = nullptr;
}

// Ссылки на устройство держим только пока Anti-Lag включён, и обновляем раз в 5 секунд.
static void Dx11Acquire(IDXGISwapChain* sc) {
    Dx11Release();
    g_dx.owner = sc;
    g_dx.acquireTick = GetTickCount();
    void* dev = nullptr;
    if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), &dev)) && dev) {
        g_dx.dev = (ID3D11Device*)dev;
        g_dx.dev->GetImmediateContext(&g_dx.ctx);
        D3D11_QUERY_DESC qd = {};
        qd.Query = D3D11_QUERY_EVENT;
        g_dx.dev->CreateQuery(&qd, &g_dx.query);
    }
}

static void AntiLagBefore(IDXGISwapChain* sc, uint32_t api) {
    if (!g_sh->antilag_on || api != 11) {
        if (g_dx.owner) Dx11Release();
        return;
    }
    if (g_dx.owner != sc || GetTickCount() - g_dx.acquireTick > 5000) Dx11Acquire(sc);
    if (!g_dx.dev || !g_dx.ctx) return;
    if (!g_dx.latencySet) {
        IDXGIDevice1* d1 = nullptr;
        if (SUCCEEDED(g_dx.dev->QueryInterface(__uuidof(IDXGIDevice1), (void**)&d1)) && d1) {
            d1->SetMaximumFrameLatency(1);
            d1->Release();
        }
        g_dx.latencySet = true;
    }
    if (g_dx.pending && g_dx.query) {
        LONGLONG t0 = Now();
        BOOL done = FALSE;
        while (g_dx.ctx->GetData(g_dx.query, &done, sizeof(done), 0) == S_FALSE) {
            if ((Now() - t0) * 1000 > 100 * g_qpf.QuadPart) break;   // не ждём дольше 100 мс
            YieldProcessor();
        }
    }
    g_dx.pending = false;
}

static void AntiLagAfter() {
    if (g_sh->antilag_on && g_dx.ctx && g_dx.query) {
        g_dx.ctx->End(g_dx.query);
        g_dx.pending = true;
    }
}

// ----------------------------------------------------------------- хуки Present
// Возвращает true, если это основной своп-чейн (ему применяем ограничитель и статистику).
static bool OnPresentBegin(IDXGISwapChain* sc, UINT& sync, UINT& flags) {
    if (!g_sh || g_sh->magic != kMagic) return false;
    uint32_t w = 0, h = 0, api = 0;
    bool tearing = false;
    EnterCriticalSection(&g_cs);
    bool primary = UpdatePrimary(sc, &w, &h, &tearing, &api);
    LeaveCriticalSection(&g_cs);
    if (!primary) return false;

    if (g_sh->hooked_pid != GetCurrentProcessId()) {
        g_sh->hooked_pid = GetCurrentProcessId();
        g_lastPresent = 0;
        g_ftCount = 0;
        g_totalFrames = 0;
    }
    g_sh->width = w;
    g_sh->height = h;
    g_sh->api = api;

    AntiLagBefore(sc, api);
    if (g_sh->limiter_on && g_sh->fps_cap >= 1.0f) {
        LimitFrame(g_sh->fps_cap);
    } else {
        g_nextTarget = 0;
    }
    RecordFrame(Now());

    if (g_sh->esync_on) {
        sync = 0;                                  // VSync выключен
        if (tearing) flags |= DXGI_PRESENT_ALLOW_TEARING;
    }
    return true;
}

// Защита от повторного входа: если dxgi.dll внутри Present вызовет Present1 через vtable,
// кадр не должен посчитаться дважды.
static thread_local int t_inPresent = 0;

static HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if ((flags & DXGI_PRESENT_TEST) || t_inPresent) return g_origPresent(sc, sync, flags);
    ++t_inPresent;
    bool primary = OnPresentBegin(sc, sync, flags);
    HRESULT hr = g_origPresent(sc, sync, flags);
    if (primary) AntiLagAfter();
    --t_inPresent;
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                              const DXGI_PRESENT_PARAMETERS* params) {
    if ((flags & DXGI_PRESENT_TEST) || t_inPresent) return g_origPresent1(sc, sync, flags, params);
    ++t_inPresent;
    bool primary = OnPresentBegin(sc, sync, flags);
    HRESULT hr = g_origPresent1(sc, sync, flags, params);
    if (primary) AntiLagAfter();
    --t_inPresent;
    return hr;
}

// --------------------------------------------------------------- установка хуков
static bool PatchSlot(void** vtbl, int index, void* hook) {
    DWORD old = 0;
    if (!VirtualProtect(&vtbl[index], sizeof(void*), PAGE_READWRITE, &old)) return false;
    InterlockedExchangePointer(&vtbl[index], hook);
    VirtualProtect(&vtbl[index], sizeof(void*), old, &old);
    return true;
}

static LRESULT CALLBACK DummyProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

static bool InstallHooks() {
    // Временное окно + устройство + своп-чейн нужны только чтобы получить vtable dxgi.dll
    // (она общая для всех своп-чейнов процесса, так что патч касается и игровых).
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DummyProc;
    wc.hInstance = g_self;
    wc.lpszClassName = L"AFXHookDummyWindow";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                                g_self, nullptr);
    if (!hwnd) return false;

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 64;
    sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* sc = nullptr;
    D3D_FEATURE_LEVEL fl;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 3,
                                               D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 3,
                                           D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    }
    bool ok = false;
    if (SUCCEEDED(hr) && sc) {
        void** vtbl = *reinterpret_cast<void***>(sc);
        g_origPresent = (PresentFn)vtbl[8];
        ok = PatchSlot(vtbl, 8, (void*)&HookPresent);
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1)) && sc1) {
            void** vtbl1 = *reinterpret_cast<void***>(sc1);
            g_origPresent1 = (Present1Fn)vtbl1[22];
            PatchSlot(vtbl1, 22, (void*)&HookPresent1);
            sc1->Release();
        }
    }
    if (sc) sc->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, g_self);
    return ok;
}

static bool OpenShared() {
    g_map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kMapName);
    if (!g_map) return false;
    g_sh = (SharedBlock*)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedBlock));
    return g_sh != nullptr && g_sh->magic == kMagic;
}

static DWORD WINAPI InitThread(LPVOID) {
    QueryPerformanceFrequency(&g_qpf);
    for (int i = 0; i < 100 && !OpenShared(); ++i) Sleep(100);
    if (!g_sh) return 0;
    InitTimer();
    InstallHooks();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_cs);
        HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
