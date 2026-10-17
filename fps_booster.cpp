// fps_booster.cpp - AdrenalinLite: генерация кадров (Windows Graphics Capture + Direct3D 11),
// повышение резкости и нагрузочный тест видеокарты.
//
// Режимы запуска:
//   fps_booster.exe --pid <PID> | --hwnd <HWND> | --title <часть заголовка>
//                   [--fg 0|1] [--sharpen 0..1] [--max-src-fps 45] [--luma-div 4|8]
//                   [--present blit|flip] [--adapter-name <часть имени адаптера>]
//   fps_booster.exe --stress <секунды> [--adapter-name 540]
//   fps_booster.exe --list-adapters
//
// Протокол stdout (одна строка - одно сообщение):
//   STATUS <текст>                         waiting_window / capturing w=.. h=.. / error .. / closed
//   STAT src=29.8 out=59.6 fg=1 cpu_ms=2.1 w=1366 h=768
//   STRESS ...  и  STRESS_RESULT OK|FAIL ...   (коды выхода: 0 ok, 2 сбой устройства, 3 ошибка данных, 4 инициализация)
// Команда "quit" в stdin завершает работу.
//
// Как работает генерация кадров:
//   1. WGC присылает каждый новый кадр окна игры -> копия в текстуру curr;
//   2. кадр уменьшается до яркости (1/4 или 1/8), блочный поиск движения (compute shader):
//      для каждого блока 8x8 перебираются смещения +-8 пикселей, выбирается минимум SAD;
//   3. промежуточный кадр: для пикселя берутся векторы 4 соседних блоков, для каждого строится
//      prev(p + d/2) и curr(p - d/2), выбирается вариант с минимальной разностью, результат смешивается 50/50;
//      там, где движение не найдено (разность велика), плавно подмешивается обычное смешивание кадров;
//   4. на экран выводится [промежуточный кадр, реальный кадр] - из 30 FPS получается 60.
// Вывод - прозрачное для кликов окно поверх игры (WS_EX_TOPMOST | LAYERED | TRANSPARENT | NOACTIVATE).
// Работает с играми в оконном режиме и режиме "окно без границ".
//
// Сборка: build_native.bat (MSVC, C++17, C++/WinRT, Windows SDK 10.0.17763+).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <unknwn.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <dwmapi.h>
#include <inspectable.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "user32.lib")

namespace wf = winrt::Windows::Foundation;
namespace wg = winrt::Windows::Graphics;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;
namespace wgd11 = winrt::Windows::Graphics::DirectX::Direct3D11;

// Доступ к ID3D11Texture2D из WinRT-поверхности (интерфейс из Win32CaptureSample).
struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1")) IDxgiInterfaceAccess : ::IUnknown {
    virtual HRESULT __stdcall GetInterface(REFIID iid, void** object) = 0;
};

// ==================================================================== общее
static std::atomic<bool> g_quit{false};
static LARGE_INTEGER g_qpf;

static LONGLONG NowQpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static bool ContainsNoCase(const std::wstring& hay, const std::wstring& needle) {
    if (needle.empty()) return true;
    std::wstring h = hay, n = needle;
    std::transform(h.begin(), h.end(), h.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });
    std::transform(n.begin(), n.end(), n.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });
    return h.find(n) != std::wstring::npos;
}

struct Options {
    DWORD pid = 0;
    HWND hwnd = nullptr;
    std::wstring title;
    bool fg = true;
    float sharpen = 0.0f;
    int maxSrcFps = 45;
    int lumaDiv = 4;
    bool flip = false;
    std::wstring adapterName;
    int stressSeconds = 0;
    bool listAdapters = false;
};

static Options ParseArgs(int argc, wchar_t** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return (i + 1 < argc) ? std::wstring(argv[++i]) : std::wstring(); };
        if (a == L"--pid") o.pid = (DWORD)wcstoul(next().c_str(), nullptr, 10);
        else if (a == L"--hwnd") o.hwnd = (HWND)(uintptr_t)wcstoull(next().c_str(), nullptr, 10);
        else if (a == L"--title") o.title = next();
        else if (a == L"--fg") o.fg = (next() != L"0");
        else if (a == L"--sharpen") o.sharpen = std::max(0.0f, std::min(1.0f, (float)_wtof(next().c_str())));
        else if (a == L"--max-src-fps") o.maxSrcFps = std::max(10, _wtoi(next().c_str()));
        else if (a == L"--luma-div") o.lumaDiv = (_wtoi(next().c_str()) >= 8) ? 8 : 4;
        else if (a == L"--present") o.flip = (next() == L"flip");
        else if (a == L"--adapter-name") o.adapterName = next();
        else if (a == L"--stress") o.stressSeconds = std::max(5, _wtoi(next().c_str()));
        else if (a == L"--list-adapters") o.listAdapters = true;
    }
    return o;
}

// ================================================================= адаптеры
static winrt::com_ptr<IDXGIAdapter1> PickAdapter(const std::wstring& nameSub, bool printList) {
    winrt::com_ptr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void()))) return nullptr;
    winrt::com_ptr<IDXGIAdapter1> found;
    for (UINT i = 0;; ++i) {
        winrt::com_ptr<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(i, a.put()) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d;
        a->GetDesc1(&d);
        if (printList) {
            printf("ADAPTER %u name=%ls vendor=%04X device=%04X vram_mb=%llu luid=%08X_%08X%s\n", i, d.Description,
                   d.VendorId, d.DeviceId, (unsigned long long)(d.DedicatedVideoMemory / (1024 * 1024)),
                   (unsigned)d.AdapterLuid.HighPart, (unsigned)d.AdapterLuid.LowPart,
                   (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " software" : "");
        }
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (!found && (nameSub.empty() || ContainsNoCase(d.Description, nameSub))) found = a;
    }
    return found;
}

