// 自测：台数配额按硬件指纹占槽。
// 整包拷到另一台 PC（同 deviceId、不同 hwfp）必须占新名额；同机换目录共用指纹不占第二台。
// 跑法：node _quota_hwfp_test.mjs
import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import crypto from "node:crypto";
import assert from "node:assert/strict";
import { createDeviceQuota } from "./scripts/twms-device-quota.mjs";

const root = await fs.mkdtemp(path.join(os.tmpdir(), "xcat-quota-hwfp-"));
const releaseRoot = path.join(root, "artifacts", "release");
const secretsDir = path.join(root, "secrets");
await fs.mkdir(releaseRoot, { recursive: true });
await fs.mkdir(secretsDir, { recursive: true });

const { publicKey } = crypto.generateKeyPairSync("ec", { namedCurve: "prime256v1" });
const pubPath = path.join(secretsDir, "gate_ec_pub.pem");
await fs.writeFile(pubPath, publicKey.export({ type: "spki", format: "pem" }), "utf8");

const fpA = "a".repeat(64);
const fpB = "b".repeat(64);
const fpC = "c".repeat(64);
const DEV = "device-copied";
const claims = { uid: "zhangsan", exp: 0, iss: 0, jti: "t" };

const quota = createDeviceQuota({
  releaseRoot,
  repoRoot: root,
  pubKeyPath: pubPath,
  defaultMax: 0,
  logInfo: () => {},
  logWarn: () => {},
  ts: () => new Date().toISOString(),
});
await quota.load();
await quota.setMax("zhangsan", 1);

function ev(deviceId, hwfp) {
  return quota.evaluate({ claims, deviceId, hwfp });
}

// 1) 第一台：deviceId + hwfpA，上限 1，放行。
const e1 = ev(DEV, fpA);
assert.equal(e1.allowed, true);
assert.equal(e1.used, 1);
console.log("ok 1: 第一台登记 used=1");

// 2) 同机换目录：新 deviceId、同一指纹 → 仍是这一台，不占第二名额。
const e2 = ev("device-other-folder", fpA);
assert.equal(e2.allowed, true, "同指纹必须认成同一台");
assert.equal(e2.used, 1, `同机换目录不应涨台数，实际 used=${e2.used}`);
const snap2 = quota.snapshot().users.find((u) => u.uid === "zhangsan");
assert.equal(snap2.used, 1);
assert.equal(snap2.devices[0].deviceId, "device-other-folder", "刷新为后到的 deviceId");
console.log("ok 2: 同机换目录共用指纹，used 仍=1");

// 3) 整包克隆到另一台：同一 deviceId、不同指纹 → 新机。上限 1 应拒。
const e3 = ev(DEV, fpB);
assert.equal(e3.allowed, false);
assert.equal(e3.reason, "quota");
assert.equal(e3.used, 1, "拒的时候不得把克隆写进台账");
console.log("ok 3: 克隆同 deviceId 不同 hwfp 且满员 → quota");

// 4) 把上限调到 2，克隆应放行并占第二名额。
await quota.setMax("zhangsan", 2);
const e4 = ev(DEV, fpB);
assert.equal(e4.allowed, true);
assert.equal(e4.used, 2);
console.log("ok 4: 上限 2 时克隆占新名额 used=2");

// 5) 再探活原机指纹，used 不涨。
const e5 = ev("device-other-folder", fpA);
assert.equal(e5.allowed, true);
assert.equal(e5.used, 2);
console.log("ok 5: 原机再探活不重复占槽");

// 6) 老台账字符串格式升级：无 hwfp 的 id 槽，第一次带指纹是绑定而不是新机。
const releaseRoot2 = path.join(root, "legacy");
await fs.mkdir(releaseRoot2, { recursive: true });
await fs.writeFile(
  path.join(releaseRoot2, "device-quota.json"),
  JSON.stringify({
    version: 1,
    defaultMax: 0,
    agingDays: 0,
    users: {
      zhangsan: { max: 1, devices: { "dev-old": new Date().toISOString() } },
    },
  }),
  "utf8",
);
const quotaLegacy = createDeviceQuota({
  releaseRoot: releaseRoot2,
  repoRoot: root,
  pubKeyPath: pubPath,
  logInfo: () => {},
  logWarn: () => {},
  ts: () => new Date().toISOString(),
});
await quotaLegacy.load();
const snapL = quotaLegacy.snapshot().users.find((u) => u.uid === "zhangsan");
assert.equal(snapL.used, 1);
assert.equal(snapL.devices[0].deviceId, "dev-old", "老台账 snapshot 仍暴露原 deviceId");
const eBind = quotaLegacy.evaluate({ claims, deviceId: "dev-old", hwfp: fpC });
assert.equal(eBind.allowed, true);
assert.equal(eBind.used, 1, "老安装补指纹不得当成新机");
const eClone = quotaLegacy.evaluate({ claims, deviceId: "dev-old", hwfp: fpA });
assert.equal(eClone.allowed, false, "补指纹之后再换机应占新名额，上限 1 则拒");
assert.equal(eClone.reason, "quota");
console.log("ok 6: 老台账补指纹不涨台数；之后换机才占新名额");

// 7) 释放按 deviceId 仍能删掉 h: 槽。
const rm = await quota.removeDevice("zhangsan", "device-other-folder");
assert.equal(rm.used, 1, `释放原机后应剩克隆 1 台，实际 ${rm.used}`);
console.log("ok 7: removeDevice 按 deviceId 能删指纹槽");

// 8) 同机 v1→v2：deviceId 不变、带上旧 v1，只迁槽不涨台数。
const releaseRoot3 = path.join(root, "v1v2");
await fs.mkdir(releaseRoot3, { recursive: true });
const quotaUp = createDeviceQuota({
  releaseRoot: releaseRoot3,
  repoRoot: root,
  pubKeyPath: pubPath,
  logInfo: () => {},
  logWarn: () => {},
  ts: () => new Date().toISOString(),
});
await quotaUp.load();
await quotaUp.setMax("zhangsan", 1);
const v1 = "1".repeat(64);
const v2 = "2".repeat(64);
assert.equal(quotaUp.evaluate({ claims, deviceId: "dev-pc", hwfp: v1 }).used, 1);
const up = quotaUp.evaluate({ claims, deviceId: "dev-pc", hwfp: v2, hwfpV1: v1 });
assert.equal(up.allowed, true);
assert.equal(up.used, 1, "同机指纹算法升级不得占第二台");
console.log("ok 8: v1→v2 同机迁槽 used 仍=1");

// 9) 整盘克隆：MachineGuid（v1）相同但已重发新 deviceId → 不能凭 v1 偷原机槽。
const clone = quotaUp.evaluate({
  claims,
  deviceId: "dev-clone",
  hwfp: "3".repeat(64),
  hwfpV1: v1,
});
assert.equal(clone.allowed, false, "克隆重发了新 id，即使 v1 相同也是新台");
assert.equal(clone.reason, "quota");
console.log("ok 9: 同 MachineGuid 的克隆（新 deviceId）占新名额");

await quota.persist();
await quotaLegacy.persist();
await quotaUp.persist();
await new Promise((r) => setTimeout(r, 50));
await fs.rm(root, { recursive: true, force: true });
console.log("\nall quota-hwfp assertions passed");
