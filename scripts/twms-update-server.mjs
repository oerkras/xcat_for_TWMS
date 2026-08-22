#!/usr/bin/env node
/**
 * XCat TWMS update API：健康检查 + /update/* 分发 + 日志上传 + 运维 admin。
 * 网页下载站为 publish_site Python :52080（非本进程）。
 *
 * 绑定默认：http://0.0.0.0:18789/twms
 * 客户端默认：http://xcat.work:18789/twms
 *   GET  /twms/health
 *   GET  /twms/ready
 *   GET  /twms/update/latest.json  (按 update-channels.json：默认版 + uid/TOKEN 分组覆盖)
 *   GET  /twms/update/force.json   (无 force-update.json → 404)
 *   GET  /twms/update/access.json  (按 X-XCat-* 头查是否禁止使用)
 *   GET  /twms/update/<zip>
 *   POST /twms/v1/logs/sessions · PUT …/files/:name · POST …/commit
 *   POST /twms/v1/logs            (legacy JSON)
 *   POST /twms/admin/shutdown      (loopback)
 *   GET  /twms/admin/stats         (loopback)
 *   GET  /twms/admin/clients       (loopback；按 X-XCat-* 头追踪，仅「此刻在线」)
 *   GET  /twms/admin/client-history(loopback；落盘历史 ?days=&limit=，含租约剩余估算)
 *   GET  /twms/admin/bans          (loopback；封禁清单，兼容)
 *   POST /twms/admin/bans          (loopback；action=ban|unban|allow|unallow|setMode|setStrict
 *                                            |revokeJti|unrevokeJti —— 后两个按卡号废单张卡)
 *   GET  /twms/admin/access        (loopback；mode+黑白名单)
 *   POST /twms/admin/log-fetch     (loopback；action=enqueue|cancel；mode=light|full)
 *   GET  /twms/admin/log-fetch     (loopback；待拉取队列)
 *   POST /twms/admin/force-target  (loopback；指定设备强更 enqueue|cancel)
 *   GET  /twms/admin/force-target  (loopback；指定强更队列)
 *   GET  /twms/admin/update-channels (loopback；对外允许版本 + 分组覆盖)
 *   POST /twms/admin/update-channels (loopback；set-default|set-group|clear-group)
 */
import http from "node:http";
import crypto from "node:crypto";
import { createReadStream } from "node:fs";
import fs from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { createLogUpload } from "./twms-log-upload.mjs";
import { createDeviceAccess } from "./twms-device-access.mjs";
import { createDeviceQuota } from "./twms-device-quota.mjs";
import { createClientHistory } from "./twms-client-history.mjs";
import { createLogFetchQueue } from "./twms-log-fetch.mjs";
import { createForceTargetQueue } from "./twms-force-target.mjs";
import { createIpGeo } from "./twms-ip-geo.mjs";
import { createUpdateChannels } from "./twms-update-channels.mjs";

const SERVER_VERSION = "0.4.12";
const kChinaTz = "Asia/Shanghai";
const __dirname = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(__dirname, "..");

// gate/1 启动 TOKEN 离线签发（供 OPS「签卡」页调用）。私钥只在本机 secrets 下，
// 不入库、不外发；接口仅 loopback 可达（见 handleAdmin 的 isLoopback 守卫）。
const gatePrivPath = path.join(repoRoot, "secrets", "gate_ec_priv.pem");
const gateCardsPath = path.join(repoRoot, "artifacts", "ops_logs", "gate_cards.jsonl");

// jti → 台账行 的内存索引。派生凭证（X-XCat-Gate-Proof）验 HMAC 需要卡的签名段，
// 而签名段只存在台账里；探活是热路径，不能每次读盘解析整个 jsonl。
// 卡都由本服务签发（签发时同步入索引），故启动 load 一次即够；手工编辑台账需重启服务。
const gateCardIndex = new Map();

// 索引键用 payload 段而非 jti：proof 里天然带着完整 payload，命中即证明这个 payload 就是
// 台账里某张卡的原文（自编 payload 必然 miss），不必再单独比对一次。
// 更重要的是 jti 上线前签发的老卡 payload 里没有 jti，用 jti 当键会让它们永远查不到。
function indexGateCard(entry) {
  const token = String(entry?.token || "");
  const dot = token.indexOf(".");
  if (dot <= 0 || dot + 1 >= token.length) return;
  gateCardIndex.set(token.slice(0, dot), {
    token,
    uid: String(entry.uid || ""),
  });
}

function lookupGateCardByPayload(payloadB64) {
  return gateCardIndex.get(String(payloadB64 || "")) || null;
}

async function loadGateCardIndex() {
  const cards = await listGateCards(100000);
  gateCardIndex.clear();
  for (const c of cards) indexGateCard(c);
  logInfo(
    `gate-card index loaded ${gateCardIndex.size} cards (派生凭证校验依赖它；台账丢失=新客户端认不出 uid)`,
  );
}

async function appendGateCard(entry) {
  try {
    await fs.mkdir(path.dirname(gateCardsPath), { recursive: true });
    await fs.appendFile(gateCardsPath, JSON.stringify(entry) + "\n", "utf8");
    indexGateCard(entry);
    return true;
  } catch (e) {
    logWarn?.(`gate-card ledger append failed: ${e?.message || e}`);
    return false;
  }
}

