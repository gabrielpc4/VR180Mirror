// VR180Mirror decode worker: demuxes fMP4 (mp4box.js) and hardware-decodes
// HEVC/AV1/H264 off the main thread, posting GPU-backed VideoFrames back.
//
// Why a worker: at 150 Mbps a 1-second segment is ~18MB, and demuxing it took
// 35-41ms. On the render thread that produced a visible hitch every second
// (measured: p99 frame interval 37-58ms against a 13.9ms median).
//
// Flow control is credit-based: the page grants one credit per frame it
// consumes, so decoding runs exactly at display rate and only a few 12.6MB
// frames exist at once (deep queues tripped Android's low-memory killer).

importScripts("mp4box.all.min.js");

let mp4 = null, decoder = null, cfg = null, offset = 0;
let pending = [], credits = 0, lastDepthPost = 0;
const PENDING_MAX = 260;   // undecoded (compressed) chunks

const log = (m) => postMessage({ type: "log", msg: String(m) });

function ensureFile() {
  if (mp4) return;
  mp4 = MP4Box.createFile();
  mp4.onError = (e) => log("mp4box: " + e);

  mp4.onReady = (info) => {
    const tr = info.videoTracks && info.videoTracks[0];
    if (!tr) { log("no video track"); return; }
    let desc = null;
    try {
      const trak = mp4.getTrackById(tr.id);
      for (const entry of trak.mdia.minf.stbl.stsd.entries) {
        const box = entry.avcC || entry.hvcC || entry.av1C || entry.vpcC;
        if (box) {
          const ds = new DataStream(undefined, 0, DataStream.BIG_ENDIAN);
          box.write(ds);
          desc = new Uint8Array(ds.buffer, 8);   // strip the box header
          break;
        }
      }
    } catch (e) { log("desc: " + e); }

    decoder = new VideoDecoder({
      output: (frame) => postMessage({ type: "frame", frame }, [frame]),
      error: (e) => log("decoder: " + e.message),
    });
    cfg = { codec: tr.codec, optimizeForLatency: true, hardwareAcceleration: "prefer-hardware" };
    if (desc) cfg.description = desc;
    try {
      decoder.configure(cfg);
      postMessage({ type: "ready", codec: tr.codec, w: tr.video && tr.video.width, h: tr.video && tr.video.height });
    } catch (e) { log("configure: " + e.message); return; }

    mp4.setExtractionOptions(tr.id, null, { nbSamples: 30 });
    mp4.start();
  };

  mp4.onSamples = (id, user, samples) => {
    for (const s of samples) {
      pending.push({
        key: !!s.is_sync,
        chunk: new EncodedVideoChunk({
          type: s.is_sync ? "key" : "delta",
          timestamp: Math.round(s.cts * 1e6 / s.timescale),
          duration: Math.round(s.duration * 1e6 / s.timescale),
          data: s.data,
        }),
      });
    }
    drain();
  };
}

function drain() {
  if (!decoder || decoder.state !== "configured") return;

  // far behind live (page not consuming, or a long stall): jump to the newest
  // keyframe rather than grinding through a backlog
  if (pending.length > PENDING_MAX) {
    let k = -1;
    for (let i = pending.length - 1; i > 0; i--) { if (pending[i].key) { k = i; break; } }
    if (k > 0) {
      pending.splice(0, k);
      try { decoder.reset(); decoder.configure(cfg); } catch (e) { log("reconfig: " + e.message); }
    }
  }

  while (credits > 0 && pending.length && decoder.decodeQueueSize < 4) {
    const p = pending.shift();
    credits--;
    try { decoder.decode(p.chunk); } catch (e) { log("decode: " + e.message); }
  }

  const now = Date.now();
  if (now - lastDepthPost > 250) {
    lastDepthPost = now;
    postMessage({ type: "depth", pending: pending.length, dq: decoder.decodeQueueSize });
  }
}

// ---------------------------------------------------------------------------
// Direct LL-HLS video fetcher (no MSE, no <video> element for video).
// Keeping hls.js/MSE in the loop meant the browser hardware-decoded the same
// 4K stream a second time (it reported ~13% dropped frames) and appended 18MB
// segments on the main thread. Here the worker pulls the video rendition
// itself, so the main thread does nothing but upload one texture per frame.
// ---------------------------------------------------------------------------
let fetcher = null;

