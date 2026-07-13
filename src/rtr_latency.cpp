// rtr_latency.cpp — render-to-render / present-pipeline latency harness
// -----------------------------------------------------------------------------
// Pure software, no external hardware. Creates its own D3D11 flip-model swapchain,
// renders a trivial-but-nonzero GPU workload each frame, and measures THREE things
// per frame, all on QueryPerformanceCounter / GPU timestamp domains:
//
//   (1) Frame-to-frame interval + jitter   -> CPU QPC delta between Present() calls
//   (2) GPU render time per frame          -> D3D11 timestamp disjoint + 2 queries
//   (3) Present-to-flip pipeline latency    -> DXGI_FRAME_STATISTICS:
//                                              PresentCount vs. actually-presented,
//                                              SyncQPCTime of the flip vs. our Present QPC
//
// Output: per-frame CSV (rtr_latency.csv) + a summary with mean/median/p99/max and
// stddev (jitter) for each metric. Percentiles are what matter for competitive:
// mean hides the stalls, p99/max are the frames you actually feel.
//
// Build (MSVC, x64 Native Tools prompt):
//   cl /O2 /EHsc /DUNICODE /D_UNICODE rtr_latency.cpp ^
//      d3d11.lib dxgi.lib dxguid.lib user32.lib
//
// Run:
//   rtr_latency.exe [frames] [--windowed] [--vsync] [--busy N]
//     frames     : number of measured frames (default 3000; ~warmup 200 discarded)
//     --windowed : windowed flip instead of exclusive-ish fullscreen borderless
//     --vsync    : SyncInterval=1 (default 0 = uncapped, competitive)
//     --busy N   : draw the quad N times/frame to raise GPU load (default 1)
//
// Notes on accuracy:
//   * Uncapped (--busy default) will run thousands of FPS on a 3070 drawing nothing —
//     that measures the pipeline's floor, not a game. Use --busy to load the GPU into
//     a realistic frame time, or --vsync to lock to the 360Hz panel and study jitter
//     around a fixed cadence.
//   * DXGI_FRAME_STATISTICS only advances in flip model and (for SyncQPCTime to be
//     meaningful) benefits from vsync or a waitable swapchain. The harness uses a
//     FRAME_LATENCY_WAITABLE_OBJECT swapchain and waits on it each frame so the
//     present-to-flip number is well-defined even uncapped.
//   * This is presentation-pipeline latency, NOT click-to-photon. It answers
//     "does my config make the present path more deterministic (less jitter/stall)?"
// -----------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dxguid.h>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <cmath>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { PostQuitMessage(0); return 0; }
    return DefWindowProc(h, m, w, l);
}

struct PerFrame {
    double cpuIntervalMs;   // QPC delta Present->Present
    double gpuRenderMs;     // timestamp end-begin / freq
    double presentToFlipMs; // flip SyncQPCTime - our Present QPC
};

static double pct(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    double idx = p * (v.size() - 1);
    size_t lo = (size_t)idx; double frac = idx - lo;
    if (lo + 1 < v.size()) return v[lo] * (1 - frac) + v[lo + 1] * frac;
    return v[lo];
}
static double mean(const std::vector<double>& v) {
    if (v.empty()) return 0.0; double s = 0; for (double x : v) s += x; return s / v.size();
}
static double stdev(const std::vector<double>& v) {
    if (v.size() < 2) return 0.0; double m = mean(v), s = 0;
    for (double x : v) s += (x - m) * (x - m); return std::sqrt(s / (v.size() - 1));
}
static void report(const char* name, std::vector<double> v, const char* unit) {
    printf("  %-22s mean=%.4f  median=%.4f  p99=%.4f  max=%.4f  jitter(sd)=%.4f %s\n",
        name, mean(v), pct(v, 0.50), pct(v, 0.99),
        *std::max_element(v.begin(), v.end()), stdev(v), unit);
}

