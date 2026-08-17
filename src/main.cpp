// ============================================================================
// VR180Mirror — Live SteamVR -> VR180 (side-by-side half, 180° equirect) mirror
//
// Grabs the SteamVR compositor mirror textures for both eyes (exactly what the
// player sees in the headset, including overlays/dashboard), reprojects each
// rectilinear eye view onto a 180°x180° equirectangular (lat/long) dome using
// the per-eye raw projection tangents, and presents the combined SBS frame in
// a window whose backbuffer is the full output resolution. Capture that window
// with OBS Game Capture and stream it (WHIP/WebRTC, HLS, ...) to a viewer.
//
// The output frame layout is standard VR180: left half = left eye, right half
// = right eye, each half spanning longitude -90..+90 and latitude -90..+90.
// Content outside the game's rendered FOV (~100-110°) is black, which is the
// geometrically correct presentation on a 180 dome.
//
//   --size WxH       output canvas (default 4096x2048)
//   --fps N          render/pacing rate (default 60)
//   --preview N      preview window width in pixels (default 1440)
//   --test-grid      render a calibration grid instead of the live mirror
//   --flip-v         flip mirror texture vertically (if your runtime differs)
//   --swap-eyes      swap left/right eye halves
//   --feather D      edge feather in degrees at FOV border (default 1.5, 0=off)
//   --topmost        keep preview window always on top
//   --dump-frame F   render one frame, write 24-bit BMP to F, then exit
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <openvr.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "winmm.lib")

using Microsoft::WRL::ComPtr;

// ----------------------------------------------------------------------------
// Config
// ----------------------------------------------------------------------------
struct Config {
    int      width       = 4096;
    int      height      = 2048;
    int      fps         = 60;
    int      previewW    = 1440;
    bool     testGrid    = false;
    bool     flipV       = false;
    bool     swapEyes    = false;
    bool     topmost     = false;
    bool     supersample = true;   // 2x2 box taps: cleaner downsample of supersampled mirrors
    float    featherDeg  = 1.5f;
    std::string dumpFrame;
};

static Config g_cfg;
static bool   g_quit = false;

static void logf(const char* fmt, ...);

// ----------------------------------------------------------------------------
// Runtime settings file: bin\runtime.json, written by the web server when the
// viewer toggles options (e.g. "full picture" = feather 0). Polled ~1x/s.
// ----------------------------------------------------------------------------
static FILETIME g_rtWriteTime = {};
static void pollRuntimeFile() {
    static char path[MAX_PATH] = {};
    if (!path[0]) {
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        char* slash = strrchr(path, '\\');
        if (slash) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "runtime.json");
    }
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return;
    if (CompareFileTime(&fad.ftLastWriteTime, &g_rtWriteTime) == 0) return;
    g_rtWriteTime = fad.ftLastWriteTime;

    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return;
    char buf[512] = {};
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    const char* k = strstr(buf, "\"feather\"");
    if (k) {
        float v = -1.0f;
        if (sscanf_s(k + 9, " : %f", &v) == 1 && v >= 0.0f && v <= 20.0f) {
            if (v != g_cfg.featherDeg) {
                g_cfg.featherDeg = v;
                logf("runtime.json: feather -> %.2f deg%s", v, v == 0.0f ? " (full picture)" : "");
            }
        }
    }
}

static void logf(const char* fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    SYSTEMTIME st; GetLocalTime(&st);
    printf("[%02d:%02d:%02d] %s\n", st.wHour, st.wMinute, st.wSecond, buf);
    fflush(stdout);
}

// ----------------------------------------------------------------------------
// Shaders (compiled at runtime)
// ----------------------------------------------------------------------------
static const char* g_shaderSrc = R"HLSL(
Texture2D texL : register(t0);
Texture2D texR : register(t1);
SamplerState samp : register(s0);

cbuffer CB : register(b0) {
    float4 tanL;              // left eye raw projection: L, R, T, B (T negative)
    float4 tanR;              // right eye
    row_major float4x4 h2eL;  // head->eye rotation (R_eye2head transposed)
    row_major float4x4 h2eR;
    float4 params0;           // x: feather (tangent units), y: gridMode, z: time s, w: swapEyes
    float4 params1;           // x: flipV, y: ss taps (1|4), zw: output pixel in per-eye uv
};

