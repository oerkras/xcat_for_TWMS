/**
 * TWMS 台数配额（产品=经典版）。
 *
 * 与 gate/1 启动激活门配套：客户端探活带 X-XCat-Gate-Token（个人签名 TOKEN）。
 * 本模块用内嵌公钥验签取出可信 uid，按 uid 累计已激活的 deviceId 集合，超过
 * 该 uid 的台数上限则拒（access.json 返回 allowed:false, reason:quota）。
 *
 * 设计要点：
 *  - 台账 device-quota.json 存 { defaultMax, agingDays, users: { uid: { max, devices } } }。
 *  - max<=0 视为不限（默认 opt-in：未显式配上限的人不受限，避免误锁）。
 *  - devices[deviceId] = lastSeen(ISO)；agingDays>0 时超期设备计数前自动释放。
 *  - uid 由验签得到，客户端改头伪造别人 uid 需要私钥 → 做不到。
 *  - 无 gateToken（老客户端 / 未升级）时本模块放行，不阻断存量。
 */
import crypto from "node:crypto";
import fs from "node:fs/promises";
import fss from "node:fs";
import path from "node:path";

/**
 * @param {{
 *   releaseRoot: string,
 *   repoRoot: string,
 *   pubKeyPath?: string,
 *   defaultMax?: number,
 *   agingDays?: number,
 *   logInfo: (msg: string) => void,
 *   logWarn: (msg: string) => void,
 *   ts: () => string,
 * }} opts
 */