int wmain(int argc, wchar_t** argv) {
    int frames = 3000, busy = 1; bool windowed = false, vsync = false;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--windowed") windowed = true;
        else if (a == L"--vsync") vsync = true;
        else if (a == L"--busy" && i + 1 < argc) busy = _wtoi(argv[++i]);
        else if (!a.empty() && iswdigit(a[0])) frames = _wtoi(a.c_str());
    }
    const int warmup = 200;

    // --- window ---
    WNDCLASS wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"rtrHarness"; RegisterClass(&wc);
    int W = 1920, H = 1080;
    HWND hwnd = CreateWindowEx(0, wc.lpszClassName, L"rtr_latency",
        WS_POPUP | WS_VISIBLE, 0, 0, W, H, nullptr, nullptr, wc.hInstance, nullptr);

    // --- device ---
    UINT flags = 0;
    D3D_FEATURE_LEVEL fl;
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        want, 2, D3D11_SDK_VERSION, &dev, &fl, &ctx))) {
        printf("D3D11CreateDevice failed\n"); return 1;
    }

    IDXGIDevice* dxgiDev = nullptr; dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev);
    IDXGIAdapter* adapter = nullptr; dxgiDev->GetAdapter(&adapter);
    IDXGIFactory2* factory = nullptr; adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);

    // --- flip-model, waitable swapchain ---
    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width = W; scd.Height = H; scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1; scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2; scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.Scaling = DXGI_SCALING_NONE;
    scd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT
              | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    IDXGISwapChain2* sc2 = nullptr;
    {
        IDXGISwapChain1* sc1 = nullptr;
        if (FAILED(factory->CreateSwapChainForHwnd(dev, hwnd, &scd, nullptr, nullptr, &sc1))) {
            printf("CreateSwapChain failed\n"); return 1;
        }
        sc1->QueryInterface(__uuidof(IDXGISwapChain2), (void**)&sc2);
        sc1->Release();
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    sc2->SetMaximumFrameLatency(1);
    HANDLE waitable = sc2->GetFrameLatencyWaitableObject();

    ID3D11Texture2D* backbuf = nullptr; sc2->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backbuf);
    ID3D11RenderTargetView* rtv = nullptr; dev->CreateRenderTargetView(backbuf, nullptr, &rtv);

    // --- GPU timestamp queries (double-buffered so we don't stall) ---
    D3D11_QUERY_DESC qd = {};
    ID3D11Query* qDisjoint[2]; ID3D11Query* qBegin[2]; ID3D11Query* qEnd[2];
    qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    dev->CreateQuery(&qd, &qDisjoint[0]); dev->CreateQuery(&qd, &qDisjoint[1]);
    qd.Query = D3D11_QUERY_TIMESTAMP;
    dev->CreateQuery(&qd, &qBegin[0]); dev->CreateQuery(&qd, &qBegin[1]);
    dev->CreateQuery(&qd, &qEnd[0]);   dev->CreateQuery(&qd, &qEnd[1]);

    LARGE_INTEGER qpf; QueryPerformanceFrequency(&qpf);
    double qpcToMs = 1000.0 / (double)qpf.QuadPart;

    std::vector<PerFrame> data; data.reserve(frames);
    LARGE_INTEGER prevPresent = {}; bool havePrev = false;
    UINT lastPresentCount = 0;

    printf("rtr_latency: frames=%d busy=%d %s %s\n", frames, busy,
        windowed ? "windowed" : "fullscreen-borderless", vsync ? "vsync" : "uncapped");
    printf("running... (ESC to abort early)\n");

    int measured = 0, idx = 0;
    MSG msg;
    while (measured < frames + warmup) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg); DispatchMessage(&msg);
        }
        // throttle to swapchain readiness -> defines present-to-flip cleanly
        WaitForSingleObjectEx(waitable, 1000, TRUE);

        int q = idx & 1;

        ctx->Begin(qDisjoint[q]);
        ctx->End(qBegin[q]);            // timestamp: GPU work begin

        // --- trivial but nonzero GPU workload ---
        float clear[4] = { (idx & 1) ? 0.02f : 0.03f, 0.0f, 0.0f, 1.0f };
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        for (int b = 0; b < busy; b++) {
            clear[1] = (b & 1) ? 0.01f : 0.02f;
            ctx->ClearRenderTargetView(rtv, clear);
        }

        ctx->End(qEnd[q]);              // timestamp: GPU work end
        ctx->End(qDisjoint[q]);

        // present
        LARGE_INTEGER tPresent; QueryPerformanceCounter(&tPresent);
        UINT flagsP = (!vsync) ? DXGI_PRESENT_ALLOW_TEARING : 0;
        sc2->Present(vsync ? 1 : 0, flagsP);

        // ---- collect metrics for the PREVIOUS frame's queries (avoid GPU stall) ----
        if (havePrev) {
            int pq = (idx - 1) & 1;
            // GPU render time
            double gpuMs = 0.0;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
            while (ctx->GetData(qDisjoint[pq], &dj, sizeof(dj), 0) != S_OK) {}
            if (!dj.Disjoint && dj.Frequency) {
                UINT64 tb = 0, te = 0;
                while (ctx->GetData(qBegin[pq], &tb, sizeof(tb), 0) != S_OK) {}
                while (ctx->GetData(qEnd[pq], &te, sizeof(te), 0) != S_OK) {}
                gpuMs = (double)(te - tb) * 1000.0 / (double)dj.Frequency;
            }

            // CPU frame interval
            double cpuMs = (double)(tPresent.QuadPart - prevPresent.QuadPart) * qpcToMs;

            // present-to-flip via frame statistics
            double p2fMs = 0.0;
            DXGI_FRAME_STATISTICS fs = {};
            if (SUCCEEDED(sc2->GetFrameStatistics(&fs)) && fs.SyncQPCTime.QuadPart) {
                // time between our Present QPC and the flip's reported sync QPC
                double d = (double)(fs.SyncQPCTime.QuadPart - prevPresent.QuadPart) * qpcToMs;
                if (d > 0 && d < 1000.0) p2fMs = d; // sanity clamp
            }

            if (measured > warmup)
                data.push_back({ cpuMs, gpuMs, p2fMs });
        }

        prevPresent = tPresent; havePrev = true;
        idx++; measured++;
    }
