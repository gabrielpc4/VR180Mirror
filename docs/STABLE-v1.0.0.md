# The known-good configuration (v1.0.0) and how we got here

This is the first configuration that ran clean end to end: **HEVC 4096x2048 at a
constant 72 fps, ~150 Mbps, verified with 0 decode errors and 0 held frames over
1224 consecutive frames of live gameplay.** Everything below is either a setting
that matters or a trap that cost real debugging time. If something regresses,
start here.

## The chain

```
Player's Quest 3 --(Virtual Desktop / Link, OpenXR runtime = SteamVR)--> SteamVR
  -> VR180Mirror.exe    reads both compositor mirror textures, reprojects each
                        eye onto an equirect dome sized to the real FOV, presents
                        a 4096x2048 SBS frame at 72 fps
  -> OBS                Game Capture of that window, NVENC HEVC, RTMP to
  -> MediaMTX           localhost:1936 -> LL-HLS :9888 + WebRTC :9889
  -> web/server.js      HTTPS :8443 / HTTP :9080, /hls proxy, /info, /devstats
  -> viewer page        worker fetches the video-only rendition, demuxes with
                        mp4box, decodes with WebCodecs (hardware), uploads GPU
                        frames to a WebGL dome in the XR frame
  -> Spectator's Quest 3 over the USB cable (adb reverse), via the launcher APK
```

## Settings that are load-bearing

| Setting | Value | Why it matters |
|---|---|---|
| Ingest transport | **RTMP (TCP) on 127.0.0.1:1936** | SRT and WHIP are UDP; at 120-150 Mbps on loopback both dropped packets, which destroyed slices *and* keyframes. TCP makes loss impossible. |
| Audio codec | **AAC** | RTMP cannot carry Opus (MediaMTX: "unsupported object type: 9"). |
| Video codec / size | **HEVC 4096x2048@72** | 4096 wide is the reliable fast path; HEVC/AV1 hardware-decode on Quest 3, H.264 is capped at 4K by the browser. |
| Canvas | **FOV-fit** (~114x116 deg) | A full 180 canvas spends ~40% of its width on black. Fitting the real FOV yields ~94% of panel-native detail at the same pixel count. |
| `hlsSegmentMaxSize` | **250M** | A 1s segment is ~19MB; with lost keyframes segments grew to 8s and blew past MediaMTX's 50MB default, crashing the muxer. |
| Source present rate | **1x stream fps (72)** | 2x was a workaround for render-loop stalls; with the stalls fixed it only wastes GPU the player's game wants (the capture hook copies the whole backbuffer per present). |
| Decode path | **WebCodecs in a worker** | The Quest browser's video->GL path serves stale frames (measured: 0.7 distinct frames/s while the element "presented" 72), which also breaks `XRMediaBinding`. |
| XR layer | **no MSAA, no depth, fixedFoveation, `texSubImage2D`** | A video dome needs neither; the wasted fill rate made the browser miss frame deadlines, so the compositor re-projected stale frames 1-6x/s. |

## Traps, in the order they bit

1. **Nothing off-the-shelf does this.** ALVR, Virtual Desktop and Steam Link are
   single-headset; Meta casting is flat mono. The pipeline had to be built.
2. **`GetProjectionRaw`'s "top"/"bottom" are swapped** relative to their names,
   and the mirror's 0..1 UV maps exactly to those tangents.
3. **Never call `ReleaseMirrorTextureD3D11` per frame** (openvr#1888: stale
   frames and runaway VRAM). Acquire once per session.
4. **PowerShell 5.1 traps**: a BOM-less UTF-8 .ps1 is parsed as ANSI, so an
   em-dash in a quoted string breaks parsing; `Set-Content -Encoding utf8`
   writes a BOM that MediaMTX's YAML parser rejects. Keep scripts ASCII and
   write YAML with a BOM-less writer.
5. **Quest browser WebRTC decodes AV1 in software.** The full 150 Mbps arrived
   over USB and rendered under 1 fps. HEVC over the buffered path is hardware.
6. **The XR media layer falls off its fast path above ~4K width.** 6144-wide
   video played at ~10 fps in VR while decoding fine at 72.
7. **Deep VideoFrame queues trip Android's low-memory killer** - one 4096x2048
   frame is ~12.6MB, and the tab reloaded itself mid-session. Keep ~3 in flight
   and close each frame right after upload.
8. **Demuxing an 18MB segment costs 35-41ms.** On the render thread that is a
   visible hitch every second; it belongs in a worker.
9. **Do not pace playback against the media element's clock.** hls.js re-bases
   live segments, so element time and muxer timestamps were ~2800s apart and the
   gate never opened (black screen). A plain jitter buffer is correct.
10. **Diagnostics can be the bug.** A `printf` to the console and a blocking
    `VR_Init` retry on the render thread stalled it ~0.75x/s, which made the
    capture sample the same frame twice.
11. **Measure the right thing.** `mpdecimate` calls near-identical frames
    duplicates (useless on a static test pattern); the test grid repeats exactly
    every 4s, so "unique frames" over-counts; and a diagnostic ffmpeg reading
    RTSP at 120 Mbps was itself "too slow" and lost frames. Compare *adjacent*
    frame hashes, and read counters from the pipeline rather than the player.

## How to verify a build quickly

```bash
# 1. every segment should be exactly 1.000s with 72 frames and 1 keyframe
curl -s "http://127.0.0.1:9080/hls/index.m3u8"      # -> video1_stream.m3u8?session=...
# 2. pull init + one segment, concatenate, then:
ffprobe -select_streams v -show_entries packet=flags -of csv one.mp4   # 72 packets, 1 K
ffmpeg -i one.mp4 -f null -                                            # 0 errors
# 3. in-headset: the HUD line should read
#    shown 72/s  dec 72/s  disc 0/s  held 0/s   and HMD ... stale 0
```
