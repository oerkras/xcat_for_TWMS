/**
 * OPS → 客户端「拉取日志」命令队列（产品=经典版 / TWMS）。
 * 经 GET /update/access.json 捎带下发；客户端复用既有 StartLogUpload（light|full）。
 */
import crypto from "node:crypto";

const kTtlMs = 15 * 60 * 1000;
const kMaxPending = 200;

/**
 * @param {{
 *   logInfo: (msg: string) => void,
 *   logWarn: (msg: string) => void,
 *   ts: () => string,
 *   normalizeMac?: (v: string) => string,
 *   parseMacList?: (v: string|string[]) => string[],
 * }} opts
 */
export function createLogFetchQueue(opts) {
  const { logInfo, logWarn, ts } = opts;

  /** @type {Map<string, any>} id → row */
  const byId = new Map();

  function normalizePart(value, max = 80) {
    return String(value || "")
      .trim()
      .toLowerCase()
      .slice(0, max);
  }

  function normalizeMac(value) {
    if (typeof opts.normalizeMac === "function") return opts.normalizeMac(value);
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
      const created = Date.parse(String(row.at || "").replace(" ", "T") + "Z") || 0;
      // at 是本地墙钟字符串；用 ageMs 字段更稳
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
    const macList = Array.isArray(macs) ? macs.map(normalizeMac).filter(Boolean) : parseMacList(macs || mac);
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
      if (row.status !== "queued" && row.status !== "offered" && row.status !== "acked") continue;
      if (!matches(row, identity)) continue;
      if (!best || (row.createdMs || 0) > (best.createdMs || 0)) best = row;
    }
    return best;
  }

  function enqueue({ machine, deviceId, mac, macs, token, mode, note, by }) {
    pruneExpired();
    const m = String(mode || "light").trim().toLowerCase() === "full" ? "full" : "light";
    const id = crypto.randomBytes(8).toString("hex");
    const matchKeys = identityKeys({ machine, deviceId, macs: macs || mac, mac, token });
    if (matchKeys.length === 0) {
      const err = new Error("need deviceId / MAC / TOKEN to target log-fetch");
      err.status = 400;
      throw err;
    }
    // 同设备未完成的旧命令取消，避免堆叠（含 acked：重新点名覆盖上传中任务）
    for (const [oldId, row] of [...byId.entries()]) {
      if (row.status === "queued" || row.status === "offered" || row.status === "acked") {
        if (matches(row, { machine, deviceId, macs: macs || mac, mac, token })) {
          row.status = "cancelled";
          byId.delete(oldId);
        }
      }
    }
    const row = {
      id,
      op: "uploadLogs",
      mode: m,
      note: String(note || "").trim().slice(0, 200),
      machine: String(machine || "").trim().slice(0, 80),
      deviceId: String(deviceId || "").trim().slice(0, 64),
      mac: normalizeMac(mac) || (parseMacList(macs)[0] || ""),
      token: normalizePart(token, 48),
      matchKeys,
      status: "queued",
      at: ts(),
      createdMs: Date.now(),
      offeredAt: "",
      ackedAt: "",
      by: String(by || "ops").slice(0, 40),
    };
    byId.set(id, row);
    logInfo(`log-fetch enqueue id=${id} mode=${m} keys=${matchKeys.join(",")}`);
    return row;
  }

  function cancel(idOrIdentity) {
    pruneExpired();
    if (typeof idOrIdentity === "string" && byId.has(idOrIdentity)) {
      const row = byId.get(idOrIdentity);
      row.status = "cancelled";
      byId.delete(idOrIdentity);
      logInfo(`log-fetch cancel id=${idOrIdentity}`);
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
    if (n) logInfo(`log-fetch cancel matched=${n}`);
    return { cancelled: n };
  }

  /** access.json 探活：ack=已开传；done=上传结束；仅 queued|offered 再下发 pendingOp */
  function onAccess(identity, ackId, doneId) {
    pruneExpired();
    if (doneId) {
      if (markDone(doneId)) {
        logInfo(`log-fetch done id=${doneId}`);
      }
    }
    if (ackId) {
      const row = byId.get(String(ackId));
      if (row && matches(row, identity)) {
        row.status = "acked";
        row.ackedAt = ts();
        logInfo(`log-fetch acked id=${ackId}`);
        // 无 Done 的旧客户端：约 60s 后 prune；新客户端应尽快 Done
        row.createdMs = Date.now() - (kTtlMs - 60_000);
      }
    }
    const pending = findActiveFor(identity);
    if (!pending) return null;
    // acked：仍显示在 clients.statusFor，但不再反复下发 uploadLogs
    if (pending.status === "acked") return null;
    if (pending.status === "queued" || pending.status === "offered") {
      pending.status = "offered";
      pending.offeredAt = ts();
    }
    return {
      op: pending.op,
      id: pending.id,
      mode: pending.mode,
      note: pending.note || `ops-fetch:${pending.id}`,
    };
  }

  /** 封禁设备仍允许把 OPS 点名的日志传上来 */
  function allowsUpload(identity) {
    pruneExpired();
    const row = findActiveFor(identity);
    return !!(row && (row.status === "offered" || row.status === "acked" || row.status === "queued"));
  }

  function statusFor(identity) {
    const row = findActiveFor(identity);
    if (!row) return null;
    return {
      id: row.id,
      mode: row.mode,
      status: row.status,
      note: row.note || "",
      at: row.at,
      offeredAt: row.offeredAt || "",
      ackedAt: row.ackedAt || "",
    };
  }

  function list() {
    pruneExpired();
    return [...byId.values()].sort((a, b) => (b.createdMs || 0) - (a.createdMs || 0));
  }

  function markDone(id) {
    const row = byId.get(String(id || ""));
    if (!row) return false;
    row.status = "done";
    byId.delete(row.id);
    return true;
  }

  return {
    enqueue,
    cancel,
    onAccess,
    allowsUpload,
    statusFor,
    list,
    markDone,
  };
}