static bool CreateDevice(const winrt::com_ptr<IDXGIAdapter1>& adapter, winrt::com_ptr<ID3D11Device>& dev,
                         winrt::com_ptr<ID3D11DeviceContext>& ctx) {
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    IDXGIAdapter* pa = adapter ? static_cast<IDXGIAdapter*>(adapter.get()) : nullptr;
    D3D_DRIVER_TYPE dt = pa ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
    HRESULT hr = D3D11CreateDevice(pa, dt, nullptr, flags, levels, 2, D3D11_SDK_VERSION, dev.put(), &got, ctx.put());
    if (hr == E_INVALIDARG) {
        hr = D3D11CreateDevice(pa, dt, nullptr, flags, levels + 1, 1, D3D11_SDK_VERSION, dev.put(), &got, ctx.put());
    }
    return SUCCEEDED(hr);
}

static winrt::com_ptr<ID3DBlob> Compile(const char* src, const char* entry, const char* profile) {
    winrt::com_ptr<ID3DBlob> code, err;
    HRESULT hr = D3DCompile(src, strlen(src), "afx", nullptr, nullptr, entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3,
                            0, code.put(), err.put());
    if (FAILED(hr)) {
        if (err) printf("STATUS error shader %s: %s\n", entry, (const char*)err->GetBufferPointer());
        return nullptr;
    }
    return code;
}

// ============================================================ шейдеры (HLSL)
static const char* kShader = R"HLSL(
cbuffer CB : register(b0)
{
    float2 srcSize;
    float2 invSrcSize;
    int2   lumaSize;
    int2   blockGrid;
    float  sharpen;
    float  lumaScale;
    float  lambdaPen;
    float  fbLo;
    float  fbHi;
    float3 padding;
};

Texture2D<float4>        texCurr  : register(t0);
Texture2D<float4>        texPrev  : register(t1);
Texture2D<float>         lumaCurr : register(t2);
Texture2D<float>         lumaPrev : register(t3);
StructuredBuffer<int2>   mvSrv    : register(t4);
Texture2D<float4>        texSrc   : register(t5);
RWStructuredBuffer<int2> mvUav    : register(u0);
SamplerState             linSamp  : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

// Один большой треугольник на весь экран, вершинный буфер не нужен.
VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.uv = p;
    o.pos = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// Яркость в уменьшенном размере (4 билинейных выборки = усреднение блока 4x4).
float PSLuma(VSOut i) : SV_Target
{
    float3 c = float3(0, 0, 0);
    c += texCurr.SampleLevel(linSamp, i.uv + float2(-1, -1) * invSrcSize, 0).rgb;
    c += texCurr.SampleLevel(linSamp, i.uv + float2( 1, -1) * invSrcSize, 0).rgb;
    c += texCurr.SampleLevel(linSamp, i.uv + float2(-1,  1) * invSrcSize, 0).rgb;
    c += texCurr.SampleLevel(linSamp, i.uv + float2( 1,  1) * invSrcSize, 0).rgb;
    c *= 0.25;
    return dot(c, float3(0.299, 0.587, 0.114));
}

// Блочный поиск движения. Одна группа = один блок 8x8; 17x17 потоков = 289 кандидатов смещения.
// Вектор d: curr(p) ~ prev(p + d). Лучший кандидат выбирается атомарным минимумом SAD.
#define BLOCK 8
#define RAD   8
#define SPAN  17
groupshared uint gBest;

[numthreads(SPAN, SPAN, 1)]
void CSMotion(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
    int2 cand = int2(tid.xy) - int2(RAD, RAD);
    int2 org  = int2(gid.xy) * BLOCK;
    int2 hi   = lumaSize - int2(1, 1);
    if (tid.x == 0 && tid.y == 0) gBest = 0xFFFFFFFFu;
    GroupMemoryBarrierWithGroupSync();

    uint sad = 0;
    [loop]
    for (int y = 0; y < BLOCK; y++)
    {
        [loop]
        for (int x = 0; x < BLOCK; x++)
        {
            int2 p = clamp(org + int2(x, y), int2(0, 0), hi);
            int2 q = clamp(org + int2(x, y) + cand, int2(0, 0), hi);
            float a = lumaCurr.Load(int3(p, 0));
            float b = lumaPrev.Load(int3(q, 0));
            sad += (uint)(abs(a - b) * 255.0f + 0.5f);
        }
    }
    sad += (uint)((abs(cand.x) + abs(cand.y)) * lambdaPen);   // предпочитаем малые смещения
    uint idx = tid.y * SPAN + tid.x;
    InterlockedMin(gBest, (sad << 9) | idx);
    GroupMemoryBarrierWithGroupSync();

    if (tid.x == 0 && tid.y == 0)
    {
        uint best = gBest & 511u;
        int2 d = int2((int)(best % SPAN), (int)(best / SPAN)) - int2(RAD, RAD);
        mvUav[gid.y * blockGrid.x + gid.x] = d;
    }
}

// Кандидат промежуточного кадра для вектора d: prev(p + d/2) и curr(p - d/2), смешанные 50/50.
float3 Cand(float2 uv, float2 d, out float err)
{
    float2 off = d * lumaScale * 0.5f * invSrcSize;
    float3 a = texPrev.SampleLevel(linSamp, uv + off, 0).rgb;
    float3 b = texCurr.SampleLevel(linSamp, uv - off, 0).rgb;
    float3 df = abs(a - b);
    err = df.r + df.g + df.b;
    return 0.5f * (a + b);
}

float4 PSInterp(VSOut i) : SV_Target
{
    float2 uv = i.uv;
    float2 bp = uv * float2(lumaSize) / 8.0f - 0.5f;
    int2 b0 = (int2)floor(bp);
    int2 b1 = b0 + int2(1, 1);
    int2 hiB = blockGrid - int2(1, 1);
    b0 = clamp(b0, int2(0, 0), hiB);
    b1 = clamp(b1, int2(0, 0), hiB);
    float2 m00 = (float2)mvSrv[b0.y * blockGrid.x + b0.x];
    float2 m10 = (float2)mvSrv[b0.y * blockGrid.x + b1.x];
    float2 m01 = (float2)mvSrv[b1.y * blockGrid.x + b0.x];
    float2 m11 = (float2)mvSrv[b1.y * blockGrid.x + b1.x];

    float e;
    float bestErr;
    float3 best = Cand(uv, m00, bestErr);
    float3 c = Cand(uv, m10, e);
    if (e < bestErr) { bestErr = e; best = c; }
    c = Cand(uv, m01, e);
    if (e < bestErr) { bestErr = e; best = c; }
    c = Cand(uv, m11, e);
    if (e < bestErr) { bestErr = e; best = c; }

    // там, где движение не нашлось, подмешиваем обычное 50% смешивание кадров
    float3 plain = 0.5f * (texPrev.SampleLevel(linSamp, uv, 0).rgb + texCurr.SampleLevel(linSamp, uv, 0).rgb);
    float w = smoothstep(fbLo, fbHi, bestErr);
    return float4(lerp(best, plain, w), 1.0f);
}

