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
                                   OBS (Game Capture → NVENC AV1 72fps → WHIP)
                                                │
                                                ▼
                                   MediaMTX     WebRTC (WHEP)  ← ~0.3-0.5 s latency
                                                LL-HLS         ← 2-6 s (DeoVR fallback)
                                                │
                                                ▼
                                   Spectator Quest 3 (browser WebXR page or DeoVR)
```

## Start / stop

```powershell
.\Start-Spectator.ps1              # starts everything: AV1 6144x3072 @ 72fps, 150 Mbps
.\Start-Spectator.ps1 -Codec h264  # compatibility mode: H.264 3840x1920 @ 72fps, 80 Mbps
.\Start-Spectator.ps1 -TestGrid    # same, but streams a calibration grid (no SteamVR needed)
.\Connect-SpectatorUSB.ps1         # route the stream over the spectator's USB cable (adb reverse)
.\Stop-Spectator.ps1               # stops only this pipeline's processes
```

First run: approve the **UAC prompt** (firewall rules for the spectator's inbound connections)
— or run `Setup-Firewall.ps1` once manually.

### Setting up on a new PC

Requirements: Windows 10/11, an NVIDIA GPU (NVENC), [OBS Studio ≥ 30](https://obsproject.com/),
[Node.js](https://nodejs.org/), Git, and MSVC Build Tools (the free
[Visual Studio Build Tools](https://visualstudio.microsoft.com/downloads/) with the C++ workload).
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

## Quest launcher app (recommended): `quest-app\VR180Spectator.apk`

A tiny sideloaded 2D app for the spectator Quest — **USB-cable only, by design** (the cable is
the only transport that guarantees the max-quality 150 Mbps stream; if the tunnel isn't up, the
app says so instead of silently degrading to Wi-Fi). It checks the adb-reverse tunnel every 2 s,
shows PC/stream status (live, codec tracks), remembers three checkboxes — **Hard-lock view**,
**Start muted**, **Full picture (no edge softening)** — and one WATCH button opens the WebXR
viewer with everything pre-applied (one more click inside: Enter VR).

- Install once: `adb install -r quest-app\VR180Spectator.apk` (or drag into SideQuest).
  Appears under Library → Unknown Sources as "VR180 Spectator".
- Rebuild: `quest-app\build-apk.ps1` (raw aapt2/d8/apksigner toolchain — no Gradle).
- The "Full picture" toggle is live: the viewer posts it to `/settings`, the web server writes
  `bin\runtime.json`, and VR180Mirror.exe applies it within a second (it removes the edge-fade
  feather; any rounded corners that remain are the game's own lens mask — those pixels are never
  rendered by the game and cannot be recovered).
- `Start-Spectator.ps1` auto-establishes the adb-reverse tunnels when it finds exactly one
  headset connected; re-run it (or `Connect-SpectatorUSB.ps1`) after re-plugging the cable.

## Spectator headset — browser routes (no app needed)

| Route | Latency | Setup |
|---|---|---|
| **A. USB cable (max quality/reliability)** | ~0.3-0.5 s | Plug the spectator Quest into the PC (USB 3, Developer Mode on), run `Connect-SpectatorUSB.ps1`, then Quest Browser → `http://localhost:9080/` → **Connect** → **Enter VR**. No certificate (localhost is a secure context); the player page pins WebRTC to the loopback tunnel so the video rides the cable, immune to Wi-Fi. |
| **B. Wi-Fi WebXR page** | ~0.3-0.5 s | Quest Browser → `https://<PC-IP>:8443/` → accept the certificate warning (Advanced → proceed) → **Connect to stream** → **Enter VR** |
| **C. Browser built-in 180 mode (no certificate)** | ~0.3-0.5 s | Quest Browser → `http://<PC-IP>:9889/vr180` → fullscreen the video → pick **180°** and **3D left-right** in the video controls |
| **D. DeoVR (HLS)** | 2-6 s | Start with `-Codec h264`, then DeoVR's browser → `http://<PC-IP>:9080/` (auto-detects 180° SBS) |

The WebXR page also requests a **72 Hz** headset refresh so the 72 fps stream plays with perfect
cadence (no 72-in-90 pulldown). A USB-C-to-Ethernet adapter on the spectator Quest works too
(no Developer Mode needed) — then just use route B over the wired LAN.

The launcher prints the exact URLs with the PC's current IP.

Route A extras: **Hard-lock to player's view** checkbox glues the dome to the spectator's head so
they always see the player's exact framing (intense! motion-sickness warning); unchecked (default)
the dome is world-fixed and the spectator can look around the 180° canvas freely.

## What to expect

- The game renders ~100-110° FOV; on the 180° dome that content occupies the central region at
  **geometrically correct angular size**, with black beyond — this is correct VR180, not stretching.
- The view is head-locked to the **player's** head (that is the point!). Spectators sensitive to
  motion sickness should sit down.
