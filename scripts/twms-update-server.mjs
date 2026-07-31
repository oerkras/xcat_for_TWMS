#!/usr/bin/env node
/**
 * XCat TWMS update API (范围 B)：健康检查 + /update/* 分发。
 * 日志上传先 stub（501）；运维台 / Caddy 仍属范围 C。
 *
 * 默认：http://127.0.0.1:18789/twms
 *   GET  /twms/health
 *   GET  /twms/ready
 *   GET  /twms/update/latest.json
 *   GET  /twms/update/force.json   (无文件 → 404)
 *   GET  /twms/update/<zip>
 *   POST /twms/admin/shutdown      (loopback)
 *   GET  /twms/admin/stats         (loopback)
 *   *   /twms/v1/logs*             → 501 stub
 */
import http from "node:http";
import { createReadStream } from "node:fs";
import fs from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const SERVER_VERSION = "0.1.0";
const __dirname = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(__dirname, "..");

const args = new Map();
for (let i = 2; i < process.argv.length; i += 1) {
  const arg = process.argv[i];
  if (!arg.startsWith("--")) continue;
  const key = arg.slice(2);
  const next = process.argv[i + 1];
  if (next && !next.startsWith("--")) {
    args.set(key, next);
    i += 1;
  } else {
    args.set(key, "1");
  }
}

const host = args.get("host") || "0.0.0.0";
const port = Number(args.get("port") || process.env.XCAT_TWMS_PORT || 18789);
const releaseRoot = path.resolve(
  args.get("release-root") || path.join(repoRoot, "artifacts", "release"),
);
const accessLogPath = path.resolve(
  args.get("access-log") || path.join(repoRoot, "artifacts", "ops_logs", "twms_access.jsonl"),
);
const basePath = normalizeBasePath(
  args.get("base-path") || process.env.XCAT_TWMS_BASE_PATH || "/twms",
);

const startedAt = Date.now();
let shuttingDown = false;
let accessLogReady = Promise.resolve();

const stats = {
  requestsTotal: 0,
  byKind: { health: 0, ready: 0, update: 0, admin: 0, upload: 0, notFound: 0, other: 0 },
  byStatus: { "2xx": 0, "3xx": 0, "4xx": 0, "5xx": 0, other: 0 },
  lastRequestAt: "",
  lastErrorAt: "",
  lastError: "",
};

/** @type {Map<string, any>} */
const activeClients = new Map();
const kClientActiveDefaultSec = 90;
const kClientPruneSec = 3600;

function headerText(req, name) {
  const v = req?.headers?.[name];
  const raw = Array.isArray(v) ? String(v[0] || "") : String(v || "");
  return raw.replace(/[\u0000-\u001f\u007f]/g, "").trim();
}

function clientIdentityFromReq(req) {
  return {
    machine: headerText(req, "x-xcat-machine").slice(0, 80),
    deviceId: headerText(req, "x-xcat-device-id").slice(0, 64),
    appVersion: headerText(req, "x-xcat-app-version").slice(0, 64),
  };
}

function touchClient({ ip, kind, pathName, status, ua, machine, deviceId, appVersion }) {
  const identified = !!(machine || deviceId);
  const key = identified
    ? `dev:${(machine || "host").toLowerCase()}:${(deviceId || "").toLowerCase() || "na"}`
    : `ip:${ip}`;
  const now = Date.now();
  let row = activeClients.get(key);
  if (!row) {
    row = {
      key,
      ip,
      machine: "",
      deviceId: "",
      device: "",
      appVersion: "",
      identified: false,
      firstSeenMs: now,
      lastSeenMs: now,
      hits: 0,
      lastKind: "",
      lastPath: "",
      lastStatus: 0,
      ua: "",
      geo: "",
      geoStatus: "",
    };
    activeClients.set(key, row);
  }
  row.ip = ip;
  row.lastSeenMs = now;
  row.hits += 1;
  row.lastKind = kind || "";
  row.lastPath = pathName || "";
  row.lastStatus = status || 0;
  if (ua) row.ua = String(ua).slice(0, 160);
  if (machine) row.machine = machine;
  if (deviceId) row.deviceId = deviceId;
  if (appVersion) row.appVersion = appVersion;
  row.identified = !!(row.machine || row.deviceId);
  row.device = row.identified ? `${row.machine || "host"}_${(row.deviceId || "").slice(0, 8)}` : "";

  // prune stale
  for (const [k, v] of activeClients) {
    if (now - v.lastSeenMs > kClientPruneSec * 1000) activeClients.delete(k);
  }
}