// Вывод кадра на экран + адаптивная резкость (контрастно-адаптивный фильтр по кресту 3x3).
float4 PSPresent(VSOut i) : SV_Target
{
    float2 uv = i.uv;
    float3 e = texSrc.SampleLevel(linSamp, uv, 0).rgb;
    if (sharpen <= 0.001f) return float4(e, 1.0f);
    float3 a = texSrc.SampleLevel(linSamp, uv + float2( 0, -1) * invSrcSize, 0).rgb;
    float3 b = texSrc.SampleLevel(linSamp, uv + float2(-1,  0) * invSrcSize, 0).rgb;
    float3 c = texSrc.SampleLevel(linSamp, uv + float2( 1,  0) * invSrcSize, 0).rgb;
    float3 d = texSrc.SampleLevel(linSamp, uv + float2( 0,  1) * invSrcSize, 0).rgb;
    float3 mn = min(min(min(a, b), min(c, d)), e);
    float3 mx = max(max(max(a, b), max(c, d)), e);
    float3 amp = saturate(min(mn, 1.0f - mx) / max(mx, 0.0001f));
    amp = sqrt(amp);
    float peak = -1.0f / lerp(8.0f, 5.0f, saturate(sharpen));
    float3 wgt = amp * peak;
    float3 outc = (wgt * (a + b + c + d) + e) / (1.0f + 4.0f * wgt);
    return float4(saturate(outc), 1.0f);
}
)HLSL";

struct alignas(16) CBData {
    float srcW, srcH, invW, invH;
    int lumaW, lumaH, blocksX, blocksY;
    float sharpen, lumaScale, lambda, fbLo;
    float fbHi, pad0, pad1, pad2;
};
static_assert(sizeof(CBData) == 64, "constant buffer layout");

// ============================================================ графический конвейер
static winrt::com_ptr<ID3D11Texture2D> MakeTex(ID3D11Device* dev, int w, int h, DXGI_FORMAT fmt, UINT bind) {
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = (UINT)w;
    d.Height = (UINT)h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bind;
    winrt::com_ptr<ID3D11Texture2D> t;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, t.put()))) return nullptr;
    return t;
}

struct Gfx {
    winrt::com_ptr<ID3D11Device> dev;
    winrt::com_ptr<ID3D11DeviceContext> ctx;
    winrt::com_ptr<ID3D11VertexShader> vs;
    winrt::com_ptr<ID3D11PixelShader> psLuma, psInterp, psPresent;
    winrt::com_ptr<ID3D11ComputeShader> csMotion;
    winrt::com_ptr<ID3D11SamplerState> samp;
    winrt::com_ptr<ID3D11Buffer> cb;

    int lumaDiv = 4;
    float sharpen = 0.0f;
    int w = 0, h = 0, lw = 0, lh = 0, bx = 0, by = 0;
    int cur = 0;

    winrt::com_ptr<ID3D11Texture2D> texNew;
    winrt::com_ptr<ID3D11Texture2D> tex[2];
    winrt::com_ptr<ID3D11ShaderResourceView> srv[2];
    winrt::com_ptr<ID3D11Texture2D> texInterp;
    winrt::com_ptr<ID3D11ShaderResourceView> srvInterp;
    winrt::com_ptr<ID3D11RenderTargetView> rtvInterp;
    winrt::com_ptr<ID3D11Texture2D> luma[2];
    winrt::com_ptr<ID3D11ShaderResourceView> srvLuma[2];
    winrt::com_ptr<ID3D11RenderTargetView> rtvLuma[2];
    winrt::com_ptr<ID3D11Buffer> mvBuf;
    winrt::com_ptr<ID3D11UnorderedAccessView> uavMv;
    winrt::com_ptr<ID3D11ShaderResourceView> srvMv;

    winrt::com_ptr<IDXGISwapChain1> sc;
    winrt::com_ptr<ID3D11RenderTargetView> rtvBack;
    int outW = 0, outH = 0;

