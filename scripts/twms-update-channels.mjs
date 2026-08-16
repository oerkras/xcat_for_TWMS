/**
 * 更新通道：把「刚打出来的 latest.json」和「允许谁升到哪一版」拆开。
 *
 * 产品 = 经典版 / TWMS。出包只更新 artifacts/release/latest.json（最新构建），
 * 客户端 GET /update/latest.json 读的是本台账：
 *   - 默认 defaultBuildId（全员）
 *   - 按签卡 uid 覆盖（连接表「同人」分组）
 *   - 未签卡时按旧调试 TOKEN 覆盖
 *
 * 未配置台账时回落到 latest.json，兼容旧行为。
 */
import crypto from "node:crypto";
import fs from "node:fs/promises";
import path from "node:path";

const kZipName =
  /^xcat_for_twms_\d{4}_(\d+\.\d+\.\d+)_build(\d+)\.zip$/i;

/**
 * @param {{
 *   releaseRoot: string,
 *   logInfo?: (msg: string) => void,
 *   logWarn?: (msg: string) => void,
 *   ts?: () => string,
 *   normalizeUid?: (v: string) => string,
 *   normalizeToken?: (v: string) => string,
 * }} opts
 */
export function createUpdateChannels(opts) {
  const releaseRoot = opts.releaseRoot;
  const policyPath = path.join(releaseRoot, "update-channels.json");
  const logInfo = opts.logInfo || (() => {});
  const logWarn = opts.logWarn || (() => {});
  const ts =
    opts.ts ||
    (() => new Date().toISOString().replace("T", " ").slice(0, 19));
  const normalizeUid =
    opts.normalizeUid ||
    ((v) => String(v || "").replace(/[\u0000-\u001f\u007f]/g, "").trim().slice(0, 64));
  const normalizeToken =
    opts.normalizeToken ||
    ((v) =>
      String(v || "")
        .replace(/[\u0000-\u001f\u007f]/g, "")
        .replace(/\s+/g, "")
        .toLowerCase()
        .slice(0, 48));

  let defaultBuildId = 0;
  /** @type {Map<string, { buildId: number, note: string }>} */
  const groups = new Map();
  /** @type {Map<string, { buildId: number, note: string }>} */
  const tokens = new Map();
  let saveChain = Promise.resolve();

  /** @type {Map<string, { size: number, mtimeMs: number, sha256: string }>} */
  const hashCache = new Map();
  let pkgCache = { at: 0, list: /** @type {any[]} */ ([]) };

  function configured() {
    return defaultBuildId > 0 || groups.size > 0 || tokens.size > 0;
  }

  function parseZipName(name) {
    const m = kZipName.exec(String(name || ""));
    if (!m) return null;
    const version = m[1];
    const buildId = Number(m[2]);
    if (!Number.isInteger(buildId) || buildId <= 0) return null;
    return { version, buildId, zipName: String(name) };
  }

  async function sha256File(filePath, size, mtimeMs) {
    const key = path.basename(filePath);
    const hit = hashCache.get(key);
    if (hit && hit.size === size && hit.mtimeMs === mtimeMs) return hit.sha256;
    const buf = await fs.readFile(filePath);
    const sha256 = crypto.createHash("sha256").update(buf).digest("hex");
    hashCache.set(key, { size, mtimeMs, sha256 });
    return sha256;
  }

  async function listPackages({ hash = true } = {}) {
    const now = Date.now();
    if (hash && pkgCache.list.length && now - pkgCache.at < 2000) return pkgCache.list;
    let names = [];
    try {
      names = await fs.readdir(releaseRoot);
    } catch {
      names = [];
    }
    const out = [];
    for (const name of names) {
      const parsed = parseZipName(name);
      if (!parsed) continue;
      const filePath = path.join(releaseRoot, name);
      let st;
      try {
        st = await fs.stat(filePath);
      } catch {
        continue;
      }
      if (!st.isFile()) continue;
      const row = {
        version: parsed.version,
        buildId: parsed.buildId,
        name: name.replace(/\.zip$/i, ""),
        zipName: name,
        downloadUrl: name,
        size: st.size,
        sha256: "",
      };
      if (hash) {
        try {
          row.sha256 = await sha256File(filePath, st.size, st.mtimeMs);
        } catch (err) {
          logWarn(`update-channels hash failed ${name}: ${err?.message || err}`);
          continue;
        }
      }
      out.push(row);
    }
    out.sort((a, b) => b.buildId - a.buildId || a.zipName.localeCompare(b.zipName));
    if (hash) pkgCache = { at: now, list: out };
    return out;
  }

  async function packageForBuild(buildId) {
    const bid = Number(buildId) || 0;
    if (bid <= 0) return null;
    const list = await listPackages({ hash: true });
    return list.find((p) => p.buildId === bid) || null;
  }

  async function readLatestFile() {
    try {
      const raw = await fs.readFile(path.join(releaseRoot, "latest.json"), "utf8");
      const j = JSON.parse(raw);
      const buildId = Number(j.buildId) || 0;
      const zipName = path.basename(String(j.zipName || j.downloadUrl || ""));
      if (buildId <= 0 || !zipName.endsWith(".zip")) return null;
      return {
        version: String(j.version || ""),
        buildId,
        name: String(j.name || zipName.replace(/\.zip$/i, "")),
        zipName,
        downloadUrl: String(j.downloadUrl || zipName),
        sha256: String(j.sha256 || "").toLowerCase(),
        size: Number(j.size) || 0,
        notes: String(j.notes || ""),
        createdAt: String(j.createdAt || ""),
      };
    } catch {
      return null;
    }
  }

  function pickBuildId(uid, token) {
    const u = normalizeUid(uid);
    if (u && groups.has(u)) return { buildId: groups.get(u).buildId, channel: `uid:${u}` };
    const t = normalizeToken(token);
    if (t && tokens.has(t)) return { buildId: tokens.get(t).buildId, channel: `token:${t}` };
    if (defaultBuildId > 0) return { buildId: defaultBuildId, channel: "default" };
    return { buildId: 0, channel: "fallback-latest" };
  }

  async function resolveManifest({ uid = "", token = "" } = {}) {
    const picked = pickBuildId(uid, token);
    if (picked.buildId > 0) {
      const pkg = await packageForBuild(picked.buildId);
      if (pkg) {
        return { ...pkg, channel: picked.channel };
      }
      logWarn(
        `update-channels missing zip for build ${picked.buildId} (${picked.channel}); fallback latest.json`,
      );
    }
    const latest = await readLatestFile();
    if (!latest) return null;
    return { ...latest, channel: "fallback-latest" };
  }

  function referencedBuildIds() {
    const ids = new Set();
    if (defaultBuildId > 0) ids.add(defaultBuildId);
    for (const rec of groups.values()) ids.add(rec.buildId);
    for (const rec of tokens.values()) ids.add(rec.buildId);
    return ids;
  }

  async function zipAllowed(zipName, extraNames = []) {
    const base = path.basename(String(zipName || ""));
    if (!base.endsWith(".zip") || base.includes("..")) return false;
    for (const extra of extraNames) {
      if (path.basename(String(extra || "")) === base) return true;
    }
    if (!configured()) return true;
    const parsed = parseZipName(base);
    if (!parsed) return false;
    return referencedBuildIds().has(parsed.buildId);
  }

  async function persist() {
    const body = {
      updatedAt: ts(),
      defaultBuildId,
      groups: {},
      tokens: {},
    };
    for (const [uid, rec] of [...groups.entries()].sort((a, b) => a[0].localeCompare(b[0]))) {
      body.groups[uid] = { buildId: rec.buildId, note: rec.note || "" };
    }
    for (const [tok, rec] of [...tokens.entries()].sort((a, b) => a[0].localeCompare(b[0]))) {
      body.tokens[tok] = { buildId: rec.buildId, note: rec.note || "" };
    }
    const json = `${JSON.stringify(body, null, 2)}\n`;
    const tmp = `${policyPath}.tmp`;
    await fs.writeFile(tmp, json, "utf8");
    await fs.rename(tmp, policyPath);
  }

  function queueSave() {
    saveChain = saveChain.then(persist).catch((err) => {
      logWarn(`update-channels persist failed: ${err?.message || err}`);
    });
    return saveChain;
  }

  async function load() {
    let raw;
    try {
      raw = await fs.readFile(policyPath, "utf8");
    } catch {
      defaultBuildId = 0;
      groups.clear();
      tokens.clear();
      logInfo("update-channels: none (GET /update/latest.json falls back to latest.json)");
      return;
    }
    let j;
    try {
      j = JSON.parse(raw);
    } catch (err) {
      logWarn(`update-channels parse failed: ${err?.message || err}`);
      return;
    }
    defaultBuildId = Number(j.defaultBuildId) > 0 ? Math.floor(Number(j.defaultBuildId)) : 0;
    groups.clear();
    tokens.clear();
    for (const [uid, rec] of Object.entries(j.groups || {})) {
      const u = normalizeUid(uid);
      const bid = Number(rec?.buildId) || 0;
      if (!u || bid <= 0) continue;
      groups.set(u, { buildId: bid, note: String(rec?.note || "").slice(0, 80) });
    }
    for (const [tok, rec] of Object.entries(j.tokens || {})) {
      const t = normalizeToken(tok);
      const bid = Number(rec?.buildId) || 0;
      if (!t || bid <= 0) continue;
      tokens.set(t, { buildId: bid, note: String(rec?.note || "").slice(0, 80) });
    }
    logInfo(
      `update-channels loaded default=#${defaultBuildId || 0} groups=${groups.size} tokens=${tokens.size}`,
    );
  }

  async function assertPackage(buildId) {
    const bid = Math.floor(Number(buildId) || 0);
    if (bid <= 0) {
      const err = new Error("buildId required");
      err.status = 400;
      throw err;
    }
    const pkg = await packageForBuild(bid);
    if (!pkg) {
      const err = new Error(`release zip missing for build ${bid}`);
      err.status = 400;
      throw err;
    }
    return pkg;
  }

  async function setDefault(buildId) {
    const pkg = await assertPackage(buildId);
    defaultBuildId = pkg.buildId;
    await queueSave();
    logInfo(`update-channels default=#${pkg.buildId} zip=${pkg.zipName}`);
    return { defaultBuildId: pkg.buildId, zipName: pkg.zipName, version: pkg.version };
  }

  async function setGroup({ uid, token, buildId, note = "" }) {
    const pkg = await assertPackage(buildId);
    const u = normalizeUid(uid);
    const t = normalizeToken(token);
    const rec = { buildId: pkg.buildId, note: String(note || "").slice(0, 80) };
    if (u) {
      groups.set(u, rec);
      await queueSave();
      logInfo(`update-channels group uid=${u} #${pkg.buildId}`);
      return { kind: "uid", key: u, buildId: pkg.buildId, zipName: pkg.zipName, version: pkg.version };
    }
    if (t) {
      tokens.set(t, rec);
      await queueSave();
      logInfo(`update-channels group token=${t} #${pkg.buildId}`);
      return { kind: "token", key: t, buildId: pkg.buildId, zipName: pkg.zipName, version: pkg.version };
    }
    const err = new Error("uid or token required");
    err.status = 400;
    throw err;
  }

  async function clearGroup({ uid, token }) {
    const u = normalizeUid(uid);
    const t = normalizeToken(token);
    if (u) {
      const had = groups.delete(u);
      if (had) await queueSave();
      logInfo(`update-channels clear uid=${u} had=${had}`);
      return { kind: "uid", key: u, cleared: had };
    }
    if (t) {
      const had = tokens.delete(t);
      if (had) await queueSave();
      logInfo(`update-channels clear token=${t} had=${had}`);
      return { kind: "token", key: t, cleared: had };
    }
    const err = new Error("uid or token required");
    err.status = 400;
    throw err;
  }

  async function snapshot() {
    const packages = await listPackages({ hash: true });
    const lastBuilt = await readLatestFile();
    const byId = new Map(packages.map((p) => [p.buildId, p]));
    const pack = (bid) => {
      const p = byId.get(bid);
      return p
        ? { buildId: p.buildId, version: p.version, zipName: p.zipName, sha256: p.sha256, size: p.size }
        : { buildId: bid, version: "", zipName: "", sha256: "", size: 0, missing: true };
    };
    const groupRows = [...groups.entries()]
      .sort((a, b) => a[0].localeCompare(b[0]))
      .map(([uid, rec]) => ({ uid, note: rec.note, ...pack(rec.buildId) }));
    const tokenRows = [...tokens.entries()]
      .sort((a, b) => a[0].localeCompare(b[0]))
      .map(([token, rec]) => ({ token, note: rec.note, ...pack(rec.buildId) }));
    return {
      configured: configured(),
      path: "artifacts/release/update-channels.json",
      defaultBuildId,
      default: defaultBuildId > 0 ? pack(defaultBuildId) : null,
      lastBuilt: lastBuilt
        ? {
            buildId: lastBuilt.buildId,
            version: lastBuilt.version,
            zipName: lastBuilt.zipName,
          }
        : null,
      groups: groupRows,
      tokens: tokenRows,
      packages: packages.map((p) => ({
        buildId: p.buildId,
        version: p.version,
        zipName: p.zipName,
        size: p.size,
        sha256: p.sha256,
      })),
    };
  }

  return {
    load,
    configured,
    parseZipName,
    listPackages,
    packageForBuild,
    readLatestFile,
    resolveManifest,
    zipAllowed,
    setDefault,
    setGroup,
    clearGroup,
    snapshot,
  };
}