static const float PI = 3.14159265358979f;

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut vsmain(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}

// Per-eye equirect coords -> head-space direction. x right, y up, -z forward.
float3 dirFromEquirect(float2 e) {
    float lon = (e.x - 0.5) * PI;      // -90..+90 deg
    float lat = (0.5 - e.y) * PI;      // +90 top .. -90 bottom
    float cl = cos(lat);
    return float3(cl * sin(lon), sin(lat), -cl * cos(lon));
}

float4 sampleEyeOnce(Texture2D tex, float4 tans, float3x3 h2e, float3 dHead) {
    float3 d = mul(h2e, dHead);
    if (d.z > -1e-4) return float4(0, 0, 0, 1);
    float tx = d.x / -d.z;
    float ty = d.y / -d.z;
    float u = (tx - tans.x) / (tans.y - tans.x);
    // OpenVR raw projection: fBottom (tans.w, positive) is the +Y/up edge and
    // fTop (tans.z, negative) the -Y/down edge; texture v=0 is the top row.
    float v = (tans.w - ty) / (tans.w - tans.z);
    if (params1.x > 0.5) v = 1.0 - v;
    if (u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0) return float4(0, 0, 0, 1);
    float4 c = tex.SampleLevel(samp, float2(u, v), 0);
    float feather = params0.x;
    if (feather > 0.0) {
        float du = min(u, 1.0 - u) * (tans.y - tans.x);
        float dv = min(v, 1.0 - v) * (tans.w - tans.z);
        c.rgb *= smoothstep(0.0, feather, min(du, dv));
    }
    c.a = 1.0;
    return c;
}

// 2x2 supersampled variant: the mirror is usually higher-res than the output
// canvas (supersampled render), so box-filtering four sub-pixel rays keeps the
// downsample clean instead of bilinear-skipping source pixels.
float4 sampleEye(Texture2D tex, float4 tans, float3x3 h2e, float2 e) {
    if (params1.y < 1.5) return sampleEyeOnce(tex, tans, h2e, dirFromEquirect(e));
    float2 px = params1.zw;   // one output pixel in per-eye uv units
    float4 acc = 0;
    acc += sampleEyeOnce(tex, tans, h2e, dirFromEquirect(e + px * float2(-0.25, -0.25)));
    acc += sampleEyeOnce(tex, tans, h2e, dirFromEquirect(e + px * float2( 0.25, -0.25)));
    acc += sampleEyeOnce(tex, tans, h2e, dirFromEquirect(e + px * float2(-0.25,  0.25)));
    acc += sampleEyeOnce(tex, tans, h2e, dirFromEquirect(e + px * float2( 0.25,  0.25)));
    return acc * 0.25;
}

// Calibration / idle grid: 10-degree lat/long graticule on the 180 dome,
// sweeping longitude bar for motion/latency, eye-exclusive squares to verify
// eye routing (L-only box on the left, R-only box on the right).
float4 grid(float2 e, bool isRight) {
    float lonD = (e.x - 0.5) * 180.0;
    float latD = (0.5 - e.y) * 180.0;

    float2 cell = float2(lonD / 10.0, latD / 10.0);
    float2 g = abs(frac(cell + 0.5) - 0.5) / fwidth(cell);
    float line10 = 1.0 - saturate(min(g.x, g.y));

    float2 axd = abs(float2(lonD, latD)) / fwidth(float2(lonD, latD));
    float axis = 1.0 - saturate(min(axd.x, axd.y) * 0.5);

    float3 col = float3(0.05, 0.06, 0.08);
    col = lerp(col, float3(0.35, 0.38, 0.42), line10);
    col = lerp(col, float3(0.2, 0.75, 0.3), axis);

    // 60-degree ring around center to show scale of typical game FOV
    float ang = degrees(acos(clamp(cos(radians(latD)) * cos(radians(lonD)), -1.0, 1.0)));
    float ring = 1.0 - saturate(abs(ang - 55.0) / (fwidth(ang) * 1.5));
    col = lerp(col, float3(0.9, 0.75, 0.2), ring * 0.8);

    // sweeping bar: crosses -90..+90 every 4 seconds
    float sweep = frac(params0.z / 4.0) * 180.0 - 90.0;
    float bar = 1.0 - saturate(abs(lonD - sweep) / 0.6);
    col = lerp(col, float3(0.95, 0.35, 0.1), bar);

    // eye-exclusive boxes at lat 20..30
    if (!isRight && lonD > -50.0 && lonD < -40.0 && latD > 20.0 && latD < 30.0)
        col = float3(0.2, 0.5, 1.0);   // blue box: LEFT eye only
    if (isRight && lonD > 40.0 && lonD < 50.0 && latD > 20.0 && latD < 30.0)
        col = float3(1.0, 0.3, 0.3);   // red box: RIGHT eye only

    return float4(col, 1.0);
}