    bool Init(const winrt::com_ptr<IDXGIAdapter1>& adapter) {
        if (!CreateDevice(adapter, dev, ctx)) {
            printf("STATUS error cannot create Direct3D 11 device\n");
            return false;
        }
        // команды из потока захвата и из потока рендера идут в один контекст
        if (auto mt = dev.try_as<ID3D11Multithread>()) mt->SetMultithreadProtected(TRUE);
        if (auto d1 = dev.try_as<IDXGIDevice1>()) d1->SetMaximumFrameLatency(1);

        auto bvs = Compile(kShader, "VSMain", "vs_5_0");
        auto bl = Compile(kShader, "PSLuma", "ps_5_0");
        auto bi = Compile(kShader, "PSInterp", "ps_5_0");
        auto bp = Compile(kShader, "PSPresent", "ps_5_0");
        auto bc = Compile(kShader, "CSMotion", "cs_5_0");
        if (!bvs || !bl || !bi || !bp || !bc) return false;
        if (FAILED(dev->CreateVertexShader(bvs->GetBufferPointer(), bvs->GetBufferSize(), nullptr, vs.put()))) return false;
        if (FAILED(dev->CreatePixelShader(bl->GetBufferPointer(), bl->GetBufferSize(), nullptr, psLuma.put()))) return false;
        if (FAILED(dev->CreatePixelShader(bi->GetBufferPointer(), bi->GetBufferSize(), nullptr, psInterp.put()))) return false;
        if (FAILED(dev->CreatePixelShader(bp->GetBufferPointer(), bp->GetBufferSize(), nullptr, psPresent.put()))) return false;
        if (FAILED(dev->CreateComputeShader(bc->GetBufferPointer(), bc->GetBufferSize(), nullptr, csMotion.put()))) return false;

        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(dev->CreateSamplerState(&sd, samp.put()))) return false;

        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = sizeof(CBData);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, cb.put()))) return false;
        return true;
    }

    // Создание ресурсов под размер кадра игры.
    bool Resize(int nw, int nh) {
        w = nw;
        h = nh;
        lw = (w + lumaDiv - 1) / lumaDiv;
        lh = (h + lumaDiv - 1) / lumaDiv;
        bx = (lw + 7) / 8;
        by = (lh + 7) / 8;
        cur = 0;
        texNew = MakeTex(dev.get(), w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
        for (int i = 0; i < 2; ++i) {
            tex[i] = MakeTex(dev.get(), w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
            luma[i] = MakeTex(dev.get(), lw, lh, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
            if (!tex[i] || !luma[i]) return false;
            srv[i] = nullptr;
            srvLuma[i] = nullptr;
            rtvLuma[i] = nullptr;
            if (FAILED(dev->CreateShaderResourceView(tex[i].get(), nullptr, srv[i].put()))) return false;
            if (FAILED(dev->CreateShaderResourceView(luma[i].get(), nullptr, srvLuma[i].put()))) return false;
            if (FAILED(dev->CreateRenderTargetView(luma[i].get(), nullptr, rtvLuma[i].put()))) return false;
        }
        texInterp = MakeTex(dev.get(), w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
        if (!texNew || !texInterp) return false;
        srvInterp = nullptr;
        rtvInterp = nullptr;
        if (FAILED(dev->CreateShaderResourceView(texInterp.get(), nullptr, srvInterp.put()))) return false;
        if (FAILED(dev->CreateRenderTargetView(texInterp.get(), nullptr, rtvInterp.put()))) return false;

        const UINT n = (UINT)(bx * by);
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = n * 8;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = 8;
        mvBuf = nullptr;
        uavMv = nullptr;
        srvMv = nullptr;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, mvBuf.put()))) return false;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = n;
        if (FAILED(dev->CreateUnorderedAccessView(mvBuf.get(), &ud, uavMv.put()))) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC rd = {};
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        rd.Buffer.NumElements = n;
        if (FAILED(dev->CreateShaderResourceView(mvBuf.get(), &rd, srvMv.put()))) return false;

        CBData c = {};
        c.srcW = (float)w;
        c.srcH = (float)h;
        c.invW = 1.0f / w;
        c.invH = 1.0f / h;
        c.lumaW = lw;
        c.lumaH = lh;
        c.blocksX = bx;
        c.blocksY = by;
        c.sharpen = sharpen;
        c.lumaScale = (float)lumaDiv;
        c.lambda = 3.0f;
        c.fbLo = 0.20f;
        c.fbHi = 0.55f;
        ctx->UpdateSubresource(cb.get(), 0, nullptr, &c, 0, 0);
        return true;
    }

    bool CreateSwap(HWND hwnd, int width, int height, bool flip) {
        auto dxgiDev = dev.as<IDXGIDevice>();
        winrt::com_ptr<IDXGIAdapter> adapter;
        if (FAILED(dxgiDev->GetAdapter(adapter.put()))) return false;
        winrt::com_ptr<IDXGIFactory2> f2;
        if (FAILED(adapter->GetParent(__uuidof(IDXGIFactory2), f2.put_void()))) return false;
        for (int attempt = 0; attempt < 2; ++attempt) {
            bool useFlip = (attempt == 0) ? flip : false;
            DXGI_SWAP_CHAIN_DESC1 d = {};
            d.Width = (UINT)width;
            d.Height = (UINT)height;
            d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            d.SampleDesc.Count = 1;
            d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            d.BufferCount = useFlip ? 3 : 2;
            d.SwapEffect = useFlip ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_DISCARD;
            d.Scaling = DXGI_SCALING_STRETCH;
            d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            sc = nullptr;
            if (SUCCEEDED(f2->CreateSwapChainForHwnd(dev.get(), hwnd, &d, nullptr, nullptr, sc.put()))) {
                f2->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
                outW = width;
                outH = height;
                return MakeBackRtv();
            }
            if (!flip) break;
        }
        return false;
    }

    bool MakeBackRtv() {
        rtvBack = nullptr;
        winrt::com_ptr<ID3D11Texture2D> back;
        if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), back.put_void()))) return false;
        return SUCCEEDED(dev->CreateRenderTargetView(back.get(), nullptr, rtvBack.put()));
    }

    void ResizeSwap(int width, int height) {
        if (!sc || width < 16 || height < 16) return;
        if (width == outW && height == outH) return;
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        rtvBack = nullptr;
        if (SUCCEEDED(sc->ResizeBuffers(0, (UINT)width, (UINT)height, DXGI_FORMAT_UNKNOWN, 0))) {
            outW = width;
            outH = height;
            MakeBackRtv();
        }
    }

    void SetSharpen(float s) {
        sharpen = s;
        CBData c = {};
        c.srcW = (float)w;
        c.srcH = (float)h;
        c.invW = 1.0f / std::max(1, w);
        c.invH = 1.0f / std::max(1, h);
        c.lumaW = lw;
        c.lumaH = lh;
        c.blocksX = bx;
        c.blocksY = by;
        c.sharpen = s;
        c.lumaScale = (float)lumaDiv;
        c.lambda = 3.0f;
        c.fbLo = 0.20f;
        c.fbHi = 0.55f;
        ctx->UpdateSubresource(cb.get(), 0, nullptr, &c, 0, 0);
    }

    // Полноэкранный проход пиксельным шейдером. s[0..5] - SRV для t0..t5.
    void DrawFS(ID3D11PixelShader* ps, ID3D11RenderTargetView* rtv, int vw, int vh,
                ID3D11ShaderResourceView* const* s) {
        ID3D11RenderTargetView* rtvs[1] = { rtv };
        ctx->OMSetRenderTargets(1, rtvs, nullptr);
        D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)vw, (float)vh, 0.0f, 1.0f };
        ctx->RSSetViewports(1, &vp);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs.get(), nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        ID3D11Buffer* cbs[1] = { cb.get() };
        ctx->PSSetConstantBuffers(0, 1, cbs);
        ID3D11SamplerState* sms[1] = { samp.get() };
        ctx->PSSetSamplers(0, 1, sms);
        ctx->PSSetShaderResources(0, 6, s);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* nulls[6] = {};
        ctx->PSSetShaderResources(0, 6, nulls);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
    }

    void RunLuma() {
        ID3D11ShaderResourceView* s[6] = { srv[cur].get(), nullptr, nullptr, nullptr, nullptr, nullptr };
        DrawFS(psLuma.get(), rtvLuma[cur].get(), lw, lh, s);
    }

    void RunMotion() {
        int prev = cur ^ 1;
        ID3D11ShaderResourceView* s[2] = { srvLuma[cur].get(), srvLuma[prev].get() };
        ID3D11Buffer* cbs[1] = { cb.get() };
        ID3D11UnorderedAccessView* u[1] = { uavMv.get() };
        ctx->CSSetShader(csMotion.get(), nullptr, 0);
        ctx->CSSetConstantBuffers(0, 1, cbs);
        ctx->CSSetShaderResources(2, 2, s);
        ctx->CSSetUnorderedAccessViews(0, 1, u, nullptr);
        ctx->Dispatch((UINT)bx, (UINT)by, 1);
        ID3D11UnorderedAccessView* nu[1] = { nullptr };
        ID3D11ShaderResourceView* ns[2] = { nullptr, nullptr };
        ctx->CSSetUnorderedAccessViews(0, 1, nu, nullptr);
        ctx->CSSetShaderResources(2, 2, ns);
        ctx->CSSetShader(nullptr, nullptr, 0);
    }

    void RunInterp() {
        int prev = cur ^ 1;
        ID3D11ShaderResourceView* s[6] = { srv[cur].get(), srv[prev].get(), nullptr, nullptr, srvMv.get(), nullptr };
        DrawFS(psInterp.get(), rtvInterp.get(), w, h, s);
    }

    void PresentSrv(ID3D11ShaderResourceView* src) {
        ID3D11ShaderResourceView* s[6] = { nullptr, nullptr, nullptr, nullptr, nullptr, src };
        DrawFS(psPresent.get(), rtvBack.get(), outW, outH, s);
        sc->Present(1, 0);
    }
};

