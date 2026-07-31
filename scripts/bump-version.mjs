#!/usr/bin/env node
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const repo = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const versionRc = join(repo, "common", "xcat_version_rc.h");

let text = readFileSync(versionRc, "utf8");

const majorMatch = /#define\s+XCAT_VER_MAJOR\s+(\d+)/.exec(text);
const minorMatch = /#define\s+XCAT_VER_MINOR\s+(\d+)/.exec(text);
const patchMatch = /#define\s+XCAT_VER_PATCH\s+(\d+)/.exec(text);
const buildMatch = /#define\s+XCAT_VER_BUILD\s+(\d+)/.exec(text);

if (!majorMatch || !minorMatch || !patchMatch || !buildMatch) {
  console.error("[bump-version] failed to parse common/xcat_version_rc.h");
  process.exit(2);
}

const major = Number(majorMatch[1]);
const minor = Number(minorMatch[1]);
const oldPatch = Number(patchMatch[1]);
const oldBuildId = Number(buildMatch[1]);

if (oldPatch !== oldBuildId) {
  console.warn(
    `[bump-version] WARN: patch(${oldPatch}) != buildId(${oldBuildId}); aligning to buildId`,
  );
}

const buildId = oldBuildId + 1;
const patch = buildId;
const version = `${major}.${minor}.${patch}`;

text = text.replace(/#define\s+XCAT_VER_PATCH\s+\d+/, `#define XCAT_VER_PATCH ${patch}`);
text = text.replace(/#define\s+XCAT_VER_BUILD\s+\d+/, `#define XCAT_VER_BUILD ${buildId}`);
text = text.replace(
  /#define\s+XCAT_VERSION_STRING\s+"[^"]+"/,
  `#define XCAT_VERSION_STRING "${version}"`,
);

writeFileSync(versionRc, text, "utf8");

console.log(`[bump-version] ${version} buildId=${buildId} → xcat_version_rc.h`);
