/**
 * TWMS log upload (session-v2 + legacy) — ported from fengxing log-upload-server.
 * Layout: user_log_uploads/devices/<device>/<uploadId>/
 */
import { createWriteStream } from "node:fs";
import fs from "node:fs/promises";
import path from "node:path";
import crypto from "node:crypto";
import { finished } from "node:stream/promises";
import { execFile } from "node:child_process";
import { promisify } from "node:util";
import { writeCatalog, migrateFlatDeviceDirs, devicesRoot, DEVICES_SUBDIR } from "./find-user-logs.mjs";

const execFileAsync = promisify(execFile);

const kMaxUploadNoteCodePoints = 500;
const kLieEventsZipName = "lie_events.zip";
const kLieEventsDirName = "lie_events";
const kSessionTtlMs = 30 * 60 * 1000;
const kChinaTz = "Asia/Shanghai";

/**
 * @param {{
 *   outRoot: string,
 *   acceptProfiles?: string[],
 *   softMaxBytes?: number,
 *   maxBytes?: number,
 *   maxFileBytes?: number,
 *   maxSessionFiles?: number,
 *   maxUploads?: number,
 *   maxUploadsPerIp?: number,
 *   rateLimitMax?: number,
 *   rateLimitWindowMs?: number,
 *   uploadIdleTimeoutMs?: number,
 *   retainDays?: number,
 *   maxUploadsPerDevice?: number,
 *   uploadToken?: string,
 *   sendJson: (res: import('node:http').ServerResponse, status: number, obj: object) => void,
 *   logInfo: (msg: string) => void,
 *   logWarn: (msg: string) => void,
 *   logError: (msg: string) => void,
 *   getShuttingDown: () => boolean,
 *   noteError: (err: any) => void,
 *   clientIp: (req: import('node:http').IncomingMessage) => string,
 * }} opts
 */