// ============================================================ захват окна (WGC)
static Gfx* g_gfx = nullptr;
static std::mutex g_resMutex;
static HANDLE g_frameEvent = nullptr;
static std::atomic<uint32_t> g_frameSeq{0};
static std::atomic<LONGLONG> g_lastArrival{0};
static std::atomic<bool> g_needResize{false};
static std::atomic<int> g_pendW{0}, g_pendH{0};

static void OnFrame(wgc::Direct3D11CaptureFramePool const& sender) {
    for (;;) {
        auto frame = sender.TryGetNextFrame();
        if (!frame) break;
        try {
            auto cs = frame.ContentSize();
            auto surface = frame.Surface();
            auto access = surface.as<IDxgiInterfaceAccess>();
            winrt::com_ptr<ID3D11Texture2D> tex;
            winrt::check_hresult(access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), tex.put_void()));
            std::lock_guard<std::mutex> lk(g_resMutex);
            Gfx* g = g_gfx;
            if (g && g->texNew && cs.Width == g->w && cs.Height == g->h) {
                D3D11_BOX box = { 0, 0, 0, (UINT)cs.Width, (UINT)cs.Height, 1 };
                g->ctx->CopySubresourceRegion(g->texNew.get(), 0, 0, 0, 0, tex.get(), 0, &box);
                g_lastArrival = NowQpc();
                g_frameSeq++;
            } else {
                g_pendW = cs.Width;
                g_pendH = cs.Height;
                g_needResize = true;
            }
            SetEvent(g_frameEvent);
        } catch (...) {
        }
        frame.Close();
    }
}

struct CaptureCtx {
    wgc::GraphicsCaptureItem item{ nullptr };
    wgc::Direct3D11CaptureFramePool pool{ nullptr };
    wgc::GraphicsCaptureSession session{ nullptr };
    wgd11::IDirect3DDevice d3d{ nullptr };
    winrt::event_token tokFrame{};
    winrt::event_token tokClosed{};
    int w = 0, h = 0;
};

static bool CreateCapture(CaptureCtx& c, Gfx& g, HWND game, std::string& err) {
    try {
        if (!wgc::GraphicsCaptureSession::IsSupported()) {
            err = "Windows Graphics Capture is not supported (Windows 10 1903+ required)";
            return false;
        }
        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(interop->CreateForWindow(
            game, winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(), winrt::put_abi(c.item)));
        auto dxgiDev = g.dev.as<IDXGIDevice>();
        winrt::com_ptr<::IInspectable> insp;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDev.get(), insp.put()));
        c.d3d = insp.as<wgd11::IDirect3DDevice>();
        auto size = c.item.Size();
        c.w = size.Width;
        c.h = size.Height;
        c.pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            c.d3d, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        c.tokFrame = c.pool.FrameArrived(
            [](wgc::Direct3D11CaptureFramePool const& sender, wf::IInspectable const&) { OnFrame(sender); });
        c.tokClosed = c.item.Closed([](wgc::GraphicsCaptureItem const&, wf::IInspectable const&) { g_quit = true; });
        c.session = c.pool.CreateCaptureSession(c.item);
        try { c.session.IsCursorCaptureEnabled(false); } catch (...) {}   // курсор в кадр не нужен
        return true;
    } catch (winrt::hresult_error const& e) {
        err = winrt::to_string(e.message());
        return false;
    } catch (...) {
        err = "unknown capture error";
        return false;
    }
}

// ================================================================ окно вывода
static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    }
    return DefWindowProcW(h, m, w, l);
}

