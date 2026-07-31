#!/usr/bin/env node
// 打包 xcat_for_twms 发布目录与 zip（范围 A：启动器 + 空状态目录 + 换包钩子）。
//
// 用法：
//   node scripts/package-release.mjs [--name xcat_for_twms_MMdd_版本号_build号]

import { cpSync, existsSync, mkdirSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { basename, dirname, join, resolve } from "node:path";
import { execFileSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { createHash } from "node:crypto";

const repo = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const productName = "xcat_for_twms";

function die(msg) {
  console.error(`[package-release] ${msg}`);
  process.exit(2);
}

function parseArgs(argv) {
  const args = {};
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === "--name") args.name = argv[++i];
    else die(`未知参数：${a}`);
  }
  return args;
}

function stampName(versionInfo) {
  const d = new Date();
  const z = (n) => String(n).padStart(2, "0");
  const stamp = `${z(d.getMonth() + 1)}${z(d.getDate())}`;
  return `${productName}_${stamp}_${versionInfo.version}_build${versionInfo.buildId}`;
}

function readVersionInfo() {
  const text = readFileSync(join(repo, "common", "xcat_version_rc.h"), "utf8");
  const version = /#define\s+XCAT_VERSION_STRING\s+"([^"]+)"/.exec(text)?.[1] ?? "0.0.0";
  const buildId = Number(/#define\s+XCAT_VER_BUILD\s+(\d+)/.exec(text)?.[1] ?? "0");
  const patch = Number((/^(\d+)\.(\d+)\.(\d+)$/.exec(version) || [])[3] || NaN);
  if (!Number.isFinite(patch) || patch !== buildId) {
    die(`version patch must equal buildId (got version=${version} buildId=${buildId})`);
  }
  return { version, buildId };
}

function sha256File(file) {
  return createHash("sha256").update(readFileSync(file)).digest("hex");
}

function copyRequired(srcRel, dstRel) {
  const src = join(repo, srcRel);
  if (!existsSync(src)) die(`缺少必需文件：${srcRel}`);
  const dst = join(outDir, dstRel);
  mkdirSync(dirname(dst), { recursive: true });
  cpSync(src, dst);
}

const args = parseArgs(process.argv.slice(2));
const versionInfo = readVersionInfo();
const name = args.name || stampName(versionInfo);
const releaseDir = join(repo, "artifacts", "release");
const outDir = join(releaseDir, name);
const zipPath = join(releaseDir, `${name}.zip`);

mkdirSync(releaseDir, { recursive: true });
if (existsSync(outDir)) rmSync(outDir, { recursive: true, force: true });
if (existsSync(zipPath)) rmSync(zipPath, { force: true });
mkdirSync(outDir, { recursive: true });

const exeSrc = join(repo, "bin", "xcat.exe");
if (!existsSync(exeSrc)) die("缺少 bin/xcat.exe，请先构建 Release");
cpSync(exeSrc, join(outDir, "xcat.exe"));

mkdirSync(join(outDir, "logs"), { recursive: true });
mkdirSync(join(outDir, "XCat_data", "state"), { recursive: true });
mkdirSync(join(outDir, "XCat_data", "update"), { recursive: true });
copyRequired("packaging/update/pre_apply.ps1", "XCat_data/update/pre_apply.ps1");
copyRequired("packaging/update/post_apply.ps1", "XCat_data/update/post_apply.ps1");

writeFileSync(
  join(outDir, "README.txt"),
  `# xcat_for_twms

## 使用
1. 解压到任意目录
2. 双击运行 xcat.exe
3. 本机更新检查默认：http://127.0.0.1:18789/twms/update/latest.json
   （网页下载站仍可用：http://127.0.0.1:52080/）

构建时间：${new Date().toISOString().replace("T", " ").slice(0, 19)}
`,
  "utf8",
);

execFileSync(
  "powershell.exe",
  [
    "-NoProfile",
    "-ExecutionPolicy",
    "Bypass",
    "-Command",
    `Add-Type -AssemblyName System.IO.Compression.FileSystem; ` +
      `[System.IO.Compression.ZipFile]::CreateFromDirectory('${outDir.replace(/'/g, "''")}', '${zipPath.replace(/'/g, "''")}', [System.IO.Compression.CompressionLevel]::Optimal, $true)`,
  ],
  { stdio: "inherit" },
);

const zipName = basename(zipPath);
const manifest = {
  version: versionInfo.version,
  buildId: versionInfo.buildId,
  name,
  zipName,
  downloadUrl: zipName,
  sha256: sha256File(zipPath),
  size: statSync(zipPath).size,
  notes: `${productName} ${name}`,
  createdAt: new Date().toISOString(),
};
writeFileSync(join(releaseDir, "latest.json"), `${JSON.stringify(manifest, null, 2)}\n`, "utf8");

console.log(`[package-release] wrote ${outDir}`);
console.log(`[package-release] wrote ${zipPath}`);
console.log(`[package-release] wrote ${join(releaseDir, "latest.json")}`);
