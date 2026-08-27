/**
 * TWMS 客户端历史台账（产品=经典版 / TWMS）。
 *
 * 为什么要它：`activeClients` 是纯内存 Map，且 `kClientPruneSec` 到点就删（默认 1h）、
 * 进程重启即清空。于是运维台只看得到「此刻在线」，看不到「谁上周还在用」「谁已经两天没上线、
 * 快掉 64h 在线租约了」。租约一过期又赶上服务没开，客户端会被 gate/3 硬拒（1h 宽限是整机
 * 一次性的），症状是「莫名启动不了」——本模块就是为了在成员报障之前先看见。
 *
 * 与 activeClients 的分工（刻意不合并，避免动在线判定口径）：
 *   activeClients  内存、短窗口、驱动在线表 / geo / 同 IP 告警
 *   本模块          落盘、按天老化、只管「最后一次见到 / 最后一次放行」这类可追溯字段
 *
 * 落盘节流：脏标记 + 最小写间隔（默认 20s），另有 flush() 供退出前收尾。
 */
import fs from "node:fs/promises";
import path from "node:path";

const kDefaultKeepDays = 30;
const kDefaultMinWriteMs = 20 * 1000;
/** 与客户端 OnlineLease TTL 对齐（gate/3 约 64h）；用于估算租约剩余。 */
const kOnlineLeaseTtlMs = 64 * 3600 * 1000;

/**
 * @param {{
 *   releaseRoot: string,
 *   repoRoot: string,
 *   logInfo: (msg: string) => void,
 *   logWarn: (msg: string) => void,
 *   ts: (d?: Date) => string,
 *   keepDays?: number,
 *   minWriteMs?: number,
 * }} opts
 */