static HWND CreateOverlay(const RECT& rc) {
    HINSTANCE hi = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"AFXBoosterOverlay";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                             wc.lpszClassName, L"AFX Booster", WS_POPUP, rc.left, rc.top, rc.right - rc.left,
                             rc.bottom - rc.top, nullptr, nullptr, hi, nullptr);
    if (h) SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA);
    return h;
}

static bool GameRect(HWND h, RECT* rc) {
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, rc, sizeof(RECT)))) return true;
    return GetWindowRect(h, rc) != 0;
}

struct FindCtx {
    DWORD pid = 0;
    std::wstring title;
    HWND best = nullptr;
    long long bestArea = 0;
};

static BOOL CALLBACK EnumProc(HWND h, LPARAM lp) {
    FindCtx* c = reinterpret_cast<FindCtx*>(lp);
    if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (c->pid && pid != c->pid) return TRUE;
    wchar_t title[256] = {};
    GetWindowTextW(h, title, 256);
    if (!c->title.empty() && !ContainsNoCase(title, c->title)) return TRUE;
    RECT rc;
    GetWindowRect(h, &rc);
    long long w = rc.right - rc.left, hh = rc.bottom - rc.top;
    if (w < 200 || hh < 150) return TRUE;
    if (w * hh > c->bestArea) {
        c->bestArea = w * hh;
        c->best = h;
    }
    return TRUE;
}

static HWND FindGameWindow(DWORD pid, const std::wstring& title) {
    FindCtx c;
    c.pid = pid;
    c.title = title;
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&c));
    return c.best;
}

// Показывает окно вывода только когда игра в фокусе; следит за положением и размером.
struct OverlayState {
    HWND hwnd = nullptr;
    bool shown = false;
    RECT last = {};
    int topmostTick = 0;
};

static bool UpdateOverlay(HWND game, DWORD gamePid, OverlayState& ov, Gfx& g) {
    if (!IsWindow(game)) {
        g_quit = true;
        return false;
    }
    HWND fg = GetForegroundWindow();
    DWORD fgPid = 0;
    if (fg) GetWindowThreadProcessId(fg, &fgPid);
    bool focused = (fg == game) || (fg == ov.hwnd) || (gamePid != 0 && fgPid == gamePid);
    RECT rc = {};
    bool haveRect = GameRect(game, &rc);
    int ww = rc.right - rc.left, hh = rc.bottom - rc.top;
    bool want = haveRect && !IsIconic(game) && focused && ww > 64 && hh > 64;
    if (want) {
        if (!EqualRect(&rc, &ov.last)) {
            SetWindowPos(ov.hwnd, HWND_TOPMOST, rc.left, rc.top, ww, hh, SWP_NOACTIVATE);
            ov.last = rc;
            g.ResizeSwap(ww, hh);
        }
        if (!ov.shown) {
            ShowWindow(ov.hwnd, SW_SHOWNOACTIVATE);
            ov.shown = true;
        } else if (++ov.topmostTick % 8 == 0) {
            SetWindowPos(ov.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    } else if (ov.shown) {
        ShowWindow(ov.hwnd, SW_HIDE);
        ov.shown = false;
    }
    return ov.shown;
}

// ============================================================ режим генерации
static int RunBoost(const Options& o) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 1. ищем окно игры (оно может появиться не сразу)
    HWND game = o.hwnd;
    printf("STATUS waiting_window\n");
    for (int i = 0; i < 240 && !g_quit && !game; ++i) {
        game = FindGameWindow(o.pid, o.title);
        if (!game) Sleep(500);
    }
    if (!game || !IsWindow(game)) {
        printf("STATUS error window not found\n");
        return 4;
    }
    DWORD gamePid = 0;
    GetWindowThreadProcessId(game, &gamePid);

    // 2. Direct3D (адаптер, к которому подключён экран: обычно встроенная Vega)
    auto adapter = PickAdapter(o.adapterName, false);
    Gfx gfx;
    gfx.lumaDiv = o.lumaDiv;
    gfx.sharpen = o.sharpen;
    if (!gfx.Init(adapter)) return 4;
    g_gfx = &gfx;
    g_frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // 3. захват окна
    CaptureCtx cap;
    std::string err;
    if (!CreateCapture(cap, gfx, game, err)) {
        printf("STATUS error %s\n", err.c_str());
        return 4;
    }
    {
        std::lock_guard<std::mutex> lk(g_resMutex);
        if (!gfx.Resize(cap.w, cap.h)) {
            printf("STATUS error cannot allocate textures %dx%d\n", cap.w, cap.h);
            return 4;
        }
    }

    // 4. окно вывода и цепочка обмена
    RECT rc = {};
    GameRect(game, &rc);
    OverlayState ov;
    ov.hwnd = CreateOverlay(rc);
    if (!ov.hwnd || !gfx.CreateSwap(ov.hwnd, std::max<LONG>(16, rc.right - rc.left), std::max<LONG>(16, rc.bottom - rc.top), o.flip)) {
        printf("STATUS error cannot create the output window\n");
        return 4;
    }
    ov.last = rc;

    cap.session.StartCapture();
    printf("STATUS capturing w=%d h=%d fg=%d\n", cap.w, cap.h, o.fg ? 1 : 0);

    // 5. основной цикл
    double srcFps = 0.0;
    LONGLONG prevArrival = 0;
    uint32_t lastSeq = 0;
    int framesSeen = 0;
    LONGLONG statT = NowQpc();
    int outCount = 0, srcCount = 0, fgCount = 0;
    double cpuMsSum = 0.0;
    int housekeeping = 0;
    bool visible = UpdateOverlay(game, gamePid, ov, gfx);

    while (!g_quit) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (++housekeeping >= 6) {
            housekeeping = 0;
            visible = UpdateOverlay(game, gamePid, ov, gfx);
        }
        {
            LONGLONG now = NowQpc();
            if (now - statT >= g_qpf.QuadPart) {
                printf("STAT src=%.2f out=%.2f fg=%d cpu_ms=%.2f w=%d h=%d\n", (double)srcCount, (double)outCount,
                       fgCount > 0 ? 1 : 0, srcCount ? cpuMsSum / srcCount : 0.0, gfx.w, gfx.h);
                statT = now;
                outCount = srcCount = fgCount = 0;
                cpuMsSum = 0.0;
            }
        }
        WaitForSingleObject(g_frameEvent, 12);

        if (g_needResize) {
            int nw = g_pendW, nh = g_pendH;
            g_needResize = false;
            if (nw > 0 && nh > 0 && (nw != gfx.w || nh != gfx.h)) {
                try {
                    cap.pool.Recreate(cap.d3d, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2,
                                      wg::SizeInt32{ nw, nh });
                } catch (...) {}
                std::lock_guard<std::mutex> lk(g_resMutex);
                gfx.Resize(nw, nh);
                gfx.SetSharpen(o.sharpen);
                framesSeen = 0;
            }
            continue;
        }

        uint32_t seq = g_frameSeq.load();
        if (seq == lastSeq) continue;
        uint32_t skipped = seq - lastSeq;
        lastSeq = seq;

        LONGLONG t0 = NowQpc();
        LONGLONG arrival = g_lastArrival.load();
        if (prevArrival != 0 && arrival > prevArrival) {
            double dt = (double)(arrival - prevArrival) / (double)g_qpf.QuadPart / (double)skipped;
            double inst = 1.0 / dt;
            srcFps = (srcFps == 0.0) ? inst : srcFps * 0.85 + inst * 0.15;
        }
        prevArrival = arrival;
        ++srcCount;

        {
            std::lock_guard<std::mutex> lk(g_resMutex);
            gfx.cur ^= 1;
            gfx.ctx->CopyResource(gfx.tex[gfx.cur].get(), gfx.texNew.get());
        }
        if (!visible) {
            framesSeen = 0;   // игра не в фокусе: кадры не выводим, история начнётся заново
            continue;
        }
        ++framesSeen;
        bool doFg = o.fg && framesSeen >= 2 && skipped == 1 && srcFps > 0.0 && srcFps <= (double)o.maxSrcFps;
        if (o.fg) gfx.RunLuma();
        if (doFg) {
            gfx.RunMotion();
            gfx.RunInterp();
            gfx.PresentSrv(gfx.srvInterp.get());
            ++outCount;
            ++fgCount;
        }
        gfx.PresentSrv(gfx.srv[gfx.cur].get());
        ++outCount;
        cpuMsSum += (double)(NowQpc() - t0) * 1000.0 / (double)g_qpf.QuadPart;

        if (gfx.dev->GetDeviceRemovedReason() != S_OK) {
            printf("STATUS error graphics device was removed\n");
            break;
        }
    }

    // 6. завершение
    try { cap.pool.FrameArrived(cap.tokFrame); } catch (...) {}
    try { cap.session.Close(); } catch (...) {}
    try { cap.pool.Close(); } catch (...) {}
    g_gfx = nullptr;
    if (ov.hwnd) DestroyWindow(ov.hwnd);
    printf("STATUS closed\n");
    return 0;
}