async function listGateCards(limit = 500) {
  let raw;
  try {
    raw = await fs.readFile(gateCardsPath, "utf8");
  } catch {
    return [];
  }
  const rows = [];
  for (const line of raw.split(/\r?\n/)) {
    const s = line.trim();
    if (!s) continue;
    try {
      const o = JSON.parse(s);
      if (o && o.uid) rows.push(o);
    } catch {
      /* skip corrupt line */
    }
  }
  // 最近签发在前；限制条数。
  rows.reverse();
  return rows.slice(0, Math.max(1, limit));
}

async function signGateToken(uid, days, note, by) {
  const u = String(uid || "").trim();
  if (!u) return { ok: false, error: "uid 必填" };
  if (u.length > 64) return { ok: false, error: "uid 过长（<=64）" };
  const d = Math.max(0, Math.floor(Number(days) || 0));
  let pem;
  try {
    pem = await fs.readFile(gatePrivPath, "utf8");
  } catch {
    return {
      ok: false,
      error: "未找到私钥 secrets/gate_ec_priv.pem；先跑 node scripts/xcat-gate-keygen.mjs",
    };
  }
  try {
    const privateKey = crypto.createPrivateKey(pem);
    const now = Math.floor(Date.now() / 1000);
    // jti = 卡号，签进 payload 才能做卡级吊销（只废这一张，同 uid 的新卡不受影响）。
    // 台账 id 复用同一个值，台账行与卡一一对应；客户端验签按字段取值，多这个字段不影响老客户端。
    const jti = crypto.randomBytes(6).toString("hex");
    const payload = { uid: u, iss: now, exp: d > 0 ? now + d * 86400 : 0, jti };
    const payloadB64 = Buffer.from(JSON.stringify(payload), "utf8").toString("base64url");
    const sig = crypto.sign("sha256", Buffer.from(payloadB64, "utf8"), {
      key: privateKey,
      dsaEncoding: "ieee-p1363",
    });
    const token = `${payloadB64}.${sig.toString("base64url")}`;
    // 台账已进入派生凭证的验证链路（verifyGateProof 要靠它取签名段算 mac）：写不进去就等于
    // 签了一张注定验不过的卡，运维照常发出去、成员永远「无 uid」。宁可签发失败让人当场修盘。
    const ledgerOk = await appendGateCard({
      id: jti,
      jti,
      uid: u,
      iss: now,
      exp: payload.exp,
      days: d,
      note: String(note || "").slice(0, 200),
      by: String(by || "ops").slice(0, 40),
      at: new Date(now * 1000).toISOString(),
      token,
    });
    if (!ledgerOk) {
      return {
        ok: false,
        error:
          "台账写入失败（artifacts/ops_logs/gate_cards.jsonl）；卡已废弃未发出。" +
          "派生凭证校验依赖台账，先修磁盘/权限再重签",
      };
    }
    return { ok: true, id: jti, jti, token, uid: u, iss: now, exp: payload.exp, days: d };
  } catch (e) {
    return { ok: false, error: `签发失败: ${e?.message || e}` };
  }
}

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
const kIpMultiLogCooldownMs = 15 * 60 * 1000;
const kIpMultiLogStartupGraceMs = 90 * 1000;
const kIpMultiLogMinJump = 2;
const kIpMultiLogMaxPerMin = 6;
const ipMultiLogStartedAt = Date.now();
/** @type {Map<string, { t: number, n: number }>} */
const ipMultiLogAt = new Map();
let ipMultiLogWindowStart = 0;
let ipMultiLogWindowCount = 0;
let ipMultiLogWindowDropped = 0;

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

const quota = createDeviceQuota({
  releaseRoot,
  repoRoot,
  defaultMax: Number(process.env.XCAT_TWMS_QUOTA_DEFAULT || 0) || 0,
  agingDays: Number(process.env.XCAT_TWMS_QUOTA_AGING_DAYS || 0) || 0,
  lookupCardByPayload: lookupGateCardByPayload,
  proofSkewSec: Number(process.env.XCAT_TWMS_GATE_PROOF_SKEW_SEC || 0) || 900,
  logInfo,
  logWarn,
  ts,
});

const clientHistory = createClientHistory({
  releaseRoot,
  repoRoot,
  logInfo,
  logWarn,
  ts,
  keepDays: Number(process.env.XCAT_TWMS_CLIENT_HISTORY_DAYS || 0) || 30,
});

const logFetch = createLogFetchQueue({
  logInfo,
  logWarn,
  ts,
  parseMacList: (v) => access.parseMacList(v),
  normalizeMac: (v) => {
    const list = access.parseMacList(v);
    return list[0] || "";
  },
});

const forceTarget = createForceTargetQueue({
  logInfo,
  ts,
  parseMacList: (v) => access.parseMacList(v),
});

const updateChannels = createUpdateChannels({
  releaseRoot,
  logInfo,
  logWarn,
  ts,
  normalizeUid: (v) => access.normalizeUid(v),
  normalizeToken: (v) => access.normalizeToken(v),
});

function headerText(req, name) {
  const v = req?.headers?.[name];
  const raw = Array.isArray(v) ? String(v[0] || "") : String(v || "");
  return raw.replace(/[\u0000-\u001f\u007f]/g, "").trim();
}

/** 解码角色名/职业：优先 b64.<base64>；旧客户端明文亦兼容。 */
function decodeCharHeaderText(raw, maxChars) {
  const s = String(raw || "").trim();
  if (!s) return "";
  if (s.startsWith("b64.")) {
    try {
      const decoded = Buffer.from(s.slice(4), "base64").toString("utf8");
      return decoded.replace(/[\u0000-\u001f\u007f]/g, "").trim().slice(0, maxChars);
    } catch {
      return "";
    }
  }
  return s.slice(0, maxChars);
}

