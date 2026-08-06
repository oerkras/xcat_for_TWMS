#!/usr/bin/env node
/**
 * XCat TWMS update API：健康检查 + /update/* 分发 + 日志上传 + 运维 admin。
 * 网页下载站为 publish_site Python :52080（非本进程）。
 *
 * 绑定默认：http://0.0.0.0:18789/twms
 * 客户端默认：http://xcat.work:18789/twms
 *   GET  /twms/health
 *   GET  /twms/ready
 *   GET  /twms/update/latest.json
 *   GET  /twms/update/force.json   (无 force-update.json → 404)
 *   GET  /twms/update/access.json  (按 X-XCat-* 头查是否禁止使用)
 *   GET  /twms/update/<zip>
 *   POST /twms/v1/logs/sessions · PUT …/files/:name · POST …/commit
 *   POST /twms/v1/logs            (legacy JSON)
 *   POST /twms/admin/shutdown      (loopback)
 *   GET  /twms/admin/stats         (loopback)
 *   GET  /twms/admin/clients       (loopback；按 X-XCat-* 头追踪)
 *   GET  /twms/admin/bans          (loopback；封禁清单，兼容)
 *   POST /twms/admin/bans          (loopback；action=ban|unban|allow|unallow|setMode)
 *   GET  /twms/admin/access        (loopback；mode+黑白名单)
 */
import http from "node:http";
import { createReadStream } from "node:fs";
import fs from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { createLogUpload } from "./twms-log-upload.mjs";
import { createDeviceAccess } from "./twms-device-access.mjs";
import { createIpGeo } from "./twms-ip-geo.mjs";

const SERVER_VERSION = "0.4.3";
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
const rawOut = args.get("out") || path.join(repoRoot, "user_log_uploads");
const outRoot = path.resolve(
  /(?:^|[\\/])artifacts[\\/]+user_log_uploads$/i.test(path.normalize(rawOut))
    ? path.join(repoRoot, "user_log_uploads")
    : rawOut,
);
const acceptProfiles = String(
  args.get("accept-profile") || process.env.XCAT_ACCEPT_PROFILES || "twms",
)
  .split(",")
  .map((s) => s.trim().toLowerCase())
  .filter(Boolean);

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
/** 与客户端 OnlineLease TTL 对齐（秒→毫秒），运维台据此估算租约剩余。 */
const kOnlineLeaseTtlMs = 64 * 3600 * 1000;
/** @type {any[]} */
const recentAccessDenies = [];
const kRecentDenyMax = 40;
/** ip → 见过的 deviceId（或 mac 兜底）集合，用于同 IP 多设备告警 */
/** @type {Map<string, Set<string>>} */
const devicesByIp = new Map();
/** @type {Map<string, { machine: string, deviceId: string, mac: string, device: string }>} */
const knownByDevice = new Map();

const ipGeo = createIpGeo({
  logWarn: (msg) => console.warn(`[geo] ${msg}`),
  userAgent: "xcat-twms-ops/1.0",
});
ipGeo.bindClientLister(() => activeClients.values());

const access = createDeviceAccess({
  releaseRoot,
  repoRoot,
  logInfo,
  logWarn,
  ts,
});

function headerText(req, name) {
  const v = req?.headers?.[name];
  const raw = Array.isArray(v) ? String(v[0] || "") : String(v || "");
  return raw.replace(/[\u0000-\u001f\u007f]/g, "").trim();
}

function clientIdentityFromReq(req) {
  const macRaw = headerText(req, "x-xcat-mac");
  const macs = access.parseMacList(macRaw);
  const token = access.normalizeToken(headerText(req, "x-xcat-token") || "");
  return {
    machine: headerText(req, "x-xcat-machine").slice(0, 80),
    deviceId: headerText(req, "x-xcat-device-id").slice(0, 64),
    appVersion: headerText(req, "x-xcat-app-version").slice(0, 64),
    macs,
    mac: macs[0] ? access.formatMac(macs[0]) : "",
    token,
  };
}

