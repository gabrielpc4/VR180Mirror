# Notes for a future native OpenXR client

The spectator viewer currently runs as a page in the Quest's Oculus Browser
(`web/player.html`, WebXR + WebCodecs). That path works but has a resolution
ceiling the browser itself imposes (see below) - if/when that gets replaced
with a native app on the headset, these are the specific things this project
already paid to learn the hard way. Skipping any of them means re-discovering
the same bug from scratch.

## Start here: Meta Spatial SDK's built-in 180 stereo media panel, not raw OpenXR

Researched 2026-08-19, before writing any native code: **don't start from bare
OpenXR + a hand-rolled dome mesh.** Meta's own **Spatial SDK** (Kotlin, the
first-party framework for building Horizon OS apps) has a panel type built
for exactly this:

- `Equirect180ShapeOptions` + `MediaPanelSettings` (`stereoMode =
  StereoMode.LeftRight` or `UpDown`) is a **built-in 180-degree stereo video
  panel**, not something to build by hand.
- `VideoSurfacePanelRegistration` connects an **ExoPlayer** instance to that
  panel through a `Surface`. Meta's own docs recommend it "for maximum
  performance... 360 content, and performance-critical scenarios" and
  describe it as rendering "directly to the panel surface" - strongly
  suggesting the panel is composited natively (like a 2D quad layer) rather
  than sampled in an app-owned GL context. If that holds up under testing, it
  sidesteps the entire precision/mipmap bug class below, the same reason
  system-composited 2D panels always look sharper than a hand-rendered scene.
- ExoPlayer already speaks LL-HLS and does hardware decode via MediaCodec
  internally - our existing MediaMTX LL-HLS output may need zero PC-side
  changes to feed it.
