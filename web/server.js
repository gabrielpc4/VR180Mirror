// VR180Mirror web server (USB-only)
//  - HTTP :9080 -> serves player.html and proxies MediaMTX's LL-HLS output.
//    Reached over the adb-reverse USB tunnel (localhost on both ends), so no
//    TLS is needed: WebXR treats http://localhost as a secure context.
//
// Usage: node server.js [--ip 192.168.0.14]
"use strict";

const http = require("http");
const fs = require("fs");
const path = require("path");
const { URL } = require("url");

const ROOT = __dirname;
const STREAM_PATH = "vr180";
const HLS_PORT = 9888;
const HTTP_PORT = 9080;   // reached via adb reverse over USB (8080 may be in use by other stacks)

let lanIp = null;
for (let i = 2; i < process.argv.length; i++) {
  if (process.argv[i] === "--ip") lanIp = process.argv[++i];
}
if (!lanIp) {
  const os = require("os");
  const ifs = os.networkInterfaces();
  outer: for (const name of Object.keys(ifs)) {
    for (const a of ifs[name]) {
      if (a.family === "IPv4" && !a.internal && !a.address.startsWith("169.")) {
        lanIp = a.address;
        break outer;
      }
    }
  }
}

const MIME = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".json": "application/json; charset=utf-8",
  ".png": "image/png",
  ".ico": "image/x-icon",
};

function serveStatic(req, res, pathname) {
  let file = pathname === "/" ? "/player.html" : pathname;
  file = path.normalize(file).replace(/^(\.\.[\/\\])+/, "");
  const full = path.join(ROOT, file);
  if (!full.startsWith(ROOT) || !fs.existsSync(full) || fs.statSync(full).isDirectory()) {
    res.writeHead(404, { "Content-Type": "text/plain" });
    res.end("not found");
    return;
  }
  res.writeHead(200, {
    "Content-Type": MIME[path.extname(full).toLowerCase()] || "application/octet-stream",
    "Cache-Control": "no-store",
  });
  fs.createReadStream(full).pipe(res);
}

// ---- runtime settings bridge ---------------------------------------------------
// Two directions share this one file: the viewer posts stream-side options
// (edge feather / "full picture"), which VR180Mirror.exe watches live; the PC's
// control panel (VR180Console.exe) posts renderScale, which the viewer reads on
// load so the headset's own XR framebuffer scale can be dialed in by trial and
// error from the desktop without needing to touch the headset each time.
// A POST merges into the existing file rather than replacing it, since either
// side may only know about its own field.
const RUNTIME_FILE = path.join(ROOT, "..", "bin", "runtime.json");
function settings(req, res) {
  if (req.method === "POST") {
    const chunks = [];
    req.on("data", (c) => chunks.push(c));
    req.on("end", () => {
      try {
        const body = JSON.parse(Buffer.concat(chunks).toString() || "{}");
        let cur = {};
        try { cur = JSON.parse(fs.readFileSync(RUNTIME_FILE, "utf8")); } catch (e) {}
        if (typeof body.feather === "number" && body.feather >= 0 && body.feather <= 20) {
          cur.feather = body.feather;
        }
        if (typeof body.renderScale === "number" && body.renderScale >= 1.0 && body.renderScale <= 2.0) {
          cur.renderScale = Math.round(body.renderScale * 100) / 100;
        }
        if (typeof body.syncOffsetMs === "number" && body.syncOffsetMs >= 0 && body.syncOffsetMs <= 500) {
          cur.syncOffsetMs = Math.round(body.syncOffsetMs);
        }
        if (typeof body.stabilize === "boolean") {
          cur.stabilize = body.stabilize;
        }
        fs.writeFileSync(RUNTIME_FILE, JSON.stringify(cur));
        res.writeHead(200, { "Content-Type": "application/json" });
        res.end(JSON.stringify(cur));
      } catch (e) {
        res.writeHead(400, { "Content-Type": "text/plain" });
        res.end("bad settings body");
      }
    });
    return;
  }
  let cur = {};
  try { cur = JSON.parse(fs.readFileSync(RUNTIME_FILE, "utf8")); } catch (e) {}
  res.writeHead(200, { "Content-Type": "application/json" });
  res.end(JSON.stringify(cur));
}