export function createLogUpload(opts) {
  const outRoot = path.resolve(opts.outRoot);
  const deviceBucketRoot = devicesRoot(outRoot);
  let catalogRefreshTimer = null;
  let catalogRefreshInFlight = false;
  const softMaxBytes = Number(opts.softMaxBytes || 12 * 1024 * 1024);
  const maxBytes = Number(opts.maxBytes || 16 * 1024 * 1024);
  const kMaxFileBytes = Number(opts.maxFileBytes || softMaxBytes);
  const kMaxSessionFiles = Number(opts.maxSessionFiles || 512);
  const maxUploads = Number(opts.maxUploads || 10);
  const maxUploadsPerIp = Number(opts.maxUploadsPerIp || 3);
  const rateLimitMax = Number(opts.rateLimitMax || 30);
  const rateLimitWindowMs = Number(opts.rateLimitWindowMs || 60_000);
  const uploadIdleTimeoutMs = Number(opts.uploadIdleTimeoutMs || 90_000);
  const retainDays = Number(opts.retainDays ?? 30);
  const maxUploadsPerDevice = Number(opts.maxUploadsPerDevice || 200);
  const uploadToken = String(opts.uploadToken || "").trim();
  const acceptProfiles = (opts.acceptProfiles || ["twms"]).map((s) =>
    String(s).trim().toLowerCase(),
  ).filter(Boolean);

  const sendJson = opts.sendJson;
  const logInfo = opts.logInfo;
  const logWarn = opts.logWarn;
  const noteError = opts.noteError;
  const clientIp = opts.clientIp;
  const getShuttingDown = opts.getShuttingDown;

  const stats = {
    uploadsOk: 0,
    uploadsRejected: 0,
    uploadsFailed: 0,
    uploadsInFlight: 0,
    bytesReceived: 0,
    lastUploadAt: "",
    prunedDirs: 0,
  };

  /** @type {Map<string, number[]>} */
  const rateBuckets = new Map();
  /** @type {Map<string, number>} */
  const inflightByIp = new Map();
  /** @type {Map<string, { dir: string, ip: string, createdAt: number, files: Array<object> }>} */
  const uploadSessions = new Map();

  const sessionRoot = () => path.join(outRoot, "_sessions");

  function chinaParts(d = new Date()) {
    const fmt = new Intl.DateTimeFormat("en-CA", {
      timeZone: kChinaTz,
      year: "numeric",
      month: "2-digit",
      day: "2-digit",
      hour: "2-digit",
      minute: "2-digit",
      second: "2-digit",
      hourCycle: "h23",
    });
    const parts = {};
    for (const p of fmt.formatToParts(d)) {
      if (p.type !== "literal") parts[p.type] = p.value;
    }
    return parts;
  }

  function ts(d = new Date()) {
    const p = chinaParts(d);
    return `${p.year}-${p.month}-${p.day} ${p.hour}:${p.minute}:${p.second}`;
  }

  function localUploadStamp(d = new Date()) {
    const p = chinaParts(d);
    return `${p.year}-${p.month}-${p.day}_${p.hour}-${p.minute}-${p.second}`;
  }

  function sanitizeName(s, fallback) {
    const cleaned = String(s || "")
      .replace(/[\\/:*?"<>|\x00-\x1f]/g, "_")
      .replace(/\s+/g, "_")
      .slice(0, 96);
    return cleaned || fallback;
  }

  function resolveDeviceKey(payload) {
    const machine = sanitizeName(payload?.machine || "client", "client");
    const rawId = String(payload?.deviceId || "").trim();
    const hex = rawId.replace(/-/g, "").toLowerCase();
    if (/^[0-9a-f]{8,}$/.test(hex)) {
      return sanitizeName(`${machine}_${hex.slice(0, 8)}`, "client");
    }
    return sanitizeName(payload?.machine || payload?.clientId || "client", "client");
  }

  function sanitizeUploadNote(raw) {
    let s = String(raw ?? "")
      .replace(/[\u0000-\u001f\u007f]/g, " ")
      .replace(/\s+/g, " ")
      .trim();
    const cps = Array.from(s);
    if (cps.length > kMaxUploadNoteCodePoints) {
      s = cps.slice(0, kMaxUploadNoteCodePoints).join("");
    }
    return s;
  }

  function takeRateSlot(ip) {
    const now = Date.now();
    let bucket = rateBuckets.get(ip);
    if (!bucket) {
      bucket = [];
      rateBuckets.set(ip, bucket);
    }
    const cutoff = now - rateLimitWindowMs;
    while (bucket.length && bucket[0] < cutoff) bucket.shift();
    if (bucket.length >= rateLimitMax) return false;
    bucket.push(now);
    if (rateBuckets.size > 5000) {
      for (const [k, v] of rateBuckets) {
        if (!v.length || v[v.length - 1] < cutoff) rateBuckets.delete(k);
      }
    }
    return true;
  }

  function checkUploadToken(req) {
    if (!uploadToken) return true;
    const headerToken = String(req.headers["x-xcat-token"] || "").trim();
    const auth = String(req.headers.authorization || "").trim();
    const bearer = auth.toLowerCase().startsWith("bearer ") ? auth.slice(7).trim() : "";
    return headerToken === uploadToken || bearer === uploadToken;
  }

  function checkUploadAdmission(ip, { rate = true, global = false, perIp = false } = {}) {
    if (getShuttingDown()) {
      return { ok: false, status: 503, error: "server shutting down" };
    }
    if (rate && !takeRateSlot(ip)) {
      return { ok: false, status: 429, error: "rate limit exceeded" };
    }
    if (perIp) {
      const ipInflight = inflightByIp.get(ip) || 0;
      if (ipInflight >= maxUploadsPerIp) {
        return { ok: false, status: 429, error: `too many uploads from ip (max ${maxUploadsPerIp})` };
      }
    }
    if (global && stats.uploadsInFlight >= maxUploads) {
      return { ok: false, status: 429, error: `too many uploads (max ${maxUploads})` };
    }
    return { ok: true };
  }

  function psSingleQuote(text) {
    return `'${String(text).replace(/'/g, "''")}'`;
  }

  async function countFilesRecursive(dir) {
    let total = 0;
    for await (const ent of await fs.readdir(dir, { withFileTypes: true, recursive: true })) {
      if (ent.isFile()) total += 1;
    }
    return total;
  }

  async function extractLieEventsArchive(uploadDir) {
    const zipPath = path.join(uploadDir, kLieEventsZipName);
    try {
      await fs.access(zipPath);
    } catch {
      return null;
    }
    const dest = path.join(uploadDir, kLieEventsDirName);
    await fs.mkdir(dest, { recursive: true });
    const st = await fs.stat(zipPath);
    try {
      await execFileAsync(
        "powershell.exe",
        [
          "-NoProfile",
          "-ExecutionPolicy",
          "Bypass",
          "-WindowStyle",
          "Hidden",
          "-Command",
          `Expand-Archive -LiteralPath ${psSingleQuote(zipPath)} -DestinationPath ${psSingleQuote(dest)} -Force`,
        ],
        { timeout: 120_000, windowsHide: true },
      );
      const extractedFiles = await countFilesRecursive(dest);
      logInfo(
        `lie_events extracted dir=${path.basename(uploadDir)} zipBytes=${st.size} files=${extractedFiles}`,
      );
      return {
        zipName: kLieEventsZipName,
        zipBytes: st.size,
        extractedDir: kLieEventsDirName,
        extractedFiles,
        ok: true,
      };
    } catch (err) {
      logWarn(`lie_events extract failed dir=${path.basename(uploadDir)}: ${err.message || err}`);
      return {
        zipName: kLieEventsZipName,
        zipBytes: st.size,
        extractedDir: kLieEventsDirName,
        extractedFiles: 0,
        ok: false,
        error: String(err?.message || err).slice(0, 200),
      };
    }
  }

  async function pruneDeviceUploads(deviceDir) {
    if (retainDays <= 0 && maxUploadsPerDevice <= 0) return;
    let entries;
    try {
      entries = await fs.readdir(deviceDir, { withFileTypes: true });
    } catch {
      return;
    }
    const dirs = [];
    for (const ent of entries) {
      if (!ent.isDirectory()) continue;
      const full = path.join(deviceDir, ent.name);
      try {
        const st = await fs.stat(full);
        dirs.push({ full, mtimeMs: st.mtimeMs });
      } catch {
        /* ignore */
      }
    }
    dirs.sort((a, b) => b.mtimeMs - a.mtimeMs);
    const cutoff = retainDays > 0 ? Date.now() - retainDays * 86400_000 : 0;
    for (let i = 0; i < dirs.length; i += 1) {
      const d = dirs[i];
      const overCount = maxUploadsPerDevice > 0 && i >= maxUploadsPerDevice;
      const tooOld = retainDays > 0 && d.mtimeMs < cutoff;
      if (!overCount && !tooOld) continue;
      try {
        await fs.rm(d.full, { recursive: true, force: true });
        stats.prunedDirs += 1;
      } catch (err) {
        logWarn(`prune failed ${d.full}: ${err.message || err}`);
      }
    }
  }

  async function finalizeSavedUpload({ dir, deviceDir, device, uploadId, ip, payload, saved, protocol }) {
    const lieEvents = await extractLieEventsArchive(dir);
    const now = new Date();
    const note = sanitizeUploadNote(payload.note);
    const uploadModeRaw = String(payload.uploadMode || "").trim().toLowerCase();
    const uploadMode = uploadModeRaw === "full" || uploadModeRaw === "light" ? uploadModeRaw : "";
    const meta = {
      uploadId,
      receivedAt: ts(now),
      remoteAddress: ip,
      version: payload.version || (protocol === "session-v2" ? 2 : 1),
      protocol,
      profile: payload.profile || "",
      clientId: payload.clientId || "",
      machine: payload.machine || "",
      deviceId: payload.deviceId || "",
      note,
      uploadMode: uploadMode || undefined,
      device,
      appVersion: payload.appVersion || "",
      saved,
      lieEvents: lieEvents || undefined,
    };
    await fs.writeFile(path.join(dir, "meta.json"), `${JSON.stringify(meta, null, 2)}\n`, "utf8");
    const notePath = path.join(dir, "note.txt");
    if (note) {
      await fs.writeFile(notePath, `${note}\n`, "utf8");
    } else {
      await fs.rm(notePath, { force: true }).catch(() => {});
    }
    await fs.writeFile(path.join(deviceDir, "latest.txt"), `${uploadId}\n`, "utf8");
    await fs.appendFile(path.join(deviceDir, "index.jsonl"), `${JSON.stringify(meta)}\n`, "utf8");
    pruneDeviceUploads(deviceDir).catch((err) => {
      logWarn(`prune async failed: ${err.message || err}`);
    });
    scheduleCatalogRefresh();
    stats.uploadsOk += 1;
    stats.lastUploadAt = ts(now);
    logInfo(
      `saved ${device}/${uploadId} files=${saved.length} protocol=${protocol}` +
        (uploadMode ? ` mode=${uploadMode}` : "") +
        (note ? ` note=${JSON.stringify(note)}` : ""),
    );
    return meta;
  }

  /** 上传后防抖重建根级 catalog.jsonl，方便按机器/IP/备注找客户日志。 */
  function scheduleCatalogRefresh() {
    if (catalogRefreshTimer) clearTimeout(catalogRefreshTimer);
    catalogRefreshTimer = setTimeout(() => {
      catalogRefreshTimer = null;
      refreshUploadCatalog().catch((err) => {
        logWarn(`catalog refresh failed: ${err.message || err}`);
      });
    }, 1500);
  }

  async function refreshUploadCatalog() {
    if (catalogRefreshInFlight) {
      scheduleCatalogRefresh();
      return;
    }
    catalogRefreshInFlight = true;
    try {
      const { rows, catalogJsonl } = await writeCatalog(outRoot);
      logInfo(
        `catalog refreshed uploads=${rows.length} -> ${path.basename(catalogJsonl)}`,
      );
    } finally {
      catalogRefreshInFlight = false;
    }
  }

  async function readBody(req) {
    const chunks = [];
    let total = 0;
    let lastDataAt = Date.now();
    const idleTimer = setInterval(() => {
      if (Date.now() - lastDataAt > uploadIdleTimeoutMs) {
        const err = new Error(`upload idle timeout after ${total} bytes`);
        err.status = 408;
        req.destroy(err);
      }
    }, 5000);
    try {
      for await (const chunk of req) {
        lastDataAt = Date.now();
        total += chunk.length;
        if (total > maxBytes) {
          const err = new Error("request too large");
          err.status = 413;
          throw err;
        }
        chunks.push(chunk);
      }
    } catch (err) {
      logWarn(`upload body error from ${clientIp(req)} bytes=${total} ${err.message || err}`);
      noteError(err);
      throw err;
    } finally {
      clearInterval(idleTimer);
    }
    return Buffer.concat(chunks).toString("utf8");
  }

  async function readBodyToFile(req, filePath, byteLimit) {
    await fs.mkdir(path.dirname(filePath), { recursive: true });
    const ws = createWriteStream(filePath);
    let total = 0;
    let lastDataAt = Date.now();
    const idleTimer = setInterval(() => {
      if (Date.now() - lastDataAt > uploadIdleTimeoutMs) {
        const err = new Error(`upload idle timeout after ${total} bytes`);
        err.status = 408;
        req.destroy(err);
      }
    }, 5000);
    try {
      for await (const chunk of req) {
        lastDataAt = Date.now();
        total += chunk.length;
        if (total > byteLimit) {
          const err = new Error("request too large");
          err.status = 413;
          throw err;
        }
        if (!ws.write(chunk)) {
          await new Promise((resolve, reject) => {
            const onDrain = () => {
              ws.off("error", onError);
              resolve();
            };
            const onError = (err) => {
              ws.off("drain", onDrain);
              reject(err);
            };
            ws.once("drain", onDrain);
            ws.once("error", onError);
          });
        }
      }
      ws.end();
      await finished(ws);
    } catch (err) {
      ws.destroy();
      try {
        await fs.rm(filePath, { force: true });
      } catch {
        /* ignore */
      }
      logWarn(`upload file body error from ${clientIp(req)} bytes=${total} ${err.message || err}`);
      noteError(err);
      throw err;
    } finally {
      clearInterval(idleTimer);
    }
    return total;
  }

  function purgeExpiredSessions() {
    const now = Date.now();
    for (const [id, s] of uploadSessions) {
      if (now - s.createdAt < kSessionTtlMs) continue;
      uploadSessions.delete(id);
      fs.rm(s.dir, { recursive: true, force: true }).catch(() => {});
    }
  }

  function beginIpInflight(ip) {
    inflightByIp.set(ip, (inflightByIp.get(ip) || 0) + 1);
    stats.uploadsInFlight += 1;
  }

  function endIpInflight(ip) {
    stats.uploadsInFlight = Math.max(0, stats.uploadsInFlight - 1);
    const left = (inflightByIp.get(ip) || 1) - 1;
    if (left <= 0) inflightByIp.delete(ip);
    else inflightByIp.set(ip, left);
  }

  async function handleSessionCreate(req, res) {
    const ip = clientIp(req);
    if (!checkUploadToken(req)) {
      stats.uploadsRejected += 1;
      sendJson(res, 401, { ok: false, error: "unauthorized" });
      return;
    }
    const admission = checkUploadAdmission(ip, { rate: true });
    if (!admission.ok) {
      stats.uploadsRejected += 1;
      sendJson(res, admission.status, { ok: false, error: admission.error });
      return;
    }
    purgeExpiredSessions();
    const sessionId = `${localUploadStamp()}_${crypto.randomBytes(4).toString("hex")}`;
    const dir = path.join(sessionRoot(), sessionId);
    await fs.mkdir(dir, { recursive: true });
    uploadSessions.set(sessionId, { dir, ip, createdAt: Date.now(), files: [] });
    logInfo(`upload session create ${ip} id=${sessionId}`);
    sendJson(res, 200, {
      ok: true,
      sessionId,
      maxFileBytes: kMaxFileBytes,
      maxFiles: kMaxSessionFiles,
      softMaxBytes,
      artifacts: [kLieEventsZipName],
    });
  }

  async function handleSessionFile(req, res, sessionId, rawName) {
    const ip = clientIp(req);
    const session = uploadSessions.get(sessionId);
    if (!session) {
      sendJson(res, 404, { ok: false, error: "session not found" });
      return;
    }
    if (!checkUploadToken(req)) {
      stats.uploadsRejected += 1;
      sendJson(res, 401, { ok: false, error: "unauthorized" });
      return;
    }
    const admission = checkUploadAdmission(ip, { rate: false, global: true, perIp: true });
    if (!admission.ok) {
      stats.uploadsRejected += 1;
      sendJson(res, admission.status, { ok: false, error: admission.error });
      return;
    }
    const name = sanitizeName(decodeURIComponent(rawName), "log");
    if (session.files.length >= kMaxSessionFiles && !session.files.some((f) => f.name === name)) {
      stats.uploadsRejected += 1;
      sendJson(res, 400, { ok: false, error: `too many session files (max ${kMaxSessionFiles})` });
      return;
    }
    const contentLengthNum = Number(req.headers["content-length"] || 0);
    if (Number.isFinite(contentLengthNum) && contentLengthNum > kMaxFileBytes) {
      stats.uploadsRejected += 1;
      sendJson(res, 413, { ok: false, error: `file too large (max ${kMaxFileBytes})` });
      return;
    }

    beginIpInflight(ip);
    try {
      const filePath = path.join(session.dir, name);
      const bytes = await readBodyToFile(req, filePath, kMaxFileBytes);
      stats.bytesReceived += bytes;
      const source = String(req.headers["x-xcat-source"] || "");
      const originalBytes = Number(req.headers["x-xcat-original-size"] || bytes);
      const truncated = String(req.headers["x-xcat-truncated"] || "") === "1";
      session.files = session.files.filter((f) => f.name !== name);
      session.files.push({ name, source, bytes, originalBytes, truncated });
      logInfo(`upload session file ${ip} id=${sessionId} name=${name} bytes=${bytes}`);
      sendJson(res, 200, { ok: true, name, bytes });
    } catch (err) {
      stats.uploadsFailed += 1;
      throw err;
    } finally {
      endIpInflight(ip);
    }
  }

  async function handleSessionCommit(req, res, sessionId) {
    const ip = clientIp(req);
    const session = uploadSessions.get(sessionId);
    if (!session) {
      sendJson(res, 404, { ok: false, error: "session not found" });
      return;
    }
    if (!checkUploadToken(req)) {
      stats.uploadsRejected += 1;
      sendJson(res, 401, { ok: false, error: "unauthorized" });
      return;
    }
    const admission = checkUploadAdmission(ip, { rate: true });
    if (!admission.ok) {
      stats.uploadsRejected += 1;
      sendJson(res, admission.status, { ok: false, error: admission.error });
      return;
    }
    if (session.files.length === 0) {
      stats.uploadsFailed += 1;
      sendJson(res, 400, { ok: false, error: "no files in session" });
      return;
    }

    const body = await readBody(req);
    let payload = {};
    try {
      payload = body ? JSON.parse(body) : {};
    } catch {
      stats.uploadsFailed += 1;
      sendJson(res, 400, { ok: false, error: "invalid json" });
      return;
    }

    const profile = String(payload.profile || "").trim().toLowerCase();
    if (acceptProfiles.length > 0 && !acceptProfiles.includes(profile)) {
      stats.uploadsRejected += 1;
      logInfo(`reject upload profile=${profile || "(empty)"} from ${ip}`);
      sendJson(res, 403, { ok: false, error: `profile not accepted: ${profile || "(empty)"}` });
      return;
    }

    const now = new Date();
    const device = resolveDeviceKey(payload);
    const uploadId = `${localUploadStamp(now)}_${device}_${crypto.randomBytes(3).toString("hex")}`;
    const deviceDir = path.join(deviceBucketRoot, device);
    const dir = path.join(deviceDir, uploadId);
    await fs.mkdir(dir, { recursive: true });

    for (const file of session.files) {
      await fs.rename(path.join(session.dir, file.name), path.join(dir, file.name));
    }

    const meta = await finalizeSavedUpload({
      dir,
      deviceDir,
      device,
      uploadId,
      ip,
      payload,
      saved: session.files,
      protocol: "session-v2",
    });

    uploadSessions.delete(sessionId);
    fs.rm(session.dir, { recursive: true, force: true }).catch(() => {});

    sendJson(res, 200, {
      ok: true,
      uploadId,
      files: session.files.length,
      note: meta.note || "",
      lieEvents: meta.lieEvents || null,
    });
  }

  async function handleLegacyUpload(req, res) {
    const ip = clientIp(req);
    if (!checkUploadToken(req)) {
      stats.uploadsRejected += 1;
      sendJson(res, 401, { ok: false, error: "unauthorized" });
      return;
    }
    const admission = checkUploadAdmission(ip, { rate: true, global: true, perIp: true });
    if (!admission.ok) {
      stats.uploadsRejected += 1;
      sendJson(res, admission.status, { ok: false, error: admission.error });
      return;
    }
    const contentLengthNum = Number(req.headers["content-length"] || 0);
    if (Number.isFinite(contentLengthNum) && contentLengthNum > softMaxBytes) {
      stats.uploadsRejected += 1;
      sendJson(res, 413, {
        ok: false,
        error: `upload too large (${contentLengthNum} > ${softMaxBytes})`,
      });
      return;
    }

    beginIpInflight(ip);
    logInfo(`upload begin ${ip} content-length=${req.headers["content-length"] || "?"} inflight=${stats.uploadsInFlight}`);
    try {
      const body = await readBody(req);
      stats.bytesReceived += Buffer.byteLength(body);
      let payload;
      try {
        payload = JSON.parse(body);
      } catch {
        stats.uploadsFailed += 1;
        sendJson(res, 400, { ok: false, error: "invalid json" });
        return;
      }
      if (!Array.isArray(payload.logs) || payload.logs.length === 0) {
        stats.uploadsFailed += 1;
        sendJson(res, 400, { ok: false, error: "logs array required" });
        return;
      }
      if (payload.logs.length > 64) {
        stats.uploadsFailed += 1;
        sendJson(res, 400, { ok: false, error: "too many log files" });
        return;
      }
      const profile = String(payload.profile || "").trim().toLowerCase();
      if (acceptProfiles.length > 0 && !acceptProfiles.includes(profile)) {
        stats.uploadsRejected += 1;
        sendJson(res, 403, { ok: false, error: `profile not accepted: ${profile || "(empty)"}` });
        return;
      }

      const now = new Date();
      const device = resolveDeviceKey(payload);
      const uploadId = `${localUploadStamp(now)}_${device}_${crypto.randomBytes(3).toString("hex")}`;
      // session-v2 与 legacy 统一落到 devices/，避免运维两套路径。
      const deviceDir = path.join(deviceBucketRoot, device);
      const dir = path.join(deviceDir, uploadId);
      await fs.mkdir(dir, { recursive: true });

      const saved = [];
      for (const entry of payload.logs) {
        if (!entry || !entry.name || typeof entry.base64 !== "string") continue;
        const name = sanitizeName(entry.name, "log");
        let buf;
        try {
          buf = Buffer.from(entry.base64, "base64");
        } catch {
          continue;
        }
        if (buf.length > maxBytes) continue;
        await fs.writeFile(path.join(dir, name), buf);
        saved.push({
          name,
          source: String(entry.source || ""),
          bytes: buf.length,
          originalBytes: Number(entry.size || 0),
          truncated: Boolean(entry.truncated),
        });
      }
      if (saved.length === 0) {
        await fs.rm(dir, { recursive: true, force: true });
        stats.uploadsFailed += 1;
        sendJson(res, 400, { ok: false, error: "no valid logs" });
        return;
      }

      const meta = await finalizeSavedUpload({
        dir,
        deviceDir,
        device,
        uploadId,
        ip,
        payload,
        saved,
        protocol: "legacy-json",
      });
      sendJson(res, 200, {
        ok: true,
        uploadId,
        files: saved.length,
        note: meta.note || "",
        lieEvents: meta.lieEvents || null,
      });
    } catch (err) {
      stats.uploadsFailed += 1;
      noteError(err);
      throw err;
    } finally {
      endIpInflight(ip);
    }
  }

  async function handleLogRoutes(req, res, routedPath) {
    if (routedPath === "/v1/logs" && req.method === "POST") {
      await handleLegacyUpload(req, res);
      return;
    }
    if (routedPath === "/v1/logs/sessions" && req.method === "POST") {
      await handleSessionCreate(req, res);
      return;
    }
    const fileMatch = routedPath.match(/^\/v1\/logs\/sessions\/([^/]+)\/files\/([^/]+)$/);
    if (fileMatch && req.method === "PUT") {
      await handleSessionFile(req, res, fileMatch[1], fileMatch[2]);
      return;
    }
    const commitMatch = routedPath.match(/^\/v1\/logs\/sessions\/([^/]+)\/commit$/);
    if (commitMatch && req.method === "POST") {
      await handleSessionCommit(req, res, commitMatch[1]);
      return;
    }
    sendJson(res, 404, { ok: false, error: "not found" });
  }

  async function ensureDirs() {
    await fs.mkdir(outRoot, { recursive: true });
    await fs.mkdir(deviceBucketRoot, { recursive: true });
    await fs.mkdir(sessionRoot(), { recursive: true });
    const mig = await migrateFlatDeviceDirs(outRoot);
    if (mig.moved || mig.merged) {
      logInfo(
        `migrated device dirs -> ${DEVICES_SUBDIR}: moved=${mig.moved} merged=${mig.merged}`,
      );
    }
  }

  function uploadHealthSlice() {
    return {
      enabled: true,
      stub: false,
      ok: stats.uploadsOk,
      rejected: stats.uploadsRejected,
      failed: stats.uploadsFailed,
      inFlight: stats.uploadsInFlight,
      maxUploads,
      maxUploadsPerIp,
      bytesReceived: stats.bytesReceived,
      lastUploadAt: stats.lastUploadAt || null,
      prunedDirs: stats.prunedDirs,
    };
  }

  function uploadLimitsSlice() {
    return {
      maxBytes,
      softMaxBytes,
      maxFileBytes: kMaxFileBytes,
      maxSessionFiles: kMaxSessionFiles,
      acceptProfiles,
      outRoot,
    };
  }

  return {
    handleLogRoutes,
    ensureDirs,
    uploadHealthSlice,
    uploadLimitsSlice,
    outRoot,
    deviceBucketRoot,
  };
}