function listActiveClients(activeSec) {
  const now = Date.now();
  const cutoff = now - activeSec * 1000;
  const online = [...activeClients.values()].filter((r) => r.lastSeenMs >= cutoff);
  const byIp = new Map();
  for (const row of online) byIp.set(row.ip, (byIp.get(row.ip) || 0) + 1);
  return online
    .map((row) => ({
      key: row.key,
      ip: row.ip,
      geo: row.geo || "",
      geoStatus: row.geoStatus || "",
      machine: row.identified ? row.machine || "" : "",
      deviceId: row.identified ? row.deviceId || "" : "",
      device: row.identified ? row.device || "" : "",
      appVersion: row.appVersion || "",
      identified: !!row.identified,
      sameIpOnline: byIp.get(row.ip) || 1,
      knownOnIp: byIp.get(row.ip) || 1,
      lastKind: row.lastKind || "",
      lastPath: row.lastPath || "",
      lastStatus: row.lastStatus || 0,
      hits: row.hits || 0,
      ua: row.ua || "",
      firstSeenAt: ts(new Date(row.firstSeenMs)),
      lastSeenAt: ts(new Date(row.lastSeenMs)),
      idleSec: Math.max(0, Math.floor((now - row.lastSeenMs) / 1000)),
    }))
    .sort((a, b) => a.idleSec - b.idleSec || a.ip.localeCompare(b.ip));
}

function normalizeBasePath(value) {
  const text = String(value || "").trim();
  if (!text || text === "/") return "";
  const withSlash = text.startsWith("/") ? text : `/${text}`;
  return withSlash.replace(/\/+$/g, "");
}

function routePath(pathname) {
  if (!basePath) return pathname || "/";
  if (pathname === basePath) return "/";
  if (pathname.startsWith(`${basePath}/`)) return pathname.slice(basePath.length) || "/";
  return pathname || "/";
}

function clientIp(req) {
  const raw = req.socket?.remoteAddress || "";
  if (raw.startsWith("::ffff:")) return raw.slice(7);
  return raw || "unknown";
}

function isLoopback(ip) {
  return ip === "127.0.0.1" || ip === "::1" || ip === "localhost";
}

function ts(d = new Date()) {
  return d.toISOString().replace("T", " ").slice(0, 19);
}

function logInfo(msg) {
  console.log(`[${ts()}] INFO  ${msg}`);
}
function logWarn(msg) {
  console.warn(`[${ts()}] WARN  ${msg}`);
}
function logError(msg) {
  console.error(`[${ts()}] ERROR ${msg}`);
}

function noteError(err) {
  stats.lastErrorAt = ts();
  stats.lastError = String(err?.message || err || "error");
}

function setSecurityHeaders(res) {
  res.setHeader("X-Content-Type-Options", "nosniff");
  res.setHeader("Referrer-Policy", "no-referrer");
  res.setHeader("X-Frame-Options", "DENY");
  res.setHeader("Cache-Control", "no-store");
  res.setHeader("X-XCat-Server", `twms-update/${SERVER_VERSION}`);
}

function sendJson(res, status, obj) {
  setSecurityHeaders(res);
  const body = `${JSON.stringify(obj)}\n`;
  res.writeHead(status, {
    "content-type": "application/json; charset=utf-8",
    "content-length": Buffer.byteLength(body),
  });
  res.end(body);
}

function classifyRoute(routedPath) {
  if (routedPath === "/health") return "health";
  if (routedPath === "/ready" || routedPath === "/healthz") return "ready";
  if (routedPath.startsWith("/admin/")) return "admin";
  if (routedPath.startsWith("/update/")) return "update";
  if (routedPath === "/v1/logs" || routedPath.startsWith("/v1/logs/")) return "upload";
  return "other";
}

function statusBucket(code) {
  if (code >= 200 && code < 300) return "2xx";
  if (code >= 300 && code < 400) return "3xx";
  if (code >= 400 && code < 500) return "4xx";
  if (code >= 500 && code < 600) return "5xx";
  return "other";
}

function isQuietForcePoll(status, kind, routedPath) {
  if (kind !== "update") return false;
  if (status !== 200 && status !== 404) return false;
  return routedPath === "/update/force.json";
}

function appendAccessLog(entry) {
  const line = `${JSON.stringify(entry)}\n`;
  accessLogReady = accessLogReady
    .then(() => fs.appendFile(accessLogPath, line, "utf8"))
    .catch((err) => logWarn(`access log write failed: ${err.message || err}`));
}