// ============================================================ нагрузочный тест GPU
static const char* kStressShader = R"HLSL(
cbuffer SC : register(b0) { uint cnt; uint iters; uint passIdx; uint pad0; };
StructuredBuffer<uint>   srcBuf : register(t0);
RWStructuredBuffer<uint> dstBuf : register(u0);

[numthreads(256, 1, 1)]
void CSAlu(uint3 id : SV_DispatchThreadID)
{
    uint i = id.x;
    if (i >= cnt) return;
    float a = (float)(i & 1023u) * 0.001f + 1.0f;
    float b = 0.5f;
    [loop]
    for (uint k = 0; k < iters; k++)
    {
        a = a * 1.000001f + b;
        b = sin(a) * 0.5f + b * 0.9999f;
        a = sqrt(abs(a) + 1.0f) + b * 0.001f;
    }
    dstBuf[i] = asuint(a) ^ (i * 2654435761u) ^ asuint(b);
}

[numthreads(256, 1, 1)]
void CSMem(uint3 id : SV_DispatchThreadID)
{
    uint i = id.x;
    if (i >= cnt) return;
    uint j = (i * 977u + passIdx * 131u + 13u) % cnt;
    uint k = (i * 1223u + 7u) % cnt;
    dstBuf[i] = srcBuf[j] ^ (srcBuf[k] << 1) ^ (i + passIdx);
}
)HLSL";

struct StressCB {
    UINT cnt, iters, pass, pad;
};

