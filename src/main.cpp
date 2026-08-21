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
#include <share.h>
#include <timeapi.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <tlhelp32.h>

#include <openvr.h>

#include "oculus_capture_shared.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <atomic>
#include <thread>

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
    // Temporary visual transport test: a 12-bit counter overlay changes on
    // every mirror Present and can be decoded from a 72 fps recording.
    bool     frameCounterTest = false;
    bool     oculusVam  = false;  // direct Oculus/Virtual Desktop eye capture
    bool     flipV       = false;
    bool     swapEyes    = false;
    bool     topmost     = false;
    bool     supersample = true;   // 2x2 box taps: cleaner downsample of supersampled mirrors
    bool     fitFov      = true;   // pack only the rendered FOV into the canvas (no black bars)
    bool     stabilization = false; // POC: source-pose rotational stabilization, default off
    bool     stabilizationSelfTest = false;
    float    featherDeg  = 1.5f;
    std::string dumpFrame;
    std::string poseTrace;
    std::string poseTraceCheck;
};

static Config g_cfg;
static bool   g_quit = false;

static void logf(const char* fmt, ...);

// ----------------------------------------------------------------------------
// Runtime settings file: bin\runtime.json, written by the web server when the
// viewer toggles options (e.g. "full picture" = feather 0). Polled ~1x/s.
// ----------------------------------------------------------------------------
static FILETIME g_rtWriteTime = {};
static std::atomic<long long> g_presentCount{0};   // total presents, for rate reporting
static std::atomic<int> g_presentFps{0};
static std::atomic<int> g_oculusSourceFps{0};
static std::atomic<long> g_oculusOpenHr{S_OK};
static std::atomic<int> g_oculusOpenStage{0};
static std::atomic<int> g_sourcePoseValid{0};
static std::atomic<int> g_stabilizationCorrectionMilliDeg{0};
static bool g_tearing = false;   // swapchain created with ALLOW_TEARING
static bool sourceIsLive();
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
    k = strstr(buf, "\"stabilization\"");
    if (k) {
        const char* colon = strchr(k, ':');
        if (colon) {
            while (*++colon == ' ' || *colon == '\t') {}
            const bool enabled = strncmp(colon, "true", 4) == 0;
            const bool disabled = strncmp(colon, "false", 5) == 0;
            if ((enabled || disabled) && g_cfg.stabilization != enabled) {
                g_cfg.stabilization = enabled;
                logf("runtime.json: stabilization -> %s", enabled ? "on" : "off");
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

// Keep Windows' idle timers from blanking the display or sleeping the system
// while OBS is capturing this process. Execution requirements are thread-local;
// keeping this guard on main also guarantees an explicit reset on every normal
// return path.
class ExecutionStateGuard {
public:
    ExecutionStateGuard() {
        const EXECUTION_STATE required = static_cast<EXECUTION_STATE>(
            ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
        SetLastError(ERROR_SUCCESS);
        if (SetThreadExecutionState(required) == 0) {
            const DWORD error = GetLastError();
            logf("FATAL: Windows keep-awake request failed (error %lu); refusing to start", error);
            return;
        }
        acquired_ = true;
        logf("Windows keep-awake: system and display idle sleep disabled");
    }

    ~ExecutionStateGuard() {
        if (SetThreadExecutionState(ES_CONTINUOUS) == 0) {
            logf("WARNING: Windows keep-awake reset failed (error %lu)", GetLastError());
        } else if (acquired_) {
            logf("Windows keep-awake: default execution state restored");
        }
    }

    ExecutionStateGuard(const ExecutionStateGuard&) = delete;
    ExecutionStateGuard& operator=(const ExecutionStateGuard&) = delete;

    bool acquired() const { return acquired_; }

private:
    bool acquired_ = false;
};

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
    float4 params2;           // x: horizontal span (rad), y: vertical span (rad)
    float4 params3;           // x: Present counter (mod 4096), y: counter overlay enabled
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
// The canvas spans params2.xy radians (FOV-fit mode packs only the rendered
// FOV instead of a full 180, so no pixels are wasted on black).
float3 dirFromEquirect(float2 e) {
    float lon = (e.x - 0.5) * params2.x;
    float lat = (0.5 - e.y) * params2.y;
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
    float lonD = (e.x - 0.5) * degrees(params2.x);
    float latD = (0.5 - e.y) * degrees(params2.y);

    float2 cell = float2(lonD / 10.0, latD / 10.0);
    float2 g = abs(frac(cell + 0.5) - 0.5) / fwidth(cell);
    float line10 = 1.0 - saturate(min(g.x, g.y) * 0.5);

    float2 axd = abs(float2(lonD, latD)) / fwidth(float2(lonD, latD));
    float axis = 1.0 - saturate(min(axd.x, axd.y) * 0.5);

    float3 col = float3(0.05, 0.06, 0.08);
    col = lerp(col, float3(0.35, 0.38, 0.42), line10);
    col = lerp(col, float3(0.2, 0.75, 0.3), axis);

    // 60-degree ring around center to show scale of typical game FOV
    float ang = degrees(acos(clamp(cos(radians(latD)) * cos(radians(lonD)), -1.0, 1.0)));
    float ring = 1.0 - saturate(abs(ang - 55.0) / (fwidth(ang) * 3.0));
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

// A 12-cell high-contrast binary counter in the upper-middle of each eye.
// It changes every VR180Mirror Present, so a 72 fps recording can determine
// whether every delivered frame is unique without relying on game motion.
float4 applyFrameCounter(float2 e, float4 base) {
    if (params3.y < 0.5 || e.x < 0.30 || e.x >= 0.70 || e.y < 0.05 || e.y >= 0.12)
        return base;
    uint bit = min((uint)((e.x - 0.30) / (0.40 / 12.0)), 11U);
    uint counter = (uint)params3.x;
    bool on = ((counter >> bit) & 1U) != 0U;
    // Four repeated rows keep each bit readily legible after HEVC compression.
    float stripe = frac((e.y - 0.05) / 0.0175);
    float edge = smoothstep(0.03, 0.08, stripe) * (1.0 - smoothstep(0.92, 0.97, stripe));
    float3 marker = on ? float3(1.0, 1.0, 1.0) : float3(0.0, 0.0, 0.0);
    return float4(lerp(base.rgb, marker, 0.97 * edge), 1.0);
}

float4 psmain(VSOut i) : SV_Target {
    bool rightHalf = i.uv.x >= 0.5;
    float2 e = float2(frac(i.uv.x * 2.0), i.uv.y);
    bool useRight = (params0.w > 0.5) ? !rightHalf : rightHalf;
    float4 color = params0.y > 0.5 ? grid(e, useRight)
        : (useRight ? sampleEye(texR, tanR, (float3x3)h2eR, e)
                    : sampleEye(texL, tanL, (float3x3)h2eL, e));
    return applyFrameCounter(e, color);
}
)HLSL";

struct CBData {
    float tanL[4];
    float tanR[4];
    float h2eL[16];
    float h2eR[16];
    float params0[4];
    float params1[4];
    float params2[4];
    float params3[4];
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
    uint32_t mirrorW = 0, mirrorH = 0;   // source size, for the sampling decision
    float hSpanRad = 3.14159265f;   // canvas angular coverage (FOV-fit mode)
    float vSpanRad = 3.14159265f;
    ULONGLONG nextRetryTick = 0;
    ULONGLONG nextRefreshTick = 0;
} vrs;

// Publish the active canvas spans for the viewer (bin\mirror_status.json,
// served through the web server's /info) so the dome layer uses matching angles.
static void writeStatusFile() {
    static char path[MAX_PATH] = {};
    if (!path[0]) {
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        char* slash = strrchr(path, '\\');
        if (slash) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "mirror_status.json");
    }
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") == 0 && f) {
        fprintf(f, "{\"hspan\":%.1f,\"vspan\":%.1f,\"live\":%d,\"srcfps\":%d,\"gamefps\":%d,\"canvasW\":%d,\"canvasH\":%d,\"oculusOpenHr\":%ld,\"oculusOpenStage\":%d,\"stabilization\":%d,\"sourcePoseValid\":%d,\"stabilizationCorrectionDeg\":%.3f}",
            vrs.hSpanRad * 180.0f / 3.14159265f,
            vrs.vSpanRad * 180.0f / 3.14159265f,
            sourceIsLive() ? 1 : 0,
            // srcfps is the fixed canvas/encoder cadence that the Quest
            // contract consumes. gamefps separately exposes native game-frame
            // submissions, which can be lower without disturbing 72-Hz output.
            g_presentFps.load(std::memory_order_relaxed),
            g_cfg.oculusVam ? g_oculusSourceFps.load(std::memory_order_relaxed) : g_presentFps.load(std::memory_order_relaxed),
            g_cfg.width, g_cfg.height, g_oculusOpenHr.load(std::memory_order_relaxed),
            g_oculusOpenStage.load(std::memory_order_relaxed),
            g_cfg.stabilization ? 1 : 0,
            g_sourcePoseValid.load(std::memory_order_relaxed),
            g_stabilizationCorrectionMilliDeg.load(std::memory_order_relaxed) / 1000.0);
        fclose(f);
    }
}

// FOV-fit: size the canvas span to the exact union of both eyes' frustums
// (relative to head-forward), so every output pixel contributes to the
// player-visible viewport. The spectator compositor receives these same angles.
static void computeSpans() {
    const float PI_ = 3.14159265f;
    float h = PI_, v = PI_;
    if (g_cfg.fitFov && vrs.connected) {
        float maxH = 0.0f, maxV = 0.0f;
        const float* projs[2] = { vrs.projL, vrs.projR };
        for (const float* p : projs) {
            maxH = std::max(maxH, std::max(fabsf(atanf(p[0])), fabsf(atanf(p[1]))));
            maxV = std::max(maxV, std::max(fabsf(atanf(p[2])), fabsf(atanf(p[3]))));
        }
        h = std::min(PI_, 2.0f * maxH);
        v = std::min(PI_, 2.0f * maxV);
    }
    if (fabsf(h - vrs.hSpanRad) > 0.002f || fabsf(v - vrs.vSpanRad) > 0.002f) {
        vrs.hSpanRad = h;
        vrs.vSpanRad = v;
        logf("canvas span: %.1f x %.1f deg%s", h * 180.0f / PI_, v * 180.0f / PI_,
            (g_cfg.fitFov && vrs.connected) ? " (FOV-fit)" : " (full 180)");
    }
    // the status file is written by the I/O thread, never from the render loop
}

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
    computeSpans();
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
    computeSpans();
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
        vrs.mirrorW = td.Width; vrs.mirrorH = td.Height;
        logf("Mirror acquired: %ux%u fmt=%d srgb=%d", td.Width, td.Height, (int)td.Format, (int)vrs.mirrorSrgb);
    }
    vrs.haveMirror = true;
    return true;
}