function recordRequest({ method, pathName, routedPath, ip, status, ms, kind, ua, machine, deviceId, appVersion }) {
  stats.requestsTotal += 1;
  stats.lastRequestAt = ts();
  if (stats.byKind[kind] != null) stats.byKind[kind] += 1;
  else stats.byKind.other += 1;
  const bucket = statusBucket(status);
  if (stats.byStatus[bucket] != null) stats.byStatus[bucket] += 1;
  else stats.byStatus.other += 1;

  if (isLoopback(ip) && (kind === "health" || kind === "ready" || kind === "admin")) return;

  touchClient({ ip, kind, pathName, status, ua, machine, deviceId, appVersion });

  if (isQuietForcePoll(status, kind, routedPath)) return;

  appendAccessLog({
    t: stats.lastRequestAt,
    ip,
    method,
    path: pathName,
    route: routedPath,
    kind,
    status,
    ms,
    machine: machine || undefined,
    deviceId: deviceId || undefined,
  });
  const level = status >= 500 ? logError : status >= 400 ? logWarn : logInfo;
  level(`${ip} ${method} ${pathName} → ${status} ${ms}ms`);
}

function attachRequestRecorder(req, res, meta) {
  const t0 = Date.now();
  const id = clientIdentityFromReq(req);
  res.on("finish", () => {
    recordRequest({
      ...meta,
      method: req.method || "GET",
      status: res.statusCode || 0,
      ms: Date.now() - t0,
      ua: headerText(req, "user-agent"),
      machine: id.machine,
      deviceId: id.deviceId,
      appVersion: id.appVersion,
    });
  });
}

async function sendFile(req, res, filePath, contentType, { cacheSeconds = 0 } = {}) {
  const st = await fs.stat(filePath);
  if (!st.isFile()) {
    sendJson(res, 404, { ok: false, error: "not found" });
    return;
  }
  const etag = `"${st.size.toString(16)}-${Math.floor(st.mtimeMs).toString(16)}"`;
  const inm = String(req.headers["if-none-match"] || "");
  setSecurityHeaders(res);
  if (cacheSeconds > 0) {
    res.setHeader("Cache-Control", `public, max-age=${cacheSeconds}`);
  }
  res.setHeader("ETag", etag);
  res.setHeader("Last-Modified", st.mtime.toUTCString());
  if (inm && inm === etag) {
    res.writeHead(304);
    res.end();
    return;
  }
  res.writeHead(200, {
    "content-type": contentType,
    "content-length": st.size,
  });
  const stream = createReadStream(filePath);
  stream.on("error", (err) => {
    logWarn(`sendFile error ${filePath}: ${err.message || err}`);
    noteError(err);
    if (!res.headersSent) sendJson(res, 500, { ok: false, error: "read file failed" });
    else res.destroy(err);
  });
  stream.pipe(res);
}

async function handleUpdate(req, res, routedPath) {
  if (req.method !== "GET") {
    sendJson(res, 405, { ok: false, error: "method not allowed" });
    return;
  }
  if (routedPath === "/update/latest.json") {
    await sendFile(req, res, path.join(releaseRoot, "latest.json"), "application/json; charset=utf-8", {
      cacheSeconds: 30,
    });
    return;
  }
  if (routedPath === "/update/force.json") {
    try {
      await sendFile(
        req,
        res,
        path.join(releaseRoot, "force-update.json"),
        "application/json; charset=utf-8",
        { cacheSeconds: 5 },
      );
    } catch (err) {
      if (err?.code === "ENOENT") {
        sendJson(res, 404, { ok: false, error: "not found" });
        return;
      }
      throw err;
    }
    return;
  }
  const prefix = "/update/";
  if (!routedPath.startsWith(prefix)) {
    sendJson(res, 404, { ok: false, error: "not found" });
    return;
  }
  const zipName = path.basename(decodeURIComponent(routedPath.slice(prefix.length)));
  if (!zipName.endsWith(".zip") || zipName.includes("..")) {
    sendJson(res, 404, { ok: false, error: "not found" });
    return;
  }
  await sendFile(req, res, path.join(releaseRoot, zipName), "application/zip", { cacheSeconds: 300 });
}

function healthPayload() {
  return {
    ok: true,
    service: "xcat-twms-update",
    version: SERVER_VERSION,
    profile: basePath ? basePath.slice(1) : "root",
    uptimeSec: Math.floor((Date.now() - startedAt) / 1000),
    shuttingDown,
    uploads: { stub: true },
    requests: {
      total: stats.requestsTotal,
      lastAt: stats.lastRequestAt || null,
      byKind: { ...stats.byKind },
      byStatus: { ...stats.byStatus },
    },
    releaseRoot: path.relative(repoRoot, releaseRoot) || ".",
    lastErrorAt: stats.lastErrorAt || null,
    lastError: stats.lastError || null,
  };
}