float4 psmain(VSOut i) : SV_Target {
    bool rightHalf = i.uv.x >= 0.5;
    float2 e = float2(frac(i.uv.x * 2.0), i.uv.y);
    bool useRight = (params0.w > 0.5) ? !rightHalf : rightHalf;
    if (params0.y > 0.5) return grid(e, useRight);
    if (useRight) return sampleEye(texR, tanR, (float3x3)h2eR, e);
    return sampleEye(texL, tanL, (float3x3)h2eL, e);
}
)HLSL";

struct CBData {
    float tanL[4];
    float tanR[4];
    float h2eL[16];
    float h2eR[16];
    float params0[4];
    float params1[4];
};

// ----------------------------------------------------------------------------
// D3D11 state
// ----------------------------------------------------------------------------
struct Gfx {
    ComPtr<ID3D11Device>           dev;
    ComPtr<ID3D11DeviceContext>    ctx;
    ComPtr<IDXGISwapChain1>        swap;
    ComPtr<ID3D11RenderTargetView> rtvUnorm;
    ComPtr<ID3D11RenderTargetView> rtvSrgb;
    ComPtr<ID3D11VertexShader>     vs;
    ComPtr<ID3D11PixelShader>      ps;
    ComPtr<ID3D11Buffer>           cb;
    ComPtr<ID3D11SamplerState>     sampler;
} g;

// ----------------------------------------------------------------------------
// OpenVR state
// ----------------------------------------------------------------------------
struct VRState {
    vr::IVRSystem*     sys  = nullptr;
    vr::IVRCompositor* comp = nullptr;
    ID3D11ShaderResourceView* srvL = nullptr;   // owned by OpenVR runtime; release via ReleaseMirrorTextureD3D11
    ID3D11ShaderResourceView* srvR = nullptr;
    bool  mirrorSrgb = false;
    float projL[4] = { -1, 1, -1, 1 };          // L,R,T,B
    float projR[4] = { -1, 1, -1, 1 };
    float rotL[9]  = { 1,0,0, 0,1,0, 0,0,1 };   // head->eye rotation, row-major
    float rotR[9]  = { 1,0,0, 0,1,0, 0,0,1 };
    bool  connected = false;
    bool  haveMirror = false;
    ULONGLONG nextRetryTick = 0;
    ULONGLONG nextRefreshTick = 0;
} vrs;

static void vrReleaseMirror() {
    // Deliberately do NOT call ReleaseMirrorTextureD3D11: per-cycle
    // Get/Release triggers a SteamVR bug (openvr #1888: stale frames +
    // runaway VRAM). The SRVs are acquired once per VR session and cleaned
    // up by VR_Shutdown, matching the proven OBS-plugin pattern.
    vrs.srvL = nullptr;
    vrs.srvR = nullptr;
    vrs.haveMirror = false;
}

static void vrDisconnect(const char* why) {
    if (vrs.connected) logf("SteamVR disconnected (%s)", why);
    vrReleaseMirror();
    if (vrs.sys) vr::VR_Shutdown();
    vrs.sys = nullptr;
    vrs.comp = nullptr;
    vrs.connected = false;
    vrs.nextRetryTick = GetTickCount64() + 3000;
}

static void mat34ToHeadToEye(const vr::HmdMatrix34_t& m, float out9[9]) {
    // m maps eye->head; we want its rotation transposed (head->eye).
    out9[0] = m.m[0][0]; out9[1] = m.m[1][0]; out9[2] = m.m[2][0];
    out9[3] = m.m[0][1]; out9[4] = m.m[1][1]; out9[5] = m.m[2][1];
    out9[6] = m.m[0][2]; out9[7] = m.m[1][2]; out9[8] = m.m[2][2];
}

