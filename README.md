# VR180Mirror — watch a PCVR player's view live on a second Quest, in stereo VR180

One PC, two Meta Quest 3 headsets:

- **Player** plays any PCVR game through **SteamVR** (Virtual Desktop, Steam Link, or Link cable).
- **Spectator** puts on the second Quest and sees **exactly what the player sees** — both eyes,
  first person, projected as **side-by-side half, 180° equirectangular (lat/long)** — view only,
  no controllers needed.

```
 Player Quest 3 ──(VD / Steam Link)── SteamVR ──┐
                                                │ compositor mirror textures (L+R eyes)
                                                ▼
                                   VR180Mirror.exe  (this project)
                                   reprojects each eye's rectilinear view
                                   onto a 180° dome using the exact per-eye
                                   projection tangents → SBS half-equirect window
                                                │
                                                ▼
                                   OBS (Game Capture → NVENC HEVC 72fps → RTMP/TCP)
                                                │
                                                ▼
                                   MediaMTX  ── LL-HLS, decoded directly by the
                                                viewer (WebCodecs), ~1 s
                                                │
                                                ▼
                                   Spectator Quest 3, over a USB 3.0 cable ONLY
                                   (adb reverse tunnel — no Wi-Fi transport exists)
```

## USB cable only — read this first

The spectator transport is **USB, exclusively**. There is no Wi-Fi fallback: if the
adb-reverse tunnel isn't up, the viewer simply doesn't connect. This is by design — it's
the only transport that reliably carries the ~150 Mbps stream.

**You need a USB 3.0 cable and a USB 3.0 port on the PC.** USB 2.0 (cable or port) tops
out around 480 Mbps shared bandwidth and cannot carry this stream cleanly — expect stalls,
dropped frames, or a tunnel that never comes up. If your Quest's stock cable is USB-C but
only wired for USB 2.0 (many are), get a rated USB 3.0/3.1/3.2 USB-C cable.

## Known-good configuration