// Anything that can block belongs off the render thread: a stalled render loop
// means the capture samples the same frame twice. VR_Init against a runtime
// that is not running took long enough to cost a frame every few seconds.
static std::atomic<bool> g_wantMirror{false};

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
    // The actual per-eye resolution the game renders at: SteamVR computes this
    // from the headset's own recommended profile (Virtual Desktop's Godlike
    // preset advertises its own to SteamVR) times the Video tab's render
    // resolution %. Neither the panel's native spec nor Link's "1.0x" number
    // apply here - this is the one SteamVR itself is actually asking for.
    uint32_t rw = 0, rh = 0;
    vrs.sys->GetRecommendedRenderTargetSize(&rw, &rh);
    logf("SteamVR recommended render target: %ux%u per eye", rw, rh);
    vrRefreshProjection();
    logf("Proj L: [%.4f %.4f %.4f %.4f]  R: [%.4f %.4f %.4f %.4f]",
        vrs.projL[0], vrs.projL[1], vrs.projL[2], vrs.projL[3],
        vrs.projR[0], vrs.projR[1], vrs.projR[2], vrs.projR[3]);
    vrs.connected = true;
    vrs.nextRefreshTick = 0;
    g_wantMirror.store(true, std::memory_order_relaxed);
}

// Render-thread half: only the mirror acquisition, which needs the D3D device.
static void vrPumpRender() {
    if (vrs.connected && !vrs.haveMirror && g_wantMirror.load(std::memory_order_relaxed)) {
        g_wantMirror.store(false, std::memory_order_relaxed);
        vrAcquireMirror();
    }
}

// I/O-thread half: connect, events, projection refresh.
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
        vrs.nextRefreshTick = vrs.haveMirror ? now + 15000 : now + 2000;
        vrRefreshProjection();
        if (!vrs.haveMirror) g_wantMirror.store(true, std::memory_order_relaxed);
    }
}