static void vrRefreshProjection() {
    if (!vrs.sys) return;
    vrs.sys->GetProjectionRaw(vr::Eye_Left,  &vrs.projL[0], &vrs.projL[1], &vrs.projL[2], &vrs.projL[3]);
    vrs.sys->GetProjectionRaw(vr::Eye_Right, &vrs.projR[0], &vrs.projR[1], &vrs.projR[2], &vrs.projR[3]);
    mat34ToHeadToEye(vrs.sys->GetEyeToHeadTransform(vr::Eye_Left),  vrs.rotL);
    mat34ToHeadToEye(vrs.sys->GetEyeToHeadTransform(vr::Eye_Right), vrs.rotR);
}

static bool vrAcquireMirror() {
    if (!vrs.comp || vrs.haveMirror) return vrs.haveMirror;
    vr::EVRCompositorError e1 = vrs.comp->GetMirrorTextureD3D11(
        vr::Eye_Left, g.dev.Get(), (void**)&vrs.srvL);
    vr::EVRCompositorError e2 = vrs.comp->GetMirrorTextureD3D11(
        vr::Eye_Right, g.dev.Get(), (void**)&vrs.srvR);
    if (e1 != vr::VRCompositorError_None || e2 != vr::VRCompositorError_None || !vrs.srvL || !vrs.srvR) {
        logf("GetMirrorTextureD3D11 failed (L=%d R=%d) — will retry", (int)e1, (int)e2);
        vrs.srvL = nullptr;
        vrs.srvR = nullptr;
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    vrs.srvL->GetDesc(&sd);
    vrs.mirrorSrgb = (sd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                      sd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    ComPtr<ID3D11Resource> res;
    vrs.srvL->GetResource(&res);
    ComPtr<ID3D11Texture2D> tex;
    if (SUCCEEDED(res.As(&tex))) {
        D3D11_TEXTURE2D_DESC td{}; tex->GetDesc(&td);
        logf("Mirror acquired: %ux%u fmt=%d srgb=%d", td.Width, td.Height, (int)td.Format, (int)vrs.mirrorSrgb);
    }
    vrs.haveMirror = true;
    return true;
}

static void vrTryConnect() {
    ULONGLONG now = GetTickCount64();
    if (now < vrs.nextRetryTick) return;
    vrs.nextRetryTick = now + 3000;

    if (!vr::VR_IsRuntimeInstalled()) { return; }
    vr::EVRInitError err = vr::VRInitError_None;
    vrs.sys = vr::VR_Init(&err, vr::VRApplication_Background);
    if (err != vr::VRInitError_None) {
        vrs.sys = nullptr;   // SteamVR not running yet — keep waiting quietly
        return;
    }
    vrs.comp = vr::VRCompositor();
    if (!vrs.comp) {
        vrDisconnect("no compositor");
        return;
    }
    char model[128] = {0}, mfr[128] = {0};
    vrs.sys->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
        vr::Prop_ModelNumber_String, model, sizeof(model));
    vrs.sys->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
        vr::Prop_ManufacturerName_String, mfr, sizeof(mfr));
    logf("Connected to SteamVR. HMD: %s %s", mfr, model);
    vrRefreshProjection();
    logf("Proj L: [%.4f %.4f %.4f %.4f]  R: [%.4f %.4f %.4f %.4f]",
        vrs.projL[0], vrs.projL[1], vrs.projL[2], vrs.projL[3],
        vrs.projR[0], vrs.projR[1], vrs.projR[2], vrs.projR[3]);
    vrs.connected = true;
    vrs.nextRefreshTick = 0;
    vrAcquireMirror();
}

static void vrPump() {
    if (!vrs.connected) { vrTryConnect(); return; }

    vr::VREvent_t ev;
    while (vrs.sys && vrs.sys->PollNextEvent(&ev, sizeof(ev))) {
        switch (ev.eventType) {
        case vr::VREvent_Quit:
            vrs.sys->AcknowledgeQuit_Exiting();
            vrDisconnect("SteamVR quitting");
            return;
        case vr::VREvent_IpdChanged:
            vrRefreshProjection();
            break;
        case vr::VREvent_SceneApplicationChanged:
            // projection can change per app; the held mirror SRV keeps updating
            vrs.nextRefreshTick = 0;
            break;
        default: break;
        }
    }

    ULONGLONG now = GetTickCount64();
    if (now >= vrs.nextRefreshTick) {
        vrs.nextRefreshTick = now + 15000;
        vrRefreshProjection();
        if (!vrs.haveMirror && !vrAcquireMirror()) {
            // compositor might not be serving frames yet; retry sooner
            vrs.nextRefreshTick = now + 2000;
        }
    }
}