/** 探活头 `2040001:3,2070005:80`；`-` 或空 = 已采到但身上无高价值消耗（卷/雷之鏢）。 */
function parseWealthScrollsHeader(raw) {
  const s = String(raw || "").trim();
  if (!s || s === "-") return "";
  const parts = [];
  for (const piece of s.split(",")) {
    const m = /^(\d{3,9}):(\d{1,8})$/.exec(String(piece || "").trim());
    if (m) parts.push(`${m[1]}:${m[2]}`);
    if (parts.join(",").length >= 360) break;
  }
  return parts.join(",").slice(0, 360);
}

function clientIdentityFromReq(req) {
  const macRaw = headerText(req, "x-xcat-mac");
  const macs = access.parseMacList(macRaw);
  const token = access.normalizeToken(headerText(req, "x-xcat-token") || "");
  const charName = decodeCharHeaderText(headerText(req, "x-xcat-char-name"), 48);
  const charLevelRaw = headerText(req, "x-xcat-char-level");
  const charJobRaw = headerText(req, "x-xcat-char-job");
  const charJobName = decodeCharHeaderText(headerText(req, "x-xcat-char-job-name"), 32);
  const charMesoRaw = headerText(req, "x-xcat-char-meso");
  const charLevel = /^-?\d+$/.test(charLevelRaw) ? Number(charLevelRaw) : null;
  const charJob = /^-?\d+$/.test(charJobRaw) ? Number(charJobRaw) : null;
  const charMeso = /^-?\d+$/.test(charMesoRaw) ? charMesoRaw : "";
  const hasWealthHeader = Object.prototype.hasOwnProperty.call(
    req.headers || {},
    "x-xcat-wealth-scrolls",
  );
  const wealthScrolls = hasWealthHeader
    ? parseWealthScrollsHeader(headerText(req, "x-xcat-wealth-scrolls"))
    : "";
  const mapIdRaw = headerText(req, "x-xcat-map-id");
  const channelRaw = headerText(req, "x-xcat-channel");
  const mapName = decodeCharHeaderText(headerText(req, "x-xcat-map-name"), 64);
  const mapId = /^\d+$/.test(mapIdRaw) ? Number(mapIdRaw) : null;
  const channelId = /^-?\d+$/.test(channelRaw) ? Number(channelRaw) : null;
  const deviceId = headerText(req, "x-xcat-device-id").slice(0, 64);
  // 新客户端发派生凭证（整张卡不上网）；老客户端仍发整张卡，两者都认，优先前者。
  const gateProof = headerText(req, "x-xcat-gate-proof").slice(0, 400);
  const gateToken = headerText(req, "x-xcat-gate-token").slice(0, 400);
  const gateClaims =
    quota.verifyGateProof(gateProof, deviceId) || quota.verifyGateToken(gateToken);
  return {
    machine: headerText(req, "x-xcat-machine").slice(0, 80),
    deviceId,
    appVersion: headerText(req, "x-xcat-app-version").slice(0, 64),
    gateToken,
    gateProof,
    gateClaims,
    gateUid: gateClaims ? access.normalizeUid(gateClaims.uid) : "",
    gateExp: gateClaims ? gateClaims.exp || 0 : 0,
    macs,
    mac: macs[0] ? access.formatMac(macs[0]) : "",
    token,
    charName,
    charLevel,
    charJob,
    charJobName,
    charMeso,
    hasWealthHeader,
    wealthScrolls,
    hasChar: !!(charName && charLevel != null && charLevel > 0),
    mapId,
    mapName,
    channelId,
    hasMap: !!(mapId != null && mapId > 0) || !!(channelId != null && channelId > 0),
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

function shouldLogSameIpMulti(ip, size) {
  const now = Date.now();
  if (now - ipMultiLogStartedAt < kIpMultiLogStartupGraceMs) return false;
  const prev = ipMultiLogAt.get(ip);
  if (prev && now - prev.t < kIpMultiLogCooldownMs && size - prev.n < kIpMultiLogMinJump) {
    return false;
  }
  if (now - ipMultiLogWindowStart >= 60 * 1000) {
    if (ipMultiLogWindowDropped > 0) {
      logWarn(
        `same-ip multi-device throttled dropped=${ipMultiLogWindowDropped} (max ${kIpMultiLogMaxPerMin}/min)`,
      );
    }
    ipMultiLogWindowStart = now;
    ipMultiLogWindowCount = 0;
    ipMultiLogWindowDropped = 0;
  }
  if (ipMultiLogWindowCount >= kIpMultiLogMaxPerMin) {
    ipMultiLogWindowDropped += 1;
    return false;
  }
  ipMultiLogWindowCount += 1;
  ipMultiLogAt.set(ip, { t: now, n: size });
  return true;
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
  // 同公网 IP 冒出新 deviceId：告警（NAT 多机或泄露扩散）。OPS 连接表仍全量记，这里只节流日志。
  if (wasNew && set.size >= 2 && shouldLogSameIpMulti(ip, set.size)) {
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

/** OPS 同 IP 告警列表：全量易把 /admin/clients 撑到 >256KB，截断 UTF-8 → 界面 '?'。 */
const kIpAlertListCap = 48;
const kIpAlertDevicesCap = 4;

function listIpMultiDeviceAlerts({ limit = kIpAlertListCap } = {}) {
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
      if (devices.length >= kIpAlertDevicesCap) break;
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
      devices,
    });
  }
  out.sort((a, b) => b.deviceCount - a.deviceCount || a.ip.localeCompare(b.ip));
  const total = out.length;
  const capped = Number.isFinite(limit) && limit > 0 ? out.slice(0, Math.floor(limit)) : out;
  return { alerts: capped, total };
}

function clientHasIdentity({ machine, deviceId, macs, mac, token }) {
  return !!(machine || deviceId || (macs && macs.length) || mac || token);
}

function hydrateCharFromHistory(row) {
  if (!row || row.charName) return;
  const prev =
    clientHistory.get(row.key) ||
    (row.deviceId ? clientHistory.getByDeviceId(row.deviceId) : null);
  if (!prev?.charName) return;
  row.charName = String(prev.charName).slice(0, 48);
  if (!row.charLevel && prev.charLevel) row.charLevel = prev.charLevel;
  if (!row.charJobName && prev.charJobName) row.charJobName = String(prev.charJobName).slice(0, 32);
}

function pruneActiveClients(now = Date.now()) {
  for (const [k, v] of activeClients) {
    // 下包 / latest.json 不带头，曾经按 ip: 建过空壳行。那些行没有身份，运维台看起来像「无名氏探活」。
    const ghost = !clientHasIdentity(v);
    if (ghost || now - v.lastSeenMs > kClientPruneSec * 1000) activeClients.delete(k);
  }
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
  uid,
  gateExp,
  charName,
  charLevel,
  charJob,
  charJobName,
  charMeso,
  hasChar,
  hasWealthHeader,
  wealthScrolls,
  mapId,
  mapName,
  channelId,
  hasMap,
}) {
  if (!ip || ip === "unknown") return;
  const now = Date.now();
  // 没身份的请求（拉 latest.json、下 zip）不算探活：进表只会多出「未识别 / 角色全空」的幽灵行。
  if (!clientHasIdentity({ machine, deviceId, macs, mac, token })) {
    pruneActiveClients(now);
    return;
  }
  const key = `dev:${(machine || "host").toLowerCase()}:${(deviceId || "").toLowerCase() || (macs && macs[0]) || mac || token || "na"}`;
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
      uid: "",
      gateExp: 0,
      identified: false,
      charName: "",
      charLevel: 0,
      charJob: 0,
      charJobName: "",
      charMeso: "",
      hasWealthScrolls: false,
      wealthScrolls: "",
      mapId: 0,
      mapName: "",
      channelId: 0,
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
    hydrateCharFromHistory(row);
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
  if (uid) row.uid = access.normalizeUid(uid);
  if (gateExp) row.gateExp = gateExp;
  if (Array.isArray(macs) && macs.length) {
    row.macs = macs.slice(0, 8);
    row.mac = mac || access.formatMac(macs[0]);
  } else if (mac) {
    row.mac = mac;
  }
  // 仅在探活带上有效角色快照时刷新；未进图的请求不抹掉上次快照。
  if (hasChar) {
    row.charName = String(charName || "").slice(0, 48);
    row.charLevel = Number.isFinite(charLevel) ? Math.max(0, Math.floor(charLevel)) : 0;
    row.charJob = Number.isFinite(charJob) ? Math.floor(charJob) : 0;
    row.charJobName = String(charJobName || "").slice(0, 32);
    row.charMeso = String(charMeso || "").replace(/[^\d-]/g, "").slice(0, 24);
    if (hasWealthHeader) {
      row.hasWealthScrolls = true;
      row.wealthScrolls = String(wealthScrolls || "").slice(0, 360);
    }
  } else {
    // 重启 / 更新后第一轮探活常不带角色（游戏还没进）。内存快照没了，从落盘历史补回名字。
    hydrateCharFromHistory(row);
  }
  // 地图/频道同口径：有新值才刷，未进图探活不抹掉上次。
  // mapName 勿在仅带 channel、mapId=0 时清空（否则 OPS 留下旧 mapId + 空名）。
  if (hasMap) {
    if (Number.isFinite(mapId) && mapId > 0) {
      row.mapId = Math.floor(mapId);
      const name = String(mapName || "").slice(0, 64);
      if (name) row.mapName = name;
    }
    if (Number.isFinite(channelId) && channelId > 0) row.channelId = Math.floor(channelId);
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

  // 落历史台账：activeClients 到点就 prune、重启即清空，长期可追溯靠这一份（节流写盘）。
  clientHistory.touch(row);

  pruneActiveClients(now);
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

  const identified = clientHasIdentity({ machine, deviceId, macs, mac, token });
  const clientKey = identified
    ? `dev:${(machine || "host").toLowerCase()}:${(deviceId || "").toLowerCase() || (macs && macs[0]) || mac || token || "na"}`
    : "";
  let row = identified ? activeClients.get(clientKey) : null;
  if (identified && !row) {
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
    // 拒绝原因要落历史：事后查「他为什么用不了」全靠这条（内存 row 一小时后就没了）。
    clientHistory.touch(row);
  }
  logWarn(
    `access deny ip=${entry.ip} match=${entry.match} mode=${entry.mode} machine=${entry.machine || "-"} device=${entry.deviceId || "-"} token=${entry.token ? "yes" : "no"} reason=${entry.reason}`,
  );
}

function noteAccessAllow({ ip, machine, deviceId, mac, macs, token }) {
  const now = Date.now();
  if (!clientHasIdentity({ machine, deviceId, macs, mac, token })) return;
  const clientKey = `dev:${(machine || "host").toLowerCase()}:${(deviceId || "").toLowerCase() || (macs && macs[0]) || mac || token || "na"}`;
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
  // 落历史：租约剩余按 lastAllowAtMs + 64h 估算，这一笔就是「他的租约续到什么时候」。
  clientHistory.touch(row);
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
  pruneActiveClients(now);
  const cutoff = now - activeSec * 1000;
  const online = [...activeClients.values()].filter((r) => r.lastSeenMs >= cutoff);
  for (const row of online) hydrateCharFromHistory(row);
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
        uid: row.uid,
      });
      const allowed = access.isAllowed({
        machine: row.machine,
        deviceId: row.deviceId,
        macs: row.macs,
        token: row.token,
        uid: row.uid,
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
        uid: row.uid || "",
        gateExp: row.gateExp || 0,
        appVersion: row.appVersion || "",
        charName: row.charName || "",
        charLevel: row.charLevel || 0,
        charJob: row.charJob || 0,
        charJobName: row.charJobName || "",
        charMeso: row.charMeso || "",
        hasWealthScrolls: !!row.hasWealthScrolls,
        wealthScrolls: row.wealthScrolls || "",
        mapId: row.mapId || 0,
        mapName: row.mapName || "",
        channelId: row.channelId || 0,
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
        logFetch: logFetch.statusFor({
          machine: row.machine,
          deviceId: row.deviceId,
          macs: row.macs,
          mac: row.mac,
          token: row.token,
        }),
        forceTarget: forceTarget.statusFor({
          machine: row.machine,
          deviceId: row.deviceId,
          macs: row.macs,
          mac: row.mac,
          token: row.token,
        }),
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
  // 运维台 / 访问日志统一北京时间；内部仍用 epoch ms。
  const fmt = new Intl.DateTimeFormat("en-CA", {
    timeZone: kChinaTz,
    year: "numeric",
    month: "2-digit",
    day: "2-digit",
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
    hourCycle: "h23",
  });
  const parts = {};
  for (const p of fmt.formatToParts(d)) {
    if (p.type !== "literal") parts[p.type] = p.value;
  }
  return `${parts.year}-${parts.month}-${parts.day} ${parts.hour}:${parts.minute}:${parts.second}`;
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
  uid,
  gateExp,
  charName,
  charLevel,
  charJob,
  charJobName,
  charMeso,
  hasChar,
  hasWealthHeader,
  wealthScrolls,
  mapId,
  mapName,
  channelId,
  hasMap,
}) {
  stats.requestsTotal += 1;
  stats.lastRequestAt = ts();
  if (stats.byKind[kind] != null) stats.byKind[kind] += 1;
  else stats.byKind.other += 1;
  const bucket = statusBucket(status);
  if (stats.byStatus[bucket] != null) stats.byStatus[bucket] += 1;
  else stats.byStatus.other += 1;

  if (isLoopback(ip) && (kind === "health" || kind === "ready" || kind === "admin")) return;

  touchClient({
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
    uid,
    gateExp,
    charName,
    charLevel,
    charJob,
    charJobName,
    charMeso,
    hasChar,
    hasWealthHeader,
    wealthScrolls,
    mapId,
    mapName,
    channelId,
    hasMap,
  });

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
      uid: id.gateUid,
      gateExp: id.gateExp,
      charName: id.charName,
      charLevel: id.charLevel,
      charJob: id.charJob,
      charJobName: id.charJobName,
      charMeso: id.charMeso,
      hasChar: id.hasChar,
      hasWealthHeader: id.hasWealthHeader,
      wealthScrolls: id.wealthScrolls,
      mapId: id.mapId,
      mapName: id.mapName,
      channelId: id.channelId,
      hasMap: id.hasMap,
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
    // 可信 uid 已在 clientIdentityFromReq 里验好（派生凭证优先，老客户端整张卡回退）：
    // 按 uid 封禁 + 严格模式 + 台数配额共用；改硬件也改不掉。
    const gateClaims = id.gateClaims;
    const gateUid = id.gateUid;
    const gateJti = gateClaims?.jti || "";
    let decision;
    if (access.getStrictToken() && !gateUid) {
      // 严格模式：缺有效签名 TOKEN（老客户端 / 被 patch 绕过）直接拒。默认关。
      decision = {
        allowed: false,
        mode: access.getMode(),
        reason: "need signed token",
        key: "",
        match: "strict",
        at: "",
      };
    } else if (gateJti && access.isJtiRevoked(gateJti)) {
      // 卡级吊销：只废这一张卡（泄露/作废），同 uid 的新卡照常放行。
      // 老卡 payload 无 jti → gateJti 为空，不进这一支，行为与上线前一致。
      decision = {
        allowed: false,
        mode: access.getMode(),
        reason: "card revoked",
        key: `jti:${gateJti}`,
        match: "jti",
        at: "",
      };
    } else {
      decision = access.evaluate({
        machine: id.machine,
        deviceId: id.deviceId,
        macs: id.macs,
        token: id.token,
        uid: gateUid,
      });
    }
    // 台数配额：黑白名单放行后再叠加。gateToken 验签取可信 uid，超额则改判拒。
    let quotaInfo = null;
    if (decision.allowed) {
      const q = quota.evaluate({ claims: gateClaims, deviceId: id.deviceId });
      if (q.enabled && q.uid) quotaInfo = q;
      if (q.enabled && !q.allowed) {
        decision.allowed = false;
        decision.reason = q.reason || "quota";
        decision.match = "quota";
      }
    }
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
    const ackId = headerText(req, "x-xcat-log-fetch-ack");
    const doneId = headerText(req, "x-xcat-log-fetch-done");
    const pending = logFetch.onAccess(
      {
        machine: id.machine,
        deviceId: id.deviceId,
        macs: id.macs,
        mac: id.mac,
        token: id.token,
      },
      ackId,
      doneId,
    );
    const payload = {
      ok: true,
      allowed: !!decision.allowed,
      mode: decision.mode,
      reason: decision.reason || "",
      key: decision.key || "",
      match: decision.match || "",
      at: decision.at || "",
    };
    // uid 必须在配额之外单独回：客户端靠它判断「这张本地卡服务端认不认」，
    // 认不出才弹重贴。以前只在 quotaInfo 里带 uid，配额关掉时已激活的人会被误判成没卡。
    if (gateUid) payload.uid = gateUid;
    if (quotaInfo) {
      payload.quotaUsed = quotaInfo.used;
      payload.quotaMax = quotaInfo.max;
    }
    if (pending) {
      payload.pendingOp = pending.op;
      payload.pendingId = pending.id;
      payload.pendingMode = pending.mode;
      payload.pendingNote = pending.note || "";
      payload.pending = pending;
    }
    sendJson(res, 200, payload);
    return;
  }
  if (routedPath === "/update/latest.json") {
    const id = clientIdentityFromReq(req);
    const manifest = await updateChannels.resolveManifest({
      uid: id.gateUid,
      token: id.token,
    });
    if (!manifest) {
      sendJson(res, 404, { ok: false, error: "no release manifest" });
      return;
    }
    sendJson(res, 200, {
      version: manifest.version,
      buildId: manifest.buildId,
      name: manifest.name,
      zipName: manifest.zipName,
      downloadUrl: manifest.downloadUrl || manifest.zipName,
      sha256: manifest.sha256,
      size: manifest.size,
      channel: manifest.channel,
    });
    return;
  }
  if (routedPath === "/update/force.json") {
    const id = clientIdentityFromReq(req);
    const targeted = forceTarget.offerForForceGet(
      {
        machine: id.machine,
        deviceId: id.deviceId,
        macs: id.macs,
        mac: id.mac,
        token: id.token,
      },
      id.appVersion,
    );
    if (targeted) {
      sendJson(res, 200, {
        version: targeted.version,
        buildId: targeted.buildId,
        zipName: targeted.zipName,
        sha256: targeted.sha256,
        issuedAt: targeted.issuedAt,
        targetId: targeted.targetId,
        scope: "device",
      });
      return;
    }
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
  const extraZips = [];
  try {
    const forced = JSON.parse(await fs.readFile(path.join(releaseRoot, "force-update.json"), "utf8"));
    if (forced?.zipName) extraZips.push(forced.zipName);
  } catch {
    /* no global force */
  }
  for (const row of forceTarget.list()) {
    if (row?.zipName) extraZips.push(row.zipName);
  }
  if (!(await updateChannels.zipAllowed(zipName, extraZips))) {
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
    // 退出前把节流中的历史落盘，别把最后 20s 的在线记录丢掉。
    void clientHistory.flush();
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
    const { alerts: ipAlerts, total: ipAlertTotal } = listIpMultiDeviceAlerts();
    // clients 放前：即便告警段被客户端截断，在线表仍可解析。
    sendJson(res, 200, {
      ok: true,
      activeSec,
      count: clients.length,
      geoProvider: ipGeo.provider,
      tracked: activeClients.size,
      accessMode: snap.mode,
      strictToken: snap.strictToken,
      banCount: snap.banCount,
      allowCount: snap.allowCount,
      clients,
      recentDenies: recentAccessDenies.slice(0, 20),
      ipMultiDeviceAlerts: ipAlerts,
      ipMultiDeviceAlertCount: ipAlertTotal,
      ipMultiDeviceAlertListed: ipAlerts.length,
    });
    return;
  }
  if (routedPath === "/admin/quota" && req.method === "GET") {
    sendJson(res, 200, { ok: true, ...quota.snapshot() });
    return;
  }
  if (routedPath === "/admin/update-channels" && req.method === "GET") {
    sendJson(res, 200, { ok: true, ...(await updateChannels.snapshot()) });
    return;
  }
  if (routedPath === "/admin/update-channels" && req.method === "POST") {
    const body = await readJsonBody(req);
    const action = String(body?.action || "").trim().toLowerCase();
    try {
      if (action === "set-default" || action === "setdefault" || action === "default") {
        const r = await updateChannels.setDefault(body?.buildId);
        sendJson(res, 200, { ok: true, action: "set-default", ...r, ...(await updateChannels.snapshot()) });
        return;
      }
      if (action === "set-group" || action === "setgroup" || action === "group") {
        const r = await updateChannels.setGroup({
          uid: body?.uid,
          token: body?.token,
          buildId: body?.buildId,
          note: body?.note,
        });
        sendJson(res, 200, { ok: true, action: "set-group", ...r, ...(await updateChannels.snapshot()) });
        return;
      }
      if (action === "clear-group" || action === "cleargroup") {
        const r = await updateChannels.clearGroup({ uid: body?.uid, token: body?.token });
        sendJson(res, 200, { ok: true, action: "clear-group", ...r, ...(await updateChannels.snapshot()) });
        return;
      }
      sendJson(res, 400, { ok: false, error: "action must be set-default|set-group|clear-group" });
    } catch (err) {
      sendJson(res, err?.status || 400, { ok: false, error: err?.message || "update-channels failed" });
    }
    return;
  }
  if (routedPath === "/admin/gate-sign" && req.method === "POST") {
    const body = await readJsonBody(req);
    const r = await signGateToken(body?.uid, body?.days, body?.note, body?.by);
    if (r.ok) logInfo(`gate-sign uid=${r.uid} days=${r.days} exp=${r.exp} id=${r.id}`);
    else logInfo(`gate-sign failed: ${r.error}`);
    sendJson(res, r.ok ? 200 : 400, r);
    return;
  }
  if (routedPath === "/admin/cards" && req.method === "GET") {
    const cards = await listGateCards(500);
    sendJson(res, 200, { ok: true, count: cards.length, cards });
    return;
  }
  if (routedPath === "/admin/quota" && req.method === "POST") {
    const body = await readJsonBody(req);
    const action = String(body?.action || "setmax").trim().toLowerCase();
    if (action === "setmax" || action === "set_max" || action === "max") {
      const r = await quota.setMax(body?.uid, body?.max);
      logInfo(`quota setMax uid=${r.uid} max=${r.max}`);
      sendJson(res, 200, { ok: true, action: "setMax", ...r, ...quota.snapshot() });
      return;
    }
    if (action === "removedevice" || action === "remove_device" || action === "release") {
      const r = await quota.removeDevice(body?.uid, body?.deviceId);
      logInfo(`quota removeDevice uid=${r.uid} device=${String(body?.deviceId || "").slice(0, 16)}`);
      sendJson(res, 200, { ok: true, action: "removeDevice", ...r, ...quota.snapshot() });
      return;
    }
    if (action === "releaseidle" || action === "release_idle") {
      const r = await quota.releaseIdle(body?.uid, body?.days);
      logInfo(
        `quota releaseIdle uid=${r.uid || "*"} days=${r.days} released=${r.released}`,
      );
      sendJson(res, 200, { ok: true, action: "releaseIdle", ...r, ...quota.snapshot() });
      return;
    }
    sendJson(res, 400, { ok: false, error: "unknown action" });
    return;
  }
  if (routedPath === "/admin/client-history" && req.method === "GET") {
    let days = 30;
    let limit = 500;
    try {
      const u = new URL(req.url, "http://127.0.0.1");
      const d = Number(u.searchParams.get("days"));
      if (Number.isFinite(d) && d > 0) days = Math.min(3650, Math.floor(d));
      const l = Number(u.searchParams.get("limit"));
      if (Number.isFinite(l) && l > 0) limit = Math.min(5000, Math.floor(l));
    } catch {
      /* 用默认值 */
    }
    // 标注哪些此刻还在线：历史是落盘的，在线与否得问内存里的 activeClients。
    const onlineCutoff = Date.now() - kClientActiveDefaultSec * 1000;
    const onlineKeys = new Set();
    for (const row of activeClients.values()) {
      if (row.lastSeenMs >= onlineCutoff) onlineKeys.add(row.key);
    }
    const list = clientHistory.list({ days, limit, onlineKeys });
    sendJson(res, 200, {
      ok: true,
      days,
      count: list.length,
      total: clientHistory.count(),
      leaseTtlHours: 64,
      path: clientHistory.path(),
      clients: list,
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
      revokedJtiCount: snap.revokedJtiCount,
      revokedJti: snap.revokedJti,
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
      uid: body?.uid,
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
    if (action === "setstrict" || action === "set_strict" || action === "strict") {
      const next = await access.setStrictToken(
        body?.strict ?? body?.strictToken ?? body?.enabled,
      );
      logInfo(`device access strictToken -> ${next}`);
      sendJson(res, 200, { ok: true, action: "setStrict", strictToken: next, ...access.snapshot() });
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
    if (action === "revokejti" || action === "revoke_card" || action === "revokecard") {
      const row = access.revokeJti({
        jti: body?.jti ?? body?.id,
        uid: body?.uid,
        reason: body?.reason,
        by: body?.by,
      });
      await access.persist();
      logInfo(`gate card revoked jti=${row.jti} uid=${row.uid || "-"}`);
      sendJson(res, 200, { ok: true, action: "revokeJti", card: row, ...access.snapshot() });
      return;
    }
    if (action === "unrevokejti" || action === "unrevoke_card" || action === "unrevokecard") {
      const existed = access.unrevokeJti({ jti: body?.jti ?? body?.id });
      await access.persist();
      logInfo(`gate card unrevoked jti=${body?.jti || body?.id || ""} found=${!!existed}`);
      sendJson(res, 200, {
        ok: true,
        action: "unrevokeJti",
        removed: !!existed,
        card: existed,
        ...access.snapshot(),
      });
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
      error: "action must be ban|unban|allow|unallow|setMode|setStrict|revokeJti|unrevokeJti",
    });
    return;
  }
  if (routedPath === "/admin/log-fetch" && req.method === "GET") {
    sendJson(res, 200, { ok: true, pending: logFetch.list() });
    return;
  }
  if (routedPath === "/admin/log-fetch" && req.method === "POST") {
    const body = await readJsonBody(req);
    const action = String(body?.action || "enqueue").trim().toLowerCase();
    if (action === "cancel") {
      const result = body?.id
        ? logFetch.cancel(String(body.id))
        : logFetch.cancel({
            machine: body?.machine,
            deviceId: body?.deviceId,
            mac: body?.mac,
            macs: body?.macs,
            token: body?.token,
          });
      sendJson(res, 200, { ok: true, action: "cancel", result, pending: logFetch.list() });
      return;
    }
    if (action === "enqueue" || action === "fetch" || action === "request") {
      try {
        const row = logFetch.enqueue({
          machine: body?.machine,
          deviceId: body?.deviceId,
          mac: body?.mac,
          macs: body?.macs,
          token: body?.token,
          mode: body?.mode,
          note: body?.note,
          by: body?.by || "ops",
        });
        sendJson(res, 200, { ok: true, action: "enqueue", job: row, pending: logFetch.list() });
      } catch (err) {
        sendJson(res, err?.status || 400, {
          ok: false,
          error: err?.message || "enqueue failed",
        });
      }
      return;
    }
    sendJson(res, 400, { ok: false, error: "action must be enqueue|cancel" });
    return;
  }
  if (routedPath === "/admin/force-target" && req.method === "GET") {
    sendJson(res, 200, { ok: true, pending: forceTarget.list() });
    return;
  }
  if (routedPath === "/admin/force-target" && req.method === "POST") {
    const body = await readJsonBody(req);
    const action = String(body?.action || "enqueue").trim().toLowerCase();
    if (action === "cancel") {
      const result = body?.id
        ? forceTarget.cancel(String(body.id))
        : forceTarget.cancel({
            machine: body?.machine,
            deviceId: body?.deviceId,
            mac: body?.mac,
            macs: body?.macs,
            token: body?.token,
          });
      sendJson(res, 200, { ok: true, action: "cancel", result, pending: forceTarget.list() });
      return;
    }
    if (action === "enqueue" || action === "push" || action === "target") {
      try {
        // 全体 force-update.json 仍在时禁止指定推送，避免「只推一台、别人照样更」
        const globalForcePath = path.join(releaseRoot, "force-update.json");
        try {
          await fs.access(globalForcePath);
          sendJson(res, 409, {
            ok: false,
            error:
              "global force-update.json is active; clear 全体强制更新 before per-device push",
            code: "global_force_active",
          });
          return;
        } catch {
          /* no global force — ok */
        }

        let version = body?.version;
        let buildId = body?.buildId;
        let zipName = body?.zipName;
        let sha256 = body?.sha256;
        // 未带包信息：先走该身份的允许通道，再回落 latest.json
        if (!zipName || !sha256 || !buildId) {
          const ch = await updateChannels.resolveManifest({
            uid: body?.uid,
            token: body?.token,
          });
          if (ch) {
            version = version || ch.version;
            buildId = buildId || ch.buildId;
            zipName = zipName || ch.zipName;
            sha256 = sha256 || ch.sha256;
          }
        }
        if (!zipName || !sha256 || !buildId) {
          const latestPath = path.join(releaseRoot, "latest.json");
          const latest = JSON.parse(await fs.readFile(latestPath, "utf8"));
          version = version || latest.version;
          buildId = buildId || latest.buildId;
          zipName = zipName || latest.zipName;
          sha256 = sha256 || latest.sha256;
        }
        const zipPath = path.join(releaseRoot, path.basename(String(zipName || "")));
        try {
          const st = await fs.stat(zipPath);
          if (!st.isFile()) throw new Error("not a file");
        } catch {
          const err = new Error(`release zip missing: ${path.basename(String(zipName || ""))}`);
          err.status = 400;
          throw err;
        }
        const row = forceTarget.enqueue({
          machine: body?.machine,
          deviceId: body?.deviceId,
          mac: body?.mac,
          macs: body?.macs,
          token: body?.token,
          version,
          buildId,
          zipName,
          sha256,
          note: body?.note,
          by: body?.by || "ops",
        });
        sendJson(res, 200, {
          ok: true,
          action: "enqueue",
          job: row,
          pending: forceTarget.list(),
          note: "queue is in-memory; restarting update server drops pending targets",
        });
      } catch (err) {
        sendJson(res, err?.status || 400, {
          ok: false,
          error: err?.message || "enqueue failed",
        });
      }
      return;
    }
    sendJson(res, 400, { ok: false, error: "action must be enqueue|cancel" });
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
      const fetchBypass = logFetch.allowsUpload({
        machine: id.machine,
        deviceId: id.deviceId,
        macs: id.macs,
        mac: id.mac,
        token: id.token,
      });
      if (!decision.allowed && !fetchBypass) {
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
  void clientHistory.flush();
  server.close(() => process.exit(0));
  setTimeout(() => process.exit(0), 1500);
});
process.on("SIGTERM", () => {
  shuttingDown = true;
  logInfo("SIGTERM, closing");
  void clientHistory.flush();
  server.close(() => process.exit(0));
  setTimeout(() => process.exit(0), 1500);
});

await fs.mkdir(releaseRoot, { recursive: true });
await fs.mkdir(path.dirname(accessLogPath), { recursive: true });
await logUpload.ensureDirs();
await access.load();
await loadGateCardIndex();
await quota.load();
await clientHistory.load();
await updateChannels.load();

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
  logInfo(`admin: POST ${basePath}/admin/log-fetch (enqueue light|full)`);
  logInfo(`admin: POST ${basePath}/admin/force-target (per-device force update)`);
  logInfo(`admin: GET/POST ${basePath}/admin/update-channels (allowed update version)`);
});

server.on("error", (err) => {
  logError(`listen error: ${err.message || err}`);
  process.exit(1);
});