- A concrete starting point exists: **`PremiumMediaSample`** in
  [meta-quest/Meta-Spatial-SDK-Samples](https://github.com/meta-quest/Meta-Spatial-SDK-Samples)
  plays 180-degree video over a stream. Get that running and pointed at our
  own LL-HLS URL before writing any custom rendering code - it may cover most
  of the work with none of the bugs the WebXR path hit.

**Unresolved, needs on-device testing, not documentation**:
- Whether the panel's internal buffering adds latency beyond the ~1s our
  current LL-HLS pipeline already has - undocumented, and low latency matters
  for spectating a live player.
- Whether overlays (stats HUD, feather edge softening, headlock) are still
  achievable alongside a system-composited panel, or need a separate
  transparent quad panel layered into the scene instead.
- Spatial SDK is Kotlin and almost certainly expects a standard Gradle/Android
  Studio project - a real departure from this project's current no-Gradle raw
  toolchain (`aapt2`/`d8`/`apksigner` in `quest-app/build-apk.ps1`), worth
  deciding on deliberately before starting, not discovering mid-build.

Everything below was written assuming a hand-rolled OpenXR + GL renderer (the
fallback if the Spatial SDK panel route doesn't pan out, e.g. if its latency
turns out too high for live spectating). Keep it - if the panel route hits a
wall, this is the fallback plan and its lessons still apply.

## Why go native at all: the browser's WebXR resolution ceiling

Measured directly on one Quest 3 (2026-08-19): with the render-scale
multiplier on the XR framebuffer pushed all the way to its 2.00 maximum, the
resolved framebuffer stayed pinned at **5040x2640** - identical to what a much
lower multiplier already produced. That is a hard ceiling, not a scale factor
working as intended:

- **1.0x reference** for this headset/session (Meta's own number, as reported
  in its Link/Air Link settings): **3360x1760**.
- **5040x2640 = exactly 1.5x** of that reference, on both axes.
- Requesting 1.6x, 1.83x, or 2.00x all produced the *same* 5040x2640 - the
  runtime silently clamps above 1.5x.

This ceiling is **specific to the Oculus Browser's WebXR implementation** -
it is a conservative power/memory safety limit for arbitrary web content, not
the panel's real limit (panel spec is 2064x2208/eye) and not related to
Meta's own Link "1.0x" number, which is a *different* runtime's recommended
size (Link/Air Link reprojection), not a ceiling on native or WebXR apps. A
native OpenXR app is not subject to the browser's clamp and can request the
swapchain size it actually wants (subject to real device/memory limits, which
are much higher).

**Two other headsets/sessions measured completely different WebXR baselines**
under the same code (3150x1650 base on one, 3840x2749 on another, before the
1.5x-ceiling finding above) - so *never* hardcode an absolute resolution
target for the WebXR path; it has to be measured per-session. This whole
class of problem (guess a base, multiply, hope it lands on target) goes away
with a native app, because OpenXR lets you request an exact swapchain size
directly instead of a scale factor on top of an opaque "recommended" default.

**Action for the OpenXR client**: call `xrEnumerateViewConfigurationViews` and
read `recommendedImageRectWidth/Height` per eye - this is OpenXR's equivalent
of `XRWebGLLayer.getNativeFramebufferScaleFactor`, but check empirically
whether it's subject to the same kind of runtime-imposed ceiling before
assuming it reports the panel's true capability.

## Matching the PC's render resolution (still applies)

The canvas/stream should be sized to whatever SteamVR is *actually* asking
the game to render, not a guessed number. `IVRSystem::GetRecommendedRenderTargetSize`
gives that directly, and it already factors in Virtual Desktop's quality
preset (Godlike, etc.) and the SteamVR Video tab's render-resolution %.
`VR180Mirror.exe` logs this on every SteamVR connect ("SteamVR recommended
render target: WxH per eye") - re-check that log line any time either
setting changes, since a different preset/percentage changes the number.

Measured on this rig: **3072x3264 per eye** (6144x3264 total canvas for the
SBS stream) with Godlike + SteamVR 100%. An earlier guess of 3072x3072 (i.e.
6144x3072) was silently cropping ~6% of the game's own vertical render before
it ever reached the encoder - always derive the canvas from the real number,
never assume a square-ish per-eye aspect.

This part of the pipeline (PC-side capture/encode) is unchanged by an OpenXR
headset-side rewrite - the mismatch this documents is between the *encoder's*
canvas and the *game's* real render, not between the encoder and the
headset's display.

## The precision bug that looked like a filtering/aliasing problem

Symptom: video looked aliased/noisy in the center of the view but smooth at
the edges, at any render-scale setting. This survived several wrong
diagnoses (barrel-distortion minification, missing mipmaps) before the actual
cause was found.

**Root cause**: the fragment shader declared `precision mediump float;`
(GLSL ES 1.00 requires an explicit precision statement for fragment shaders;
mediump was picked without thinking about it). The texel-addressing math
(`uUvOff + vUV * uUvScale`) ran at that precision before being handed to
`texture2D()`. mediump's quantization step is far coarser than one texel on a
6144px-wide texture, so the computed UV coordinate snapped between texels.

**Why only the center was visibly broken**: a FOV-fit dome projection puts
the *most* screen pixels per unit of source content right at the center of
the view (that is where angular resolution is highest); the periphery needs
much coarser UV steps per screen pixel, so the exact same absolute
quantization error was simply invisible there. A test pattern or visual check
limited to the edges of the frame will miss this class of bug entirely.

**Fix**: `precision highp float;` in the fragment shader (universally
supported on Quest's GPU; the mediump/highp choice was never actually
constrained by hardware, just an oversight).

**Action for the OpenXR client**: whatever shading language is used
(GLSL/HLSL/whatever the chosen graphics API demands), explicitly use full
(32-bit) float precision for any UV/texel-addressing math against a texture
wider than a few hundred pixels - do not rely on a language/platform default,
and do not assume a visual check at the edge of frame is representative of
the center, or vice versa.

## Video texture minification: mipmapping and a likely native-specific trap

Alongside the precision fix, the video texture was switched from plain
`LINEAR` filtering to trilinear (`LINEAR_MIPMAP_LINEAR`) with mipmaps
regenerated every frame (`gl.generateMipmap`), plus anisotropic filtering
where the extension is available. Minification without mipmaps aliases
instead of averaging down; this is real and additive to the precision bug
above; both were needed together.

**Likely trap for a native Android implementation**: if the native app takes
MediaCodec's decoded output via a `Surface` backed by
`GL_TEXTURE_EXTERNAL_OES` (the standard zero-copy path for hardware video
decode on Android), **that texture type does not support mipmapping** in
OpenGL ES - `glGenerateMipmap` on a `TEXTURE_EXTERNAL_OES` target is invalid.
If minification aliasing reappears in the native client, this is the first
thing to check: either blit/copy the external texture into a regular
`RGBA`/`TEXTURE_2D` texture each frame before mipmapping (extra copy cost,
but enables normal filtering), or investigate whether the chosen graphics API
exposes a different route to anisotropic sampling of external textures.

## Decode pipeline lessons that should transfer directly

These were learned against WebCodecs + a Web Worker, but the underlying
constraints are about video decoding and Android memory pressure in general,
not specific to running in a browser - expect the same failure modes against
MediaCodec in a native app unless deliberately designed around:

- **Keep the in-flight frame queue very shallow** (a handful of frames) and
  release/close each decoded frame immediately after it's consumed (uploaded
  to a texture, or in MediaCodec's case, released back with
  `releaseOutputBuffer`). A ~12.6MB decoded frame at this resolution, queued
  deeply, was enough to trip Android's low-memory killer and silently kill/
  reload the whole page - the native equivalent is a silently killed process,
  which will be *harder* to diagnose than a page reload.
- **Never decode AV1 in software.** Confirmed on this hardware: AV1 over
  WebRTC decoded at <1 fps despite the full bitrate arriving. Stick to the
  HEVC hardware decode path; treat AV1 as a fallback only if a given device
  is confirmed to hardware-decode it.
- **Demuxing/parsing cost must stay off any latency-critical thread.** An
  18-24MB segment took 35-41ms to demux on the main thread, a visible hitch
  once per second. Do this on a background thread in the native app too.
- **Do not pace playback against a media clock that gets re-based by a
  library you don't control** (this project's specific trap was hls.js
  re-basing live segment timestamps onto the video element's clock, which
  drifted ~2800s from the muxer's own timestamps and caused a black screen).
  A native app has no hls.js, but the general lesson holds: use a simple
  credit/queue-depth-based jitter buffer for pacing, not clock comparison
  across two different timelines.
- **A credit-deadlock is easy to introduce** with any queue-depth-based flow
  control between a decode thread/worker and the render/main thread (this
  project hit a state where the queue was empty, credits were exhausted, and
  nothing would ever refill - `dec 0/s held 72/s`). Whatever pacing mechanism
  the native app uses needs an explicit watchdog for this state, not just an
  assumption that credits and consumption stay in sync.

## PC-side pipeline quirks that remain true regardless of the client

Not fixed by a native headset-side rewrite - these live entirely on the PC
side of the pipeline and apply no matter what the headset runs:

- **UDP ingest (WHIP and SRT) silently drops packets above ~120-150 Mbps on
  loopback** - corrupts slices and keyframes, causes blocky artifacts, black
  flicker, wildly variable segment durations, and repeated muxer crashes.
  Fixed by moving OBS -> MediaMTX ingest to **RTMP over TCP**, which makes
  loss structurally impossible on loopback. RTMP cannot carry Opus audio -
  use AAC.
- A settings bridge that lets two independent writers (the viewer, and this
  project's PC-side control panel) share one JSON file must **merge** on
  write, not replace - an early version of `/settings` fully overwrote the
  file with only the fields present in a given POST body, so one side's
  update silently erased the other's.
- A "provision only" fast path must still run every step whose output later
  steps depend on - an early version of `Start-Spectator.ps1 -ProvisionOnly`
  exited *before* setting up the adb-reverse USB tunnel, so calling it from
  the control panel never actually brought up USB connectivity at all. When
  splitting setup into a "just provision" mode, audit every later step for
  hidden must-run-first dependencies, don't assume the early-exit point is
  safe to add without re-reading everything after it.
- OpenVR quirks specific to `VR180Mirror.exe` (the PC-side capture app):
  `GetProjectionRaw`'s tangent fields are swapped vs. their names (top/bottom
  reversed); never call `ReleaseMirrorTextureD3D11` per frame, only once per
  session (openvr#1888).

## Methodology worth repeating

Nearly every finding above came from *measuring* against the live
device/session rather than reasoning from spec sheets or documentation -
remote CDP debugging (`adb forward tcp:9222 localabstract:chrome_devtools_remote`
+ a WebSocket client driving `Runtime.evaluate`/`Input.dispatchMouseEvent`/
`Page.reload`) made it possible to read live WebXR state (`window.__fb`,
`RENDER_SCALE`, etc.) and even click through the UI without physically
wearing the headset for every iteration. When the native OpenXR app exists,
find or build the equivalent (Android's `adb logcat`, GPU/CPU telemetry via
`/devstats`-style polling, and whatever debugging hooks the OpenXR runtime
exposes) before trusting a spec number over a live measurement.