// ----------------------------------------------------------------------------
// Rendering
// ----------------------------------------------------------------------------
static bool compileShaders() {
    ComPtr<ID3DBlob> vsb, psb, err;
    if (FAILED(D3DCompile(g_shaderSrc, strlen(g_shaderSrc), "vr180", nullptr, nullptr,
        "vsmain", "vs_5_0", 0, 0, &vsb, &err))) {
        logf("VS compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    err.Reset();
    if (FAILED(D3DCompile(g_shaderSrc, strlen(g_shaderSrc), "vr180", nullptr, nullptr,
        "psmain", "ps_5_0", 0, 0, &psb, &err))) {
        logf("PS compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    if (FAILED(g.dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g.vs))) return false;
    if (FAILED(g.dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g.ps))) return false;
    return true;
}

static void renderFrame(double timeSec) {
    CBData cb{};
    memcpy(cb.tanL, vrs.projL, sizeof(cb.tanL));
    memcpy(cb.tanR, vrs.projR, sizeof(cb.tanR));
    auto pack = [](const float r9[9], float out16[16]) {
        memset(out16, 0, 16 * sizeof(float));
        out16[0]=r9[0]; out16[1]=r9[1]; out16[2]=r9[2];
        out16[4]=r9[3]; out16[5]=r9[4]; out16[6]=r9[5];
        out16[8]=r9[6]; out16[9]=r9[7]; out16[10]=r9[8];
        out16[15]=1.0f;
    };
    pack(vrs.rotL, cb.h2eL);
    pack(vrs.rotR, cb.h2eR);
    bool gridMode = g_cfg.testGrid || !vrs.haveMirror;
    cb.params0[0] = tanf(g_cfg.featherDeg * 3.14159265f / 180.0f);
    cb.params0[1] = gridMode ? 1.0f : 0.0f;
    cb.params0[2] = (float)timeSec;
    cb.params0[3] = g_cfg.swapEyes ? 1.0f : 0.0f;
    cb.params1[0] = g_cfg.flipV ? 1.0f : 0.0f;
    cb.params1[1] = g_cfg.supersample ? 4.0f : 1.0f;
    cb.params1[2] = 2.0f / g_cfg.width;    // per-eye u units per output pixel
    cb.params1[3] = 1.0f / g_cfg.height;

    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED(g.ctx->Map(g.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        memcpy(map.pData, &cb, sizeof(cb));
        g.ctx->Unmap(g.cb.Get(), 0);
    }

    // match output gamma handling to the mirror's colorspace: sRGB SRV decodes
    // to linear on sample, so encode back via an sRGB RTV — net passthrough.
    ID3D11RenderTargetView* rtv = (vrs.haveMirror && !gridMode && vrs.mirrorSrgb)
        ? g.rtvSrgb.Get() : g.rtvUnorm.Get();

    const float black[4] = { 0, 0, 0, 1 };
    g.ctx->ClearRenderTargetView(rtv, black);
    g.ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{ 0, 0, (float)g_cfg.width, (float)g_cfg.height, 0, 1 };
    g.ctx->RSSetViewports(1, &vp);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->IASetInputLayout(nullptr);
    g.ctx->VSSetShader(g.vs.Get(), nullptr, 0);
    g.ctx->PSSetShader(g.ps.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[2] = { vrs.srvL, vrs.srvR };
    g.ctx->PSSetShaderResources(0, 2, srvs);
    ID3D11SamplerState* ss[1] = { g.sampler.Get() };
    g.ctx->PSSetSamplers(0, 1, ss);
    ID3D11Buffer* cbs[1] = { g.cb.Get() };
    g.ctx->PSSetConstantBuffers(0, 1, cbs);
    g.ctx->Draw(3, 0);

    ID3D11ShaderResourceView* nulls[2] = { nullptr, nullptr };
    g.ctx->PSSetShaderResources(0, 2, nulls);
}

static bool dumpBackbufferBMP(const char* path) {
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(g.swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    D3D11_TEXTURE2D_DESC td{}; back->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &staging))) return false;
    g.ctx->CopyResource(staging.Get(), back.Get());
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(g.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return false;

    int w = td.Width, h = td.Height;
    std::vector<uint8_t> row(w * 3);
    FILE* f = nullptr;
    fopen_s(&f, path, "wb");
    if (!f) { g.ctx->Unmap(staging.Get(), 0); return false; }
    uint32_t rowBytes = ((w * 3 + 3) / 4) * 4;
    uint32_t imgSize = rowBytes * h;
    uint8_t hdr[54] = {0};
    hdr[0]='B'; hdr[1]='M';
    *(uint32_t*)(hdr+2) = 54 + imgSize;
    *(uint32_t*)(hdr+10) = 54;
    *(uint32_t*)(hdr+14) = 40;
    *(int32_t*)(hdr+18) = w;
    *(int32_t*)(hdr+22) = h;
    *(uint16_t*)(hdr+26) = 1;
    *(uint16_t*)(hdr+28) = 24;
    *(uint32_t*)(hdr+34) = imgSize;
    fwrite(hdr, 1, 54, f);
    std::vector<uint8_t> pad(rowBytes - w * 3, 0);
    for (int y = h - 1; y >= 0; --y) {
        const uint8_t* src = (const uint8_t*)map.pData + (size_t)y * map.RowPitch;
        for (int x = 0; x < w; ++x) {
            row[x*3+0] = src[x*4+2];
            row[x*3+1] = src[x*4+1];
            row[x*3+2] = src[x*4+0];
        }
        fwrite(row.data(), 1, w * 3, f);
        if (!pad.empty()) fwrite(pad.data(), 1, pad.size(), f);
    }
    fclose(f);
    g.ctx->Unmap(staging.Get(), 0);
    return true;
}

// ----------------------------------------------------------------------------
// Window
// ----------------------------------------------------------------------------
static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: g_quit = true; PostQuitMessage(0); return 0;
    case WM_KEYDOWN:
        if (w == 'Q' && (GetKeyState(VK_CONTROL) & 0x8000)) { DestroyWindow(h); }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void setDpiAware() {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (!u32) return;
    typedef BOOL(WINAPI* Fn)(DPI_AWARENESS_CONTEXT);
    Fn fn = (Fn)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
    if (fn) fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--size") { sscanf_s(next(), "%dx%d", &g_cfg.width, &g_cfg.height); }
        else if (a == "--fps") { g_cfg.fps = atoi(next()); }
        else if (a == "--preview") { g_cfg.previewW = atoi(next()); }
        else if (a == "--test-grid") { g_cfg.testGrid = true; }
        else if (a == "--flip-v") { g_cfg.flipV = true; }
        else if (a == "--swap-eyes") { g_cfg.swapEyes = true; }
        else if (a == "--topmost") { g_cfg.topmost = true; }
        else if (a == "--no-ss") { g_cfg.supersample = false; }
        else if (a == "--feather") { g_cfg.featherDeg = (float)atof(next()); }
        else if (a == "--dump-frame") { g_cfg.dumpFrame = next(); }
        else if (a == "--help" || a == "-h") {
            printf("VR180Mirror [--size WxH] [--fps N] [--preview W] [--test-grid]\n"
                   "            [--flip-v] [--swap-eyes] [--feather deg] [--topmost]\n"
                   "            [--dump-frame out.bmp]\n");
            return 0;
        }
    }
    g_cfg.width  = std::max(640,  g_cfg.width  & ~1);
    g_cfg.height = std::max(320,  g_cfg.height & ~1);
    g_cfg.fps    = std::clamp(g_cfg.fps, 10, 144);

    setDpiAware();
    timeBeginPeriod(1);

    logf("VR180Mirror starting: %dx%d @ %d fps (SBS half 180 equirect)",
        g_cfg.width, g_cfg.height, g_cfg.fps);

    // --- window ---
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"VR180MirrorWnd";
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);

    int pw = std::clamp(g_cfg.previewW, 320, 3840);
    int ph = pw * g_cfg.height / g_cfg.width;
    RECT r{ 0, 0, pw, ph };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(g_cfg.topmost ? WS_EX_TOPMOST : 0, wc.lpszClassName,
        L"VR180Mirror — SBS half 180 — waiting for SteamVR",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        60, 60, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { logf("CreateWindow failed"); return 1; }

    // --- D3D11 ---
    UINT flags = 0;
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        nullptr, 0, D3D11_SDK_VERSION, &g.dev, &fl, &g.ctx))) {
        logf("D3D11CreateDevice failed"); return 1;
    }
    ComPtr<IDXGIDevice> dxgiDev;  g.dev.As(&dxgiDev);
    ComPtr<IDXGIAdapter> adapter; dxgiDev->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC ad{}; adapter->GetDesc(&ad);
    logf("GPU: %S", ad.Description);
    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(&factory));

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = g_cfg.width;
    sd.Height = g_cfg.height;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Scaling = DXGI_SCALING_STRETCH;   // backbuffer stays full-res; DWM scales to window
    if (FAILED(factory->CreateSwapChainForHwnd(g.dev.Get(), hwnd, &sd, nullptr, nullptr, &g.swap))) {
        logf("CreateSwapChain failed"); return 1;
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    ComPtr<ID3D11Texture2D> back;
    g.swap->GetBuffer(0, IID_PPV_ARGS(&back));
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    g.dev->CreateRenderTargetView(back.Get(), &rd, &g.rtvUnorm);
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    g.dev->CreateRenderTargetView(back.Get(), &rd, &g.rtvSrgb);

    if (!compileShaders()) return 1;

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(CBData);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g.dev->CreateBuffer(&bd, nullptr, &g.cb);

    D3D11_SAMPLER_DESC smp{};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    g.dev->CreateSamplerState(&smp, &g.sampler);

    // --- dump-frame mode (offline verification) ---
    if (!g_cfg.dumpFrame.empty()) {
        if (!g_cfg.testGrid) { vrTryConnect(); }
        renderFrame(1.0);
        g.ctx->Flush();
        bool ok = dumpBackbufferBMP(g_cfg.dumpFrame.c_str());
        logf("dump-frame %s: %s", g_cfg.dumpFrame.c_str(), ok ? "OK" : "FAILED");
        vrDisconnect("exit");
        timeEndPeriod(1);
        return ok ? 0 : 1;
    }

    logf(vrs.connected ? "Ready." : "Waiting for SteamVR... (start SteamVR with the player headset; this app auto-connects)");
    if (g_cfg.testGrid) logf("TEST GRID mode — rendering calibration pattern");

    // --- main loop ---
    LARGE_INTEGER qpf, qpc0, qpc;
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&qpc0);
    double period = 1.0 / g_cfg.fps;
    double nextT = 0.0;
    bool wasLive = false;

    while (!g_quit) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_quit) break;

        static int rtCounter = 0;
        if (++rtCounter >= g_cfg.fps) { rtCounter = 0; pollRuntimeFile(); }

        vrPump();

        bool live = vrs.haveMirror && !g_cfg.testGrid;
        if (live != wasLive) {
            wasLive = live;
            logf(live ? "LIVE: mirroring headset view" : "Idle: showing grid (no headset feed)");
            SetWindowTextW(hwnd, live
                ? L"VR180Mirror — LIVE — SBS half 180"
                : L"VR180Mirror — SBS half 180 — waiting for SteamVR");
        }

        QueryPerformanceCounter(&qpc);
        double now = double(qpc.QuadPart - qpc0.QuadPart) / qpf.QuadPart;
        renderFrame(now);
        g.swap->Present(0, 0);

        // pacing
        nextT += period;
        QueryPerformanceCounter(&qpc);
        now = double(qpc.QuadPart - qpc0.QuadPart) / qpf.QuadPart;
        if (nextT < now - 0.25) nextT = now;  // fell behind badly; resync
        double wait = nextT - now;
        if (wait > 0.002) Sleep((DWORD)((wait - 0.001) * 1000));
        for (;;) {
            QueryPerformanceCounter(&qpc);
            now = double(qpc.QuadPart - qpc0.QuadPart) / qpf.QuadPart;
            if (now >= nextT) break;
            YieldProcessor();
        }
    }

    vrDisconnect("exit");
    timeEndPeriod(1);
    logf("bye");
    return 0;
}
