// 自测：派生凭证（X-XCat-Gate-Proof）—— 整张卡不上网，抓包拿到的凭证不可复用。
// 同时验证老客户端（整张卡上报）仍然认。
// 跑法：node _gate_proof_test.mjs
import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import crypto from "node:crypto";
import assert from "node:assert/strict";
import { createDeviceQuota } from "./scripts/twms-device-quota.mjs";

const root = await fs.mkdtemp(path.join(os.tmpdir(), "xcat-gate-proof-"));
const releaseRoot = path.join(root, "artifacts", "release");
const secretsDir = path.join(root, "secrets");
await fs.mkdir(releaseRoot, { recursive: true });
await fs.mkdir(secretsDir, { recursive: true });

const { publicKey, privateKey } = crypto.generateKeyPairSync("ec", { namedCurve: "prime256v1" });
const pubPath = path.join(secretsDir, "gate_ec_pub.pem");
await fs.writeFile(pubPath, publicKey.export({ type: "spki", format: "pem" }), "utf8");

function signPayload(payloadObj) {
  const payloadB64 = Buffer.from(JSON.stringify(payloadObj), "utf8").toString("base64url");
  const sig = crypto.sign("sha256", Buffer.from(payloadB64, "utf8"), {
    key: privateKey,
    dsaEncoding: "ieee-p1363",
  });
  return { token: `${payloadB64}.${sig.toString("base64url")}`, payloadB64 };
}

// 按服务端 signGateToken 的口径签一张卡。
function signCard(uid, { days = 0, jti = crypto.randomBytes(6).toString("hex") } = {}) {
  const now = Math.floor(Date.now() / 1000);
  const { token, payloadB64 } = signPayload({
    uid,
    iss: now,
    exp: days > 0 ? now + days * 86400 : 0,
    jti,
  });
  return { token, jti, uid, payloadB64 };
}

// 模拟客户端 BuildGateProof（common/xcat_start_gate.cpp）的算法。
function buildProof(token, deviceId, unixSec) {
  const dot = token.indexOf(".");
  const payloadB64 = token.slice(0, dot);
  const sig = Buffer.from(token.slice(dot + 1), "base64url");
  const msg = `${payloadB64}|${unixSec}|${deviceId}`;
  const mac = crypto.createHmac("sha256", sig).update(msg, "utf8").digest();
  return `${payloadB64}.${unixSec}.${mac.toString("base64url")}`;
}

// 台账索引按 payload 段建（与 twms-update-server 的 indexGateCard 同口径）：
// jti 只用于卡级吊销，不参与验证，否则 jti 上线前签发的老卡永远查不到。
const ledger = new Map();
function addToLedger(token, uid) {
  const dot = token.indexOf(".");
  ledger.set(token.slice(0, dot), { token, uid });
}
const ledgerByPayload = (p) => ledger.get(String(p || "")) || null;

const cardZs = signCard("zhangsan", { days: 30 });
const cardLs = signCard("lisi", { days: 30 });
addToLedger(cardZs.token, cardZs.uid);
addToLedger(cardLs.token, cardLs.uid);

const quota = createDeviceQuota({
  releaseRoot,
  repoRoot: root,
  pubKeyPath: pubPath,
  lookupCardByPayload: ledgerByPayload,
  logInfo: () => {},
  logWarn: () => {},
  ts: () => new Date().toISOString(),
});
await quota.load();

const DEV = "device-aaa";
const now = () => Math.floor(Date.now() / 1000);

// 1) 正常 proof 验通，取到可信 uid。
const p1 = buildProof(cardZs.token, DEV, now());
const c1 = quota.verifyGateProof(p1, DEV);
assert.ok(c1, "正常 proof 应该验通");
assert.equal(c1.uid, "zhangsan");
assert.equal(c1.jti, cardZs.jti);
console.log("ok 1: 正常派生凭证验通，uid/jti 正确");

// 2) 整张卡的签名段不在 proof 里 —— 抓包者拿不到可复用的卡。
assert.ok(!p1.includes(cardZs.token.split(".")[1]), "proof 里绝不能出现卡的签名段");
assert.equal(quota.verifyGateToken(p1), null, "proof 不该被误当成整张卡通过验签");
console.log("ok 2: proof 不含卡签名段，且本身不能当卡用");