static uint64_t Fnv1a(const void* data, size_t bytes) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < bytes; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static int RunStress(const Options& o) {
    auto adapter = PickAdapter(o.adapterName, false);
    if (!adapter) {
        printf("STRESS_RESULT FAIL adapter_not_found\n");
        return 4;
    }
    DXGI_ADAPTER_DESC1 ad;
    adapter->GetDesc1(&ad);
    printf("STRESS adapter=%ls\n", ad.Description);
    winrt::com_ptr<ID3D11Device> dev;
    winrt::com_ptr<ID3D11DeviceContext> ctx;
    if (!CreateDevice(adapter, dev, ctx)) {
        printf("STRESS_RESULT FAIL create_device\n");
        return 4;
    }
    auto bAlu = Compile(kStressShader, "CSAlu", "cs_5_0");
    auto bMem = Compile(kStressShader, "CSMem", "cs_5_0");
    if (!bAlu || !bMem) {
        printf("STRESS_RESULT FAIL shader\n");
        return 4;
    }
    winrt::com_ptr<ID3D11ComputeShader> csAlu, csMem;
    if (FAILED(dev->CreateComputeShader(bAlu->GetBufferPointer(), bAlu->GetBufferSize(), nullptr, csAlu.put())) ||
        FAILED(dev->CreateComputeShader(bMem->GetBufferPointer(), bMem->GetBufferSize(), nullptr, csMem.put()))) {
        printf("STRESS_RESULT FAIL shader_create\n");
        return 4;
    }

    const UINT count = 1u << 22;   // 4M слов = 16 МБ на буфер
    winrt::com_ptr<ID3D11Buffer> buf[2];
    winrt::com_ptr<ID3D11UnorderedAccessView> uav[2];
    winrt::com_ptr<ID3D11ShaderResourceView> srv[2];
    for (int i = 0; i < 2; ++i) {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = count * 4;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = 4;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = count;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sd.Buffer.NumElements = count;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, buf[i].put())) ||
            FAILED(dev->CreateUnorderedAccessView(buf[i].get(), &ud, uav[i].put())) ||
            FAILED(dev->CreateShaderResourceView(buf[i].get(), &sd, srv[i].put()))) {
            printf("STRESS_RESULT FAIL buffers\n");
            return 4;
        }
    }
    winrt::com_ptr<ID3D11Buffer> stage;
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = count * 4;
        bd.Usage = D3D11_USAGE_STAGING;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, stage.put()))) {
            printf("STRESS_RESULT FAIL staging\n");
            return 4;
        }
    }
    winrt::com_ptr<ID3D11Buffer> cbuf;
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = sizeof(StressCB);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, cbuf.put()))) {
            printf("STRESS_RESULT FAIL cbuffer\n");
            return 4;
        }
    }

    auto setCb = [&](UINT iters, UINT pass) {
        StressCB c = { count, iters, pass, 0 };
        ctx->UpdateSubresource(cbuf.get(), 0, nullptr, &c, 0, 0);
        ID3D11Buffer* cbs[1] = { cbuf.get() };
        ctx->CSSetConstantBuffers(0, 1, cbs);
    };
    auto unbind = [&]() {
        ID3D11UnorderedAccessView* nu[1] = { nullptr };
        ID3D11ShaderResourceView* ns[1] = { nullptr };
        ctx->CSSetUnorderedAccessViews(0, 1, nu, nullptr);
        ctx->CSSetShaderResources(0, 1, ns);
    };

    using clock = std::chrono::steady_clock;
    UINT iters = 96;
    uint64_t ref = 0;
    bool haveRef = false;
    int calib = 0, done = 0;
    auto t0 = clock::now();
    auto lastPrint = t0;

    while (std::chrono::duration_cast<std::chrono::seconds>(clock::now() - t0).count() < o.stressSeconds) {
        auto it0 = clock::now();
        // 1) арифметика: sin/sqrt в цикле
        setCb(iters, 0);
        ID3D11UnorderedAccessView* ua[1] = { uav[0].get() };
        ctx->CSSetShader(csAlu.get(), nullptr, 0);
        ctx->CSSetUnorderedAccessViews(0, 1, ua, nullptr);
        ctx->Dispatch((count + 255) / 256, 1, 1);
        unbind();
        // 2) память: 8 проходов чтения/записи с перестановкой, буферы A <-> B
        ctx->CSSetShader(csMem.get(), nullptr, 0);
        for (UINT p = 0; p < 8; ++p) {
            int s = (p % 2 == 0) ? 0 : 1;
            int d = 1 - s;
            setCb(iters, p);
            ID3D11ShaderResourceView* sv[1] = { srv[s].get() };
            ID3D11UnorderedAccessView* uv[1] = { uav[d].get() };
            ctx->CSSetShaderResources(0, 1, sv);
            ctx->CSSetUnorderedAccessViews(0, 1, uv, nullptr);
            ctx->Dispatch((count + 255) / 256, 1, 1);
            unbind();
        }
        ctx->CSSetShader(nullptr, nullptr, 0);
        // 3) результат (после 8 проходов он снова в буфере A) -> CPU, контрольная сумма
        ctx->CopyResource(stage.get(), buf[0].get());
        D3D11_MAPPED_SUBRESOURCE ms = {};
        HRESULT hr = ctx->Map(stage.get(), 0, D3D11_MAP_READ, 0, &ms);
        if (FAILED(hr)) {
            HRESULT why = dev->GetDeviceRemovedReason();
            printf("STRESS_RESULT FAIL device_removed hr=0x%08X reason=0x%08X\n", (unsigned)hr, (unsigned)why);
            return 2;
        }
        uint64_t h = Fnv1a(ms.pData, (size_t)count * 4);
        ctx->Unmap(stage.get(), 0);
        if (dev->GetDeviceRemovedReason() != S_OK) {
            printf("STRESS_RESULT FAIL device_removed\n");
            return 2;
        }
        double ms_it = std::chrono::duration<double, std::milli>(clock::now() - it0).count();
        if (calib < 3) {
            // подбираем нагрузку так, чтобы один проход занимал около 60 мс (далеко от лимита TDR в 2 с)
            double k = 60.0 / std::max(1.0, ms_it);
            iters = (UINT)std::max(16.0, std::min(4096.0, iters * std::max(0.25, std::min(4.0, k))));
            ++calib;
        } else if (!haveRef) {
            ref = h;
            haveRef = true;
        } else if (h != ref) {
            printf("STRESS_RESULT FAIL checksum_mismatch iteration=%d\n", done);
            return 3;
        }
        ++done;
        if (clock::now() - lastPrint >= std::chrono::seconds(1)) {
            printf("STRESS t=%lld iterations=%d iter_ms=%.1f ok\n",
                   (long long)std::chrono::duration_cast<std::chrono::seconds>(clock::now() - t0).count(), done, ms_it);
            lastPrint = clock::now();
        }
    }
    printf("STRESS_RESULT OK iterations=%d\n", done);
    return 0;
}

// ===================================================================== main
static void StdinThread() {
    char buf[256];
    while (fgets(buf, sizeof(buf), stdin)) {
        if (strncmp(buf, "quit", 4) == 0) break;
    }
    g_quit = true;
}

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    QueryPerformanceFrequency(&g_qpf);
    Options o = ParseArgs(argc, argv);
    if (o.listAdapters) {
        PickAdapter(L"", true);
        return 0;
    }
    if (o.stressSeconds > 0) {
        return RunStress(o);
    }
    if (!o.pid && !o.hwnd && o.title.empty()) {
        printf("STATUS error usage: fps_booster.exe --pid <PID> | --hwnd <HWND> | --title <text> | --stress <sec>\n");
        return 4;
    }
    std::thread(StdinThread).detach();
    return RunBoost(o);
}
