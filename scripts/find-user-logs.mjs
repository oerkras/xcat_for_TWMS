#!/usr/bin/env node
/**
 * TWMS 客户回传日志检索工具（产品=经典版；逻辑对照枫星仓 find-user-logs）。
 *
 * 用法：
 *   node scripts/find-user-logs.mjs                  # 最近上传列表
 *   node scripts/find-user-logs.mjs list [--limit N]
 *   node scripts/find-user-logs.mjs search <关键字> [--in-logs] [--limit N]
 *   node scripts/find-user-logs.mjs catalog           # 重建 catalog.jsonl
 *   node scripts/find-user-logs.mjs open <关键字>     # 打开匹配到的最新上传目录
 *
 * 关键字可匹配：note(备注) / machine / device / deviceId / IP / uploadId / 安装路径片段 / 日志正文(--in-logs)
 */
import fs from 'node:fs/promises';
import { existsSync, createReadStream } from 'node:fs';
import path from 'node:path';
import { createInterface } from 'node:readline';
import { fileURLToPath } from 'node:url';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';

const execFileAsync = promisify(execFile);
const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const defaultOutRoot = path.join(repoRoot, 'user_log_uploads');

/** 设备桶目录名：内嵌一层，让 outRoot 根上只剩 catalog.jsonl + devices/ + _sessions/。 */
export const DEVICES_SUBDIR = 'devices';
/** outRoot 根下保留、不迁入 devices/ 的目录。 */
export const RESERVED_OUT_DIRS = new Set([DEVICES_SUBDIR, '_sessions']);

export function devicesRoot(outRoot = defaultOutRoot) {
  return path.join(outRoot, DEVICES_SUBDIR);
}

/**
 * 把 outRoot 根下遗留的设备目录迁入 devices/（幂等）。
 * 冲突时把源目录条目并入目标（已存在同名则跳过）。
 */
export async function migrateFlatDeviceDirs(outRoot = defaultOutRoot) {
  if (!existsSync(outRoot)) return { moved: 0, merged: 0 };
  const bucket = devicesRoot(outRoot);
  await fs.mkdir(bucket, { recursive: true });
  const ents = await fs.readdir(outRoot, { withFileTypes: true });
  let moved = 0;
  let merged = 0;
  for (const ent of ents) {
    if (!ent.isDirectory()) continue;
    if (RESERVED_OUT_DIRS.has(ent.name)) continue;
    const src = path.join(outRoot, ent.name);
    const dst = path.join(bucket, ent.name);
    if (!existsSync(dst)) {
      await fs.rename(src, dst);
      moved += 1;
      continue;
    }
    const children = await fs.readdir(src, { withFileTypes: true });
    for (const child of children) {
      const from = path.join(src, child.name);
      const to = path.join(dst, child.name);
      if (existsSync(to)) continue;
      await fs.rename(from, to);
    }
    try {
      await fs.rm(src, { recursive: true, force: true });
    } catch {
      // 仍有残留则下次再扫
    }
    merged += 1;
  }
  return { moved, merged };
}

function parseArgs(argv) {
  const positional = [];
  const flags = new Map();
  for (let i = 0; i < argv.length; i += 1) {
    const a = argv[i];
    if (!a.startsWith('--')) {
      positional.push(a);
      continue;
    }
    const key = a.slice(2);
    const next = argv[i + 1];
    if (next && !next.startsWith('--')) {
      flags.set(key, next);
      i += 1;
    } else {
      flags.set(key, '1');
    }
  }
  return { positional, flags };
}

async function readJson(filePath) {
  return JSON.parse(await fs.readFile(filePath, 'utf8'));
}

async function safeStat(filePath) {
  try {
    return await fs.stat(filePath);
  } catch {
    return null;
  }
}

/** 从 launcher.jsonl / 旧 launcher.log 抽一点人话线索（安装路径 / Windows 用户名） */
async function extractHints(uploadDir) {
  const hints = { installPath: '', winUser: '', releaseFolder: '' };
  const candidates = ['launcher.jsonl', 'launcher.log', 'prev_launcher.jsonl', 'prev_launcher.log'];
  for (const name of candidates) {
    const launcher = path.join(uploadDir, name);
    if (!existsSync(launcher)) continue;
    try {
      const text = await fs.readFile(launcher, 'utf8');
      const pathMatch = text.match(/[A-Za-z]:\\Users\\[^\\\r\n]+\\[^\r\n]*?xcat[^\r\n]*?/i);
      if (pathMatch) {
        hints.installPath = pathMatch[0].slice(0, 240);
        const userMatch = hints.installPath.match(/Users\\([^\\]+)/i);
        if (userMatch) hints.winUser = userMatch[1];
        const folderMatch = hints.installPath.match(
          /\\([^\\]*(?:xcat_for_twms|xcat_for_fengxing|xcat)[^\\]*)\\/i,
        );
        if (folderMatch) hints.releaseFolder = folderMatch[1];
        return hints;
      }
    } catch {
      // ignore
    }
  }
  return hints;
}