export function createDeviceQuota(opts) {
  const { releaseRoot, repoRoot, logInfo, logWarn, ts } = opts;
  const quotaPath = path.join(releaseRoot, "device-quota.json");
  const pubKeyPath = opts.pubKeyPath || path.join(repoRoot, "secrets", "gate_ec_pub.pem");

  let defaultMax = Number.isFinite(opts.defaultMax) ? Math.max(0, Math.floor(opts.defaultMax)) : 0;
  let agingDays = Number.isFinite(opts.agingDays) ? Math.max(0, Math.floor(opts.agingDays)) : 0;
  // 派生凭证要查卡的签名段（只在签发台账里）；宿主注入同步查询，避免把 evaluate 变成 async。
  const lookupCardByPayload =
    typeof opts.lookupCardByPayload === "function" ? opts.lookupCardByPayload : null;
  const proofSkewSec = Number.isFinite(opts.proofSkewSec)
    ? Math.max(60, Math.floor(opts.proofSkewSec))
    : 900;
  /** @type {Map<string, { max: number, devices: Map<string, string> }>} */
  const users = new Map();
  let saveChain = Promise.resolve();

  /** @type {crypto.KeyObject | null} */
  let pubKey = null;
  try {
    pubKey = crypto.createPublicKey(fss.readFileSync(pubKeyPath, "utf8"));
    logInfo(`quota pubkey loaded -> ${path.relative(repoRoot, pubKeyPath)}`);
  } catch (err) {
    logWarn(
      `quota disabled: 公钥缺失 ${path.relative(repoRoot, pubKeyPath)}` +
        `（先跑 node scripts/xcat-gate-keygen.mjs）：${err?.message || err}`,
    );
  }

  const enabled = () => !!pubKey;

  function verifyGateToken(gateToken) {
    if (!pubKey) return null;
    const raw = String(gateToken || "").trim();
    if (!raw) return null;
    const dot = raw.indexOf(".");
    if (dot <= 0 || dot + 1 >= raw.length) return null;
    const payloadB64 = raw.slice(0, dot);
    const sigB64 = raw.slice(dot + 1);
    let sig;
    try {
      sig = Buffer.from(sigB64, "base64url");
    } catch {
      return null;
    }
    if (sig.length !== 64) return null;
    let ok = false;
    try {
      ok = crypto.verify(
        "sha256",
        Buffer.from(payloadB64, "utf8"),
        { key: pubKey, dsaEncoding: "ieee-p1363" },
        sig,
      );
    } catch {
      return null;
    }
    if (!ok) return null;
    let payload;
    try {
      payload = JSON.parse(Buffer.from(payloadB64, "base64url").toString("utf8"));
    } catch {
      return null;
    }
    const uid = String(payload?.uid || "").trim().slice(0, 64);
    if (!uid) return null;
    const exp = Number(payload?.exp || 0) || 0;
    if (exp > 0 && Math.floor(Date.now() / 1000) >= exp) return null;
    // jti = 卡号，供卡级吊销用。老卡（本字段上线前签发的）没有，取到空串，判定处按「不查」处理。
    const jti = String(payload?.jti || "").trim().slice(0, 32);
    return { uid, exp, iss: Number(payload?.iss || 0) || 0, jti };
  }

  // 日志里出现的 uid/jti 可能来自未验证的 payload（攻击者可控）：去掉控制字符防伪造日志行。
  function logSafe(text, max = 64) {
    return String(text || "")
      .replace(/[\r\n\t\x00-\x1f\x7f]/g, "?")
      .slice(0, max);
  }

  // 派生凭证失败原因诊断：探活约 60s/客户端，同一原因每 60s 只记一条，避免刷满日志。
  // 没有这条日志，跨语言 HMAC 口径不一致的表现只是「客户端莫名变成无 uid」，极难定位。
  //
  // 节流键**只能**是下面那组固定原因码：/update/access.json 公网可达且不校验凭证，
  // 键里一旦掺入请求可控的值（jti / uid / skew 秒数），攻击者换着值刷就能把这张表撑爆。
  const proofFailSeen = new Map();
  function proofFail(code, detail) {
    const now = Date.now();
    const last = proofFailSeen.get(code) || 0;
    if (now - last > 60000) {
      proofFailSeen.set(code, now);
      logWarn(
        `gate proof reject: ${code}${detail ? ` ${detail}` : ""}` +
          `（该客户端本轮按「无签名 TOKEN」处理）`,
      );
    }
    return null;
  }

  // 派生凭证（X-XCat-Gate-Proof）验证：payloadB64.ts.macB64url
  //   mac = HMAC-SHA256(key=卡签名段原字节, msg=payloadB64|ts|deviceId)
  // 客户端只发这个，整张卡不上网 → 明文 HTTP 被抓包也拿不到可复用的卡。
  // 验 mac 需要卡的签名段，只能从签发台账取；台账缺失时返回 null（降级为「无 uid」，不硬拒）。
  function verifyGateProof(proof, deviceId) {
    if (!lookupCardByPayload) return null;
    const raw = String(proof || "").trim();
    if (!raw) return null;
    const parts = raw.split(".");
    if (parts.length !== 3) return proofFail("shape");
    const [payloadB64, tsText, macB64] = parts;
    if (!payloadB64 || !tsText || !macB64) return proofFail("shape");

    const ts = Number(tsText);
    if (!Number.isFinite(ts)) return proofFail("ts-nan");
    // 时间窗：抓到的凭证出窗即废。窗口给得比理论值宽，成员机器时钟常有偏差。
    const nowSec = Math.floor(Date.now() / 1000);
    const skew = nowSec - ts;
    if (Math.abs(skew) > proofSkewSec) return proofFail("ts-skew", `(${skew}s)`);

    let payload;
    try {
      payload = JSON.parse(Buffer.from(payloadB64, "base64url").toString("utf8"));
    } catch {
      return proofFail("payload-parse");
    }
    const uid = String(payload?.uid || "").trim().slice(0, 64);
    if (!uid) return proofFail("no-uid");
    // jti 只用于卡级吊销，**不参与验证**：拿它当台账键会让 jti 上线前签发的老卡永远查不到。
    const jti = String(payload?.jti || "").trim().slice(0, 32);

    // 按 payload 段查台账：命中即证明这个 payload 就是台账里某张卡的原文，
    // 自编 payload（哪怕借了别人的真卡号）必然 miss，故无需再单独比对一次。
    const card = lookupCardByPayload(payloadB64);
    const cardToken = String(card?.token || "");
    // 查不到：台账被清/被换过、台账行缺 token 字段，或卡不是本服务签的。降级为无 uid，不硬拒。
    // 详情里的 uid 来自未验证的 payload，只作线索、不能当事实。
    if (!cardToken) return proofFail("ledger-miss", `(uid=${logSafe(uid)} 未验证)`);
    const dot = cardToken.indexOf(".");
    if (dot <= 0) return proofFail("ledger-token-shape");

    let sig;
    try {
      sig = Buffer.from(cardToken.slice(dot + 1), "base64url");
    } catch {
      return proofFail("sig-decode");
    }
    if (sig.length !== 64) return proofFail("sig-len");

    const msg = `${payloadB64}|${tsText}|${String(deviceId || "")}`;
    const want = crypto.createHmac("sha256", sig).update(msg, "utf8").digest();
    let got;
    try {
      got = Buffer.from(macB64, "base64url");
    } catch {
      return proofFail("mac-decode");
    }
    // mac 不匹配最常见的两个原因：客户端与服务端的 HMAC/base64url 口径不一致（跨语言实现差异），
    // 或 deviceId 头与算 mac 时用的不是同一个值。
    if (got.length !== want.length) return proofFail("mac-len");
    // 走到这里 payload 已在台账里命中（索引键就是 payload），uid 可信（不是请求方随手编的）。
    if (!crypto.timingSafeEqual(got, want)) return proofFail("mac-mismatch", `(uid=${logSafe(uid)})`);

    const exp = Number(payload?.exp || 0) || 0;
    if (exp > 0 && nowSec >= exp) return proofFail("expired", `(uid=${logSafe(uid)})`);
    return { uid, exp, iss: Number(payload?.iss || 0) || 0, jti };
  }

  function normDevice(deviceId) {
    return String(deviceId || "").trim().slice(0, 64);
  }

  // lastSeen 解析不出来（老台账手工编辑坏了）时返回 -1，调用方按「未知」处理而不是当成刚见过。
  function idleSecOf(lastSeen, now) {
    const t = Date.parse(lastSeen);
    if (!Number.isFinite(t)) return -1;
    return Math.max(0, Math.floor((now - t) / 1000));
  }

  function pruneAged(rec) {
    if (agingDays <= 0) return;
    const cutoff = Date.now() - agingDays * 86400 * 1000;
    for (const [dev, seen] of [...rec.devices.entries()]) {
      const t = Date.parse(seen);
      if (Number.isFinite(t) && t < cutoff) rec.devices.delete(dev);
    }
  }

  async function load() {
    let parsed = null;
    try {
      parsed = JSON.parse(await fs.readFile(quotaPath, "utf8"));
    } catch (err) {
      if (err?.code !== "ENOENT") logWarn(`quota load failed: ${err.message || err}`);
    }
    users.clear();
    if (parsed && typeof parsed === "object") {
      if (Number.isFinite(parsed.defaultMax)) defaultMax = Math.max(0, Math.floor(parsed.defaultMax));
      if (Number.isFinite(parsed.agingDays)) agingDays = Math.max(0, Math.floor(parsed.agingDays));
      const u = parsed.users && typeof parsed.users === "object" ? parsed.users : {};
      for (const [uid, rec] of Object.entries(u)) {
        const max = Number.isFinite(rec?.max) ? Math.max(0, Math.floor(rec.max)) : 0;
        const devices = new Map();
        const dsrc = rec?.devices && typeof rec.devices === "object" ? rec.devices : {};
        for (const [dev, seen] of Object.entries(dsrc)) {
          const d = normDevice(dev);
          if (d) devices.set(d, String(seen || ts()));
        }
        users.set(String(uid).slice(0, 64), { max, devices });
      }
    }
    logInfo(
      `quota loaded users=${users.size} defaultMax=${defaultMax} agingDays=${agingDays} -> ${path.relative(repoRoot, quotaPath)}`,
    );
  }

  function persist() {
    saveChain = saveChain
      .then(async () => {
        await fs.mkdir(path.dirname(quotaPath), { recursive: true });
        const usersObj = {};
        for (const [uid, rec] of [...users.entries()].sort((a, b) => a[0].localeCompare(b[0]))) {
          usersObj[uid] = {
            max: rec.max,
            devices: Object.fromEntries([...rec.devices.entries()]),
          };
        }
        const body = `${JSON.stringify({ version: 1, updatedAt: ts(), defaultMax, agingDays, users: usersObj }, null, 2)}\n`;
        const tmp = `${quotaPath}.tmp`;
        await fs.writeFile(tmp, body, "utf8");
        await fs.rename(tmp, quotaPath);
      })
      .catch((err) => logWarn(`quota save failed: ${err.message || err}`));
    return saveChain;
  }

  // 探活判定：返回 { enabled, allowed, reason, uid, used, max }。
  // 未启用（无公钥）或无 gateToken/验签失败 → allowed:true（不阻断），仅 enabled/uid 反映情况。
  // claims 可由调用方预先验好传入（探活路径已经验过一次，别重复做 ECDSA/HMAC）。
  function evaluate({ gateToken, gateProof, deviceId, claims }) {
    if (!enabled()) return { enabled: false, allowed: true, reason: "", uid: "", used: 0, max: 0 };
    const verified =
      claims || verifyGateProof(gateProof, deviceId) || verifyGateToken(gateToken);
    if (!verified) return { enabled: true, allowed: true, reason: "", uid: "", used: 0, max: 0 };
    const dev = normDevice(deviceId);
    const uid = verified.uid;
    let rec = users.get(uid);
    if (!rec) {
      rec = { max: 0, devices: new Map() };
      users.set(uid, rec);
    }
    pruneAged(rec);
    const max = rec.max > 0 ? rec.max : defaultMax; // <=0 视为不限
    const known = dev && rec.devices.has(dev);

    if (dev) {
      if (known) {
        rec.devices.set(dev, ts()); // 刷新 lastSeen
        void persist();
        return { enabled: true, allowed: true, reason: "", uid, used: rec.devices.size, max };
      }
      if (max > 0 && rec.devices.size >= max) {
        // 超额：不收录新设备。
        return {
          enabled: true,
          allowed: false,
          reason: "quota",
          uid,
          used: rec.devices.size,
          max,
        };
      }
      rec.devices.set(dev, ts());
      void persist();
      return { enabled: true, allowed: true, reason: "", uid, used: rec.devices.size, max };
    }
    // 无 deviceId：无法计数，放行但不收录。
    return { enabled: true, allowed: true, reason: "", uid, used: rec.devices.size, max };
  }

  function snapshot() {
    const list = [];
    const now = Date.now();
    for (const [uid, rec] of users.entries()) {
      list.push({
        uid,
        max: rec.max,
        effectiveMax: rec.max > 0 ? rec.max : defaultMax,
        used: rec.devices.size,
        // idleSec 由服务端算：OPS 与服务端同机也不必赌两边时区/时钟一致。
        devices: [...rec.devices.entries()].map(([deviceId, lastSeen]) => ({
          deviceId,
          lastSeen,
          idleSec: idleSecOf(lastSeen, now),
        })),
      });
    }
    list.sort((a, b) => a.uid.localeCompare(b.uid));
    return {
      enabled: enabled(),
      defaultMax,
      agingDays,
      users: list,
      path: path.relative(repoRoot, quotaPath),
    };
  }

  async function setMax(uid, max) {
    const u = String(uid || "").trim().slice(0, 64);
    if (!u) {
      const err = new Error("uid required");
      err.status = 400;
      throw err;
    }
    let rec = users.get(u);
    if (!rec) {
      rec = { max: 0, devices: new Map() };
      users.set(u, rec);
    }
    rec.max = Math.max(0, Math.floor(Number(max) || 0));
    await persist();
    return { uid: u, max: rec.max, used: rec.devices.size };
  }

  async function removeDevice(uid, deviceId) {
    const u = String(uid || "").trim().slice(0, 64);
    const d = normDevice(deviceId);
    const rec = users.get(u);
    if (rec && d) rec.devices.delete(d);
    await persist();
    return { uid: u, used: rec ? rec.devices.size : 0 };
  }

  // 清僵尸名额：释放超过 days 天未见的设备。uid 为空=对全部用户执行。
  // 释放是可逆的：设备下次探活会重新登记（前提是那时还没超上限）。
  async function releaseIdle(uid, days) {
    const d = Math.floor(Number(days) || 0);
    if (d <= 0) {
      const err = new Error("days must be > 0");
      err.status = 400;
      throw err;
    }
    const u = String(uid || "").trim().slice(0, 64);
    const now = Date.now();
    const cutoffSec = d * 86400;
    const targets = u ? (users.has(u) ? [[u, users.get(u)]] : []) : [...users.entries()];
    let released = 0;
    const detail = [];
    for (const [key, rec] of targets) {
      let n = 0;
      for (const [dev, seen] of [...rec.devices.entries()]) {
        const idle = idleSecOf(seen, now);
        // idle<0（时间戳坏）不动：宁可留着让人工看，也不误删名额。
        if (idle >= cutoffSec) {
          rec.devices.delete(dev);
          n += 1;
        }
      }
      if (n > 0) detail.push({ uid: key, released: n, used: rec.devices.size });
      released += n;
    }
    if (released > 0) await persist();
    return { uid: u, days: d, released, detail };
  }

  return {
    load,
    persist,
    evaluate,
    verifyGateToken,
    verifyGateProof,
    snapshot,
    setMax,
    removeDevice,
    releaseIdle,
    isEnabled: enabled,
  };
}
