import os from "node:os";
import path from "node:path";
import fs from "node:fs";
import cp from "node:child_process";
import { createDeviceAccess } from "./scripts/twms-device-access.mjs";
import { createDeviceQuota } from "./scripts/twms-device-quota.mjs";

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "xcat-gate-test-"));
const noop = () => {};
const ts = () => new Date().toISOString();
const access = createDeviceAccess({ releaseRoot: tmp, repoRoot: process.cwd(), logInfo: noop, logWarn: noop, ts });
const quota = createDeviceQuota({ releaseRoot: tmp, repoRoot: process.cwd(), logInfo: noop, logWarn: noop, ts });
await access.load();

function sign(uid) {
  const out = cp.execSync(`node scripts/xcat-gate-sign.mjs --uid ${uid}`).toString();
  return out.split(/\r?\n/).find((l) => l && !l.startsWith("[") && !l.includes(" ") && l.includes("."));
}

// 模拟 update-server 的 access.json 判定：先验签取 uid，再 strict/evaluate。
function decide(gateToken, id) {
  const claims = quota.verifyGateToken(gateToken);
  const uid = claims ? access.normalizeUid(claims.uid) : "";
  if (access.getStrictToken() && !uid) return { allowed: false, match: "strict" };
  return access.evaluate({ ...id, uid });
}

const tokBan = sign("banme");
const tokOther = sign("gooduser");

// 1) 按 uid 封禁
access.ban({ uid: "banme", reason: "test uid ban" });
await access.persist();

// 改硬件前：设备 A
const dA = decide(tokBan, { machine: "PC1", deviceId: "dev-AAAA", macs: ["aabbccddeeff"] });
// 改硬件后：全新 deviceId + 全新 MAC，但同一张签名 TOKEN
const dB = decide(tokBan, { machine: "PC1", deviceId: "dev-ZZZZ-changed", macs: ["001122334455"] });
// 另一个人（未封）
const dGood = decide(tokOther, { machine: "PC2", deviceId: "dev-BBBB", macs: ["66778899aabb"] });

console.log("uid封-设备A :", dA.allowed, dA.match, dA.reason || "");
console.log("uid封-改硬件后:", dB.allowed, dB.match, dB.reason || "");
console.log("未封用户    :", dGood.allowed, dGood.match);

// 2) 解封后放行
access.unban({ uid: "banme" });
await access.persist();
const dAfter = decide(tokBan, { machine: "PC1", deviceId: "dev-QQQQ", macs: ["aa0011bb2233"] });
console.log("解封后       :", dAfter.allowed, dAfter.match);

// 3) 严格模式：无有效 TOKEN 拒；有效 TOKEN 放行；默认关不误伤
await access.setStrictToken(true);
const noTok = decide("", { machine: "OLD", deviceId: "dev-OLD", macs: ["ffee11223344"] });
const withTok = decide(tokOther, { machine: "PC3", deviceId: "dev-CCCC", macs: ["112233aabbcc"] });
console.log("严格-无TOKEN :", noTok.allowed, noTok.match);
console.log("严格-有TOKEN :", withTok.allowed, withTok.match);
await access.setStrictToken(false);
const looseNoTok = decide("", { machine: "OLD", deviceId: "dev-OLD2", macs: ["ffee11225566"] });
console.log("宽松-无TOKEN :", looseNoTok.allowed, looseNoTok.match);

fs.rmSync(tmp, { recursive: true, force: true });