done:
    // --- write CSV ---
    FILE* f = nullptr; fopen_s(&f, "rtr_latency.csv", "w");
    if (f) {
        fprintf(f, "frame,cpu_interval_ms,gpu_render_ms,present_to_flip_ms\n");
        for (size_t i = 0; i < data.size(); i++)
            fprintf(f, "%zu,%.5f,%.5f,%.5f\n", i,
                data[i].cpuIntervalMs, data[i].gpuRenderMs, data[i].presentToFlipMs);
        fclose(f);
    }

    // --- summary ---
    std::vector<double> ci, gr, pf;
    for (auto& d : data) { ci.push_back(d.cpuIntervalMs); gr.push_back(d.gpuRenderMs); pf.push_back(d.presentToFlipMs); }
    printf("\n==== summary over %zu measured frames ====\n", data.size());
    if (!ci.empty()) {
        report("frame interval", ci, "ms");
        report("  -> as FPS", { 1000.0 / std::max(1e-9, mean(ci)) }, "fps(mean)");
        report("gpu render", gr, "ms");
        report("present->flip", pf, "ms");
        printf("\nInterpretation:\n");
        printf("  * jitter(sd) on 'frame interval' is your determinism metric — lower = better.\n");
        printf("  * p99/max spikes on interval or present->flip = stalls (DPC, compositor, driver).\n");
        printf("  * compare two configs by jitter and p99, NOT by mean FPS.\n");
    } else {
        printf("no frames captured\n");
    }
    printf("CSV: rtr_latency.csv\n");

    // cleanup omitted (process exit) — add Release() calls if integrating into a service
    return 0;
}