**v1.0.0 was the first fully stable state**; the pipeline now runs **HEVC 6144×3264 at a
constant 72 fps, ~150 Mbps** — per eye that's 3072×3264, matching exactly what SteamVR
reports as its recommended render target (`IVRSystem::GetRecommendedRenderTargetSize`,
also printed by `VR180Mirror.exe` on connect as "SteamVR recommended render target: WxH
per eye") with Virtual Desktop's Godlike profile and the SteamVR Video tab at 100%. The
canvas is sized from that number specifically so the pipeline never crops or stretches
the game's own render before encoding — re-check that log line if you change either
setting, since a different preset/percentage changes the number SteamVR asks for.
The Quest's own XR framebuffer (in the browser viewer) is scaled to match this exactly,
regardless of that headset's own default recommended resolution — see
[Player resolution matching](#player-resolution-matching) below.

If you change anything and it regresses, `docs/STABLE-v1.0.0.md` lists the
load-bearing settings, the traps that cost real debugging time, and a three-step
verification recipe (the resolution/bitrate numbers there predate the 6144×3264 upgrade
above, but the methodology still applies).

### Player resolution matching

`web/player.html` does not assume a fixed multiplier on top of whatever the spectator
Quest's browser considers its "recommended" XR framebuffer size — that default varies by
headset and has nothing to do with the panel's native resolution (a Quest 3 panel is
2064×2208/eye, but its own menu system renders around 1680×1760/eye by default). Instead,
on connect it fetches the actual encoder canvas size from `/info` (published by
`VR180Mirror.exe` via `mirror_status.json`, so it always matches whatever `-Codec`/
resolution is active) and solves for the exact `framebufferScaleFactor` that targets those
pixels 1:1, whatever they are. The `?ss=` query param still works as a multiplier on top of
that computed target for experiments (default 1.6, which is a no-op against the target).

## Start / stop

The **VR180 Console** (`tools\VR180Console.exe`, launched from the desktop shortcut) is
the recommended way to run this: one window with merged logs from every process, controls
for resolution/bitrate/target headset, a USB speed test, and a guaranteed clean shutdown —
closing it (even a forced Task Manager "End Task") kills every child process it started,
including OBS, with no confirmation dialog. See [Control panel](#control-panel-vr180consoleexe).

Or drive it from PowerShell directly:

```powershell
.\Start-Spectator.ps1                    # starts everything: HEVC 6144x3264 @ 72fps, 150 Mbps
.\Start-Spectator.ps1 -Bitrate 100000     # lower bitrate (kbps)
.\Start-Spectator.ps1 -Serial <adb-id>    # target a specific headset when more than one is plugged in
.\Start-Spectator.ps1 -TestGrid           # same, but streams a calibration grid (no SteamVR needed)
.\Connect-SpectatorUSB.ps1 [-Serial <id>] # (re-)establish the adb-reverse tunnel after a re-plug
.\Stop-Spectator.ps1                      # stops only this pipeline's processes
```

No firewall rules are needed — the whole path is a USB adb tunnel to `localhost`, nothing
listens for inbound LAN connections.

### Setting up on a new PC

Requirements: Windows 10/11, an NVIDIA GPU (NVENC), [OBS Studio ≥ 30](https://obsproject.com/),
[Node.js](https://nodejs.org/), Git, MSVC Build Tools (the free
[Visual Studio Build Tools](https://visualstudio.microsoft.com/downloads/) with the C++ workload),
and Android platform-tools (`adb`) for the USB tunnel.
Then just run `Start-Spectator.ps1` — on first run it builds `VR180Mirror.exe` (fetching the
OpenVR SDK), downloads MediaMTX, and installs the OBS profile + scene collection from `obs\`
(also available standalone as `Install-OBSProfile.ps1`).

The pipeline is order-independent and self-healing: VR180Mirror waits for SteamVR and switches
from the idle grid to the live feed automatically when the player starts playing; OBS retries the
stream; the spectator can join/leave any time.

## Player headset — one required setting

The capture reads the **SteamVR compositor**, so the game must run through SteamVR:

- **Virtual Desktop**: in the Streamer app (or VD's Streaming tab), set **OpenXR runtime = SteamVR**
  (not VDXR / not Automatic — Automatic runs MSFS, DCS and Vail on VDXR, which bypasses SteamVR).
- **Steam Link**: nothing to do (always SteamVR; set SteamVR as the OpenXR runtime if asked).
- **Link cable / Air Link**: launch the game in SteamVR mode (OpenVR titles do this automatically).

## Spectator headset

Plug the spectator Quest into a **USB 3.0** port (Developer Mode enabled), run
`Connect-SpectatorUSB.ps1` (or let `Start-Spectator.ps1`/the Console do it automatically),
then on the Quest: Browser → `http://localhost:9080/` → **Connect to stream** → **Enter VR**.
No certificate warning (localhost is a secure context); the whole stream rides the cable.

If more than one Quest is plugged in via USB, pass `-Serial <adb-device-id>` to
`Connect-SpectatorUSB.ps1` / `Start-Spectator.ps1` (see `adb devices` for the id), or pick it
from the dropdown in the Console.

The WebXR page requests a **72 Hz** headset refresh so the 72 fps stream plays with perfect
cadence (no 72-in-90 pulldown).

**Hard-lock to player's view** checkbox glues the dome to the spectator's head so they always
see the player's exact framing (intense! motion-sickness warning); unchecked (default) the dome
is world-fixed and the spectator can look around the 180° canvas freely.

## Control panel (`VR180Console.exe`)

`tools\VR180Console.cs`, built by `tools\build-console.ps1` into `tools\VR180Console.exe`, and
registered as an elevated one-click launch by `Setup-Console.ps1` (repoints the desktop
shortcut at a scheduled task, so it starts elevated without a UAC prompt every time). It:

- Runs `Start-Spectator.ps1 -ProvisionOnly` then spawns MediaMTX, the web server, VR180Mirror,
  and OBS itself, assigning every one of them to a Windows Job Object with `KILL_ON_JOB_CLOSE` —
  closing the console window, or force-killing it from Task Manager, terminates the entire
  pipeline (OBS included) immediately, with no "save changes?" prompt.
- Shows one merged, color-coded log view: stdout/stderr from every spawned process, OBS's own
  log file, and viewer-side errors reported by the Quest page (`web\client.log`).
- Lets you pick **resolution** (6144×3264 native / 4096×2048 / 3840×1920) and **bitrate**
  (50–200 Mbps) before starting, instead of editing `Start-Spectator.ps1` flags by hand.
- Lists connected Quest headsets (`adb devices`) in a dropdown when more than one is plugged
  in over USB, so you can pick which one the pipeline targets.
- Has a **Measure USB speed** button: times a throwaway `adb push` to the selected headset and
  reports the throughput, so you can confirm the cable/port are actually delivering USB 3.0
  speeds before starting the stream (warns if the measured throughput is close to or below the
  configured bitrate).

## What to expect

- The canvas is fitted to the game's real FOV (~114°×116°) and the viewer's dome is built with
  matching angles, so every pixel carries picture instead of black bars.
  `-VR180` restores the classic full-180 canvas.
- The view is head-locked to the **player's** head (that is the point!). Spectators sensitive to
  motion sickness should sit down.
- SteamVR overlays/dashboard the player sees are included (it mirrors the compositor output).
- When the player's headset is streamed (VD/Steam Link), SteamVR applies FOV culling during fast
  head turns — brief black wedges at the edges of the mirrored image are normal and not a bug.

## Components

| Piece | Where | Job |
|---|---|---|
| `bin\VR180Mirror.exe` | built by `build.ps1` from `src\main.cpp` | OpenVR background app: grabs both compositor mirror textures, GPU-reprojects to SBS half-equirect (per-eye raw projection tangents + eye-to-head rotation), presents a 6144×3264 window |
| OBS profile+collection `VR180Mirror` | templates in `obs\`, synced to `%APPDATA%\obs-studio` by the launcher | Game Capture of that window → NVENC HEVC 6144×3264@72 CBR 150 Mbps (preset p4, AAC audio) → RTMP |
| `tools\mediamtx\` | MediaMTX | RTMP ingest 127.0.0.1:1936 (TCP - lossless), LL-HLS out :9888, RTSP :9554 for frame-exact local pulls, local API :9998 |
| `web\server.js` | Node | HTTP :9080, reached over the adb-reverse USB tunnel: serves the WebXR player, proxies MediaMTX's LL-HLS under `/hls`, `/info`, `/devstats`, `/settings`, `/clientlog` |
| `web\player.html` | | WebXR viewer. Renders the dome itself in WebGL and gets frames from `wcworker.js` — the Quest browser's video→GL path serves stale frames (measured: 0.7 distinct frames/s while the element "presents" 72), which is why `XRMediaBinding` and plain `<video>` textures both stuttered |
| `web\wcworker.js` | | Decode worker: fetches the video-only LL-HLS rendition, demuxes (mp4box), hardware-decodes (WebCodecs `VideoDecoder`), posts GPU `VideoFrame`s. Credit-based flow control keeps only ~3 frames (12.6MB each) in flight. Verified in-headset: 72 fps, p99 frame interval 14.8ms, 0 discards, ~0.8s latency |
| `tools\VR180Console.exe` | built by `tools\build-console.ps1` from `tools\VR180Console.cs` | Single-window launcher/log-merger/control-panel; see above |

Ports used (all loopback-only, reached via the adb-reverse USB tunnel): 9080 (web/HLS), 9888
(MediaMTX LL-HLS), 1936 (RTMP ingest), 9554 (RTSP diagnostics), 9998 (MediaMTX API).

## Tuning

- **Resolution/fps/codec**: defaults to **HEVC 6144×3264 @ 72 fps, 150 Mbps CBR** (NVENC HEVC →
  Quest 3 hardware decode via WebCodecs), with the Quest's XR framebuffer at **1.6x** its native
  scale — 1:1 with the stream (see "Known-good configuration" above). `-Codec h264` (3840×1920)
  and `-Codec av1` (4096×2048) remain available as lower-resolution OBS encoder options; `av1`
  decodes in software over this pipeline's viewer path and is not recommended.
- **Bitrate**: launcher flags — `-Bitrate <kbps>` (target; also the floor) and optional
  `-MaxBitrate <kbps>` (ceiling; when set above the target the encoder runs VBR between the two,
  otherwise constant bitrate). Default 150 Mbps CBR at HEVC/AV1, 80 Mbps at H264. 150 Mbps is a
  *decoder* choice, not a bandwidth one — the USB tunnel has far more headroom than that, but
  HEVC entropy decoding on the headset falls behind above ~150 Mbps.
- **Target headset**: `-Serial <adb-device-id>` on `Start-Spectator.ps1` / `Connect-SpectatorUSB.ps1`
  when more than one Quest is connected via USB (or pick it in the Console's dropdown).
- **Stats HUD in VR** (checkbox, on by default): a head-locked panel at the centre of view showing
  our own pipeline — `XR fps`, `shown`, `dec`, `disc` (discarded for rate matching), `held` (XR
  frames with no new frame), queue depths — plus the headset's own telemetry pulled over adb
  (`/devstats`: compositor fps, stale frames, GPU busy %, clocks, temperature) and the latency
  estimate. Use it to tell a *delivery* problem from a *decode* or *compositor* problem.
- **Supersampled source**: Virtual Desktop Godlike renders 3072×3216 per eye on Quest 3 (a 1.46×
  linear supersample of the 2064×2208 panel); the reprojection box-filters it with a 2×2
  supersampling tap (`--no-ss` disables) so the downsample to the stream happens once, cleanly,
  on the PC.
- **VR180Mirror flags**: `--swap-eyes` (if stereo feels inverted), `--flip-v` (if image is upside
  down), `--feather <deg>` (edge softening, default 1.5), `--test-grid`, `--preview <px>`,
  `--dump-frame out.bmp` (writes one frame and exits — handy for checks).

## Troubleshooting

- **Spectator can't connect at all** → the adb-reverse tunnel isn't up: re-run
  `Connect-SpectatorUSB.ps1` (or the Console's reconnect), confirm `adb devices` shows the
  headset as `device` (not `unauthorized`/`offline`), and confirm the cable/port are USB 3.0
  with the Console's **Measure USB speed** button.
- **Black video in OBS/stream** → don't minimize the VR180Mirror window (background is fine).
- **Grid shows but game never appears** → SteamVR isn't the compositor: check the VD OpenXR
  runtime setting (see above). The VR180Mirror console logs the connection state.
- **Stereo feels wrong/painful** → `--swap-eyes` in the launcher's `$mirrorArgs`.
- **Choppy/low-bitrate stream despite a USB 3.0 cable** → measure actual throughput with the
  Console's speed test; a cable that *looks* USB-C can still be wired for USB 2.0 only.
- **Multiple headsets confuse the tunnel** → pass `-Serial <id>` (or use the Console's dropdown)
  so the pipeline targets the intended spectator headset.

## Rebuilding

```powershell
.\build.ps1              # needs MSVC Build Tools; outputs bin\VR180Mirror.exe
.\tools\build-console.ps1  # rebuilds tools\VR180Console.exe (in-box csc.exe, no SDK needed)
```

## Considering a native OpenXR client

The current spectator viewer runs as a WebXR page in the Quest's browser
(`web/player.html`). It works, but the browser's own WebXR implementation
imposes a resolution ceiling well below the panel's real capability (measured
at exactly 1.5x a reported "1.0x" baseline on one headset - see
`docs/OPENXR-MIGRATION-NOTES.md`). That doc records every quirk, bug, and
measurement this project already paid to learn - the shader precision bug,
video-texture mipmapping, decode-pipeline memory traps, and more - so a future
native rewrite doesn't have to re-discover any of them from scratch.

## How it came to be / design notes

- Studied the local **VRto3D** clone (SteamVR virtual-HMD driver): confirmed the direct-mode frame
  path, the canonical-SBS-then-repack pattern, and OpenVR's raw-projection conventions
  (`GetProjectionRaw` returns half-angle tangents; the *bottom* value is the **up** edge — the
  names are historically swapped; the mirror texture's 0..1 UV maps exactly to those tangents).
- VRto3D itself can't do this job (it *replaces* the HMD; the player needs their Quest to be the
  HMD), so capture happens app-side via `IVRCompositor::GetMirrorTextureD3D11` — full composited
  per-eye view, overlays included. The mirror SRVs are acquired **once** and never released
  per-frame (SteamVR bug openvr#1888: per-frame Get/Release → stale frames + VRAM leak).
- Nothing off-the-shelf does this end-to-end (ALVR/Virtual Desktop/Steam Link are single-headset;
  Meta casting is flat mono; no SteamVR→VR180 tool exists) — hence this project.
- Alternative viewers that also work with this stream: Twinkle Video Player (free, Meta Store,
  NDI — would need the DistroAV OBS plugin instead), Whirligig NDI, VR.NDI.
