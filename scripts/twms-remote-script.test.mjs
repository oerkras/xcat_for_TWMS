import assert from "node:assert/strict";
import { createRemoteScriptQueue } from "./twms-remote-script.mjs";

let now = 1_000_000;
const logs = [];
const q = createRemoteScriptQueue({
  logInfo: (m) => logs.push(String(m)),
  ts: () => "t",
  now: () => now,
});

function assertThrows(fn, status, re) {
  try {
    fn();
    throw new Error("expected throw");
  } catch (err) {
    if (err.message === "expected throw") throw err;
    assert.equal(err.status, status, err.message);
    assert.match(err.message, re);
  }
}

assertThrows(() => q.enqueue({ script: "Write-Output 1" }), 400, /deviceId \/ MAC \/ TOKEN/);
assertThrows(
  () => q.enqueue({ deviceId: "abc", script: "x".repeat(17 * 1024) }),
  413,
  /too large/,
);
assertThrows(() => q.enqueue({ deviceId: "abc", op: "showMsg", body: "  " }), 400, /message empty/);
assertThrows(
  () => q.enqueue({ deviceId: "abc", op: "msgbox", body: "y".repeat(5 * 1024) }),
  413,
  /too large/,
);

const msg = q.enqueue({
  deviceId: "dev-1",
  token: "tok-aaa",
  op: "notify",
  title: "维护",
  body: "今晚 22:00 停机\n请下线",
});
assert.equal(msg.op, "showMsg");
assert.equal(msg.title, "维护");
assert.equal(msg.status, "queued");
assert.equal("script" in msg, false);
assert.equal("body" in msg, false);
assert.ok(msg.bytes > 0);
assert.ok(msg.matchKeys.includes("id:dev-1"));
assert.ok(msg.matchKeys.includes("tok:tok-aaa"));

const listed = q.list();
assert.equal(listed.length, 1);
assert.equal("script" in listed[0], false);
assert.equal("body" in listed[0], false);

const offered = q.onAccess({ deviceId: "dev-1" });
assert.equal(offered.op, "showMsg");
assert.equal(offered.title, "维护");
assert.equal(offered.body, "今晚 22:00 停机\n请下线");
assert.equal("script" in offered, false);

const stillOffered = q.onAccess({ deviceId: "dev-1" });
assert.equal(stillOffered.id, offered.id);
assert.equal(q.onAccess({ deviceId: "dev-1" }, offered.id), null);

now += 10 * 60 * 1000;
assert.equal(q.list()[0].status, "acked");

now += 21 * 60 * 1000;
const afterAckStale = q.list()[0];
assert.equal(afterAckStale.status, "done");
assert.equal(afterAckStale.result, "timeout");

const run = q.enqueue({ deviceId: "dev-2", script: "Write-Output hi" });
assert.equal(run.op, "runScript");
const runOffer = q.onAccess({ deviceId: "dev-2" });
assert.equal(runOffer.op, "runScript");
assert.equal(runOffer.script, "Write-Output hi");
assert.equal("body" in runOffer, false);

now += 30_000 + 45_000 + 1;
const runStale = q.list().find((r) => r.id === run.id);
assert.ok(runStale);
assert.equal(runStale.status, "done");
assert.equal(runStale.result, "no_ack");

const old = q.enqueue({ deviceId: "dev-3", op: "showMsg", body: "old" });
q.enqueue({ deviceId: "dev-3", op: "showMsg", body: "new" });
assert.equal(
  q.list().filter((r) => r.deviceId === "dev-3" && r.status !== "done").length,
  1,
);
const latest = q.onAccess({ deviceId: "dev-3" });
assert.equal(latest.body, "new");
assert.notEqual(latest.id, old.id);

q.markDone(latest.id, { result: "ok", exitCode: 0 });
assert.equal(q.list().find((r) => r.id === latest.id).result, "ok");

const both = q.enqueue({ deviceId: "dev-4", script: "Write-Output both" });
q.onAccess({ deviceId: "dev-4" });
q.onAccess({ deviceId: "dev-4" }, both.id, both.id, { result: "ok", exitCode: 0, outTail: "both" });
const bothRow = q.list().find((r) => r.id === both.id);
assert.equal(bothRow.status, "done");
assert.equal(bothRow.result, "ok");
assert.equal(bothRow.outTail, "both");

const ign = q.enqueue({ deviceId: "legacy", op: "showMsg", body: "ping", timeoutSec: 5 });
q.onAccess({ deviceId: "legacy" });
now += 5_000 + 45_000 + 1;
const noAck = q.list().find((r) => r.id === ign.id);
assert.equal(noAck.status, "done");
assert.equal(noAck.result, "no_ack");

console.log("twms-remote-script.test.mjs ok");