export function createClientHistory(opts) {
  const { releaseRoot, repoRoot, logInfo, logWarn, ts } = opts;
  const historyPath = path.join(releaseRoot, "client-history.json");
  const keepDays = Number(opts.keepDays) > 0 ? Math.floor(Number(opts.keepDays)) : kDefaultKeepDays;
  const minWriteMs =
    Number(opts.minWriteMs) >= 0 ? Math.floor(Number(opts.minWriteMs)) : kDefaultMinWriteMs;

  /** @type {Map<string, any>} */
  const rows = new Map();
  let dirty = false;
  let lastWriteMs = 0;
  let writeTimer = null;
  let saveChain = Promise.resolve();

  function str(v, max) {
    return String(v ?? "")
      .replace(/[\u0000-\u001f\u007f]/g, "")
      .slice(0, max);
  }

  function num(v) {
    const n = Number(v);
    return Number.isFinite(n) && n > 0 ? Math.floor(n) : 0;
  }

  function rowFromRaw(raw) {
    const key = str(raw?.key, 160);
    if (!key) return null;
    const lastSeenMs = num(raw?.lastSeenMs);
    if (!lastSeenMs) return null;
    return {
      key,
      ip: str(raw?.ip, 64),
      machine: str(raw?.machine, 80),
      deviceId: str(raw?.deviceId, 64),
      mac: str(raw?.mac, 32),
      uid: str(raw?.uid, 64),
      token: str(raw?.token, 48),
      gateExp: num(raw?.gateExp),
      appVersion: str(raw?.appVersion, 32),
      charName: str(raw?.charName, 48),
      charLevel: num(raw?.charLevel),
      charJobName: str(raw?.charJobName, 32),
      worldId: num(raw?.worldId),
      worldName: str(raw?.worldName, 32),
      firstSeenMs: num(raw?.firstSeenMs) || lastSeenMs,
      lastSeenMs,
      // 最后一次探活放行：租约剩余按它 + 64h 估算（与 gateViewForRow 同口径）。
      lastAllowAtMs: num(raw?.lastAllowAtMs),
      lastDenyAtMs: num(raw?.lastDenyAtMs),
      lastDenyReason: str(raw?.lastDenyReason, 120),
      lastDenyMatch: str(raw?.lastDenyMatch, 32),
      hits: num(raw?.hits),
    };
  }

  function pruneAged(now) {
    if (keepDays <= 0) return 0;
    const cutoff = now - keepDays * 86400 * 1000;
    let dropped = 0;
    for (const [k, v] of [...rows.entries()]) {
      if (v.lastSeenMs < cutoff) {
        rows.delete(k);
        dropped += 1;
      }
    }
    return dropped;
  }

  // 下包/查版本不带头，曾经按 ip: 落过空壳。历史页也别再展示这些。
  function isGhostKey(key) {
    return String(key || "").startsWith("ip:");
  }

  function isGhostRow(row) {
    if (!row) return true;
    if (isGhostKey(row.key)) return true;
    return !(row.machine || row.deviceId || row.mac || row.token);
  }

  function dropGhosts() {
    let dropped = 0;
    for (const [k, v] of [...rows.entries()]) {
      if (isGhostRow(v)) {
        rows.delete(k);
        dropped += 1;
      }
    }
    return dropped;
  }

  async function load() {
    let parsed = null;
    try {
      parsed = JSON.parse(await fs.readFile(historyPath, "utf8"));
    } catch (err) {
      if (err?.code !== "ENOENT") logWarn(`client history load failed: ${err.message || err}`);
    }
    rows.clear();
    const list = Array.isArray(parsed?.clients) ? parsed.clients : [];
    for (const raw of list) {
      const row = rowFromRaw(raw);
      if (row) rows.set(row.key, row);
    }
    const droppedAge = pruneAged(Date.now());
    const droppedGhost = dropGhosts();
    if (droppedAge > 0 || droppedGhost > 0) dirty = true;
    logInfo(
      `client history loaded ${rows.size} rows (keep ${keepDays}d, aged out ${droppedAge}, ghosts ${droppedGhost}) -> ${path.relative(repoRoot, historyPath)}`,
    );
  }

  function writeNow() {
    lastWriteMs = Date.now();
    dirty = false;
    const body = `${JSON.stringify(
      {
        version: 1,
        updatedAt: ts(),
        keepDays,
        count: rows.size,
        clients: [...rows.values()].sort((a, b) => b.lastSeenMs - a.lastSeenMs),
      },
      null,
      2,
    )}\n`;
    saveChain = saveChain
      .then(async () => {
        await fs.mkdir(path.dirname(historyPath), { recursive: true });
        const tmp = `${historyPath}.tmp`;
        await fs.writeFile(tmp, body, "utf8");
        await fs.rename(tmp, historyPath);
      })
      .catch((err) => logWarn(`client history save failed: ${err.message || err}`));
    return saveChain;
  }

  // 探活是高频请求（每客户端约 60s 一次，多客户端叠加），不能每次都落盘。
  function scheduleWrite() {
    if (!dirty || writeTimer) return;
    const waitMs = Math.max(0, lastWriteMs + minWriteMs - Date.now());
    writeTimer = setTimeout(() => {
      writeTimer = null;
      if (dirty) void writeNow();
    }, waitMs);
    // 别因为这个定时器把进程吊住不退出。
    if (typeof writeTimer.unref === "function") writeTimer.unref();
  }

  /** 从 activeClients 的 row 抽取可追溯字段落账。只写有值的字段，不用空值抹掉旧快照。 */
  function touch(src) {
    const key = str(src?.key, 160);
    if (!key || isGhostKey(key)) return;
    const now = Date.now();
    let row = rows.get(key);
    if (!row) {
      row = {
        key,
        ip: "",
        machine: "",
        deviceId: "",
        mac: "",
        uid: "",
        token: "",
        gateExp: 0,
        appVersion: "",
        charName: "",
        charLevel: 0,
        charJobName: "",
        worldId: 0,
        worldName: "",
        firstSeenMs: num(src?.firstSeenMs) || now,
        lastSeenMs: now,
        lastAllowAtMs: 0,
        lastDenyAtMs: 0,
        lastDenyReason: "",
        lastDenyMatch: "",
        hits: 0,
      };
      rows.set(key, row);
      pruneAged(now);
    }
    row.lastSeenMs = Math.max(row.lastSeenMs, num(src?.lastSeenMs) || now);
    if (src?.ip) row.ip = str(src.ip, 64);
    if (src?.machine) row.machine = str(src.machine, 80);
    if (src?.deviceId) row.deviceId = str(src.deviceId, 64);
    if (src?.mac) row.mac = str(src.mac, 32);
    if (src?.uid) row.uid = str(src.uid, 64);
    if (src?.token) row.token = str(src.token, 48);
    if (src?.gateExp) row.gateExp = num(src.gateExp);
    if (src?.appVersion) row.appVersion = str(src.appVersion, 32);
    if (src?.charName) row.charName = str(src.charName, 48);
    if (src?.charLevel) row.charLevel = num(src.charLevel);
    if (src?.charJobName) row.charJobName = str(src.charJobName, 32);
    if (num(src?.worldId)) {
      row.worldId = num(src.worldId);
      if (src?.worldName) row.worldName = str(src.worldName, 32);
    }
    if (num(src?.hits) > row.hits) row.hits = num(src.hits);
    if (num(src?.lastAllowAtMs) > row.lastAllowAtMs) row.lastAllowAtMs = num(src.lastAllowAtMs);
    if (num(src?.lastDenyAtMs) > row.lastDenyAtMs) {
      row.lastDenyAtMs = num(src.lastDenyAtMs);
      row.lastDenyReason = str(src?.lastDenyReason, 120);
      row.lastDenyMatch = str(src?.lastDenyMatch, 32);
    }
    dirty = true;
    scheduleWrite();
  }

  /**
   * 历史清单（含租约剩余估算）。
   * @param {{ days?: number, limit?: number, onlineKeys?: Set<string> }} [q]
   */
  function list(q) {
    const now = Date.now();
    const days = Number(q?.days) > 0 ? Number(q.days) : keepDays;
    const limit = Number(q?.limit) > 0 ? Math.floor(Number(q.limit)) : 500;
    const cutoff = now - days * 86400 * 1000;
    const onlineKeys = q?.onlineKeys instanceof Set ? q.onlineKeys : null;
    return [...rows.values()]
      .filter((r) => r.lastSeenMs >= cutoff && !isGhostRow(r))
      .sort((a, b) => b.lastSeenMs - a.lastSeenMs)
      .slice(0, limit)
      .map((r) => {
        const leaseRemainSec =
          r.lastAllowAtMs > 0
            ? Math.max(0, Math.floor((r.lastAllowAtMs + kOnlineLeaseTtlMs - now) / 1000))
            : 0;
        return {
          key: r.key,
          ip: r.ip,
          machine: r.machine,
          deviceId: r.deviceId,
          mac: r.mac,
          uid: r.uid,
          token: r.token,
          gateExp: r.gateExp,
          appVersion: r.appVersion,
          charName: r.charName,
          charLevel: r.charLevel,
          charJobName: r.charJobName,
          firstSeenAt: ts(new Date(r.firstSeenMs)),
          lastSeenAt: ts(new Date(r.lastSeenMs)),
          lastSeenSec: Math.max(0, Math.floor((now - r.lastSeenMs) / 1000)),
          lastAllowAt: r.lastAllowAtMs > 0 ? ts(new Date(r.lastAllowAtMs)) : "",
          lastDenyAt: r.lastDenyAtMs > 0 ? ts(new Date(r.lastDenyAtMs)) : "",
          lastDenyReason: r.lastDenyReason,
          lastDenyMatch: r.lastDenyMatch,
          hits: r.hits,
          leaseRemainSec,
          leaseTtlHours: 64,
          online: onlineKeys ? onlineKeys.has(r.key) : false,
        };
      });
  }

  function get(key) {
    const k = str(key, 160);
    if (!k || isGhostKey(k)) return null;
    const row = rows.get(k);
    return row && !isGhostRow(row) ? row : null;
  }

  function getByDeviceId(deviceId) {
    const id = str(deviceId, 64).toLowerCase();
    if (!id) return null;
    let best = null;
    for (const row of rows.values()) {
      if (isGhostRow(row)) continue;
      if (str(row.deviceId, 64).toLowerCase() !== id) continue;
      if (!best || row.lastSeenMs > best.lastSeenMs) best = row;
    }
    return best;
  }

  function flush() {
    if (writeTimer) {
      clearTimeout(writeTimer);
      writeTimer = null;
    }
    if (dirty) return writeNow();
    return saveChain;
  }

  return {
    load,
    touch,
    list,
    get,
    getByDeviceId,
    flush,
    count: () => rows.size,
    path: () => path.relative(repoRoot, historyPath),
  };
}
