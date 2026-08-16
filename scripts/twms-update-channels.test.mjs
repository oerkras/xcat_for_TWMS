import assert from "node:assert/strict";
import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { createUpdateChannels } from "./twms-update-channels.mjs";

const tmp = await fs.mkdtemp(path.join(os.tmpdir(), "xcat-channels-"));
const zip = async (name, body) => fs.writeFile(path.join(tmp, name), body);

await zip("xcat_for_twms_0815_0.1.139_build139.zip", "pkg-139");
await zip("xcat_for_twms_0816_0.1.146_build146.zip", "pkg-146");
await fs.writeFile(
  path.join(tmp, "latest.json"),
  JSON.stringify({
    version: "0.1.146",
    buildId: 146,
    name: "xcat_for_twms_0816_0.1.146_build146",
    zipName: "xcat_for_twms_0816_0.1.146_build146.zip",
    downloadUrl: "xcat_for_twms_0816_0.1.146_build146.zip",
    sha256: "a".repeat(64),
    size: 7,
  }),
);

const ch = createUpdateChannels({ releaseRoot: tmp });
await ch.load();
assert.equal(ch.configured(), false);

const before = await ch.resolveManifest({});
assert.equal(before.buildId, 146);
assert.equal(before.channel, "fallback-latest");
assert.equal(await ch.zipAllowed("xcat_for_twms_0816_0.1.146_build146.zip"), true);

await ch.setDefault(139);
assert.equal(ch.configured(), true);
const def = await ch.resolveManifest({});
assert.equal(def.buildId, 139);
assert.equal(def.channel, "default");
assert.equal(def.zipName, "xcat_for_twms_0815_0.1.139_build139.zip");
assert.equal(def.sha256.length, 64);

assert.equal(await ch.zipAllowed("xcat_for_twms_0816_0.1.146_build146.zip"), false);
assert.equal(await ch.zipAllowed("xcat_for_twms_0815_0.1.139_build139.zip"), true);
assert.equal(
  await ch.zipAllowed("xcat_for_twms_0816_0.1.146_build146.zip", [
    "xcat_for_twms_0816_0.1.146_build146.zip",
  ]),
  true,
);

await ch.setGroup({ uid: "cwc", buildId: 146 });
const grouped = await ch.resolveManifest({ uid: "cwc" });
assert.equal(grouped.buildId, 146);
assert.equal(grouped.channel, "uid:cwc");
const others = await ch.resolveManifest({ uid: "ql" });
assert.equal(others.buildId, 139);

await ch.setGroup({ token: "OldTok", buildId: 146 });
const tok = await ch.resolveManifest({ token: "oldtok" });
assert.equal(tok.buildId, 146);

await ch.clearGroup({ uid: "cwc" });
const afterClear = await ch.resolveManifest({ uid: "cwc" });
assert.equal(afterClear.buildId, 139);

const ch2 = createUpdateChannels({ releaseRoot: tmp });
await ch2.load();
assert.equal(ch2.configured(), true);
const persisted = await ch2.resolveManifest({});
assert.equal(persisted.buildId, 139);

let threw = false;
try {
  await ch.setDefault(999);
} catch (e) {
  threw = true;
  assert.equal(e.status, 400);
}
assert.equal(threw, true);

await fs.rm(tmp, { recursive: true, force: true });
console.log("PASS  twms-update-channels");