async function readJsonBody(req, limit = 64 * 1024) {
  const chunks = [];
  let total = 0;
  for await (const chunk of req) {
    total += chunk.length;
    if (total > limit) {
      const err = new Error("body too large");
      err.status = 413;
      throw err;
    }
    chunks.push(chunk);
  }
  const raw = Buffer.concat(chunks).toString("utf8").trim();
  if (!raw) return {};
  try {
    return JSON.parse(raw);
  } catch {
    const err = new Error("invalid json");
    err.status = 400;
    throw err;
  }
}

function deviceFingerprint({ machine, deviceId, mac, macs }) {
  const id = String(deviceId || "")
    .trim()
    .toLowerCase()
    .slice(0, 64);
  if (id) return `id:${id}`;
  const macList = Array.isArray(macs) ? macs : [];
  const macHex = String(mac || macList[0] || "")
    .toLowerCase()
    .replace(/[^0-9a-f]/g, "");
  if (macHex.length === 12) return `mac:${macHex}`;
  const m = String(machine || "")
    .trim()
    .toLowerCase()
    .slice(0, 80);
  if (m) return `host:${m}`;
  return "";
}

function rememberDeviceOnIp(ip, identity) {
  if (!ip || ipGeo.isLoopback(ip)) return;
  const fp = deviceFingerprint(identity);
  if (!fp) return;
  let set = devicesByIp.get(ip);
  if (!set) {
    set = new Set();
    devicesByIp.set(ip, set);
  }
  const wasNew = !set.has(fp);
  set.add(fp);
  knownByDevice.set(fp, {
    machine: String(identity.machine || "").slice(0, 80),
    deviceId: String(identity.deviceId || "").slice(0, 64),
    mac: String(identity.mac || (identity.macs && identity.macs[0]) || "").slice(0, 32),
    device: String(identity.device || "").slice(0, 96),
  });
  // 同公网 IP 冒出新 deviceId：告警（NAT 多机或泄露扩散）
  if (wasNew && set.size >= 2) {
    let geoHint = "";
    for (const row of activeClients.values()) {
      if (row.ip === ip && row.geo) {
        geoHint = row.geo;
        break;
      }
    }
    logWarn(
      `same-ip multi-device ip=${ip} devices=${set.size}${geoHint ? ` geo=${geoHint}` : ""} new=${fp} fps=${[...set].slice(0, 8).join(",")}`,
    );
  }
}

function listIpMultiDeviceAlerts() {
  const out = [];
  for (const [ip, set] of devicesByIp) {
    if (!set || set.size < 2) continue;
    if (ipGeo.isLoopback(ip)) continue;
    const devices = [];
    for (const fp of set) {
      const known = knownByDevice.get(fp) || {};
      devices.push({
        fp,
        machine: known.machine || "",
        deviceId: known.deviceId || "",
        mac: known.mac || "",
        device: known.device || fp,
      });
    }
    let geo = "";
    let geoStatus = "";
    for (const row of activeClients.values()) {
      if (row.ip === ip) {
        geo = ipGeo.displayGeo(row);
        geoStatus = row.geoStatus || "";
        break;
      }
    }
    out.push({
      ip,
      geo,
      geoStatus,
      deviceCount: set.size,
      devices: devices.slice(0, 16),
    });
  }
  out.sort((a, b) => b.deviceCount - a.deviceCount || a.ip.localeCompare(b.ip));
  return out;
}