async function readyCheck() {
  const checks = {};
  try {
    await fs.mkdir(releaseRoot, { recursive: true });
    const probe = path.join(releaseRoot, `.ready-${process.pid}`);
    await fs.writeFile(probe, "ok\n");
    await fs.unlink(probe);
    checks.releaseRoot = "ok";
  } catch (err) {
    checks.releaseRoot = err.message || String(err);
  }
  const ok = Object.values(checks).every((v) => v === "ok");
  return { ok, checks, shuttingDown };
}

async function handleAdmin(req, res, routedPath) {
  const ip = clientIp(req);
  if (!isLoopback(ip)) {
    sendJson(res, 403, { ok: false, error: "admin local only" });
    return;
  }
  if (routedPath === "/admin/shutdown" && req.method === "POST") {
    sendJson(res, 200, { ok: true, shuttingDown: true });
    shuttingDown = true;
    setTimeout(() => process.exit(0), 200);
    return;
  }
  if (routedPath === "/admin/stats" && req.method === "GET") {
    sendJson(res, 200, { ok: true, ...healthPayload() });
    return;
  }
  if (routedPath === "/admin/clients" && req.method === "GET") {
    let activeSec = kClientActiveDefaultSec;
    try {
      const u = new URL(req.url || "/", "http://127.0.0.1");
      const n = Number(u.searchParams.get("activeSec") || kClientActiveDefaultSec);
      if (Number.isFinite(n) && n > 0) activeSec = Math.min(3600, Math.floor(n));
    } catch {
      /* keep default */
    }
    const clients = listActiveClients(activeSec);
    sendJson(res, 200, {
      ok: true,
      activeSec,
      count: clients.length,
      geoProvider: "none",
      tracked: activeClients.size,
      clients,
    });
    return;
  }
  sendJson(res, 404, { ok: false, error: "not found" });
}

const server = http.createServer(async (req, res) => {
  const ip = clientIp(req);
  let routedPath = "/";
  let pathName = req.url || "/";
  try {
    const url = new URL(req.url || "/", "http://127.0.0.1");
    pathName = url.pathname + (url.search || "");
    routedPath = routePath(url.pathname);
  } catch {
    routedPath = "/";
  }

  const kind = classifyRoute(routedPath);
  attachRequestRecorder(req, res, { ip, pathName, routedPath, kind });

  try {
    if (shuttingDown && req.method !== "GET") {
      sendJson(res, 503, { ok: false, error: "server shutting down" });
      return;
    }

    if (req.method === "GET" && routedPath === "/health") {
      sendJson(res, 200, healthPayload());
      return;
    }
    if (req.method === "GET" && (routedPath === "/ready" || routedPath === "/healthz")) {
      const ready = await readyCheck();
      sendJson(res, ready.ok && !shuttingDown ? 200 : 503, ready);
      return;
    }
    if (routedPath.startsWith("/admin/")) {
      await handleAdmin(req, res, routedPath);
      return;
    }
    if (routedPath.startsWith("/update/")) {
      await handleUpdate(req, res, routedPath);
      return;
    }
    if (routedPath === "/v1/logs" || routedPath.startsWith("/v1/logs/")) {
      sendJson(res, 501, {
        ok: false,
        error: "log upload not implemented yet (TWMS scope B stub)",
        path: routedPath,
      });
      return;
    }
    sendJson(res, 404, { ok: false, error: "not found" });
  } catch (err) {
    noteError(err);
    const status = err && err.status ? err.status : 500;
    if (!res.headersSent) {
      sendJson(res, status, { ok: false, error: err.message || "server error" });
    } else {
      res.destroy(err);
    }
  }
});

server.requestTimeout = 5 * 60 * 1000;
server.headersTimeout = 60_000;
server.keepAliveTimeout = 10_000;

process.on("SIGINT", () => {
  shuttingDown = true;
  logInfo("SIGINT, closing");
  server.close(() => process.exit(0));
  setTimeout(() => process.exit(0), 1500);
});
process.on("SIGTERM", () => {
  shuttingDown = true;
  logInfo("SIGTERM, closing");
  server.close(() => process.exit(0));
  setTimeout(() => process.exit(0), 1500);
});

await fs.mkdir(releaseRoot, { recursive: true });
await fs.mkdir(path.dirname(accessLogPath), { recursive: true });

server.listen(port, host, () => {
  logInfo(`xcat twms update server v${SERVER_VERSION} listening on http://${host}:${port}`);
  logInfo(`base path: ${basePath || "/"}`);
  logInfo(`updates: ${releaseRoot}`);
  logInfo(`access log: ${accessLogPath}`);
  logInfo(`client default: http://127.0.0.1:${port}${basePath}`);
  logInfo(`admin: POST ${basePath}/admin/shutdown (loopback only)`);
});

server.on("error", (err) => {
  logError(`listen error: ${err.message || err}`);
  process.exit(1);
});
