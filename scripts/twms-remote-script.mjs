/**
 * OPS → 指定设备远程脚本队列（产品=经典版 / TWMS）。
 * 经 GET /update/access.json 捎带下发；管理口仅 loopback。
 * 不写脚本正文到访问日志，只记 id / sha / 字节数 / 目标键。
 */
import crypto from "node:crypto";

const kTtlMs = 6 * 60 * 60 * 1000;
const kDoneKeepMs = 15 * 60 * 1000;
const kShowMsgStaleMs = 30 * 60 * 1000;
const kRunScriptStaleGraceMs = 45 * 1000;
const kMaxPending = 80;
const kMaxScriptBytes = 16 * 1024;
const kMaxMsgBodyBytes = 4 * 1024;
const kMinTimeoutSec = 5;
const kMaxTimeoutSec = 120;
const kDefaultTimeoutSec = 30;

/**
 * @param {{
 *   logInfo: (msg: string) => void,
 *   ts: () => string,
 *   normalizeMac?: (v: string) => string,
 *   parseMacList?: (v: string|string[]) => string[],
 * }} opts
 */
export function createRemoteScriptQueue(opts) {
  const { logInfo, ts } = opts;

  function nowMs() {
    return typeof opts.now === "function" ? Number(opts.now()) || Date.now() : Date.now();
  }

  /** @type {Map<string, any>} */
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

  function pruneExpired(now = nowMs()) {
    for (const [id, row] of byId) {
      const age = typeof row.createdMs === "number" ? now - row.createdMs : now;
      if (row.status === "cancelled") {
        byId.delete(id);
        continue;
      }
      if (row.status === "offered" || row.status === "acked") {
        const start = row.offeredMs || row.createdMs || now;
        const runLimit =
          (Number(row.timeoutSec) || kDefaultTimeoutSec) * 1000 + kRunScriptStaleGraceMs;
        const limit =
          row.status === "acked" && row.op === "showMsg" ? kShowMsgStaleMs : runLimit;
        if (now - start > limit) {
          row.status = "done";
          row.result = row.ackedAt ? "timeout" : "no_ack";
          row.doneAt = ts();
          row.doneMs = now;
          logInfo(`remote-script stale id=${id} result=${row.result}`);
        }
      }
      if (row.status === "done") {
        const doneAge = typeof row.doneMs === "number" ? now - row.doneMs : age;
        if (doneAge > kDoneKeepMs) byId.delete(id);
        continue;
      }
      if (age > kTtlMs) byId.delete(id);
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
      if (row.status !== "queued" && row.status !== "offered" && row.status !== "acked") continue;
      if (!matches(row, identity)) continue;
      if (!best || (row.createdMs || 0) > (best.createdMs || 0)) best = row;
    }
    return best;
  }

  function clampTimeout(v) {
    const n = Number(v);
    if (!Number.isFinite(n)) return kDefaultTimeoutSec;
    return Math.min(kMaxTimeoutSec, Math.max(kMinTimeoutSec, Math.floor(n)));
  }

  function sanitizeScript(raw) {
    const text = String(raw || "").replace(/\u0000/g, "");
    const bytes = Buffer.byteLength(text, "utf8");
    if (!text.trim()) {
      const err = new Error("script empty");
      err.status = 400;
      throw err;
    }
    if (bytes > kMaxScriptBytes) {
      const err = new Error(`script too large (${bytes} > ${kMaxScriptBytes})`);
      err.status = 413;
      throw err;
    }
    return { text, bytes };
  }

  function normalizeOp(raw) {
    const s = String(raw || "runScript").trim().toLowerCase();
    if (s === "showmsg" || s === "msgbox" || s === "notify" || s === "message") return "showMsg";
    return "runScript";
  }

  function publicRow(row) {
    if (!row) return null;
    return {
      id: row.id,
      op: row.op,
      status: row.status,
      note: row.note || "",
      machine: row.machine || "",
      deviceId: row.deviceId || "",
      mac: row.mac || "",
      matchKeys: row.matchKeys || [],
      timeoutSec: row.timeoutSec,
      bytes: row.bytes || 0,
      sha256: row.sha256 || "",
      title: row.title || "",
      at: row.at || "",
      offeredAt: row.offeredAt || "",
      ackedAt: row.ackedAt || "",
      doneAt: row.doneAt || "",
      result: row.result || "",
      exitCode: row.exitCode,
      outTail: row.outTail || "",
      by: row.by || "",
    };
  }

  function enqueue({
    machine,
    deviceId,
    mac,
    macs,
    token,
    script,
    title,
    body,
    op: opRaw,
    timeoutSec,
    note,
    by,
  }) {
    pruneExpired();
    const op = normalizeOp(opRaw);
    let text = "";
    let bytes = 0;
    let msgTitle = "";
    let msgBody = "";
    if (op === "showMsg") {
      msgTitle = String(title || "").trim().slice(0, 80) || "提示";
      msgBody = String(body || script || "").replace(/\u0000/g, "");
      if (!msgBody.trim()) {
        const err = new Error("message empty");
        err.status = 400;
        throw err;
      }
      bytes = Buffer.byteLength(msgBody, "utf8");
      if (bytes > kMaxMsgBodyBytes) {
        const err = new Error(`message too large (${bytes} > ${kMaxMsgBodyBytes})`);
        err.status = 413;
        throw err;
      }
    } else {
      const sanitized = sanitizeScript(script);
      text = sanitized.text;
      bytes = sanitized.bytes;
    }
    const id = crypto.randomBytes(8).toString("hex");
    const matchKeys = identityKeys({ machine, deviceId, macs: macs || mac, mac, token });
    if (matchKeys.length === 0) {
      const err = new Error("need deviceId / MAC / TOKEN to target remote-script");
      err.status = 400;
      throw err;
    }
    for (const [oldId, row] of [...byId.entries()]) {
      if (row.status === "queued" || row.status === "offered" || row.status === "acked") {
        if (matches(row, { machine, deviceId, macs: macs || mac, mac, token })) {
          row.status = "cancelled";
          byId.delete(oldId);
        }
      }
    }
    const hashSrc = op === "showMsg" ? `${msgTitle}\n${msgBody}` : text;
    const sha256 = crypto.createHash("sha256").update(hashSrc, "utf8").digest("hex");
    const row = {
      id,
      op,
      script: text,
      title: msgTitle,
      body: msgBody,
      bytes,
      sha256,
      timeoutSec: clampTimeout(timeoutSec),
      note: String(note || msgTitle || "").trim().slice(0, 200),
      machine: String(machine || "").trim().slice(0, 80),
      deviceId: String(deviceId || "").trim().slice(0, 64),
      mac: normalizeMac(mac) || parseMacList(macs)[0] || "",
      token: normalizePart(token, 48),
      matchKeys,
      status: "queued",
      at: ts(),
      createdMs: nowMs(),
      offeredAt: "",
      offeredMs: 0,
      ackedAt: "",
      doneAt: "",
      result: "",
      exitCode: null,
      outTail: "",
      by: String(by || "ops").slice(0, 40),
    };
    byId.set(id, row);
    logInfo(
      `remote-script enqueue op=${op} id=${id} sha=${sha256.slice(0, 16)} bytes=${bytes} timeout=${row.timeoutSec}s keys=${matchKeys.join(",")}`,
    );
    return publicRow(row);
  }

  function cancel(idOrIdentity) {
    pruneExpired();
    if (typeof idOrIdentity === "string" && byId.has(idOrIdentity)) {
      const row = byId.get(idOrIdentity);
      row.status = "cancelled";
      byId.delete(idOrIdentity);
      logInfo(`remote-script cancel id=${idOrIdentity}`);
      return publicRow(row);
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
    if (n) logInfo(`remote-script cancel matched=${n}`);
    return { cancelled: n };
  }

  function markDone(id, { result, exitCode, outTail } = {}) {
    const row = byId.get(String(id || ""));
    if (!row) return false;
    row.status = "done";
    row.doneAt = ts();
    row.doneMs = nowMs();
    if (result) row.result = String(result).slice(0, 16);
    if (exitCode != null && Number.isFinite(Number(exitCode))) {
      row.exitCode = Math.floor(Number(exitCode));
    }
    if (outTail) row.outTail = String(outTail).slice(0, 400);
    logInfo(
      `remote-script done id=${id} result=${row.result || "-"} exit=${row.exitCode ?? "-"}`,
    );
    return true;
  }

  /** access.json：ack=已开跑；done=跑完。仅 queued|offered 再下发脚本正文。 */
  function onAccess(identity, ackId, doneId, doneMeta) {
    pruneExpired();
    if (doneId) markDone(doneId, doneMeta || {});
    if (ackId) {
      const row = byId.get(String(ackId));
      // 同一探活可能同时带 Done+Ack：不得把已 done 打回 acked。
      if (row && matches(row, identity) && row.status !== "done" && row.status !== "cancelled") {
        row.status = "acked";
        if (!row.ackedAt) row.ackedAt = ts();
        logInfo(`remote-script acked id=${ackId}`);
      }
    }
    const pending = findActiveFor(identity);
    if (!pending) return null;
    if (pending.status === "acked") return null;
    if (pending.status === "queued") pending.offeredMs = nowMs();
    if (pending.status === "queued" || pending.status === "offered") {
      pending.status = "offered";
      pending.offeredAt = ts();
    }
    if (pending.op === "showMsg") {
      return {
        op: pending.op,
        id: pending.id,
        timeoutSec: pending.timeoutSec,
        note: pending.note || "",
        title: pending.title || "提示",
        body: pending.body || "",
      };
    }
    return {
      op: pending.op,
      id: pending.id,
      timeoutSec: pending.timeoutSec,
      note: pending.note || "",
      script: pending.script,
    };
  }

  function statusFor(identity) {
    pruneExpired();
    const active = findActiveFor(identity);
    if (active) return publicRow(active);
    let best = null;
    for (const row of byId.values()) {
      if (row.status !== "done") continue;
      if (!matches(row, identity)) continue;
      if (!best || (row.doneMs || row.createdMs || 0) > (best.doneMs || best.createdMs || 0)) {
        best = row;
      }
    }
    return publicRow(best);
  }

  function list() {
    pruneExpired();
    return [...byId.values()]
      .sort((a, b) => (b.createdMs || 0) - (a.createdMs || 0))
      .map(publicRow);
  }

  return {
    enqueue,
    cancel,
    onAccess,
    statusFor,
    list,
    markDone,
    kMaxScriptBytes,
  };
}