function touchClient({
  ip,
  kind,
  pathName,
  status,
  ua,
  machine,
  deviceId,
  appVersion,
  macs,
  mac,
  token,
}) {
  if (!ip || ip === "unknown") return;
  const identified = !!(machine || deviceId || (macs && macs.length) || mac || token);
  const key = identified
    ? `dev:${(machine || "host").toLowerCase()}:${(deviceId || "").toLowerCase() || (macs && macs[0]) || mac || token || "na"}`
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
      mac: "",
      macs: [],
      token: "",
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
  if (token) row.token = access.normalizeToken(token);
  if (Array.isArray(macs) && macs.length) {
    row.macs = macs.slice(0, 8);
    row.mac = mac || access.formatMac(macs[0]);
  } else if (mac) {
    row.mac = mac;
  }
  row.identified = !!(
    row.machine ||
    row.deviceId ||
    (row.macs && row.macs.length) ||
    row.mac ||
    row.token
  );
  row.device = row.identified
    ? `${row.machine || "host"}_${(row.deviceId || row.mac || row.token || "na").slice(0, 8)}`
    : "";

  if (row.identified) {
    rememberDeviceOnIp(ip, {
      machine: row.machine,
      deviceId: row.deviceId,
      mac: row.mac,
      macs: row.macs,
      device: row.device,
    });
  }

  if (ipGeo.isPrivateOrLocalIp(ip)) {
    ipGeo.markPrivate(row);
  } else if (!row.geo || row.geoStatus === "error") {
    ipGeo.scheduleGeoLookup(ip);
  }

  // prune stale
  for (const [k, v] of activeClients) {
    if (now - v.lastSeenMs > kClientPruneSec * 1000) activeClients.delete(k);
  }
}

function noteAccessDeny({ ip, machine, deviceId, mac, macs, token, decision }) {
  const now = Date.now();
  const entry = {
    at: ts(new Date(now)),
    atMs: now,
    ip: String(ip || "").slice(0, 64),
    machine: String(machine || "").slice(0, 80),
    deviceId: String(deviceId || "").slice(0, 64),
    mac: String(mac || "").slice(0, 32),
    token: String(token || "").slice(0, 48),
    reason: String(decision?.reason || "").slice(0, 200),
    match: String(decision?.match || "").slice(0, 40),
    mode: String(decision?.mode || "").slice(0, 16),
    key: String(decision?.key || "").slice(0, 96),
  };
  recentAccessDenies.unshift(entry);
  if (recentAccessDenies.length > kRecentDenyMax) recentAccessDenies.length = kRecentDenyMax;

  const identified = !!(machine || deviceId || (macs && macs.length) || token);
  const clientKey = identified
    ? `dev:${(machine || "host").toLowerCase()}:${(deviceId || "").toLowerCase() || (macs && macs[0]) || mac || token || "na"}`
    : `ip:${ip}`;
  let row = activeClients.get(clientKey);
  if (!row) {
    touchClient({
      ip,
      kind: "update",
      pathName: "/update/access.json",
      status: 200,
      machine,
      deviceId,
      macs,
      mac,
      token,
    });
    row = activeClients.get(clientKey);
  }
  if (row) {
    row.lastDenyAt = entry.at;
    row.lastDenyAtMs = now;
    row.lastDenyReason = entry.reason;
    row.lastDenyMatch = entry.match;
    row.lastAccessAllowed = false;
    row.lastAccessAtMs = now;
  }
  logWarn(
    `access deny ip=${entry.ip} match=${entry.match} mode=${entry.mode} machine=${entry.machine || "-"} device=${entry.deviceId || "-"} token=${entry.token ? "yes" : "no"} reason=${entry.reason}`,
  );
}

function noteAccessAllow({ ip, machine, deviceId, mac, macs, token }) {
  const now = Date.now();
  const identified = !!(machine || deviceId || (macs && macs.length) || mac || token);
  const clientKey = identified
    ? `dev:${(machine || "host").toLowerCase()}:${(deviceId || "").toLowerCase() || (macs && macs[0]) || mac || token || "na"}`
    : `ip:${ip}`;
  let row = activeClients.get(clientKey);
  if (!row) {
    // touchClient 通常已在 finish 时创建；此处兜底，避免竞态丢 lastAllow。
    touchClient({
      ip,
      kind: "update",
      pathName: "/update/access.json",
      status: 200,
      machine,
      deviceId,
      macs,
      mac,
      token,
    });
    row = activeClients.get(clientKey);
  }
  if (!row) return;
  row.lastAllowAt = ts(new Date(now));
  row.lastAllowAtMs = now;
  row.lastAccessAllowed = true;
  row.lastAccessAtMs = now;
}