- SteamVR overlays/dashboard the player sees are included (it mirrors the compositor output).
- When the player's headset is streamed (VD/Steam Link), SteamVR applies FOV culling during fast
  head turns — brief black wedges at the edges of the mirrored image are normal and not a bug.

## Components

| Piece | Where | Job |
|---|---|---|
| `bin\VR180Mirror.exe` | built by `build.ps1` from `src\main.cpp` | OpenVR background app: grabs both compositor mirror textures, GPU-reprojects to SBS half-equirect (per-eye raw projection tangents + eye-to-head rotation), presents a 4096×2048 window |
| OBS profile+collection `VR180Mirror` | templates in `obs\`, synced to `%APPDATA%\obs-studio` by the launcher | Game Capture of that window → NVENC AV1 6144×3072@72 CBR 150 Mbps → WHIP |
| `tools\mediamtx\` | MediaMTX v1.20.0 | WebRTC server: WHIP ingest :9889, WHEP out, LL-HLS out :9888, media UDP+TCP :9189 (TCP = USB tunnel), local API :9998 |
| `web\server.js` | Node | HTTPS :8443 serves the WebXR player + proxies WHEP (single cert acceptance); HTTP :9080 serves the DeoVR JSON |
| `web\player.html` | | WHEP WebRTC client + WebXR viewer using `XRMediaBinding.createEquirectLayer` (`stereo-left-right`, 180°) — the Quest-accelerated zero-copy video path |

Ports (chosen to coexist with other streaming stacks): 8443, 9080, 9888, 9889/TCP, 9189/UDP.

## Tuning

- **Resolution/fps/codec**: defaults to **AV1 6144×3072 @ 72 fps, 150 Mbps CBR** (NVENC AV1 →
  Quest 3 hardware AV1 decode). This is the practical maximum at a constant 72 fps: it consumes
  ~68% of the Quest 3 decoder's rated 8K60 (~2 Gpx/s) budget, and across the game's ~104° FOV it
  delivers ~1780 of the spectator panel's 2064 native horizontal pixels (~86%). `-Codec h264`
  drops to 3840×1920@72 — Meta's browser caps H.264 at 4K and level 5.2 tops out there anyway;
  use it for DeoVR or non-Quest-3 viewers. The launcher syncs the OBS profile/canvas
  automatically at each start (while OBS is closed). Verified sustained on an RTX 5090
  (719 frames / 10.0 s, 150.1 Mb/s measured).
- **Bitrate**: OBS → Settings → Output → Streaming (AV1 150 Mbps / H264 80 Mbps defaults; USB or
  wired LAN handles it — drop to 40-60 on marginal Wi-Fi).
- **Supersampled source**: Virtual Desktop Godlike renders 3072×3216 per eye on Quest 3 (a 1.46×
  linear supersample of the 2064×2208 panel); the reprojection box-filters it with a 2×2
  supersampling tap (`--no-ss` disables) so the downsample to the stream happens once, cleanly,
  on the PC. Streaming the raw supersampled mirror instead would add nothing: the spectator-side
  decoder is the bottleneck at 72 fps, not the downsample.
- **If you'd trade fps for pixels**: at 60 fps the decoder budget allows ~7680×3840 (edit `$fps`
  and the av1 size in `Start-Spectator.ps1`) — that saturates the spectator panel across the
  game FOV. 72 fps constant is the default per this project's priority.
- **VR180Mirror flags**: `--swap-eyes` (if stereo feels inverted), `--flip-v` (if image is upside
  down), `--feather <deg>` (edge softening, default 1.5), `--test-grid`, `--preview <px>`,
  `--dump-frame out.bmp` (writes one frame and exits — handy for checks).

## Troubleshooting

- **Spectator can't connect at all** → firewall rules missing: run `Setup-Firewall.ps1` (UAC).
- **Black video in OBS/stream** → don't minimize the VR180Mirror window (background is fine).
- **Page loads but "WebXR blocked: not a secure context"** → the certificate warning wasn't
  accepted; reload `https://…:8443` and proceed through Advanced. Route B needs no certificate.
- **Grid shows but game never appears** → SteamVR isn't the compositor: check the VD OpenXR
  runtime setting (see above). The VR180Mirror console logs the connection state.
- **Stereo feels wrong/painful** → `--swap-eyes` in the launcher's `$mirrorArgs`.
- **Latency creeping up in route A** → tap Reconnect on the page (WebRTC recovers to ~0.3 s).
- **DeoVR shows flat/wrong projection** → open settings in DeoVR player and set 180° + SBS.
- **Other streaming software on this PC** (ports 8889/8189/8554/8080) is untouched; this stack
  uses its own ports and `Stop-Spectator.ps1` only kills processes it started.

## Rebuilding

```powershell
.\build.ps1     # needs MSVC Build Tools; outputs bin\VR180Mirror.exe
```

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
  NDI — would need the DistroAV OBS plugin instead of WHIP), Whirligig NDI, VR.NDI.
