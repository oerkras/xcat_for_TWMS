/**
 * OPS → 指定设备强制更新队列（产品=经典版 / TWMS）。
 * GET /update/force.json 按 X-XCat-* 身份命中后才返回 manifest；
 * 全体广播仍走 artifacts/release/force-update.json（与本队列独立）。
 */
import crypto from "node:crypto";

const kTtlMs = 6 * 60 * 60 * 1000; // 6h：给离线机留窗
const kMaxPending = 200;

/**
 * @param {{
 *   logInfo: (msg: string) => void,
 *   ts: () => string,
 *   parseMacList?: (v: string|string[]) => string[],
 * }} opts
 */
export function createForceTargetQueue(opts) {
  const { logInfo, ts } = opts;

  /** @type {Map<string, any>} */
  const byId = new Map();

  function normalizePart(value, max = 80) {
    return String(value || "")
      .trim()
      .toLowerCase()
      .slice(0, max);
  }

  function normalizeMac(value) {
    const hex = String(value || "")
      .toLowerCase()
      .replace(/[^0-9a-f]/g, "");
    if (hex.length !== 12) return "";
    if (/^0+$/.test(hex) || /^f+$/.test(hex)) return "";
    return hex;
  }

  function parseMacList(raw) {
    if (typeof opts.parseMacList === "function") return opts.parseMacList(raw);
    if (Array.isArray(raw)) {
      return raw.map(normalizeMac).filter(Boolean).slice(0, 8);
    }
    const text = String(raw || "");
    if (!text) return [];
    const out = [];
    const seen = new Set();
    for (const part of text.split(/[,\s;]+/)) {
      const hex = normalizeMac(part);
      if (!hex || seen.has(hex)) continue;
      seen.add(hex);
      out.push(hex);
      if (out.length >= 8) break;
    }
    return out;
  }

  function pruneExpired(now = Date.now()) {
    for (const [id, row] of byId) {
      const age = typeof row.createdMs === "number" ? now - row.createdMs : now;
      if (age > kTtlMs || row.status === "done" || row.status === "cancelled") {
        byId.delete(id);
      }
    }
    while (byId.size > kMaxPending) {
      const oldest = byId.keys().next().value;
      byId.delete(oldest);
    }
  }

  function identityKeys({ machine, deviceId, macs, mac, token }) {
    const keys = [];
    const d = normalizePart(deviceId, 64);
    if (d) keys.push(`id:${d}`);
    const m = normalizePart(machine);
    if (m && d) keys.push(`dev:${m}:${d}`);
    const macList = Array.isArray(macs)
      ? macs.map(normalizeMac).filter(Boolean)
      : parseMacList(macs || mac);
    for (const hex of macList) keys.push(`mac:${hex}`);
    const tok = normalizePart(token, 48);
    if (tok) keys.push(`tok:${tok}`);
    return keys;
  }

  function matches(row, identity) {
    const keys = new Set(identityKeys(identity));
    for (const k of row.matchKeys || []) {
      if (keys.has(k)) return true;
    }
    return false;
  }

  function findActiveFor(identity) {
    pruneExpired();
    let best = null;
    for (const row of byId.values()) {
      if (row.status !== "queued" && row.status !== "offered") continue;
      if (!matches(row, identity)) continue;
      if (!best || (row.createdMs || 0) > (best.createdMs || 0)) best = row;
    }
    return best;
  }

  /** @param {string} appVersion e.g. "0.1.113 build 113" —— 只认 build N，避免把版本号尾数当 build */
  function parseClientBuildId(appVersion) {
    const m = String(appVersion || "").match(/build\s+(\d+)/i);
    if (!m) return 0;
    const n = Number(m[1]);
    return Number.isFinite(n) && n > 0 ? Math.floor(n) : 0;
  }

  function enqueue({
    machine,
    deviceId,
    mac,
    macs,
    token,
    version,
    buildId,
    zipName,
    sha256,
    note,
    by,
  }) {
    pruneExpired();
    const bid = Number(buildId) || 0;
    const zip = String(zipName || "").trim();
    const sha = String(sha256 || "").trim().toLowerCase();
    if (bid <= 0 || !zip || sha.length !== 64) {
      const err = new Error("need valid version/buildId/zipName/sha256");
      err.status = 400;
      throw err;
    }
    const matchKeys = identityKeys({ machine, deviceId, macs: macs || mac, mac, token });
    if (matchKeys.length === 0) {
      const err = new Error("need deviceId / MAC / TOKEN to target force update");
      err.status = 400;
      throw err;
    }
    for (const [oldId, row] of [...byId.entries()]) {
      if (row.status === "queued" || row.status === "offered") {
        if (matches(row, { machine, deviceId, macs: macs || mac, mac, token })) {
          row.status = "cancelled";
          byId.delete(oldId);
        }
      }
    }
    const id = crypto.randomBytes(8).toString("hex");
    const row = {
      id,
      version: String(version || "").trim().slice(0, 40),
      buildId: bid,
      zipName: zip.slice(0, 200),
      sha256: sha,
      note: String(note || "").trim().slice(0, 200),
      machine: String(machine || "").trim().slice(0, 80),
      deviceId: String(deviceId || "").trim().slice(0, 64),
      mac: normalizeMac(mac) || parseMacList(macs)[0] || "",
      token: normalizePart(token, 48),
      matchKeys,
      status: "queued",
      at: ts(),
      createdMs: Date.now(),
      offeredAt: "",
      by: String(by || "ops").slice(0, 40),
    };
    byId.set(id, row);
    logInfo(
      `force-target enqueue id=${id} build=${bid} keys=${matchKeys.join(",")}`,
    );
    return row;
  }

  function cancel(idOrIdentity) {
    pruneExpired();
    if (typeof idOrIdentity === "string" && byId.has(idOrIdentity)) {
      const row = byId.get(idOrIdentity);
      row.status = "cancelled";
      byId.delete(idOrIdentity);
      logInfo(`force-target cancel id=${idOrIdentity}`);
      return row;
    }
    const identity = idOrIdentity || {};
    let n = 0;
    for (const [id, row] of [...byId.entries()]) {
      if (matches(row, identity)) {
        row.status = "cancelled";
        byId.delete(id);
        n += 1;
      }
    }
    if (n) logInfo(`force-target cancel matched=${n}`);
    return { cancelled: n };
  }

  /**
   * force.json 命中：已达目标 build → 清任务返回 null；否则返回可下发的 manifest 字段。
   */
  function offerForForceGet(identity, appVersion) {
    pruneExpired();
    const row = findActiveFor(identity);
    if (!row) return null;
    const clientBuild = parseClientBuildId(appVersion);
    if (clientBuild > 0 && clientBuild >= row.buildId) {
      row.status = "done";
      byId.delete(row.id);
      logInfo(
        `force-target done id=${row.id} clientBuild=${clientBuild} >= ${row.buildId}`,
      );
      return null;
    }
    row.status = "offered";
    row.offeredAt = ts();
    return {
      id: row.id,
      version: row.version,
      buildId: row.buildId,
      zipName: row.zipName,
      sha256: row.sha256,
      issuedAt: row.at.replace(" ", "T") + "Z",
      targetId: row.id,
    };
  }

  function statusFor(identity) {
    const row = findActiveFor(identity);
    if (!row) return null;
    return {
      id: row.id,
      status: row.status,
      version: row.version,
      buildId: row.buildId,
      at: row.at,
      offeredAt: row.offeredAt || "",
      note: row.note || "",
    };
  }

  function list() {
    pruneExpired();
    return [...byId.values()].sort((a, b) => (b.createdMs || 0) - (a.createdMs || 0));
  }

  return {
    enqueue,
    cancel,
    offerForForceGet,
    statusFor,
    list,
    parseClientBuildId,
  };
}
