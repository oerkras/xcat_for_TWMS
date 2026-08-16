/**
 * 卡级吊销（jti）自测：模拟 update-server 的探活判定，验证
 *  1) 签发的卡 payload 带 jti
 *  2) 废掉某张卡后该卡被拒，同 uid 的另一张卡照常放行（这是 uid 级封禁做不到的）
 *  3) 老卡（payload 无 jti）不受影响 —— 向后兼容
 *  4) 解废后恢复放行
 * 跑法：node _gate_jti_test.mjs
 */
import os from "node:os";
import path from "node:path";
import fs from "node:fs";
import cp from "node:child_process";
import crypto from "node:crypto";
import { createDeviceAccess } from "./scripts/twms-device-access.mjs";
import { createDeviceQuota } from "./scripts/twms-device-quota.mjs";

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "xcat-jti-test-"));
const noop = () => {};
const ts = () => new Date().toISOString();
const access = createDeviceAccess({
  releaseRoot: tmp,
  repoRoot: process.cwd(),
  logInfo: noop,
  logWarn: noop,
  ts,
});
const quota = createDeviceQuota({
  releaseRoot: tmp,
  repoRoot: process.cwd(),
  logInfo: noop,
  logWarn: noop,
  ts,
});
await access.load();

function sign(uid) {
  const out = cp.execSync(`node scripts/xcat-gate-sign.mjs --uid ${uid}`).toString();
  return out
    .split(/\r?\n/)
    .find((l) => l && !l.startsWith("[") && !l.includes(" ") && l.includes("."));
}

// 老卡：不带 jti 的 payload，用同一把私钥签，模拟本次改动之前签发的卡。
function signLegacy(uid) {
  const pem = fs.readFileSync(path.join(process.cwd(), "secrets", "gate_ec_priv.pem"), "utf8");
  const now = Math.floor(Date.now() / 1000);
  const payloadB64 = Buffer.from(JSON.stringify({ uid, iss: now, exp: 0 }), "utf8").toString(
    "base64url",
  );
  const sig = crypto.sign("sha256", Buffer.from(payloadB64, "utf8"), {
    key: crypto.createPrivateKey(pem),
    dsaEncoding: "ieee-p1363",
  });
  return `${payloadB64}.${sig.toString("base64url")}`;
}

// 与 update-server handleProbe 同口径：验签 → strict → jti 废卡 → evaluate。
function decide(gateToken, id) {
  const claims = quota.verifyGateToken(gateToken);
  const uid = claims ? access.normalizeUid(claims.uid) : "";
  const jti = claims?.jti || "";
  if (access.getStrictToken() && !uid) return { allowed: false, match: "strict" };
  if (jti && access.isJtiRevoked(jti)) return { allowed: false, match: "jti", reason: "card revoked" };
  return access.evaluate({ ...id, uid });
}

const dev = { machine: "PC1", deviceId: "dev-AAAA", macs: ["aabbccddeeff"] };

// 同一个人的两张卡：模拟「续签」后旧卡泄露。
const tokOld = sign("kras");
const tokNew = sign("kras");
const jtiOld = quota.verifyGateToken(tokOld)?.jti || "";
const jtiNew = quota.verifyGateToken(tokNew)?.jti || "";
console.log("旧卡卡号 :", jtiOld || "(空!)");
console.log("新卡卡号 :", jtiNew || "(空!)");
console.log("卡号唯一 :", !!jtiOld && !!jtiNew && jtiOld !== jtiNew);

// 只废旧卡
access.revokeJti({ jti: jtiOld, uid: "kras", reason: "test leak" });
await access.persist();

const rOld = decide(tokOld, dev);
const rNew = decide(tokNew, dev);
console.log("废旧卡-旧卡 :", rOld.allowed, rOld.match, rOld.reason || "");
console.log("废旧卡-新卡 :", rNew.allowed, rNew.match);

// 老卡（无 jti）不受任何影响
const tokLegacy = signLegacy("kras");
const rLegacy = decide(tokLegacy, dev);
console.log("老卡无jti   :", rLegacy.allowed, rLegacy.match, "(jti=" +
  (quota.verifyGateToken(tokLegacy)?.jti === "" ? "空" : "非空!") + ")");

// 落盘后重载：吊销名单要能持久化
await access.load();
const rReload = decide(tokOld, dev);
console.log("重载后-旧卡 :", rReload.allowed, rReload.match);

// 解废恢复
access.unrevokeJti({ jti: jtiOld });
await access.persist();
const rUn = decide(tokOld, dev);
console.log("解废后-旧卡 :", rUn.allowed, rUn.match);

// uid 级封禁仍然是「封人」：两张卡一起拦（与卡级吊销的区别）
access.ban({ uid: "kras", reason: "test uid ban" });
await access.persist();
const bOld = decide(tokOld, dev);
const bNew = decide(tokNew, dev);
console.log("uid封-旧卡  :", bOld.allowed, bOld.match);
console.log("uid封-新卡  :", bNew.allowed, bNew.match);

fs.rmSync(tmp, { recursive: true, force: true });