// ---- LL-HLS proxy ---------------------------------------------------------------
// Serves MediaMTX's HLS under our own origin (/hls/*), so the buffered player
// works over the USB localhost tunnel and never trips mixed-content on https.
function proxyHls(req, res, pathname, search) {
  const sub = pathname.replace(/^\/hls\/?/, "") || "index.m3u8";
  const preq = http.request(
    // keep the query string: LL-HLS uses _HLS_msn/_HLS_part blocking requests
    {
      hostname: "127.0.0.1", port: 9888,
      path: `/${STREAM_PATH}/${sub}${search || ""}`, method: "GET",
      headers: req.headers.cookie ? { Cookie: req.headers.cookie } : {},
    },
    (pres) => {
      // MediaMTX 302s once for its cookie check; rewrite Location to our mount
      const headers = {
        "Content-Type": pres.headers["content-type"] || "application/octet-stream",
        "Cache-Control": "no-store",
      };
      if (pres.headers["location"]) {
        headers["Location"] = pres.headers["location"].replace(`/${STREAM_PATH}/`, "/hls/");
      }
      if (pres.headers["set-cookie"]) headers["Set-Cookie"] = pres.headers["set-cookie"];
      res.writeHead(pres.statusCode, headers);
      pres.pipe(res);
    }
  );
  preq.on("error", () => { res.writeHead(502); res.end("HLS upstream unreachable"); });
  preq.end();
}