// 3) 换一台设备重放同一条 proof → 失效（凭证绑定 deviceId）。
assert.equal(quota.verifyGateProof(p1, "device-bbb"), null);
console.log("ok 3: 换 deviceId 重放失效");

// 4) 时间戳出窗 → 失效（默认窗口 900s）。
const pOld = buildProof(cardZs.token, DEV, now() - 1000);
assert.equal(quota.verifyGateProof(pOld, DEV), null, "超窗的旧凭证必须拒");
const pFuture = buildProof(cardZs.token, DEV, now() + 1000);
assert.equal(quota.verifyGateProof(pFuture, DEV), null, "超窗的未来凭证必须拒");
// 窗口内的偏差要容忍（成员机器时钟常有偏差）。
assert.ok(quota.verifyGateProof(buildProof(cardZs.token, DEV, now() - 600), DEV));
console.log("ok 4: 出窗即废，窗口内时钟偏差仍容忍");

// 5) 篡改 mac / ts / payload 全部拒。
const [pl, tsx, mac] = p1.split(".");
assert.equal(quota.verifyGateProof(`${pl}.${tsx}.${"A".repeat(mac.length)}`, DEV), null);
assert.equal(quota.verifyGateProof(`${pl}.${Number(tsx) + 1}.${mac}`, DEV), null);
assert.equal(quota.verifyGateProof(`${pl}xx.${tsx}.${mac}`, DEV), null);
assert.equal(quota.verifyGateProof(`${pl}.${tsx}`, DEV), null, "段数不对要拒");
assert.equal(quota.verifyGateProof("", DEV), null);
console.log("ok 5: 篡改 mac/ts/payload 与畸形输入全拒");

// 6) 拿别人的真卡号配自己编的 payload → 拒。
// 索引键就是 payload 段，自编 payload 必然 miss，借真 jti 也没用。
const fakePayload = Buffer.from(
  JSON.stringify({ uid: "hacker", iss: now(), exp: 0, jti: cardZs.jti }),
  "utf8",
).toString("base64url");
const fakeSig = Buffer.from(cardLs.token.split(".")[1], "base64url");
const fakeMac = crypto
  .createHmac("sha256", fakeSig)
  .update(`${fakePayload}|${now()}|${DEV}`, "utf8")
  .digest("base64url");
assert.equal(quota.verifyGateProof(`${fakePayload}.${now()}.${fakeMac}`, DEV), null);
console.log("ok 6: 借用他人卡号 + 自编 payload 被拒");

// 7) 台账丢失（查不到）→ 返回 null 降级为「无 uid」，而不是抛异常。
const quotaNoLedger = createDeviceQuota({
  releaseRoot,
  repoRoot: root,
  pubKeyPath: pubPath,
  lookupCardByPayload: () => null,
  logInfo: () => {},
  logWarn: () => {},
  ts: () => new Date().toISOString(),
});
await quotaNoLedger.load();
assert.equal(quotaNoLedger.verifyGateProof(buildProof(cardZs.token, DEV, now()), DEV), null);
// 降级路径不能阻断：evaluate 仍放行（与老客户端同样待遇）。
const degraded = quotaNoLedger.evaluate({ gateProof: p1, deviceId: DEV });
assert.equal(degraded.allowed, true, "台账缺失必须降级放行，不能硬拒");
assert.equal(degraded.uid, "", "降级时拿不到 uid");
console.log("ok 7: 台账缺失降级为无 uid 且不阻断");

// 8) 没注入台账查询时也不炸（老宿主 / 单测直接构造）。
const quotaNoHook = createDeviceQuota({
  releaseRoot,
  repoRoot: root,
  pubKeyPath: pubPath,
  logInfo: () => {},
  logWarn: () => {},
  ts: () => new Date().toISOString(),
});
await quotaNoHook.load();
assert.equal(quotaNoHook.verifyGateProof(p1, DEV), null);
console.log("ok 8: 未注入台账查询时安全返回 null");

// 9) 老客户端整张卡上报仍认（向后兼容，不可一次性删）。
const legacy = quota.verifyGateToken(cardZs.token);
assert.ok(legacy && legacy.uid === "zhangsan", "整张卡上报必须继续认");
console.log("ok 9: 老客户端整张卡路径仍兼容");

