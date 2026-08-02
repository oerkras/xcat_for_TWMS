#!/usr/bin/env node
/**
 * Smoke for TWMS update API.
 * Usage: node scripts/smoke-twms-server.mjs [baseUrl]
 * Default: http://127.0.0.1:18789/twms
 */
import http from "node:http";

const base = (process.argv[2] || "http://127.0.0.1:18789/twms").replace(/\/+$/, "");

function request(method, urlPath, body) {
  return new Promise((resolve, reject) => {
    const u = new URL(base + urlPath);
    const req = http.request(
      {
        protocol: u.protocol,
        hostname: u.hostname,
        port: u.port,
        path: u.pathname + u.search,
        method,
        headers: body
          ? { "content-type": "application/json", "content-length": Buffer.byteLength(body) }
          : {},
        timeout: 5000,
      },
      (res) => {
        const chunks = [];
        res.on("data", (c) => chunks.push(c));
        res.on("end", () => {
          resolve({
            status: res.statusCode,
            headers: res.headers,
            body: Buffer.concat(chunks).toString("utf8"),
          });
        });
      },
    );
    req.on("error", reject);
    req.on("timeout", () => req.destroy(new Error("timeout")));
    if (body) req.write(body);
    req.end();
  });
}

function assert(cond, msg) {
  if (!cond) throw new Error(msg);
}

const results = [];
async function check(name, fn) {
  try {
    await fn();
    results.push({ name, ok: true });
    console.log(`PASS  ${name}`);
  } catch (err) {
    results.push({ name, ok: false, error: err.message || String(err) });
    console.error(`FAIL  ${name}: ${err.message || err}`);
  }
}

await check("GET /health", async () => {
  const r = await request("GET", "/health");
  assert(r.status === 200, `status=${r.status}`);
  const j = JSON.parse(r.body);
  assert(j.ok === true, "ok!=true");
  assert(j.service === "xcat-twms-update", `service=${j.service}`);
  assert(j.version, "missing version");
});

await check("GET /ready", async () => {
  const r = await request("GET", "/ready");
  assert(r.status === 200, `status=${r.status}`);
  const j = JSON.parse(r.body);
  assert(j.ok === true, "ready not ok");
});

await check("GET /update/latest.json", async () => {
  const r = await request("GET", "/update/latest.json");
  assert(r.status === 200, `status=${r.status} body=${r.body.slice(0, 200)}`);
  const j = JSON.parse(r.body);
  assert(j.buildId > 0, "buildId missing");
  assert(j.zipName, "zipName missing");
  assert(j.sha256 && j.sha256.length === 64, "sha256 missing");
});

await check("GET /update/force.json (404 ok)", async () => {
  const r = await request("GET", "/update/force.json");
  assert(r.status === 404 || r.status === 200, `status=${r.status}`);
});

await check("session-v2 upload", async () => {
  const created = await request("POST", "/v1/logs/sessions", "{}");
  assert(created.status === 200, `create status=${created.status} body=${created.body.slice(0, 200)}`);
  const cj = JSON.parse(created.body);
  assert(cj.sessionId, "missing sessionId");

  const putBody = "hello-twms-log\n";
  const put = await new Promise((resolve, reject) => {
    const u = new URL(base + `/v1/logs/sessions/${cj.sessionId}/files/smoke.log`);
    const req = http.request(
      {
        protocol: u.protocol,
        hostname: u.hostname,
        port: u.port,
        path: u.pathname,
        method: "PUT",
        headers: {
          "content-type": "application/octet-stream",
          "content-length": Buffer.byteLength(putBody),
          "x-xcat-source": "smoke",
        },
        timeout: 5000,
      },
      (res) => {
        const chunks = [];
        res.on("data", (c) => chunks.push(c));
        res.on("end", () =>
          resolve({
            status: res.statusCode,
            body: Buffer.concat(chunks).toString("utf8"),
          }),
        );
      },
    );
    req.on("error", reject);
    req.on("timeout", () => req.destroy(new Error("timeout")));
    req.write(putBody);
    req.end();
  });
  assert(put.status === 200, `put status=${put.status} body=${put.body.slice(0, 200)}`);

  const commitBody = JSON.stringify({
    version: 2,
    profile: "twms",
    clientId: "smoke",
    machine: "smoke-pc",
    deviceId: "deadbeef-0000-0000-0000-000000000001",
    appVersion: "smoke",
    note: "smoke upload",
    uploadMode: "light",
  });
  const committed = await request(
    "POST",
    `/v1/logs/sessions/${cj.sessionId}/commit`,
    commitBody,
  );
  assert(
    committed.status === 200,
    `commit status=${committed.status} body=${committed.body.slice(0, 200)}`,
  );
  const mj = JSON.parse(committed.body);
  assert(mj.uploadId, "missing uploadId");
});

await check("GET /admin/clients", async () => {
  const r = await request("GET", "/admin/clients?activeSec=90");
  assert(r.status === 200, `status=${r.status} body=${r.body.slice(0, 200)}`);
  const j = JSON.parse(r.body);
  assert(j.ok === true, "ok!=true");
  assert(Array.isArray(j.clients), "clients not array");
  assert(typeof j.count === "number", "count missing");
});

const failed = results.filter((x) => !x.ok);
console.log(`\n${results.length - failed.length}/${results.length} passed`);
if (failed.length) process.exitCode = 1;