// ----------------------------------------------------------------------------
// Native Oculus / Virtual Desktop capture state
// ----------------------------------------------------------------------------
// The hook lives in VaM and only exports D3D11 shared eye textures.  Keeping
// injection and the producer side isolated means the normal OpenVR path is
// untouched, and no desktop/window capture ever downscales the game image.
namespace capture = vr180::oculus_capture;
struct Quaternion {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
};
struct SourcePoseFrame {
    Quaternion orientation{};
    double sensorSampleTime = 0.0;
    uint64_t serial = 0;
    bool valid = false;
};
struct OculusCaptureState {
    HANDLE mapping = nullptr;
    const capture::SharedState* shared = nullptr;
    ComPtr<ID3D11Texture2D> texL, texR;
    ComPtr<ID3D11ShaderResourceView> srvL, srvR;
    // The producer-owned textures are protected by keyed mutexes.  Render
    // from private copies so a short lock miss repeats the last good VaM
    // frame instead of replacing the entire stream with the calibration grid.
    ComPtr<ID3D11Texture2D> cacheL, cacheR;
    ComPtr<ID3D11ShaderResourceView> cacheSrvL, cacheSrvR;
    ComPtr<IDXGIKeyedMutex> mutexL, mutexR;
    uint64_t epoch = 0;
    uint64_t lastSerial = 0;
    ULONGLONG lastFrameTick = 0;
    ULONGLONG nextInjectTick = 0;
    DWORD lastInjectedPid = 0;
    bool hookArmedLogged = false;
    bool mirrorSrgb = false;
    bool connected = false;
    bool haveMirror = false;
    SourcePoseFrame pose{};
    bool cacheValid = false;
    float projL[4] = { -1, 1, -1, 1 };
    float projR[4] = { -1, 1, -1, 1 };
    uint32_t sourceW = 0, sourceH = 0;
} ocs;

static Quaternion quatNormalize(Quaternion q) {
    const float n2 = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
    if (n2 < 0.0001f || !std::isfinite(n2)) return {};
    const float inv = 1.0f / sqrtf(n2);
    q.x *= inv; q.y *= inv; q.z *= inv; q.w *= inv;
    return q;
}
static float quatDot(const Quaternion& a, const Quaternion& b) {
    return a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
}
static Quaternion quatConjugate(const Quaternion& q) { return {-q.x, -q.y, -q.z, q.w}; }
static Quaternion quatMultiply(const Quaternion& a, const Quaternion& b) {
    return quatNormalize({
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z,
    });
}
static Quaternion quatSlerp(Quaternion a, Quaternion b, float t) {
    float d = quatDot(a, b);
    if (d < 0.0f) { b = {-b.x, -b.y, -b.z, -b.w}; d = -d; }
    d = std::clamp(d, -1.0f, 1.0f);
    if (d > 0.9995f) return quatNormalize({
        a.x + (b.x-a.x)*t, a.y + (b.y-a.y)*t,
        a.z + (b.z-a.z)*t, a.w + (b.w-a.w)*t,
    });
    const float theta = acosf(d);
    const float s = sinf(theta);
    const float wa = sinf((1.0f-t)*theta) / s;
    const float wb = sinf(t*theta) / s;
    return quatNormalize({a.x*wa+b.x*wb, a.y*wa+b.y*wb, a.z*wa+b.z*wb, a.w*wa+b.w*wb});
}
static float quatAngleDeg(const Quaternion& a, const Quaternion& b) {
    return 2.0f * acosf(std::clamp(fabsf(quatDot(a, b)), 0.0f, 1.0f)) * 57.2957795f;
}
static void quatToMatrix(const Quaternion& q, float out[9]) {
    const float xx=q.x*q.x, yy=q.y*q.y, zz=q.z*q.z;
    const float xy=q.x*q.y, xz=q.x*q.z, yz=q.y*q.z;
    const float wx=q.w*q.x, wy=q.w*q.y, wz=q.w*q.z;
    out[0]=1.0f-2.0f*(yy+zz); out[1]=2.0f*(xy-wz);      out[2]=2.0f*(xz+wy);
    out[3]=2.0f*(xy+wz);      out[4]=1.0f-2.0f*(xx+zz); out[5]=2.0f*(yz-wx);
    out[6]=2.0f*(xz-wy);      out[7]=2.0f*(yz+wx);      out[8]=1.0f-2.0f*(xx+yy);
}

// A bounded first-order reference pose is deliberately conservative: it
// attenuates involuntary motion while following a deliberate turn before the
// source FOV can run out of coverage.  It is disabled by default and resets
// exactly on every toggle/off/pose discontinuity.
struct PoseStabilizer {
    Quaternion reference{};
    Quaternion source{};
    uint64_t lastSerial = 0;
    ULONGLONG lastTick = 0;
    bool active = false;

    void reset() {
        reference = {}; source = {}; lastSerial = 0; lastTick = 0; active = false;
        g_stabilizationCorrectionMilliDeg.store(0, std::memory_order_relaxed);
    }
    bool consumeAt(const SourcePoseFrame& frame, bool enabled, ULONGLONG now) {
        g_sourcePoseValid.store(frame.valid ? 1 : 0, std::memory_order_relaxed);
        if (!enabled || !frame.valid) { reset(); return false; }
        const Quaternion current = quatNormalize(frame.orientation);
        if (!active || frame.serial <= lastSerial || now - lastTick > 250U) {
            reference = current; source = current; lastSerial = frame.serial; lastTick = now; active = true;
            return true;
        }
        if (frame.serial != lastSerial) {
            const float dt = std::clamp(float(now - lastTick) / 1000.0f, 0.0f, 0.10f);
            // 0.85 s reference-follow time lets short natural motion settle,
            // without turning an intentional head turn into a long lag.
            const float follow = 1.0f - expf(-dt / 0.85f);
            reference = quatSlerp(reference, current, follow);
            // Never hold more than 12 degrees away from the source image;
            // beyond that, smoothly bring the reference along to preserve FOV.
            const float residual = quatAngleDeg(reference, current);
            if (residual > 12.0f) reference = quatSlerp(reference, current, (residual - 12.0f) / residual);
            source = current; lastSerial = frame.serial; lastTick = now;
        }
        const float correction = quatAngleDeg(reference, source);
        g_stabilizationCorrectionMilliDeg.store(static_cast<int>(correction * 1000.0f + 0.5f), std::memory_order_relaxed);
        return true;
    }
    bool consume(const SourcePoseFrame& frame, bool enabled) {
        return consumeAt(frame, enabled, GetTickCount64());
    }
    void headToSource(float out[9]) const {
        if (!active) { const float identity[9]={1,0,0,0,1,0,0,0,1}; memcpy(out, identity, sizeof(identity)); return; }
        // Output directions live in the stabilized reference frame.  Convert
        // them into the source head frame used to render this texture.
        quatToMatrix(quatMultiply(quatConjugate(source), reference), out);
    }
} g_stabilizer;