function handler(req, res) {
  const u = new URL(req.url, "http://x");
  if (u.pathname === "/hls" || u.pathname.startsWith("/hls/")) return proxyHls(req, res, u.pathname, u.search);
  if (u.pathname === "/settings") return settings(req, res);
  if (u.pathname === "/clientlog" && req.method === "POST") {
    // The viewer runs on the headset, so its errors used to be invisible here.
    // They now land in this console window and in web/client.log.
    const chunks = [];
    req.on("data", (c) => chunks.push(c));
    req.on("end", () => {
      const line = "[viewer " + new Date().toLocaleTimeString() + "] "
        + Buffer.concat(chunks).toString().slice(0, 500);
      console.log(line);
      try { fs.appendFileSync(path.join(ROOT, "client.log"), line + String.fromCharCode(10)); } catch (e) {}
      res.writeHead(204); res.end();
    });
    return;
  }
  if (u.pathname === "/poses") {
    // Player HMD pose history for the stabilization feature - published by
    // VR180Mirror.exe (bin/poses.json, atomic tmp+rename write, ~200ms cadence).
    // ?since=<ms> trims to samples newer than that UTC-ms timestamp, so the
    // viewer only has to transfer what it doesn't already have.
    const since = Number(u.searchParams.get("since")) || 0;
    let poses = [];
    try {
      const raw = JSON.parse(fs.readFileSync(path.join(ROOT, "..", "bin", "poses.json"), "utf8"));
      poses = (raw.poses || []).filter((p) => p.t > since);
    } catch (e) {}
    res.writeHead(200, { "Content-Type": "application/json", "Cache-Control": "no-store" });
    res.end(JSON.stringify({ poses }));
    return;
  }
  if (u.pathname === "/devstats") {
    res.writeHead(200, { "Content-Type": "application/json", "Cache-Control": "no-store" });
    res.end(JSON.stringify(dev));
    return;
  }
  if (u.pathname === "/info") {
    // Enriched status for the Quest launcher app: is the ingest live, which codec.
    const send = (extra) => {
      // canvas spans published by VR180Mirror.exe (FOV-fit mode)
      let spans = {};
      try { spans = JSON.parse(fs.readFileSync(path.join(ROOT, "..", "bin", "mirror_status.json"), "utf8")); } catch (e) {}
      res.writeHead(200, { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" });
      res.end(JSON.stringify({ app: "vr180mirror", lanIp, stream: STREAM_PATH,
        httpPort: HTTP_PORT,
        hspan: spans.hspan, vspan: spans.vspan, mirrorLive: spans.live === 1,
        srcFps: spans.srcfps, canvasW: spans.canvasW, canvasH: spans.canvasH, ...extra }));
    };
    const req2 = http.get("http://127.0.0.1:9998/v3/paths/list", { timeout: 1500 }, (r2) => {
      let body = "";
      r2.on("data", (c) => (body += c));
      r2.on("end", () => {
        try {
          const item = (JSON.parse(body).items || []).find((i) => i.name === STREAM_PATH);
          send({ ready: !!(item && item.ready), tracks: item ? item.tracks : [] });
        } catch (e) { send({ ready: false, tracks: [] }); }
      });
    });
    req2.on("error", () => send({ ready: false, tracks: [] }));
    req2.on("timeout", () => { req2.destroy(); send({ ready: false, tracks: [] }); });
    return;
  }
  return serveStatic(req, res, u.pathname);
}

// ---- headset telemetry (/devstats) ----------------------------------------------
// Streams the Quest compositor's VrApi log line (FPS, stale frames, clock levels,
// temperature) plus GPU busy % over adb, for the in-VR stats HUD.
const { spawn, execFile } = require("child_process");
const dev = { fps: null, maxFps: null, stale: null, cpuLvl: null, gpuLvl: null,
              cpuMHz: null, gpuMHz: null, temp: null, gpuBusy: null, updated: 0 };
const ADB = (() => {
  const cands = [
    path.join(process.env.LOCALAPPDATA || "", "Android", "Sdk", "platform-tools", "adb.exe"),
    "adb",
  ];
  for (const c of cands) { try { if (c === "adb" || fs.existsSync(c)) return c; } catch (e) {} }
  return null;
})();
// which headset to read telemetry from, when more than one is plugged in
// (set by Start-Spectator.ps1 -Serial, threaded through from the control panel)
let adbSerial = null;
for (let i = 2; i < process.argv.length; i++) {
  if (process.argv[i] === "--serial") adbSerial = process.argv[++i];
}
const adbArgs = (args) => (adbSerial ? ["-s", adbSerial, ...args] : args);

function startVrApiTail() {
  if (!ADB) return;
  const p = spawn(ADB, adbArgs(["logcat", "-s", "VrApi"]), { windowsHide: true });
  p.stdout.on("data", (buf) => {
    const line = buf.toString();
    const m = line.match(/FPS=(\d+)\/(\d+).*?Stale=(\d+).*?CPU4\/GPU=(\d+)\/(\d+),(\d+)\/(\d+)MHz.*?Temp=([\d.]+)/);
    if (m) {
      dev.fps = +m[1]; dev.maxFps = +m[2]; dev.stale = +m[3];
      dev.cpuLvl = +m[4]; dev.gpuLvl = +m[5]; dev.cpuMHz = +m[6]; dev.gpuMHz = +m[7];
      dev.temp = +m[8]; dev.updated = Date.now();
    }
  });
  p.on("exit", () => setTimeout(startVrApiTail, 5000));
  p.on("error", () => {});
}
startVrApiTail();
if (ADB) {
  setInterval(() => {
    execFile(ADB, adbArgs(["shell", "cat", "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage"]),
      { windowsHide: true, timeout: 1500 }, (err, out) => {
        if (!err && out) { const n = parseInt(out, 10); if (!isNaN(n)) dev.gpuBusy = n; }
      });
  }, 1000);
}

// ---- start -------------------------------------------------------------------
http.createServer(handler).listen(HTTP_PORT, () => {
  console.log("");
  console.log("  VR180 Spectator web server (USB only)");
  console.log("  ======================================");
  console.log(`  On the Quest (via adb reverse over USB): http://localhost:${HTTP_PORT}/`);
  console.log("");
});