// 10) jti 上线前签发的老卡（payload 里根本没有 jti 字段）也必须能验通。
// 新客户端只发 proof、不再发整张卡，若这里认不了，持老卡的成员会静默变成「无 uid」，
// 严格模式下被永久拒，且日志只说 no-jti 看不出是谁。
const legacyCard = signPayload({ uid: "laoka", iss: now(), exp: 0 });
addToLedger(legacyCard.token, "laoka");
const legacyClaims = quota.verifyGateProof(buildProof(legacyCard.token, DEV, now()), DEV);
assert.ok(legacyClaims, "无 jti 的老卡必须能验通（jti 不该是验证的前置条件）");
assert.equal(legacyClaims.uid, "laoka");
assert.equal(legacyClaims.jti, "", "老卡没有卡号，吊销粒度只能到人");
console.log("ok 10: 无 jti 的老卡也能验通，uid 照样拿得到");

// 11) evaluate 走 proof 能正常记账（收录 deviceId、算台数）。
const ev = quota.evaluate({ gateProof: buildProof(cardZs.token, DEV, now()), deviceId: DEV });
assert.equal(ev.uid, "zhangsan");
assert.equal(ev.allowed, true);
const snap = quota.snapshot().users.find((u) => u.uid === "zhangsan");
assert.equal(snap.used, 1, "proof 路径也要把设备登记进配额台账");
console.log("ok 11: proof 路径正常参与台数配额记账");

// 12) 过期卡的 proof 拒（exp 检查不能被 proof 路径绕过）。
const expiredCard = signPayload({
  uid: "guoqi",
  iss: now() - 100,
  exp: now() - 10,
  jti: crypto.randomBytes(6).toString("hex"),
});
addToLedger(expiredCard.token, "guoqi");
assert.equal(quota.verifyGateProof(buildProof(expiredCard.token, DEV, now()), DEV), null);
console.log("ok 12: 过期卡的 proof 被拒");

// 13) 诊断日志的节流键必须是固定原因码。
// /update/access.json 公网可达且不校验凭证：键里一旦掺入请求可控的值（uid / 偏差秒数），
// 攻击者换着值刷就能把节流表撑爆 → 未授权远程内存耗尽。用日志条数间接证明键是有限集。
const dosWarns = [];
// 独立 releaseRoot：与主实例共用会让两边的 persist 互相覆盖同一个 device-quota.json。
const dosRoot = path.join(root, "dos", "release");
await fs.mkdir(dosRoot, { recursive: true });
const quotaDos = createDeviceQuota({
  releaseRoot: dosRoot,
  repoRoot: root,
  pubKeyPath: pubPath,
  lookupCardByPayload: ledgerByPayload,
  logInfo: () => {},
  logWarn: (m) => dosWarns.push(m),
  ts: () => new Date().toISOString(),
});
await quotaDos.load();
for (let i = 0; i < 200; i += 1) {
  // 每次换一个台账里没有的 payload（ledger-miss 分支）。
  const plx = Buffer.from(
    JSON.stringify({ uid: `attacker${i}`, iss: 0, exp: 0 }),
    "utf8",
  ).toString("base64url");
  quotaDos.verifyGateProof(`${plx}.${now()}.${"A".repeat(43)}`, DEV);
}
for (let i = 0; i < 200; i += 1) {
  // 每次换一个不同的超窗偏差（ts-skew 分支）。
  quotaDos.verifyGateProof(buildProof(cardZs.token, DEV, now() - 100000 - i), DEV);
}
assert.ok(
  dosWarns.length <= 2,
  `节流键掺了请求可控的值：400 次伪造凭证打出 ${dosWarns.length} 条日志（应 <=2，即每个原因码一条）`,
);
console.log("ok 13: 诊断日志节流键是固定原因码，伪造凭证刷不爆");

// evaluate 里的 persist() 是 fire-and-forget，等它落完再删临时目录（否则 Windows 上 EBUSY）。
await quota.persist();
await quotaDos.persist();
await new Promise((r) => setTimeout(r, 50));
await fs.rm(root, { recursive: true, force: true });
console.log("\nall gate-proof assertions passed");