static Quaternion yawDegrees(float degrees) {
    const float half = degrees * 0.00872664626f;
    return quatNormalize({0.0f, sinf(half), 0.0f, cosf(half)});
}
static bool runStabilizationSelfTest() {
    PoseStabilizer poc;
    SourcePoseFrame first{}; first.serial = 1; first.valid = true;
    if (!poc.consumeAt(first, true, 1000)) return false;
    SourcePoseFrame smallFrame{}; smallFrame.orientation = yawDegrees(5.0f); smallFrame.serial = 2; smallFrame.valid = true;
    if (!poc.consumeAt(smallFrame, true, 1016)) return false;
    const float smallCorrection = quatAngleDeg(poc.reference, poc.source);
    SourcePoseFrame largeFrame{}; largeFrame.orientation = yawDegrees(30.0f); largeFrame.serial = 3; largeFrame.valid = true;
    if (!poc.consumeAt(largeFrame, true, 1032)) return false;
    const float largeCorrection = quatAngleDeg(poc.reference, poc.source);
    poc.consumeAt(largeFrame, false, 1048);
    float off[9] = {}; poc.headToSource(off);
    const bool identity = fabsf(off[0]-1.0f) < 0.0001f && fabsf(off[4]-1.0f) < 0.0001f &&
        fabsf(off[8]-1.0f) < 0.0001f && fabsf(off[1]) < 0.0001f && fabsf(off[2]) < 0.0001f;
    const bool pass = smallCorrection > 4.5f && smallCorrection < 5.1f &&
        largeCorrection <= 12.01f && identity;
    logf("STABILIZATION_SELF_TEST %s small=%.3fdeg large=%.3fdeg identityOff=%d",
        pass ? "PASS" : "FAIL", smallCorrection, largeCorrection, identity ? 1 : 0);
    return pass;
}

// Deterministic, offline validation of the exact pose records produced by a
// direct Oculus capture.  It does not render or need either headset: it proves
// that the same bounded filter accepts the captured data and reports how much
// movement it would suppress.  A future real-motion trace can use this exact
// command unchanged.
static bool runPoseTraceCheck(const char* path) {
    // Match the writer's sharing mode so this can validate an active capture
    // rather than requiring the mirror to be stopped first.
    FILE* f = _fsopen(path, "rb", _SH_DENYNO);
    if (!f) {
        logf("POSE_TRACE_CHECK FAIL could not open %s", path);
        return false;
    }
    char line[256] = {};
    fgets(line, sizeof(line), f); // CSV header
    PoseStabilizer poc;
    uint64_t samples = 0;
    float maxStepDeg = 0.0f, maxCorrectionDeg = 0.0f;
    Quaternion previous{};
    bool havePrevious = false;
    while (fgets(line, sizeof(line), f)) {
        unsigned long long tick = 0, serial = 0;
        double sensorTime = 0.0;
        SourcePoseFrame pose{};
        if (sscanf_s(line, "%llu,%llu,%lf,%f,%f,%f,%f", &tick, &serial, &sensorTime,
                     &pose.orientation.x, &pose.orientation.y, &pose.orientation.z, &pose.orientation.w) != 7) {
            continue;
        }
        pose.serial = static_cast<uint64_t>(serial);
        pose.sensorSampleTime = sensorTime;
        pose.valid = true;
        const Quaternion current = quatNormalize(pose.orientation);
        if (havePrevious) maxStepDeg = std::max(maxStepDeg, quatAngleDeg(previous, current));
        previous = current; havePrevious = true;
        poc.consumeAt(pose, true, static_cast<ULONGLONG>(tick));
        maxCorrectionDeg = std::max(maxCorrectionDeg, quatAngleDeg(poc.reference, poc.source));
        ++samples;
    }
    fclose(f);
    const bool pass = samples >= 2 && maxCorrectionDeg <= 12.01f;
    logf("POSE_TRACE_CHECK %s samples=%llu maxStepDeg=%.3f maxCorrectionDeg=%.3f boundDeg=12.000 path=%s",
        pass ? "PASS" : "FAIL", static_cast<unsigned long long>(samples), maxStepDeg, maxCorrectionDeg, path);
    return pass;
}

// Runs on the I/O thread with the mapping pump, never the render thread.  A
// trace contains the real submitted source pose for each new VaM frame and is
// suitable for later deterministic off/on analysis without another headset.
static void recordSourcePose(const SourcePoseFrame& pose) {
    if (g_cfg.poseTrace.empty() || !pose.valid) return;
    static FILE* trace = nullptr;
    static uint64_t lastSerial = 0;
    static uint32_t sinceFlush = 0;
    if (pose.serial == lastSerial) return;
    if (!trace) {
        trace = _fsopen(g_cfg.poseTrace.c_str(), "wb", _SH_DENYNO);
        if (!trace) {
            logf("POSE_TRACE_ERROR could not open %s", g_cfg.poseTrace.c_str());
            g_cfg.poseTrace.clear();
            return;
        }
        fprintf(trace, "tickMs,frameSerial,sensorSampleTime,qx,qy,qz,qw\n");
        logf("POSE_TRACE_STARTED path=%s", g_cfg.poseTrace.c_str());
    }
    lastSerial = pose.serial;
    fprintf(trace, "%llu,%llu,%.9f,%.8f,%.8f,%.8f,%.8f\n",
        static_cast<unsigned long long>(GetTickCount64()),
        static_cast<unsigned long long>(pose.serial), pose.sensorSampleTime,
        pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w);
    // Keep evidence recoverable without paying a disk flush for every frame.
    if (++sinceFlush >= 72U) { fflush(trace); sinceFlush = 0U; }
}

