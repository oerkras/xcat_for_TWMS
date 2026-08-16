/**
 * 客户端历史台账自测：验证「重启不丢」这个核心目的确实成立。
 *  1) touch → flush → 新实例 load 能读回（activeClients 做不到这点）
 *  2) 租约剩余按 lastAllowAtMs + 64h 估算，与 gateViewForRow 同口径
 *  3) 超过 keepDays 的记录在 load 时老化掉
 *  4) 空值不抹掉旧快照（未进图的探活不该把角色名清空）
 *  5) 拒绝原因落账，事后查得到「他为什么用不了」
 * 跑法：node _client_history_test.mjs
 */
import os from "node:os";
import path from "node:path";
import fs from "node:fs";
import { createClientHistory } from "./scripts/twms-client-history.mjs";

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "xcat-hist-test-"));
const noop = () => {};
const ts = (d) => (d instanceof Date ? d : new Date()).toISOString();
const mk = (keepDays) =>
  createClientHistory({
    releaseRoot: tmp,
    repoRoot: process.cwd(),
    logInfo: noop,
    logWarn: noop,
    ts,
    keepDays,
    minWriteMs: 0,
  });

const now = Date.now();
const H = 3600 * 1000;

// ── 1) 落盘 + 重启读回 ──
{
  const h = mk(30);
  await h.load();
  h.touch({
    key: "dev:pc1:aaaa",
    ip: "1.2.3.4",
    machine: "PC1",
    deviceId: "dev-AAAA",
    uid: "kras",
    appVersion: "1.2.3",
    charName: "小猫",
    charLevel: 200,
    lastSeenMs: now,
    lastAllowAtMs: now - 4 * H, // 4 小时前放行过 → 租约还剩约 60h
    hits: 7,
  });
  await h.flush();
}
const h2 = mk(30);
await h2.load();
const rows = h2.list({});
const r = rows.find((x) => x.key === "dev:pc1:aaaa");
console.log("重启读回     :", !!r, r ? `uid=${r.uid} ver=${r.appVersion} 角色=${r.charName}` : "");
const remainH = r ? Math.round(r.leaseRemainSec / 3600) : -1;
console.log("租约剩余(h)  :", remainH, "期望≈60 →", remainH === 60);

// ── 2) 空值不抹掉旧快照 ──
h2.touch({ key: "dev:pc1:aaaa", ip: "1.2.3.4", lastSeenMs: now + 60 * 1000 });
const r2 = h2.list({}).find((x) => x.key === "dev:pc1:aaaa");
console.log("空值不抹旧   :", r2.charName === "小猫" && r2.uid === "kras" && r2.appVersion === "1.2.3");

// ── 3) 拒绝原因落账 ──
h2.touch({
  key: "dev:pc2:bbbb",
  ip: "5.6.7.8",
  machine: "PC2",
  uid: "leaker",
  lastSeenMs: now,
  lastDenyAtMs: now,
  lastDenyReason: "card revoked",
  lastDenyMatch: "jti",
});
await h2.flush();
const h3 = mk(30);
await h3.load();
const d = h3.list({}).find((x) => x.key === "dev:pc2:bbbb");
console.log("拒绝原因落账 :", d?.lastDenyMatch === "jti" && d?.lastDenyReason === "card revoked");
console.log("无放行则剩0  :", d?.leaseRemainSec === 0);

// ── 4) 老化：把一条改成 40 天前，用 keepDays=30 重载应被丢掉 ──
{
  const p = path.join(tmp, "client-history.json");
  const j = JSON.parse(fs.readFileSync(p, "utf8"));
  j.clients.push({
    key: "dev:old:zzzz",
    ip: "9.9.9.9",
    machine: "OLDPC",
    lastSeenMs: now - 40 * 24 * H,
    firstSeenMs: now - 41 * 24 * H,
  });
  fs.writeFileSync(p, JSON.stringify(j, null, 2));
}
const h4 = mk(30);
await h4.load();
const aged = h4.list({}).find((x) => x.key === "dev:old:zzzz");
console.log("40天前被老化 :", !aged);
const h5 = mk(60); // keepDays 放宽到 60 天则应保留
await h5.load();
console.log("keepDays=60留:", !!h5.list({}).find((x) => x.key === "dev:old:zzzz"));

// ── 5) days 过滤 + online 标注 ──
const recent = h5.list({ days: 1, onlineKeys: new Set(["dev:pc1:aaaa"]) });
console.log("days=1 只留近:", recent.every((x) => x.lastSeenSec <= 86400));
console.log("online 标注  :", recent.find((x) => x.key === "dev:pc1:aaaa")?.online === true);
console.log("离线标注     :", recent.find((x) => x.key === "dev:pc2:bbbb")?.online === false);

// ── 6) 下包幽灵（key=ip:…）不进历史、load 时清掉已落盘的 ──
h5.touch({ key: "ip:8.8.8.8", ip: "8.8.8.8", lastSeenMs: now });
console.log("touch 拒幽灵 :", !h5.list({}).find((x) => String(x.key).startsWith("ip:")));
{
  const p = path.join(tmp, "client-history.json");
  const j = JSON.parse(fs.readFileSync(p, "utf8"));
  j.clients.push({
    key: "ip:1.1.1.1",
    ip: "1.1.1.1",
    lastSeenMs: now,
    firstSeenMs: now,
  });
  fs.writeFileSync(p, JSON.stringify(j, null, 2));
}
const h6 = mk(30);
await h6.load();
console.log(
  "load 清幽灵  :",
  !h6.list({}).some((x) => String(x.key).startsWith("ip:")),
);

// ── 7) get / getByDeviceId：重启后在线表用历史补角色名 ──
{
  const byKey = h2.get("dev:pc1:aaaa");
  const byDev = h2.getByDeviceId("dev-AAAA");
  const ghost = h2.get("ip:1.1.1.1");
  console.log("get 按 key   :", byKey?.charName === "小猫");
  console.log("get 按设备   :", byDev?.charName === "小猫");
  console.log("get 拒幽灵   :", ghost == null);
}

fs.rmSync(tmp, { recursive: true, force: true });
