/**
 * TWMS 设备访问策略（产品=经典版）。
 * - mode=deny（默认）：黑名单止血，未登记也可使用
 * - mode=allow：仅白名单可用（泄露紧急锁盘）
 * 匹配键：TOKEN(tok:) / MAC / 精确设备(dev:) / deviceId(id:) —— 不用仅计算机名
 */
import fs from "node:fs/promises";
import path from "node:path";

/**
 * @param {{
 *   releaseRoot: string,
 *   repoRoot: string,
 *   logInfo: (msg: string) => void,
 *   logWarn: (msg: string) => void,
 *   ts: () => string,
 * }} opts
 */
export function createDeviceAccess(opts) {
  const { releaseRoot, repoRoot, logInfo, logWarn, ts } = opts;
  const accessPath = path.join(releaseRoot, "device-access.json");
  const legacyBansPath = path.join(releaseRoot, "device-bans.json");

  /** @type {"deny"|"allow"} */
  let mode = "deny";
  /** @type {Map<string, any>} */
  const bans = new Map();
  /** @type {Map<string, any>} */
  const allows = new Map();
  let saveChain = Promise.resolve();

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

  function formatMac(hex) {
    if (!hex || hex.length !== 12) return "";
    return hex.match(/.{1,2}/g).join(":");
  }

  function parseMacList(raw) {
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

  function normalizeToken(value) {
    // 大小写不敏感；去掉空白与控制符，最长 48。
    return String(value || "")
      .replace(/[\u0000-\u001f\u007f]/g, "")
      .replace(/\s+/g, "")
      .toLowerCase()
      .slice(0, 48);
  }

  function tokenKey(tok) {
    return tok ? `tok:${tok}` : "";
  }

  function makeEntryKey({ machine, deviceId, mac, token }) {
    const tok = normalizeToken(token);
    if (tok) return tokenKey(tok);
    const macHex = normalizeMac(mac);
    if (macHex) return `mac:${macHex}`;
    const m = normalizePart(machine);
    const d = normalizePart(deviceId, 64);
    if (m && d) return `dev:${m}:${d}`;
    if (d) return `id:${d}`;
    // 不再生成 host: —— 仅计算机名会误伤同名机
    return "";
  }

  function findInMap(map, { machine, deviceId, macs, token }) {
    const tok = normalizeToken(token);
    if (tok) {
      const byTok = map.get(tokenKey(tok));
      if (byTok) return byTok;
    }
    const macList = Array.isArray(macs) ? macs : parseMacList(macs);
    for (const hex of macList) {
      const hit = map.get(`mac:${hex}`);
      if (hit) return hit;
    }
    const m = normalizePart(machine);
    const d = normalizePart(deviceId, 64);
    if (m && d) {
      const exact = map.get(`dev:${m}:${d}`);
      if (exact) return exact;
    }
    if (d) {
      const byId = map.get(`id:${d}`);
      if (byId) return byId;
    }
    // 故意不匹配 host: —— 仅计算机名会误伤同名机；须 MAC / deviceId / TOKEN。
    return null;
  }

  function rowFromRaw(row, fallbackReason) {
    const machine = String(row?.machine || "").trim().slice(0, 80);
    const deviceId = String(row?.deviceId || "").trim().slice(0, 64);
    const macHex = normalizeMac(row?.mac || "");
    let token = normalizeToken(row?.token || "");
    let key = String(row?.key || "").trim();
    if (key.startsWith("host:")) key = ""; // 历史 host: 专条作废
    if (!key) key = makeEntryKey({ machine, deviceId, mac: macHex, token });
    if (!key) return null;
    if (!token && key.startsWith("tok:")) {
      token = normalizeToken(key.slice(4));
    }
    if (key.startsWith("pass:")) return null;
    return {
      key,
      machine,
      deviceId,
      mac: formatMac(macHex),
      token,
      device:
        String(row?.device || "").slice(0, 96) ||
        (token
          ? `tok_${token.slice(0, 8)}`
          : macHex
            ? `mac_${macHex.slice(0, 8)}`
            : machine && deviceId
              ? `${machine}_${deviceId.slice(0, 8)}`
              : machine || deviceId.slice(0, 16)),
      reason: String(row?.reason || fallbackReason).trim().slice(0, 200) || fallbackReason,
      at: String(row?.at || row?.bannedAt || row?.allowedAt || ""),
      by: String(row?.by || row?.bannedBy || row?.allowedBy || "ops").slice(0, 40),
    };
  }

  function loadMap(map, list, fallbackReason) {
    map.clear();
    if (!Array.isArray(list)) return;
    for (const raw of list) {
      const row = rowFromRaw(raw, fallbackReason);
      if (!row) continue;
      map.set(row.key, row);
    }
  }

  async function load() {
    let parsed = null;
    try {
      parsed = JSON.parse(await fs.readFile(accessPath, "utf8"));
    } catch (err) {
      if (err?.code !== "ENOENT") logWarn(`device access load failed: ${err.message || err}`);
    }

    if (!parsed) {
      // 兼容上一版仅黑名单文件
      try {
        const legacy = JSON.parse(await fs.readFile(legacyBansPath, "utf8"));
        parsed = { version: 1, mode: "deny", bans: legacy?.bans || [], allows: [] };
        logInfo("device access migrated from device-bans.json");
      } catch (err) {
        if (err?.code !== "ENOENT") {
          logWarn(`legacy device-bans load failed: ${err.message || err}`);
        }
        parsed = { version: 1, mode: "deny", bans: [], allows: [] };
      }
    }

    mode = String(parsed?.mode || "deny").toLowerCase() === "allow" ? "allow" : "deny";
    loadMap(bans, parsed?.bans, "ops ban");
    loadMap(allows, parsed?.allows, "ops allow");
    logInfo(
      `device access loaded mode=${mode} bans=${bans.size} allows=${allows.size} -> ${path.relative(repoRoot, accessPath)}`,
    );
  }

  function persist() {
    saveChain = saveChain
      .then(async () => {
        await fs.mkdir(path.dirname(accessPath), { recursive: true });
        const body = `${JSON.stringify(
          {
            version: 1,
            mode,
            updatedAt: ts(),
            bans: [...bans.values()].sort((a, b) => String(a.at).localeCompare(String(b.at))),
            allows: [...allows.values()].sort((a, b) => String(a.at).localeCompare(String(b.at))),
          },
          null,
          2,
        )}\n`;
        const tmp = `${accessPath}.tmp`;
        await fs.writeFile(tmp, body, "utf8");
        await fs.rename(tmp, accessPath);
      })
      .catch((err) => logWarn(`device access save failed: ${err.message || err}`));
    return saveChain;
  }

  function upsert(map, { machine, deviceId, mac, token, reason, by }, fallbackReason) {
    const macHex = normalizeMac(mac);
    const m = normalizePart(machine);
    const d = normalizePart(deviceId, 64);
    const tok = normalizeToken(token);
    const keys = [];
    if (tok) keys.push(tokenKey(tok));
    if (macHex) keys.push(`mac:${macHex}`);
    if (m && d) keys.push(`dev:${m}:${d}`);
    else if (d) keys.push(`id:${d}`);
    // 禁止仅 host:（同名机误伤）；TOKEN / deviceId / MAC 至少其一
    const uniq = [...new Set(keys)];
    if (!uniq.length) {
      const err = new Error("deviceId / mac / token required (host-only not allowed)");
      err.status = 400;
      throw err;
    }
    let primary = null;
    for (const key of uniq) {
      const row = rowFromRaw(
        {
          key,
          machine: String(machine || "").trim().slice(0, 80),
          deviceId: String(deviceId || "").trim().slice(0, 64),
          mac: macHex,
          token: tok,
          reason: reason || fallbackReason,
          at: ts(),
          by: by || "ops",
        },
        fallbackReason,
      );
      if (!row) continue;
      map.set(key, row);
      if (!primary) primary = row;
    }
    if (!primary) {
      const err = new Error("deviceId / mac / token required");
      err.status = 400;
      throw err;
    }
    return primary;
  }

  function remove(map, { key, machine, deviceId, mac, token }) {
    const explicit = String(key || "").trim();
    let seed = null;
    if (explicit) {
      seed = map.get(explicit) || null;
      map.delete(explicit);
    }
    const macHex = normalizeMac(mac || seed?.mac || "");
    const m = normalizePart(machine || seed?.machine || "");
    const d = normalizePart(deviceId || seed?.deviceId || "", 64);
    const tok = normalizeToken(token || seed?.token || "");
    const candidates = [];
    if (tok) candidates.push(tokenKey(tok));
    if (macHex) candidates.push(`mac:${macHex}`);
    if (m && d) candidates.push(`dev:${m}:${d}`);
    if (d) candidates.push(`id:${d}`);
    if (m) candidates.push(`host:${m}`);
    let last = seed;
    for (const k of [...new Set(candidates)]) {
      if (map.has(k)) {
        last = map.get(k);
        map.delete(k);
      }
    }
    // 扫全表：ban/unban 可能留下同机不同 key（tok/mac/dev）孤儿条，解禁后仍命中 evaluate。
    if (tok || macHex || d || (m && d)) {
      for (const [k, row] of [...map.entries()]) {
        if (!row) continue;
        const rowTok = normalizeToken(row.token || (String(k).startsWith("tok:") ? k.slice(4) : ""));
        const rowMac = normalizeMac(row.mac || (String(k).startsWith("mac:") ? k.slice(4) : ""));
        const rowM = normalizePart(row.machine || "");
        const rowD = normalizePart(row.deviceId || "", 64);
        const hitTok = tok && rowTok && tok === rowTok;
        const hitMac = macHex && rowMac && macHex === rowMac;
        const hitDev = d && rowD && d === rowD && (!m || !rowM || m === rowM);
        if (hitTok || hitMac || hitDev) {
          last = row;
          map.delete(k);
        }
      }
    }
    if (!explicit && !candidates.length && !tok && !macHex && !d) {
      const err = new Error("key or machine/deviceId/mac/token required");
      err.status = 400;
      throw err;
    }
    return last;
  }

  function evaluate({ machine, deviceId, macs, token }) {
    const macList = Array.isArray(macs) ? macs.map(normalizeMac).filter(Boolean) : parseMacList(macs);
    const ban = findInMap(bans, { machine, deviceId, macs: macList, token });
    if (ban) {
      return {
        allowed: false,
        mode,
        reason: ban.reason || "ops ban",
        key: ban.key,
        match: "ban",
        at: ban.at || "",
      };
    }
    if (mode === "allow") {
      const allow = findInMap(allows, { machine, deviceId, macs: macList, token });
      if (!allow) {
        return {
          allowed: false,
          mode,
          reason: "not in whitelist",
          key: "",
          match: "allow-miss",
          at: "",
        };
      }
      return {
        allowed: true,
        mode,
        reason: "",
        key: allow.key,
        match: "allow",
        at: allow.at || "",
      };
    }
    return { allowed: true, mode, reason: "", key: "", match: "deny-pass", at: "" };
  }

  function snapshot() {
    return {
      mode,
      banCount: bans.size,
      allowCount: allows.size,
      bans: [...bans.values()].sort((a, b) => String(b.at || "").localeCompare(String(a.at || ""))),
      allows: [...allows.values()].sort((a, b) =>
        String(b.at || "").localeCompare(String(a.at || "")),
      ),
      path: path.relative(repoRoot, accessPath),
    };
  }

  async function setMode(next) {
    const m = String(next || "").toLowerCase() === "allow" ? "allow" : "deny";
    mode = m;
    await persist();
    return mode;
  }

  return {
    load,
    persist,
    evaluate,
    snapshot,
    setMode,
    getMode: () => mode,
    parseMacList,
    normalizeMac,
    formatMac,
    normalizeToken,
    ban: (args) => upsert(bans, args, "ops ban"),
    unban: (args) => remove(bans, args),
    allow: (args) => upsert(allows, args, "ops allow"),
    unallow: (args) => remove(allows, args),
    isBanned: (id) => !!findInMap(bans, id),
    isAllowed: (id) => !!findInMap(allows, id),
  };
}