static void oculusCloseResources() {
    ocs.texL.Reset(); ocs.texR.Reset();
    ocs.srvL.Reset(); ocs.srvR.Reset();
    ocs.cacheL.Reset(); ocs.cacheR.Reset();
    ocs.cacheSrvL.Reset(); ocs.cacheSrvR.Reset();
    ocs.mutexL.Reset(); ocs.mutexR.Reset();
    ocs.haveMirror = false;
    ocs.cacheValid = false;
    ocs.pose = {};
    g_sourcePoseValid.store(0, std::memory_order_relaxed);
    g_stabilizer.reset();
    ocs.epoch = 0;
}

static bool createOculusRenderCache(ID3D11Texture2D* source,
                                    ComPtr<ID3D11Texture2D>& cache,
                                    ComPtr<ID3D11ShaderResourceView>& srv) {
    if (source == nullptr) return false;
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    // The cache has the same image layout as the source, but is not shared or
    // producer-writable.  Only mip zero is sampled by the reprojection shader.
    desc.MipLevels = 1U;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0U;
    desc.MiscFlags = 0U;
    if (FAILED(g.dev->CreateTexture2D(&desc, nullptr, &cache)) || !cache) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS
        ? DXGI_FORMAT_R8G8B8A8_UNORM : desc.Format;
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1U;
    return SUCCEEDED(g.dev->CreateShaderResourceView(cache.Get(), &view, &srv)) && srv;
}

static void oculusDisconnect() {
    oculusCloseResources();
    if (ocs.shared) { UnmapViewOfFile(ocs.shared); ocs.shared = nullptr; }
    if (ocs.mapping) { CloseHandle(ocs.mapping); ocs.mapping = nullptr; }
    ocs.connected = false;
}

static DWORD findProcessIdByName(const wchar_t* wanted) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W row{}; row.dwSize = sizeof(row);
    DWORD result = 0;
    if (Process32FirstW(snapshot, &row)) {
        do {
            if (_wcsicmp(row.szExeFile, wanted) == 0) { result = row.th32ProcessID; break; }
        } while (Process32NextW(snapshot, &row));
    }
    CloseHandle(snapshot);
    return result;
}

static bool injectOculusHook(DWORD pid) {
    char modulePath[MAX_PATH] = {};
    if (!GetModuleFileNameA(nullptr, modulePath, MAX_PATH)) return false;
    char* slash = strrchr(modulePath, '\\');
    if (!slash) return false;
    strcpy_s(slash + 1, MAX_PATH - (slash + 1 - modulePath), "VR180OculusHook.dll");
    wchar_t hookPath[MAX_PATH] = {};
    if (!MultiByteToWideChar(CP_UTF8, 0, modulePath, -1, hookPath, MAX_PATH)) return false;
    if (GetFileAttributesW(hookPath) == INVALID_FILE_ATTRIBUTES) {
        logf("Oculus capture hook missing: %S", hookPath);
        return false;
    }
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!process) return false;
    const SIZE_T bytes = (wcslen(hookPath) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { CloseHandle(process); return false; }
    SIZE_T written = 0;
    bool ok = WriteProcessMemory(process, remote, hookPath, bytes, &written) && written == bytes;
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel, "LoadLibraryW"));
    HANDLE thread = ok && loadLibrary ? CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr) : nullptr;
    if (thread) {
        WaitForSingleObject(thread, 5000);
        DWORD exitCode = 0; GetExitCodeThread(thread, &exitCode);
        ok = exitCode != 0;
        CloseHandle(thread);
    } else {
        ok = false;
    }
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
    if (ok) logf("Oculus capture hook injected into VaM PID %lu", pid);
    return ok;
}

static bool openOculusEye(DWORD producerPid, uint64_t sharedHandle, ComPtr<ID3D11Texture2D>& texture,
                          ComPtr<ID3D11ShaderResourceView>& srv,
                          ComPtr<IDXGIKeyedMutex>& mutex) {
    g_oculusOpenStage.store(0, std::memory_order_relaxed);
    if (!sharedHandle) { g_oculusOpenHr.store(E_HANDLE, std::memory_order_relaxed); return false; }
    HANDLE producer = OpenProcess(PROCESS_DUP_HANDLE, FALSE, producerPid);
    if (!producer) { g_oculusOpenHr.store(HRESULT_FROM_WIN32(GetLastError()), std::memory_order_relaxed); return false; }
    HANDLE localHandle = nullptr;
    const BOOL duplicated = DuplicateHandle(producer, reinterpret_cast<HANDLE>((uintptr_t)sharedHandle),
        GetCurrentProcess(), &localHandle, 0, FALSE, DUPLICATE_SAME_ACCESS);
    CloseHandle(producer);
    if (!duplicated) { g_oculusOpenHr.store(HRESULT_FROM_WIN32(GetLastError()), std::memory_order_relaxed); return false; }
    g_oculusOpenStage.store(1, std::memory_order_relaxed);
    ComPtr<ID3D11Device1> device1;
    HRESULT hr = g.dev.As(&device1);
    if (FAILED(hr) || !device1) { CloseHandle(localHandle); g_oculusOpenHr.store(hr, std::memory_order_relaxed); return false; }
    g_oculusOpenStage.store(2, std::memory_order_relaxed);
    hr = device1->OpenSharedResource1(localHandle, IID_PPV_ARGS(&texture));
    CloseHandle(localHandle);
    if (FAILED(hr) || !texture) { g_oculusOpenHr.store(hr, std::memory_order_relaxed); return false; }
    g_oculusOpenStage.store(3, std::memory_order_relaxed);
    hr = texture.As(&mutex);
    if (FAILED(hr) || !mutex) { g_oculusOpenHr.store(hr, std::memory_order_relaxed); return false; }
    g_oculusOpenStage.store(4, std::memory_order_relaxed);
    D3D11_TEXTURE2D_DESC textureDesc{};
    texture->GetDesc(&textureDesc);
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
    viewDesc.Format = textureDesc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS
        ? DXGI_FORMAT_R8G8B8A8_UNORM : textureDesc.Format;
    viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    viewDesc.Texture2D.MostDetailedMip = 0;
    viewDesc.Texture2D.MipLevels = textureDesc.MipLevels;
    hr = g.dev->CreateShaderResourceView(texture.Get(), &viewDesc, &srv);
    if (FAILED(hr) || !srv) { g_oculusOpenHr.store(hr, std::memory_order_relaxed); return false; }
    g_oculusOpenStage.store(5, std::memory_order_relaxed);
    g_oculusOpenHr.store(S_OK, std::memory_order_relaxed);
    return true;
}