async function fetchFeed(url) {
  const resp = await fetch(url, { cache: "no-store" });
  if (!resp.ok) throw new Error("HTTP " + resp.status + " for " + url);
  const buf = await resp.arrayBuffer();
  ensureFile();
  buf.fileStart = offset;
  offset += buf.byteLength;
  try { mp4.appendBuffer(buf); } catch (e) { log("append: " + e); }
  drain();
}

function parsePlaylist(txt, baseUrl) {
  const lines = txt.split(/\r?\n/);
  const out = { mediaSeq: 0, initUri: null, segs: [] };
  for (let i = 0; i < lines.length; i++) {
    const l = lines[i].trim();
    if (l.startsWith("#EXT-X-MEDIA-SEQUENCE:")) {
      out.mediaSeq = parseInt(l.slice(l.indexOf(":") + 1), 10) || 0;
    } else if (l.startsWith("#EXT-X-MAP:")) {
      const m = l.match(/URI="([^"]+)"/);
      if (m) out.initUri = new URL(m[1], baseUrl).href;
    } else if (l.startsWith("#EXTINF")) {
      for (let j = i + 1; j < lines.length; j++) {
        const u = lines[j].trim();
        if (u && !u.startsWith("#")) { out.segs.push(new URL(u, baseUrl).href); break; }
      }
    }
  }
  return out;
}

async function startDirect(masterUrl) {
  fetcher = { running: true };
  try {
    // master -> video variant (MediaMTX puts audio in a separate rendition, so
    // this variant is video-only, which is exactly what we want)
    const masterTxt = await (await fetch(masterUrl, { cache: "no-store" })).text();
    let variantUrl = masterUrl;
    const ml = masterTxt.split(/\r?\n/);
    for (let i = 0; i < ml.length; i++) {
      if (ml[i].startsWith("#EXT-X-STREAM-INF")) {
        for (let j = i + 1; j < ml.length; j++) {
          const u = ml[j].trim();
          if (u && !u.startsWith("#")) { variantUrl = new URL(u, masterUrl).href; break; }
        }
        break;
      }
    }
    postMessage({ type: "log", msg: "direct variant: " + variantUrl });

    let initFetched = false, nextSeq = -1, idle = 0;
    while (fetcher && fetcher.running) {
      const txt = await (await fetch(variantUrl, { cache: "no-store" })).text();
      const pl = parsePlaylist(txt, variantUrl);
      if (pl.initUri && !initFetched) { await fetchFeed(pl.initUri); initFetched = true; }
      if (nextSeq < 0) {
        // start one segment back from the live edge: enough to prime, minimal latency
        nextSeq = pl.mediaSeq + Math.max(0, pl.segs.length - 2);
      }
      let got = 0;
      for (let k = 0; k < pl.segs.length; k++) {
        const seq = pl.mediaSeq + k;
        if (seq >= nextSeq) {
          await fetchFeed(pl.segs[k]);
          nextSeq = seq + 1;
          got++;
        }
      }
      // if the playlist rolled past us (we fell behind), rejoin at the edge
      if (pl.mediaSeq > nextSeq) nextSeq = pl.mediaSeq;
      idle = got ? 0 : idle + 1;
      await new Promise((r) => setTimeout(r, got ? 120 : 250));
      if (idle > 120) throw new Error("playlist produced no segments");
    }
  } catch (e) {
    log("direct: " + e.message);
    postMessage({ type: "direct-failed", msg: String(e.message || e) });
  }
}

onmessage = (e) => {
  const m = e.data;
  if (m.type === "direct") {
    startDirect(m.url);
  } else if (m.type === "data") {
    ensureFile();
    const buf = m.buf;                 // transferred ArrayBuffer (no copy here)
    buf.fileStart = offset;
    offset += buf.byteLength;
    try { mp4.appendBuffer(buf); } catch (err) { log("append: " + err); }
    drain();
  } else if (m.type === "credit") {
    credits += m.n || 1;
    if (credits > 16) credits = 16;    // never let credit debt build up
    drain();
  } else if (m.type === "reset") {
    if (fetcher) { fetcher.running = false; fetcher = null; }
    try { if (decoder && decoder.state !== "closed") decoder.close(); } catch (err) {}
    mp4 = null; decoder = null; cfg = null; offset = 0; pending = []; credits = 0;
  }
};
