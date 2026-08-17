// VR180Mirror web server
//  - HTTPS :8443  -> serves player.html (WebXR needs a secure context) and
//                    proxies WHEP signaling to MediaMTX on localhost:8889,
//                    so the Quest only ever has to accept ONE certificate.
//  - HTTP  :8080  -> DeoVR launch JSON (fallback player route, no TLS needed)
//                    and a plain redirect/info page.
//
// Usage: node server.js [--ip 192.168.0.14]
"use strict";

const https = require("https");
const http = require("http");
const fs = require("fs");
const path = require("path");
const { URL } = require("url");

const ROOT = __dirname;
const MEDIAMTX = "http://127.0.0.1:9889";
const STREAM_PATH = "vr180";
const HLS_PORT = 9888;
const HTTP_PORT = 9080;   // DeoVR JSON + info (8080 may be in use by other stacks)
const HTTPS_PORT = 8443;

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

// ---- WHEP proxy -------------------------------------------------------------
// POST /whep                -> POST  MediaMTX /vr180/whep      (offer -> answer)
// PATCH/DELETE /whep-res/*  -> same on MediaMTX session resource
function proxyWhep(req, res) {
  const u = new URL(req.url, "http://x");
  let target;
  if (req.method === "POST" && u.pathname === "/whep") {
    target = `${MEDIAMTX}/${STREAM_PATH}/whep`;
  } else if (u.pathname.startsWith("/whep-res/")) {
    target = MEDIAMTX + decodeURIComponent(u.pathname.replace("/whep-res", ""));
  } else {
    res.writeHead(404); res.end(); return;
  }

  const chunks = [];
  req.on("data", (c) => chunks.push(c));
  req.on("end", () => {
    const body = Buffer.concat(chunks);
    const t = new URL(target);
    const preq = http.request(
      {
        hostname: t.hostname,
        port: t.port,
        path: t.pathname + t.search,
        method: req.method,
        headers: {
          "Content-Type": req.headers["content-type"] || "application/sdp",
          "Content-Length": body.length,
          ...(req.headers["if-match"] ? { "If-Match": req.headers["if-match"] } : {}),
        },
      },
      (pres) => {
        const headers = {
          "Content-Type": pres.headers["content-type"] || "application/sdp",
          "Access-Control-Allow-Origin": "*",
        };
        if (pres.headers["location"]) {
          // session resource lives on MediaMTX; expose it via our proxy path
          headers["Location"] = "/whep-res" + pres.headers["location"].replace(/^https?:\/\/[^/]+/, "");
        }
        if (pres.headers["etag"]) headers["ETag"] = pres.headers["etag"];
        res.writeHead(pres.statusCode, headers);
        pres.pipe(res);
      }
    );
    preq.on("error", (e) => {
      res.writeHead(502, { "Content-Type": "text/plain" });
      res.end("MediaMTX unreachable: " + e.message);
    });
    preq.end(body);
  });
}

function handler(req, res) {
  const u = new URL(req.url, "http://x");
  if (u.pathname === "/whep" || u.pathname.startsWith("/whep-res/")) return proxyWhep(req, res);
  if (u.pathname === "/deovr" || u.pathname === "/deovr.json") return deovr(req, res);
  if (u.pathname === "/info") {
    res.writeHead(200, { "Content-Type": "application/json" });
    res.end(JSON.stringify({ lanIp, stream: STREAM_PATH }));
    return;
  }
  return serveStatic(req, res, u.pathname);
}

// ---- DeoVR fallback ----------------------------------------------------------
// DeoVR's built-in browser: navigate to http://<ip>:8080/deovr — it detects the
// JSON and opens the player. Uses MediaMTX LL-HLS output on :8888.
function deovr(req, res) {
  // DeoVR auto-fetches "/deovr" from a site root; flat single-video format.
  const j = {
    path: `http://${lanIp}:${HLS_PORT}/${STREAM_PATH}/index.m3u8`,
    title: "VR180 Spectator — live PCVR view",
    is3d: true,
    screenType: "dome",     // 180 degree equirect
    stereoMode: "sbs",      // side-by-side, left eye = left half
  };
  res.writeHead(200, { "Content-Type": "application/json; charset=utf-8" });
  res.end(JSON.stringify(j));
}

// ---- start -------------------------------------------------------------------
const certFile = path.join(ROOT, "cert.pem");
const keyFile = path.join(ROOT, "key.pem");
if (!fs.existsSync(certFile) || !fs.existsSync(keyFile)) {
  console.error("cert.pem / key.pem missing — run setup.ps1 first");
  process.exit(1);
}

https
  .createServer({ cert: fs.readFileSync(certFile), key: fs.readFileSync(keyFile) }, handler)
  .listen(HTTPS_PORT, () => {
    console.log("");
    console.log("  VR180 Spectator web server");
    console.log("  ==========================");
    console.log(`  Quest viewer (WebXR, lowest latency): https://${lanIp}:${HTTPS_PORT}/`);
    console.log(`  DeoVR fallback (LL-HLS):              http://${lanIp}:${HTTP_PORT}/deovr`);
    console.log("");
  });

http.createServer(handler).listen(HTTP_PORT);