async function listUploadDirs(deviceDir) {
  const entries = await fs.readdir(deviceDir, { withFileTypes: true });
  return entries
    .filter((e) => e.isDirectory())
    .map((e) => e.name)
    .filter((name) => /^\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2}_/.test(name))
    .sort()
    .reverse();
}

/**
 * 扫描全部上传，返回按 receivedAt 降序的记录。
 * @returns {Promise<Array<object>>}
 */
export async function scanUploads(outRoot = defaultOutRoot) {
  if (!existsSync(outRoot)) return [];
  await migrateFlatDeviceDirs(outRoot);
  const bucket = devicesRoot(outRoot);
  if (!existsSync(bucket)) return [];
  const devices = (await fs.readdir(bucket, { withFileTypes: true }))
    .filter((e) => e.isDirectory())
    .map((e) => e.name);

  const rows = [];
  for (const device of devices) {
    const deviceDir = path.join(bucket, device);
    const uploads = await listUploadDirs(deviceDir);
    let latest = '';
    try {
      latest = (await fs.readFile(path.join(deviceDir, 'latest.txt'), 'utf8')).trim();
    } catch {
      latest = uploads[0] || '';
    }

    for (const uploadId of uploads) {
      const uploadDir = path.join(deviceDir, uploadId);
      const metaPath = path.join(uploadDir, 'meta.json');
      let meta = null;
      if (existsSync(metaPath)) {
        try {
          meta = await readJson(metaPath);
        } catch {
          meta = null;
        }
      }
      const st = await safeStat(uploadDir);
      const hints = await extractHints(uploadDir);
      const receivedAt = meta?.receivedAt || (st ? formatLocal(st.mtime) : '');
      let note = '';
      if (meta && Object.prototype.hasOwnProperty.call(meta, 'note')) {
        note = typeof meta.note === 'string' ? meta.note : '';
      } else {
        // 旧 meta 无 note：回退读 note.txt（服务端双写，便于只更 catalog 脚本时也能扫到）
        const notePath = path.join(uploadDir, 'note.txt');
        if (existsSync(notePath)) {
          try {
            note = (await fs.readFile(notePath, 'utf8')).replace(/\r?\n$/, '').trim();
          } catch {
            note = '';
          }
        }
      }
      rows.push({
        device,
        uploadId,
        path: uploadDir,
        relPath: path.relative(repoRoot, uploadDir).replace(/\\/g, '/'),
        isLatest: uploadId === latest,
        receivedAt,
        note,
        remoteAddress: meta?.remoteAddress || '',
        machine: meta?.machine || device.split('_')[0] || '',
        deviceId: meta?.deviceId || '',
        clientId: meta?.clientId || '',
        appVersion: meta?.appVersion || '',
        uploadMode: meta?.uploadMode || '',
        profile: meta?.profile || '',
        fileCount: Array.isArray(meta?.saved) ? meta.saved.length : 0,
        hasLieEvents: Boolean(meta?.lieEvents?.ok),
        hasFreeze: existsSync(path.join(uploadDir, 'freeze_incident.log'))
          || existsSync(path.join(uploadDir, 'freeze_incident.jsonl')),
        ...hints,
      });
    }
  }

  rows.sort((a, b) => String(b.receivedAt).localeCompare(String(a.receivedAt)));
  return rows;
}

function formatLocal(d) {
  const pad = (n) => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
}

function matchRow(row, query) {
  const q = String(query || '').trim().toLowerCase();
  if (!q) return true;
  const hay = [
    row.note,
    row.device,
    row.uploadId,
    row.machine,
    row.deviceId,
    row.clientId,
    row.remoteAddress,
    row.appVersion,
    row.uploadMode,
    row.installPath,
    row.winUser,
    row.releaseFolder,
    row.relPath,
  ]
    .filter(Boolean)
    .join('\n')
    .toLowerCase();
  return hay.includes(q);
}

async function grepUploadLogs(uploadDir, query, { maxHits = 3 } = {}) {
  const q = String(query || '').trim().toLowerCase();
  if (!q) return [];
  const candidates = [
    'x.jsonl',
    'launcher.jsonl',
    'freeze_incident.jsonl',
    'prev_x.jsonl',
    'prev_launcher.jsonl',
    'x.log',
    'launcher.log',
    'freeze_incident.log',
    'prev_x.log',
    'prev_launcher.log',
    'update_apply.log',
  ];
  const hits = [];
  for (const name of candidates) {
    const filePath = path.join(uploadDir, name);
    if (!existsSync(filePath)) continue;
    const rl = createInterface({ input: createReadStream(filePath, { encoding: 'utf8' }), crlfDelay: Infinity });
    let lineNo = 0;
    for await (const line of rl) {
      lineNo += 1;
      if (line.toLowerCase().includes(q)) {
        hits.push({ file: name, line: lineNo, text: line.slice(0, 220) });
        if (hits.length >= maxHits) {
          rl.close();
          return hits;
        }
      }
    }
  }
  return hits;
}

