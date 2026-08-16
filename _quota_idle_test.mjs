// 自测：配额台账 idleSec 回显 + releaseIdle 批量释放僵尸名额。
// 跑法：node _quota_idle_test.mjs
import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import assert from "node:assert/strict";
import { createDeviceQuota } from "./scripts/twms-device-quota.mjs";

const root = await fs.mkdtemp(path.join(os.tmpdir(), "xcat-quota-idle-"));
const releaseRoot = path.join(root, "artifacts", "release");
await fs.mkdir(releaseRoot, { recursive: true });

const iso = (msAgo) => new Date(Date.now() - msAgo).toISOString();
const DAY = 86400 * 1000;

await fs.writeFile(
  path.join(releaseRoot, "device-quota.json"),
  JSON.stringify({
    version: 1,
    defaultMax: 0,
    agingDays: 0,
    users: {
      zhangsan: {
        max: 5,
        devices: {
          "dev-fresh": iso(2 * 3600 * 1000),
          "dev-7d": iso(7 * DAY),
          "dev-45d": iso(45 * DAY),
          "dev-90d": iso(90 * DAY),
          "dev-broken": "not-a-date",
        },
      },
      lisi: { max: 3, devices: { "dev-l-1": iso(60 * DAY) } },
    },
  }),
  "utf8",
);

const quota = createDeviceQuota({
  releaseRoot,
  repoRoot: root,
  logInfo: () => {},
  logWarn: () => {},
  ts: () => new Date().toISOString(),
});
await quota.load();

// 1) snapshot 每台设备都带 idleSec；坏时间戳给 -1 而不是 0。
const snap = quota.snapshot();
const zs = snap.users.find((u) => u.uid === "zhangsan");
const byId = new Map(zs.devices.map((d) => [d.deviceId, d]));
assert.ok(byId.get("dev-fresh").idleSec >= 7000 && byId.get("dev-fresh").idleSec < 8000,
  `fresh idle ~2h, got ${byId.get("dev-fresh").idleSec}`);
assert.equal(Math.round(byId.get("dev-45d").idleSec / 86400), 45);
assert.equal(byId.get("dev-broken").idleSec, -1, "坏时间戳必须是 -1（未知），不能当刚见过");
assert.ok(byId.get("dev-fresh").lastSeen, "lastSeen 原字段要保留（老 OPS 兼容）");
console.log("ok 1: snapshot idleSec 正确，坏时间戳=-1，lastSeen 保留");

// 2) 单用户释放 30 天以上：只动 45d/90d，fresh/7d/broken 全留。
const r1 = await quota.releaseIdle("zhangsan", 30);
assert.equal(r1.released, 2, `期望释放 2 台，实际 ${r1.released}`);
const left = new Set(
  quota.snapshot().users.find((u) => u.uid === "zhangsan").devices.map((d) => d.deviceId),
);
assert.deepEqual([...left].sort(), ["dev-7d", "dev-broken", "dev-fresh"]);
console.log("ok 2: 单用户清僵尸只删超阈值的，坏时间戳不误删");

// 3) 未点名的用户不受影响。
assert.equal(quota.snapshot().users.find((u) => u.uid === "lisi").used, 1);
console.log("ok 3: 指定 uid 时不波及其他用户");

// 4) 全量释放（uid 空）：lisi 的 60d 也清掉。
const r2 = await quota.releaseIdle("", 30);
assert.equal(r2.released, 1, `期望释放 1 台，实际 ${r2.released}`);
assert.equal(quota.snapshot().users.find((u) => u.uid === "lisi").used, 0);
console.log("ok 4: uid 空=全部用户");

// 5) days<=0 必须拒（防手滑清空整个台账）。
await assert.rejects(() => quota.releaseIdle("zhangsan", 0), /days must be > 0/);
await assert.rejects(() => quota.releaseIdle("zhangsan", -1), /days must be > 0/);
console.log("ok 5: days<=0 被拒");

// 6) 落盘生效：重开一个实例读同一文件，释放结果还在。
const quota2 = createDeviceQuota({
  releaseRoot,
  repoRoot: root,
  logInfo: () => {},
  logWarn: () => {},
  ts: () => new Date().toISOString(),
});
await quota2.load();
const zs2 = quota2.snapshot().users.find((u) => u.uid === "zhangsan");
assert.equal(zs2.used, 3, `重载后应剩 3 台，实际 ${zs2.used}`);
console.log("ok 6: 释放已持久化");

// 7) 释放可逆：被清的设备再探活能重新登记。
const evalRes = quota2.evaluate({ gateToken: "", deviceId: "dev-90d" });
assert.equal(evalRes.allowed, true, "无 gateToken 时应放行（老客户端不阻断）");
console.log("ok 7: 无签名 TOKEN 仍放行（存量不阻断）");

await fs.rm(root, { recursive: true, force: true });
console.log("\nall quota-idle assertions passed");