/** 运维台门禁/租约一眼状态（服务端估算，非客户端本地真相）。 */
function gateViewForRow(row, now, activeSec, banned, allowed, accessMode) {
  const lastAllowMs = row.lastAllowAtMs || 0;
  const lastDenyMs = row.lastDenyAtMs || 0;
  const lastAccessAtMs = row.lastAccessAtMs || 0;
  const leaseRemainSec =
    lastAllowMs > 0
      ? Math.max(0, Math.floor((lastAllowMs + kOnlineLeaseTtlMs - now) / 1000))
      : 0;
  let gate = "unknown";
  if (banned || (accessMode === "allow" && !allowed)) {
    gate = "policy_deny";
  } else if (lastDenyMs > 0 && lastDenyMs >= lastAllowMs) {
    gate = "denied";
  } else if (lastAllowMs > 0) {
    const recentAllow =
      row.lastAccessAllowed === true && now - lastAccessAtMs <= Math.max(activeSec, 120) * 1000;
    if (recentAllow) gate = "probe_ok";
    else if (leaseRemainSec > 0) gate = "lease";
    else gate = "lease_expired";
  } else {
    gate = "no_allow";
  }
  return {
    gate,
    leaseRemainSec,
    lastAllowAt: row.lastAllowAt || "",
    leaseTtlHours: 64,
  };
}

function listActiveClients(activeSec) {
  const now = Date.now();
  const cutoff = now - activeSec * 1000;
  const online = [...activeClients.values()].filter((r) => r.lastSeenMs >= cutoff);
  const byIp = new Map();
  for (const row of online) byIp.set(row.ip, (byIp.get(row.ip) || 0) + 1);
  const accessMode = access.getMode();
  return online
    .map((row) => {
      const banned = access.isBanned({
        machine: row.machine,
        deviceId: row.deviceId,
        macs: row.macs,
        token: row.token,
      });
      const allowed = access.isAllowed({
        machine: row.machine,
        deviceId: row.deviceId,
        macs: row.macs,
        token: row.token,
      });
      const gv = gateViewForRow(row, now, activeSec, banned, allowed, accessMode);
      return {
        key: row.key,
        ip: row.ip,
        geo: ipGeo.displayGeo(row),
        geoStatus: row.geoStatus || "",
        machine: row.identified ? row.machine || "" : "",
        deviceId: row.identified ? row.deviceId || "" : "",
        device: row.identified ? row.device || "" : "",
        mac: row.identified ? row.mac || "" : "",
        macs: row.identified ? row.macs || [] : [],
        token: row.identified ? row.token || "" : "",
        appVersion: row.appVersion || "",
        identified: !!row.identified,
        sameIpOnline: byIp.get(row.ip) || 1,
        knownOnIp: devicesByIp.get(row.ip)?.size || 0,
        lastKind: row.lastKind || "",
        lastPath: row.lastPath || "",
        lastStatus: row.lastStatus || 0,
        hits: row.hits || 0,
        ua: row.ua || "",
        firstSeenAt: ts(new Date(row.firstSeenMs)),
        lastSeenAt: ts(new Date(row.lastSeenMs)),
        idleSec: Math.max(0, Math.floor((now - row.lastSeenMs) / 1000)),
        banned,
        allowed,
        accessMode,
        lastDenyAt: row.lastDenyAt || "",
        lastDenyReason: row.lastDenyReason || "",
        lastDenyMatch: row.lastDenyMatch || "",
        lastAllowAt: gv.lastAllowAt,
        gate: gv.gate,
        leaseRemainSec: gv.leaseRemainSec,
        leaseTtlHours: gv.leaseTtlHours,
      };
    })
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

const logUpload = createLogUpload({
  outRoot,
  acceptProfiles,
  sendJson,
  logInfo,
  logWarn,
  logError,
  getShuttingDown: () => shuttingDown,
  noteError,
  clientIp,
});

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
  return routedPath === "/update/force.json" || routedPath === "/update/access.json";
}

function appendAccessLog(entry) {
  const line = `${JSON.stringify(entry)}\n`;
  accessLogReady = accessLogReady
    .then(() => fs.appendFile(accessLogPath, line, "utf8"))
    .catch((err) => logWarn(`access log write failed: ${err.message || err}`));
}