export async function writeCatalog(outRoot = defaultOutRoot) {
  const rows = await scanUploads(outRoot);
  await fs.mkdir(outRoot, { recursive: true });

  const catalogJsonl = path.join(outRoot, 'catalog.jsonl');
  const legacyCatalogMd = path.join(outRoot, 'CATALOG.md');

  const jsonl = rows.map((r) => JSON.stringify({
    receivedAt: r.receivedAt,
    note: r.note || '',
    device: r.device,
    uploadId: r.uploadId,
    machine: r.machine,
    deviceId: r.deviceId,
    remoteAddress: r.remoteAddress,
    appVersion: r.appVersion,
    uploadMode: r.uploadMode || '',
    winUser: r.winUser,
    releaseFolder: r.releaseFolder,
    isLatest: r.isLatest,
    hasFreeze: r.hasFreeze,
    hasLieEvents: r.hasLieEvents,
    relPath: r.relPath,
  })).join('\n') + (rows.length ? '\n' : '');
  await fs.writeFile(catalogJsonl, jsonl, 'utf8');

  // 旧版曾双写 CATALOG.md；现只保留 jsonl，顺手清掉残留 md。
  try {
    await fs.unlink(legacyCatalogMd);
  } catch (err) {
    if (err && err.code !== 'ENOENT') throw err;
  }

  return { rows, catalogJsonl };
}

function printTable(rows) {
  if (!rows.length) {
    console.log('(无匹配)');
    return;
  }
  const pad = (s, n) => String(s ?? '').slice(0, n).padEnd(n);
  console.log(
    `${pad('receivedAt', 19)}  ${pad('machine', 15)}  ${pad('device', 26)}  ${pad('ip', 15)}  ${pad('ver', 18)}  clue`,
  );
  console.log('-'.repeat(120));
  for (const r of rows) {
    const clue = [
      r.note && `note=${r.note}`,
      r.uploadMode && `mode=${r.uploadMode}`,
      r.winUser && `user=${r.winUser}`,
      r.releaseFolder,
      r.hasFreeze && 'freeze',
      r.isLatest && 'latest',
    ]
      .filter(Boolean)
      .join(' · ');
    console.log(
      `${pad(r.receivedAt, 19)}  ${pad(r.machine, 15)}  ${pad(r.device, 26)}  ${pad(r.remoteAddress, 15)}  ${pad(r.appVersion, 18)}  ${clue}`,
    );
    console.log(`  → ${r.relPath}`);
    if (r.logHits?.length) {
      for (const h of r.logHits) {
        console.log(`     [${h.file}:${h.line}] ${h.text}`);
      }
    }
  }
}

async function openPath(target) {
  if (process.platform === 'win32') {
    await execFileAsync('cmd', ['/c', 'start', '', target], { windowsHide: true });
    return;
  }
  await execFileAsync(process.platform === 'darwin' ? 'open' : 'xdg-open', [target]);
}

async function main() {
  const { positional, flags } = parseArgs(process.argv.slice(2));
  const outRoot = path.resolve(flags.get('out') || defaultOutRoot);
  const cmd = (positional[0] || 'list').toLowerCase();
  const limit = Number(flags.get('limit') || 30);

  if (cmd === 'catalog' || cmd === 'rebuild') {
    const { rows, catalogJsonl } = await writeCatalog(outRoot);
    console.log(`[find-user-logs] catalog: ${rows.length} uploads -> ${path.relative(repoRoot, catalogJsonl)}`);
    return;
  }

  if (cmd === 'list' || cmd === 'ls') {
    const rows = (await scanUploads(outRoot)).slice(0, limit);
    printTable(rows);
    console.log(`\n共显示 ${rows.length} 条。完整索引：node scripts/find-user-logs.mjs catalog`);
    return;
  }

  if (cmd === 'search' || cmd === 'find' || cmd === 'open') {
    const query = positional.slice(1).join(' ').trim() || positional[1] || '';
    if (!query) {
      console.error('用法: node scripts/find-user-logs.mjs search <关键字> [--in-logs]');
      process.exit(2);
    }
    let rows = (await scanUploads(outRoot)).filter((r) => matchRow(r, query));
    if (flags.has('in-logs')) {
      const enriched = [];
      for (const r of await scanUploads(outRoot)) {
        if (matchRow(r, query)) {
          enriched.push(r);
          continue;
        }
        const logHits = await grepUploadLogs(r.path, query, { maxHits: 2 });
        if (logHits.length) enriched.push({ ...r, logHits });
      }
      rows = enriched;
    }
    rows = rows.slice(0, limit);
    printTable(rows);
    if (cmd === 'open') {
      if (!rows.length) {
        console.error('无匹配，未打开');
        process.exit(1);
      }
      await openPath(rows[0].path);
      console.log(`已打开: ${rows[0].path}`);
    }
    return;
  }

  console.error(`未知命令: ${cmd}`);
  console.error('支持: list | search | catalog | open');
  process.exit(2);
}

const isDirect = process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url);
if (isDirect) {
  main().catch((err) => {
    console.error(err);
    process.exit(1);
  });
}
