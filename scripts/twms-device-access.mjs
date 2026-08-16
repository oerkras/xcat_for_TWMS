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
  // 严格模式：为 true 时，探活缺有效签名 TOKEN（无可信 uid）一律拒。默认关（不误伤未升级客户端）。
  let strictToken = false;
  /** @type {Map<string, any>} */
  const bans = new Map();
  /** @type {Map<string, any>} */
  const allows = new Map();
  // 卡级吊销名单：jti -> row。与 bans 分开——bans 封的是「人/设备」，这里废的是「某一张卡」。
  /** @type {Map<string, any>} */
  const revokedJti = new Map();
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

  function normalizeUid(value) {
    // 个人签名 TOKEN 的 uid（来自验签，改硬件也改不掉）；去控制符/首尾空白，最长 64。
    // 不做 lowercase：兼容中文名，且与配额面板显示的 uid 保持一致，便于按 uid 复制封禁。
    return String(value || "")
      .replace(/[\u0000-\u001f\u007f]/g, "")
      .trim()
      .slice(0, 64);
  }

  function normalizeJti(value) {
    // 卡号：签发时写进 TOKEN payload 的 jti（hex）。按 uid 封是封人，按 jti 废是废单张卡——
    // 续签后可以只作废泄露的那张，人还能继续用新卡。老卡 payload 无 jti，取到空即跳过判定。
    return String(value || "")
      .replace(/[^0-9a-zA-Z_-]/g, "")
      .toLowerCase()
      .slice(0, 32);
  }

  function tokenKey(tok) {
    return tok ? `tok:${tok}` : "";
  }

  function uidKey(uid) {
    return uid ? `uid:${uid}` : "";
  }

  function makeEntryKey({ machine, deviceId, mac, token, uid }) {
    const u = normalizeUid(uid);
    if (u) return uidKey(u); // uid 最可信（签名 TOKEN，改硬件也不变）→ 最高优先
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

  function findInMap(map, { machine, deviceId, macs, token, uid }) {
    const u = normalizeUid(uid);
    if (u) {
      const byUid = map.get(uidKey(u));
      if (byUid) return byUid;
    }
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
    // 故意不匹配 host: —— 仅计算机名会误伤同名机；须 uid / MAC / deviceId / TOKEN。
    return null;
  }

  function rowFromRaw(row, fallbackReason) {
    const machine = String(row?.machine || "").trim().slice(0, 80);
    const deviceId = String(row?.deviceId || "").trim().slice(0, 64);
    const macHex = normalizeMac(row?.mac || "");
    let token = normalizeToken(row?.token || "");
    let uid = normalizeUid(row?.uid || "");
    let key = String(row?.key || "").trim();
    if (key.startsWith("host:")) key = ""; // 历史 host: 专条作废
    if (!key) key = makeEntryKey({ machine, deviceId, mac: macHex, token, uid });
    if (!key) return null;
    if (!token && key.startsWith("tok:")) {
      token = normalizeToken(key.slice(4));
    }
    if (!uid && key.startsWith("uid:")) {
      uid = normalizeUid(key.slice(4));
    }
    if (key.startsWith("pass:")) return null;
    return {
      key,
      machine,
      deviceId,
      mac: formatMac(macHex),
      token,
      uid,
      device:
        String(row?.device || "").slice(0, 96) ||
        (uid
          ? `uid_${uid.slice(0, 12)}`
          : token
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

  function jtiRowFromRaw(raw) {
    const jti = normalizeJti(raw?.jti || raw?.id);
    if (!jti) return null;
    return {
      jti,
      uid: normalizeUid(raw?.uid || ""),
      reason: String(raw?.reason || "ops 废卡").trim().slice(0, 200) || "ops 废卡",
      at: String(raw?.at || ""),
      by: String(raw?.by || "ops").slice(0, 40),
    };
  }

  function loadJtiMap(list) {
    revokedJti.clear();
    if (!Array.isArray(list)) return;
    for (const raw of list) {
      const row = jtiRowFromRaw(raw);
      if (row) revokedJti.set(row.jti, row);
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
    strictToken = parsed?.strictToken === true;
    loadMap(bans, parsed?.bans, "ops ban");
    loadMap(allows, parsed?.allows, "ops allow");
    loadJtiMap(parsed?.revokedJti);
    logInfo(
      `device access loaded mode=${mode} strictToken=${strictToken} bans=${bans.size} allows=${allows.size} revokedCards=${revokedJti.size} -> ${path.relative(repoRoot, accessPath)}`,
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
            strictToken,
            updatedAt: ts(),
            bans: [...bans.values()].sort((a, b) => String(a.at).localeCompare(String(b.at))),
            allows: [...allows.values()].sort((a, b) => String(a.at).localeCompare(String(b.at))),
            revokedJti: [...revokedJti.values()].sort((a, b) =>
              String(a.at).localeCompare(String(b.at)),
            ),
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

  function upsert(map, { machine, deviceId, mac, token, uid, reason, by }, fallbackReason) {
    const macHex = normalizeMac(mac);
    const m = normalizePart(machine);
    const d = normalizePart(deviceId, 64);
    const tok = normalizeToken(token);
    const u = normalizeUid(uid);
    const keys = [];
    if (u) keys.push(uidKey(u));
    if (tok) keys.push(tokenKey(tok));
    if (macHex) keys.push(`mac:${macHex}`);
    if (m && d) keys.push(`dev:${m}:${d}`);
    else if (d) keys.push(`id:${d}`);
    // 禁止仅 host:（同名机误伤）；uid / TOKEN / deviceId / MAC 至少其一
    const uniq = [...new Set(keys)];
    if (!uniq.length) {
      const err = new Error("uid / deviceId / mac / token required (host-only not allowed)");
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
          uid: u,
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
      const err = new Error("uid / deviceId / mac / token required");
      err.status = 400;
      throw err;
    }
    return primary;
  }

  function remove(map, { key, machine, deviceId, mac, token, uid }) {
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
    const u = normalizeUid(uid || seed?.uid || "");
    const candidates = [];
    if (u) candidates.push(uidKey(u));
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
    // 扫全表：ban/unban 可能留下同机不同 key（uid/tok/mac/dev）孤儿条，解禁后仍命中 evaluate。
    if (u || tok || macHex || d || (m && d)) {
      for (const [k, row] of [...map.entries()]) {
        if (!row) continue;
        const rowUid = normalizeUid(row.uid || (String(k).startsWith("uid:") ? k.slice(4) : ""));
        const rowTok = normalizeToken(row.token || (String(k).startsWith("tok:") ? k.slice(4) : ""));
        const rowMac = normalizeMac(row.mac || (String(k).startsWith("mac:") ? k.slice(4) : ""));
        const rowM = normalizePart(row.machine || "");
        const rowD = normalizePart(row.deviceId || "", 64);
        const hitUid = u && rowUid && u === rowUid;
        const hitTok = tok && rowTok && tok === rowTok;
        const hitMac = macHex && rowMac && macHex === rowMac;
        const hitDev = d && rowD && d === rowD && (!m || !rowM || m === rowM);
        if (hitUid || hitTok || hitMac || hitDev) {
          last = row;
          map.delete(k);
        }
      }
    }
    if (!explicit && !candidates.length && !u && !tok && !macHex && !d) {
      const err = new Error("key or uid/machine/deviceId/mac/token required");
      err.status = 400;
      throw err;
    }
    return last;
  }

  function evaluate({ machine, deviceId, macs, token, uid }) {
    const macList = Array.isArray(macs) ? macs.map(normalizeMac).filter(Boolean) : parseMacList(macs);
    const ban = findInMap(bans, { machine, deviceId, macs: macList, token, uid });
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
      const allow = findInMap(allows, { machine, deviceId, macs: macList, token, uid });
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

  // ── 卡级吊销（按 jti 废单张卡，不影响同 uid 的其他卡） ──
  function revokeJti({ jti, uid, reason, by }) {
    const j = normalizeJti(jti);
    if (!j) {
      const err = new Error("jti required (老卡未带卡号，只能按 uid 封人)");
      err.status = 400;
      throw err;
    }
    const row = {
      jti: j,
      uid: normalizeUid(uid || ""),
      reason: String(reason || "ops 废卡").trim().slice(0, 200) || "ops 废卡",
      at: ts(),
      by: String(by || "ops").slice(0, 40),
    };
    revokedJti.set(j, row);
    return row;
  }

  function unrevokeJti({ jti }) {
    const j = normalizeJti(jti);
    if (!j) {
      const err = new Error("jti required");
      err.status = 400;
      throw err;
    }
    const existed = revokedJti.get(j) || null;
    revokedJti.delete(j);
    return existed;
  }

  function isJtiRevoked(jti) {
    const j = normalizeJti(jti);
    return !!j && revokedJti.has(j);
  }

  function snapshot() {
    return {
      mode,
      strictToken,
      banCount: bans.size,
      allowCount: allows.size,
      revokedJtiCount: revokedJti.size,
      revokedJti: [...revokedJti.values()].sort((a, b) =>
        String(b.at || "").localeCompare(String(a.at || "")),
      ),
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

  async function setStrictToken(next) {
    strictToken = next === true || String(next).toLowerCase() === "true" || next === 1;
    await persist();
    return strictToken;
  }

  return {
    load,
    persist,
    evaluate,
    snapshot,
    setMode,
    getMode: () => mode,
    setStrictToken,
    getStrictToken: () => strictToken,
    revokeJti,
    unrevokeJti,
    isJtiRevoked,
    parseMacList,
    normalizeMac,
    formatMac,
    normalizeToken,
    normalizeUid,
    normalizeJti,
    ban: (args) => upsert(bans, args, "ops ban"),
    unban: (args) => remove(bans, args),
    allow: (args) => upsert(allows, args, "ops allow"),
    unallow: (args) => remove(allows, args),
    isBanned: (id) => !!findInMap(bans, id),
    isAllowed: (id) => !!findInMap(allows, id),
  };
}
