# World-lock head stabilization

When the player turns their head, the streamed image used to slide around
inside the spectator's dome, because the canvas is authored in the player's
**head space** (see `dirFromEquirect` in `src/main.cpp` and `buildDome()` in
`web/player.html` - both use the identical UV->direction mapping). This
feature counter-rotates the dome by the player's own HMD orientation so a
point in the game world - the "candle on the table" test - stays fixed for
the spectator instead of swinging with the player's head, up to the edge of
whatever the player actually rendered.

**Only rotation is compensated.** There is no depth information in the
stream, so the player's own translation (walking, leaning) cannot be undone.

**World-lock is absolute, by design.** There is no dead zone or auto-recenter:
if the player turns 90° and holds it, the spectator has to turn their own
body to follow, or look at black (there is no pixel data outside the
player's rendered field of view - the ~114°x116° FOV-fit patch is a real,
hard edge). Press **Recenter** (or toggle the checkbox off/on) to re-anchor
to however the player is currently facing.

## The math

- `q_p` = the player's HMD orientation (head -> world), sampled by
  `VR180Mirror.exe` once per presented frame via
  `IVRCompositor::GetLastPoseForTrackedDeviceIndex` - the compositor's actual
  **render** pose, not a freshly predicted one (predicting "now" would
  describe a different instant than the frame being captured).
  `WaitGetPoses` cannot be used here: the app runs as
  `vr::VRApplication_Background`, and that call fails with
  `VRCompositorError_IsNotSceneApplication` under that application type.
- `q_ref` = the orientation captured at Recenter (or automatically, on
  entering VR) - it exists only to align the game's world frame with the
  spectator's WebXR `local` space, which are otherwise arbitrary relative to
  each other.
- Dome model matrix: **`M = conj(q_ref) * q_p`**.
- Per eye: `mvp = P * V_rot * M` in the default (free-look) mode, or
  `P * M` in hard-lock mode - both compose the same way, since `M` is a model
  matrix independent of whichever view matrix is used.
- OpenVR and WebXR share the same convention (right-handed, Y-up, -Z forward),
  so the quaternion transfers directly - no handedness conversion needed.

## Synchronizing the pose to the frame actually on screen

The stream is roughly 1 second behind the player. Using the *current* pose
would be worse than not stabilizing at all - at a plausible ~100°/s head
turn, matching within 1° of error requires matching the *time* within ~10ms.

```
t_capture(frame) = frame.timestamp/1000 + C - K

C = median(pdtMs - ptsUs/1000)   over the last ~15 segments (a per-session constant)
K = capture -> ingest delay (OBS encode + RTMP), a few tens of ms, calibrated once
```

- **`frame.timestamp`** is the decoded `VideoFrame`'s container PTS (µs) -
  it already exists on every frame delivered by `web/wcworker.js` and was
  simply never read before this feature.
- **`C`** comes for free: MediaMTX's gohlslib muxer stamps every LL-HLS
  segment with `#EXT-X-PROGRAM-DATE-TIME` (`muxer_stream.go`:
  `DateTime: &seg.startNTP`), which the demux worker now parses and pairs
  with the PTS of that segment's first sample (`wcworker.js`, the `pdt`
  message). The player keeps a rolling median of the last 15 samples -
  robust to per-segment jitter in exactly when the muxer stamps `startNTP`.
  It is re-estimated from scratch after any parser reset (stream restart,
  MSE fallback), since the PTS epoch itself restarts then.
- **`K`** is the one number that has to be calibrated by hand: the fixed
  delay through OBS's encoder and the RTMP hop into MediaMTX. Set it via the
  **Sync offset (ms)** field (on the headset page or in `VR180Console.exe` -
  both write the same `/settings` bridge) and watch for drift while the
  player turns at a roughly constant rate: a nonzero residual drift means K
  is off, and the drift direction flips sign as you cross the correct value.

Why indexing by PTS instead of any other frame identity: frames are dropped
silently in three places in this pipeline (queue overflow, catch-up shedding,
and a worker-side GOP skip under backlog) via plain `Array.shift()`. Since
the PTS travels *inside* the `VideoFrame` object itself, none of those drop
paths need to know about pose lookup at all - there is nothing to leak or
desync.

## Pose transport

`VR180Mirror.exe` samples the pose every render frame, keeps the last ~250
samples (~3.5s at 72fps) in an in-memory ring buffer, and flushes them to
`bin/poses.json` roughly every 200ms from its I/O thread - written atomically
(temp file + rename) so `web/server.js`'s `GET /poses?since=<ms>` (polled by
the viewer every 250ms) never reads a torn file. This differs from the older
`bin/mirror_status.json`, which uses a plain non-atomic write.

## Known limits

- **No translation compensation** - only the player's head *orientation* is
  countered. Their positional movement (walking, leaning) has no depth data
  to be undone with.
- **The FOV-fit patch is a hard edge.** Outside the ~114°x116° the player
  actually rendered, there is no pixel data - a soft vignette (new
  `uVignette` fragment shader uniform) fades the dome's own border instead of
  a hard black cut, since world-lock swings that border into view far more
  often than the old head-locked-content mode did.
- **Timewarp residual**: the mirror texture may be sampled post-timewarp
  while `GetLastPoseForTrackedDeviceIndex` returns the pre-timewarp render
  pose, which could leave a small residual. If this becomes visible, the
  alternative is `IVRCompositor::GetFrameTiming().m_HmdPose`, which also
  carries `m_nFrameIndex` - useful if PTS/PDT synchronization (above) ever
  proves insufficiently stable and a literal per-frame watermark becomes
  necessary instead.