static void oculusOpenResources(const capture::SharedState& state) {
    oculusCloseResources();
    if (!openOculusEye(state.producerPid, state.left.sharedHandle, ocs.texL, ocs.srvL, ocs.mutexL) ||
        !openOculusEye(state.producerPid, state.right.sharedHandle, ocs.texR, ocs.srvR, ocs.mutexR)) {
        oculusCloseResources();
        return;
    }
    if (!createOculusRenderCache(ocs.texL.Get(), ocs.cacheL, ocs.cacheSrvL) ||
        !createOculusRenderCache(ocs.texR.Get(), ocs.cacheR, ocs.cacheSrvR)) {
        oculusCloseResources();
        return;
    }
    ocs.epoch = state.epoch;
    ocs.sourceW = state.left.width;
    ocs.sourceH = state.left.height;
    ocs.mirrorSrgb = state.left.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
        state.left.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    // The shared crop is already a single rectilinear eye.  LibOVR reports
    // positive tangent magnitudes in every direction; the shader expects a
    // signed left/right frustum followed by signed down/up.
    ocs.projL[0] = -state.left.leftTan; ocs.projL[1] = state.left.rightTan;
    ocs.projL[2] = -state.left.downTan; ocs.projL[3] = state.left.upTan;
    ocs.projR[0] = -state.right.leftTan; ocs.projR[1] = state.right.rightTan;
    ocs.projR[2] = -state.right.downTan; ocs.projR[3] = state.right.upTan;
    ocs.haveMirror = true;
    logf("Oculus eye textures acquired: %ux%u fmt=%u epoch=%llu", ocs.sourceW, ocs.sourceH,
         state.left.format, (unsigned long long)ocs.epoch);
}

static void oculusPump() {
    const ULONGLONG now = GetTickCount64();
    if (!ocs.shared) {
        ocs.mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, capture::kMappingName);
        if (ocs.mapping) {
            ocs.shared = static_cast<const capture::SharedState*>(MapViewOfFile(
                ocs.mapping, FILE_MAP_READ, 0, 0, sizeof(capture::SharedState)));
            if (!ocs.shared) { CloseHandle(ocs.mapping); ocs.mapping = nullptr; }
        }
    }
    if (!ocs.shared) {
        if (now >= ocs.nextInjectTick) {
            // Unity resolves LibOVR immediately after loading OVRPlugin. Poll
            // tightly only while no producer mapping exists so the opt-in
            // hook lands before that one-time resolver runs.
            ocs.nextInjectTick = now + 100;
            DWORD pid = findProcessIdByName(L"VaM.exe");
            if (pid && pid != ocs.lastInjectedPid) {
                ocs.lastInjectedPid = pid;
                injectOculusHook(pid);
            }
        }
        return;
    }
    capture::SharedState state = *ocs.shared;
    if (state.magic != capture::kMagic || state.version != capture::kVersion ||
        state.bytes != sizeof(state) || state.producerPid == 0) return;
    // A named mapping stays alive while this reader holds it.  If VaM was
    // restarted, that can otherwise leave us attached to the dead producer
    // forever and prevent the hook from being injected into the replacement.
    const DWORD currentVaMPid = findProcessIdByName(L"VaM.exe");
    if (currentVaMPid == 0 || state.producerPid != currentVaMPid) {
        logf("VaM Oculus producer PID %lu is stale; rearming capture", state.producerPid);
        oculusDisconnect();
        return;
    }
    if (state.left.width == 0 || state.right.width == 0) {
        if (!ocs.hookArmedLogged) {
            ocs.hookArmedLogged = true;
            logf("VaM Oculus hook armed in PID %lu; waiting for first native eye frame", state.producerPid);
        }
        return;
    }
    if (!ocs.connected) {
        ocs.connected = true;
        logf("Connected to VaM Oculus producer PID %lu", state.producerPid);
    }
    if (!ocs.haveMirror || state.epoch != ocs.epoch) oculusOpenResources(state);
    if (state.frameSerial != ocs.lastSerial) {
        ocs.lastSerial = state.frameSerial;
        ocs.lastFrameTick = now;
        ocs.pose.orientation = quatNormalize({state.pose.x, state.pose.y, state.pose.z, state.pose.w});
        ocs.pose.sensorSampleTime = state.pose.sensorSampleTime;
        ocs.pose.serial = state.frameSerial;
        ocs.pose.valid = state.pose.valid != 0U;
        recordSourcePose(ocs.pose);
    }
}

static bool oculusLatchFrame() {
    if (!ocs.haveMirror || GetTickCount64() - ocs.lastFrameTick > 1500) return false;
    if (ocs.mutexL->AcquireSync(1U, 0U) != S_OK) return false;
    if (ocs.mutexR->AcquireSync(1U, 0U) != S_OK) { ocs.mutexL->ReleaseSync(0U); return false; }
    g.ctx->CopyResource(ocs.cacheL.Get(), ocs.texL.Get());
    g.ctx->CopyResource(ocs.cacheR.Get(), ocs.texR.Get());
    g.ctx->Flush();
    ocs.mutexL->ReleaseSync(0U);
    ocs.mutexR->ReleaseSync(0U);
    ocs.cacheValid = true;
    return true;
}

