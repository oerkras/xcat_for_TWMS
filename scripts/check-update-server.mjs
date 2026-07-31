#!/usr/bin/env node
import crypto from "node:crypto";
import http from "node:http";
import https from "node:https";

const args = process.argv.slice(2).filter((a) => a !== "--zip");
const base = (args[0] || "http://127.0.0.1:18789/twms").replace(/\/+$/, "");
const checkZip = process.argv.includes("--zip");
const manifestUrl = `${base}/update/latest.json`;

function getBuffer(url) {
  const lib = url.startsWith("https:") ? https : http;
  return new Promise((resolve, reject) => {
    const req = lib.get(url, { timeout: 30000 }, (res) => {
      const chunks = [];
      res.on("data", (chunk) => chunks.push(chunk));
      res.on("end", () => {
        resolve({
          status: res.statusCode || 0,
          headers: res.headers,
          body: Buffer.concat(chunks),
        });
      });
    });
    req.on("timeout", () => req.destroy(new Error(`timeout: ${url}`)));
    req.on("error", reject);
  });
}

function fail(message) {
  console.error(`[check-update-server] ${message}`);
  process.exitCode = 1;
}

async function main() {
  const health = await getBuffer(`${base}/health`);
  console.log(`health status=${health.status} body=${health.body.toString("utf8").trim()}`);
  if (health.status !== 200) fail("health endpoint failed");

  const manifestRes = await getBuffer(manifestUrl);
  console.log(`manifest status=${manifestRes.status}`);
  if (manifestRes.status !== 200) {
    fail("manifest endpoint failed");
    return;
  }

  let manifest;
  try {
    manifest = JSON.parse(manifestRes.body.toString("utf8"));
  } catch (err) {
    fail(`manifest JSON parse failed: ${err.message || err}`);
    return;
  }

  console.log(`version=${manifest.version} buildId=${manifest.buildId}`);
  console.log(`zip=${manifest.zipName}`);
  console.log(`sha256=${manifest.sha256}`);
  console.log(`size=${manifest.size}`);

  if (checkZip) {
    const rawDownloadUrl = manifest.downloadUrl || manifest.zipName || "";
    const downloadUrl = rawDownloadUrl
      ? new URL(String(rawDownloadUrl), manifestUrl).toString()
      : new URL(`/update/${manifest.zipName}`, manifestUrl).toString();
    const zip = await getBuffer(downloadUrl);
    console.log(`zip status=${zip.status} bytes=${zip.body.length}`);
    if (zip.status !== 200) {
      fail("zip download failed");
    } else {
      const digest = crypto.createHash("sha256").update(zip.body).digest("hex");
      console.log(`zip sha256=${digest}`);
      if (Number(manifest.size || 0) !== zip.body.length) fail("zip size mismatch");
      if (String(manifest.sha256 || "").toLowerCase() !== digest) fail("zip sha256 mismatch");
    }
  }
}

main().catch((err) => {
  fail(err?.message || String(err));
});