function recordRequest({
  method,
  pathName,
  routedPath,
  ip,
  status,
  ms,
  kind,
  ua,
  machine,
  deviceId,
  appVersion,
  macs,
  mac,
  token,
}) {
  stats.requestsTotal += 1;
  stats.lastRequestAt = ts();
  if (stats.byKind[kind] != null) stats.byKind[kind] += 1;
  else stats.byKind.other += 1;
  const bucket = statusBucket(status);
  if (stats.byStatus[bucket] != null) stats.byStatus[bucket] += 1;
  else stats.byStatus.other += 1;

  if (isLoopback(ip) && (kind === "health" || kind === "ready" || kind === "admin")) return;

  touchClient({ ip, kind, pathName, status, ua, machine, deviceId, appVersion, macs, mac, token });

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
    token: token || undefined,
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
      macs: id.macs,
      mac: id.mac,
      token: id.token,
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
  if (routedPath === "/update/access.json") {
    const id = clientIdentityFromReq(req);
    const decision = access.evaluate({
      machine: id.machine,
      deviceId: id.deviceId,
      macs: id.macs,
      token: id.token,
    });
    if (!decision.allowed) {
      noteAccessDeny({
        ip: clientIp(req),
        machine: id.machine,
        deviceId: id.deviceId,
        mac: id.mac,
        macs: id.macs,
        token: id.token,
        decision,
      });
    } else {
      noteAccessAllow({
        ip: clientIp(req),
        machine: id.machine,
        deviceId: id.deviceId,
        mac: id.mac,
        macs: id.macs,
        token: id.token,
      });
    }
    sendJson(res, 200, {
      ok: true,
      allowed: !!decision.allowed,
      mode: decision.mode,
      reason: decision.reason || "",
      key: decision.key || "",
      match: decision.match || "",
      at: decision.at || "",
    });
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
    uploads: logUpload.uploadHealthSlice(),
    limits: logUpload.uploadLimitsSlice(),
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
  try {
    await logUpload.ensureDirs();
    const probe = path.join(logUpload.outRoot, `.ready-${process.pid}`);
    await fs.writeFile(probe, "ok\n");
    await fs.unlink(probe);
    checks.outRoot = "ok";
  } catch (err) {
    checks.outRoot = err.message || String(err);
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
    let refreshGeo = false;
    try {
      const u = new URL(req.url || "/", "http://127.0.0.1");
      const n = Number(u.searchParams.get("activeSec") || kClientActiveDefaultSec);
      if (Number.isFinite(n) && n > 0) activeSec = Math.min(3600, Math.floor(n));
      refreshGeo = u.searchParams.get("refreshGeo") === "1";
    } catch {
      /* keep default */
    }
    const clients = listActiveClients(activeSec);
    if (refreshGeo) {
      ipGeo.invalidateAllAndRefresh();
    } else {
      for (const c of clients) {
        if (c.geoStatus === "pending" || (!c.geo && c.geoStatus !== "private")) {
          ipGeo.scheduleGeoLookup(c.ip);
        }
      }
    }
    const snap = access.snapshot();
    const ipAlerts = listIpMultiDeviceAlerts();
    sendJson(res, 200, {
      ok: true,
      activeSec,
      count: clients.length,
      geoProvider: ipGeo.provider,
      tracked: activeClients.size,
      accessMode: snap.mode,
      banCount: snap.banCount,
      allowCount: snap.allowCount,
      recentDenies: recentAccessDenies.slice(0, 20),
      ipMultiDeviceAlerts: ipAlerts,
      ipMultiDeviceAlertCount: ipAlerts.length,
      clients,
    });
    return;
  }
  if ((routedPath === "/admin/bans" || routedPath === "/admin/access") && req.method === "GET") {
    const snap = access.snapshot();
    sendJson(res, 200, {
      ok: true,
      mode: snap.mode,
      count: snap.banCount,
      banCount: snap.banCount,
      allowCount: snap.allowCount,
      bans: snap.bans,
      allows: snap.allows,
      path: snap.path,
    });
    return;
  }
  if ((routedPath === "/admin/bans" || routedPath === "/admin/access") && req.method === "POST") {
    const body = await readJsonBody(req);
    const action = String(body?.action || "ban").trim().toLowerCase();
    const identity = {
      machine: body?.machine,
      deviceId: body?.deviceId,
      mac: body?.mac,
      token: body?.token,
      key: body?.key,
      reason: body?.reason,
      by: body?.bannedBy || body?.allowedBy || body?.by || "ops",
    };
    if (action === "setmode" || action === "set_mode" || action === "mode") {
      const next = await access.setMode(body?.mode);
      logInfo(`device access mode -> ${next}`);
      sendJson(res, 200, { ok: true, action: "setMode", mode: next, ...access.snapshot() });
      return;
    }
    if (action === "ban") {
      const row = access.ban(identity);
      await access.persist();
      logInfo(`device ban add key=${row.key}`);
      sendJson(res, 200, { ok: true, action: "ban", ban: row, ...access.snapshot() });
      return;
    }
    if (action === "unban") {
      const existed = access.unban(identity);
      await access.persist();
      logInfo(`device ban remove key=${existed?.key || identity.key || ""} found=${!!existed}`);
      sendJson(res, 200, {
        ok: true,
        action: "unban",
        removed: !!existed,
        ban: existed,
        ...access.snapshot(),
      });
      return;
    }
    if (action === "allow") {
      const row = access.allow(identity);
      await access.persist();
      logInfo(`device allow add key=${row.key}`);
      sendJson(res, 200, { ok: true, action: "allow", allow: row, ...access.snapshot() });
      return;
    }
    if (action === "unallow" || action === "deny-allow" || action === "revoke") {
      const existed = access.unallow(identity);
      await access.persist();
      logInfo(`device allow remove key=${existed?.key || identity.key || ""} found=${!!existed}`);
      sendJson(res, 200, {
        ok: true,
        action: "unallow",
        removed: !!existed,
        allow: existed,
        ...access.snapshot(),
      });
      return;
    }
    sendJson(res, 400, {
      ok: false,
      error: "action must be ban|unban|allow|unallow|setMode",
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
    if (routedPath === "/v1/logs" || routedPath.startsWith("/v1/logs/")) {
      const id = clientIdentityFromReq(req);
      const decision = access.evaluate({
        machine: id.machine,
        deviceId: id.deviceId,
        macs: id.macs,
        token: id.token,
      });
      if (!decision.allowed) {
        noteAccessDeny({
          ip,
          machine: id.machine,
          deviceId: id.deviceId,
          mac: id.mac,
          macs: id.macs,
          token: id.token,
          decision,
        });
        sendJson(res, 403, {
          ok: false,
          error: "device access denied",
          reason: decision.reason || "denied",
          mode: decision.mode,
          key: decision.key || "",
        });
        return;
      }
      await logUpload.handleLogRoutes(req, res, routedPath);
      return;
    }
    if (routedPath.startsWith("/update/")) {
      await handleUpdate(req, res, routedPath);
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
await logUpload.ensureDirs();
await access.load();

server.listen(port, host, () => {
  logInfo(`xcat twms update server v${SERVER_VERSION} listening on http://${host}:${port}`);
  logInfo(`base path: ${basePath || "/"}`);
  logInfo(`updates: ${releaseRoot}`);
  logInfo(`uploads: ${logUpload.outRoot} (devices -> ${logUpload.deviceBucketRoot})`);
  logInfo(`accept profiles: ${acceptProfiles.join(",") || "(any)"}`);
  logInfo(`access log: ${accessLogPath}`);
  logInfo(`ip geo: ${ipGeo.provider} (multi-source; cache v2)`);
  const snap = access.snapshot();
  logInfo(
    `device access: mode=${snap.mode} bans=${snap.banCount} allows=${snap.allowCount} (${snap.path})`,
  );
  logInfo(`client default: http://xcat.work:${port}${basePath}`);
  logInfo(`admin: POST ${basePath}/admin/shutdown (loopback only)`);
});

server.on("error", (err) => {
  logError(`listen error: ${err.message || err}`);
  process.exit(1);
});