static bool sourceIsLive() {
    return g_cfg.oculusVam
        ? (ocs.haveMirror && GetTickCount64() - ocs.lastFrameTick <= 1500)
        : vrs.haveMirror;
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
    if (g_cfg.oculusVam) oculusLatchFrame();
    const bool sourceFrame = g_cfg.oculusVam
        ? (ocs.cacheValid && sourceIsLive())
        : vrs.haveMirror;
    const float* sourceProjL = g_cfg.oculusVam ? ocs.projL : vrs.projL;
    const float* sourceProjR = g_cfg.oculusVam ? ocs.projR : vrs.projR;
    const uint32_t sourceW = g_cfg.oculusVam ? ocs.sourceW : vrs.mirrorW;
    const bool sourceSrgb = g_cfg.oculusVam ? ocs.mirrorSrgb : vrs.mirrorSrgb;
    CBData cb{};
    memcpy(cb.tanL, sourceProjL, sizeof(cb.tanL));
    memcpy(cb.tanR, sourceProjR, sizeof(cb.tanR));
    auto pack = [](const float r9[9], float out16[16]) {
        memset(out16, 0, 16 * sizeof(float));
        out16[0]=r9[0]; out16[1]=r9[1]; out16[2]=r9[2];
        out16[4]=r9[3]; out16[5]=r9[4]; out16[6]=r9[5];
        out16[8]=r9[6]; out16[9]=r9[7]; out16[10]=r9[8];
        out16[15]=1.0f;
    };
    if (g_cfg.oculusVam) {
        g_stabilizer.consume(ocs.pose, g_cfg.stabilization);
        float stabilized[9] = {};
        g_stabilizer.headToSource(stabilized);
        pack(stabilized, cb.h2eL);
        pack(stabilized, cb.h2eR);
    } else {
        pack(vrs.rotL, cb.h2eL);
        pack(vrs.rotR, cb.h2eR);
    }
    bool gridMode = g_cfg.testGrid || !sourceFrame;
    cb.params0[0] = tanf(g_cfg.featherDeg * 3.14159265f / 180.0f);
    cb.params0[1] = gridMode ? 1.0f : 0.0f;
    cb.params0[2] = (float)timeSec;
    cb.params0[3] = g_cfg.swapEyes ? 1.0f : 0.0f;
    // Unity/LibOVR's D3D render texture has the opposite vertical convention
    // from the OpenVR mirror used by the original path.
    cb.params1[0] = (g_cfg.flipV || g_cfg.oculusVam) ? 1.0f : 0.0f;
    // Sampling taps: a 2x2 box filter is right when the canvas is smaller than
    // the source (clean downsample), but it SOFTENS the image at 1:1. Decide
    // from the actual ratio instead of assuming.
    const uint32_t halfW = (uint32_t)g_cfg.width / 2u;
    const bool downscaling = sourceW > 0 && halfW < (uint32_t)(sourceW * 0.9f);
    cb.params1[1] = (g_cfg.supersample && downscaling) ? 4.0f : 1.0f;
    cb.params1[2] = 2.0f / g_cfg.width;    // per-eye u units per output pixel
    cb.params1[3] = 1.0f / g_cfg.height;
    cb.params2[0] = vrs.hSpanRad;
    cb.params2[1] = vrs.vSpanRad;
    cb.params3[0] = static_cast<float>(g_presentCount.load(std::memory_order_relaxed) & 4095LL);
    cb.params3[1] = g_cfg.frameCounterTest ? 1.0f : 0.0f;

    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED(g.ctx->Map(g.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        memcpy(map.pData, &cb, sizeof(cb));
        g.ctx->Unmap(g.cb.Get(), 0);
    }

    // match output gamma handling to the mirror's colorspace: sRGB SRV decodes
    // to linear on sample, so encode back via an sRGB RTV — net passthrough.
    ID3D11RenderTargetView* rtv = (sourceFrame && !gridMode && sourceSrgb)
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
    ID3D11ShaderResourceView* srvs[2] = {
        g_cfg.oculusVam ? ocs.cacheSrvL.Get() : vrs.srvL,
        g_cfg.oculusVam ? ocs.cacheSrvR.Get() : vrs.srvR,
    };
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

// Raise our GPU scheduling priority so the reprojection isn't starved when the
// game saturates the GPU. (D3DKMT high priority; fails silently if unavailable.)
static void setGpuPriority() {
    typedef LONG(WINAPI* Fn)(int);
    HMODULE gdi = LoadLibraryW(L"gdi32.dll");
    if (!gdi) return;
    Fn fn = (Fn)GetProcAddress(gdi, "D3DKMTSetProcessSchedulingPriorityClass");
    if (fn) {
        // 4 = D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH
        LONG r = fn(4);
        logf("GPU scheduling priority: %s", r == 0 ? "HIGH" : "default (boost unavailable)");
    }
}

static void setDpiAware() {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (!u32) return;
    typedef BOOL(WINAPI* Fn)(DPI_AWARENESS_CONTEXT);
    Fn fn = (Fn)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
    if (fn) fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

int main(int argc, char** argv) {
    ExecutionStateGuard executionState;
    if (!executionState.acquired()) return 1;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--size") { sscanf_s(next(), "%dx%d", &g_cfg.width, &g_cfg.height); }
        else if (a == "--fps") { g_cfg.fps = atoi(next()); }
        else if (a == "--preview") { g_cfg.previewW = atoi(next()); }
        else if (a == "--test-grid") { g_cfg.testGrid = true; }
        else if (a == "--frame-counter-test") { g_cfg.frameCounterTest = true; }
        else if (a == "--oculus-vam") { g_cfg.oculusVam = true; }
        else if (a == "--stabilization") { g_cfg.stabilization = true; }
        else if (a == "--stabilization-self-test") { g_cfg.stabilizationSelfTest = true; }
        else if (a == "--pose-trace") { g_cfg.poseTrace = next(); }
        else if (a == "--pose-trace-check") { g_cfg.poseTraceCheck = next(); }
        else if (a == "--flip-v") { g_cfg.flipV = true; }
        else if (a == "--swap-eyes") { g_cfg.swapEyes = true; }
        else if (a == "--topmost") { g_cfg.topmost = true; }
        else if (a == "--no-ss") { g_cfg.supersample = false; }
        else if (a == "--vr180") { g_cfg.fitFov = false; }   // classic full-180 canvas
        else if (a == "--feather") { g_cfg.featherDeg = (float)atof(next()); }
        else if (a == "--dump-frame") { g_cfg.dumpFrame = next(); }
        else if (a == "--help" || a == "-h") {
            printf("VR180Mirror [--size WxH] [--fps N] [--preview W] [--test-grid] [--frame-counter-test] [--oculus-vam]\n"
                   "            [--stabilization] [--stabilization-self-test] [--pose-trace out.csv] [--pose-trace-check in.csv] [--flip-v] [--swap-eyes] [--feather deg] [--topmost]\n"
                   "            [--dump-frame out.bmp]\n");
            return 0;
        }
    }
    g_cfg.width  = std::max(640,  g_cfg.width  & ~1);
    g_cfg.height = std::max(320,  g_cfg.height & ~1);
    // Present faster than the capture samples: OBS grabs presented frames on
    // its own 72Hz clock, so if we also present at ~72 the two clocks beat and
    // OBS captures the same frame twice about once per second (measured with
    // ffmpeg framemd5: 7 duplicate frames per 432). Presenting at 2x means a
    // fresh frame is always waiting, which removes the duplicates.
    g_cfg.fps    = std::clamp(g_cfg.fps, 10, 300);

    if (g_cfg.stabilizationSelfTest) return runStabilizationSelfTest() ? 0 : 1;
    if (!g_cfg.poseTraceCheck.empty()) return runPoseTraceCheck(g_cfg.poseTraceCheck.c_str()) ? 0 : 1;

    setDpiAware();
    setGpuPriority();
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    timeBeginPeriod(1);
    computeSpans();   // publish initial (full-180) spans for the viewer

    logf("VR180Mirror starting: %dx%d @ %d fps (SBS half 180 equirect, source=%s)",
        g_cfg.width, g_cfg.height, g_cfg.fps,
        g_cfg.oculusVam ? "VaM Oculus/Virtual Desktop" : "SteamVR");
    logf("Source-pose stabilization: %s%s", g_cfg.stabilization ? "ON (POC)" : "OFF",
        g_cfg.oculusVam ? "; direct Oculus RenderPose transport" : "; unavailable on SteamVR mirror POC");

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
        g_cfg.oculusVam ? L"VR180Mirror - waiting for VaM Oculus/Virtual Desktop"
                        : L"VR180Mirror - SBS half 180 - waiting for SteamVR",
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
    sd.BufferCount = 3;   // 3 buffers: never let a flip stall the render loop
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    // Allow tearing: without it a windowed flip swapchain paces presents against
    // the desktop compositor, so an occasional Present blocks past the frame
    // budget. The capture then samples the same frame twice (measured: ~1% of
    // frames held). We are never scanned out directly, so tearing is moot here.
    {
        Microsoft::WRL::ComPtr<IDXGIFactory5> f5;
        BOOL allow = FALSE;
        if (SUCCEEDED(factory.As(&f5)) &&
            SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))) &&
            allow) {
            sd.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
            g_tearing = true;
        }
    }
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
        if (!g_cfg.testGrid && !g_cfg.oculusVam) { vrTryConnect(); }
        renderFrame(1.0);
        g.ctx->Flush();
        bool ok = dumpBackbufferBMP(g_cfg.dumpFrame.c_str());
        logf("dump-frame %s: %s", g_cfg.dumpFrame.c_str(), ok ? "OK" : "FAILED");
        if (g_cfg.oculusVam) oculusDisconnect(); else vrDisconnect("exit");
        timeEndPeriod(1);
        return ok ? 0 : 1;
    }

    logf(g_cfg.oculusVam
        ? "Waiting for VaM's native Oculus/Virtual Desktop session... (start VaM after this mirror is armed)"
        : (vrs.connected ? "Ready." : "Waiting for SteamVR... (start SteamVR with the player headset; this app auto-connects)"));
    if (g_cfg.testGrid) logf("TEST GRID mode - rendering calibration pattern");

    // File I/O off the render thread: a stat/read/write on the render loop is a
    // stall, and a stalled render loop means a duplicated frame downstream.
    std::atomic<bool> ioRun{true};
    std::thread ioThread([&ioRun]() {
        long long lastCount = 0;
        uint64_t lastOculusSerial = 0;
        ULONGLONG lastTick = GetTickCount64();
        ULONGLONG nextHousekeepingTick = 0;
        while (ioRun.load(std::memory_order_relaxed)) {
            if (g_cfg.oculusVam) oculusPump(); else vrPump();
            const ULONGLONG nowTick = GetTickCount64();
            // Direct Oculus poses are carried with submitted frames.  Poll the
            // shared mapping faster than the source cadence so stabilization
            // never turns head motion into a 5 Hz stair-step.  File work stays
            // at 5 Hz and remains off the render thread.
            if (nowTick >= nextHousekeepingTick) {
                pollRuntimeFile();
                writeStatusFile();
                nextHousekeepingTick = nowTick + 200;
            }
            if (nowTick - lastTick >= 5000) {
                const long long c = g_presentCount.load(std::memory_order_relaxed);
                const double rate = (c - lastCount) * 1000.0 / (nowTick - lastTick);
                // published in mirror_status.json (and /info); not printed
                g_presentFps.store((int)(rate + 0.5), std::memory_order_relaxed);
                if (g_cfg.oculusVam) {
                    const uint64_t serial = ocs.lastSerial;
                    const double sourceRate = (serial >= lastOculusSerial)
                        ? (serial - lastOculusSerial) * 1000.0 / (nowTick - lastTick)
                        : 0.0;
                    g_oculusSourceFps.store((int)(sourceRate + 0.5), std::memory_order_relaxed);
                    lastOculusSerial = serial;
                }
                lastCount = c;
                lastTick = nowTick;
            }
            Sleep(g_cfg.oculusVam ? 4 : 200);
        }
    });

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

        if (!g_cfg.oculusVam) vrPumpRender();

        bool live = sourceIsLive() && !g_cfg.testGrid;
        if (live != wasLive) {
            wasLive = live;
            logf(live ? "LIVE: mirroring headset view" : "Idle: showing grid (no headset feed)");
            SetWindowTextW(hwnd, live
                ? L"VR180Mirror - LIVE - SBS half 180"
                : (g_cfg.oculusVam
                    ? L"VR180Mirror - waiting for VaM Oculus/Virtual Desktop"
                    : L"VR180Mirror - SBS half 180 - waiting for SteamVR"));
        }

        QueryPerformanceCounter(&qpc);
        double now = double(qpc.QuadPart - qpc0.QuadPart) / qpf.QuadPart;
        renderFrame(now);
        g.swap->Present(0, g_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);

        // present-rate report: a source that misses its target duplicates
        // frames downstream, so make the actual rate visible
        // No logging here: a console write blocks for milliseconds, which costs
        // a frame. Publish the count; the I/O thread turns it into a rate.
        g_presentCount.fetch_add(1, std::memory_order_relaxed);

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

    ioRun.store(false);
    if (ioThread.joinable()) ioThread.join();
    if (g_cfg.oculusVam) oculusDisconnect(); else vrDisconnect("exit");
    timeEndPeriod(1);
    logf("bye");
    return 0;
}
