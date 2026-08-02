#!/usr/bin/env python3
"""经典版 TWMS · 单文件 NGService（ProgramData）补丁加载器。

门禁：ngs_fingerprint baseline 必须 OK；只改
  C:\\ProgramData\\Nexon\\NGS\\NGService.exe
游戏树 grap\\NGService.exe 默认不动（避开 ClientFileCRC）。

补丁表：bin/XCat_data/state/ngs_patch.tsv
  rva_hex \\t before_hex \\t after_hex \\t note
  - 偏移按 *文件 raw 偏移* 解释（可用 raw=0x..）；等长 before/after hex

用法：
  python scripts/ngs_patch_apply.py --dry-run
  python scripts/ngs_patch_apply.py --apply
  python scripts/ngs_patch_apply.py --restore
  python scripts/ngs_patch_apply.py --probe-notes
"""
from __future__ import annotations

import argparse
import hashlib
import shutil
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STATE = ROOT / "bin" / "XCat_data" / "state"
BASELINE = STATE / "ngs_fingerprint.tsv"
OFFICIAL = STATE / "ngs_official_crc.tsv"
PATCH_TSV = STATE / "ngs_patch.tsv"
BACKUP_DIR = STATE / "ngs_backup"
TARGET = Path(r"C:\ProgramData\Nexon\NGS\NGService.exe")
BACKUP_FILE = BACKUP_DIR / "NGService.exe.official"

PATCH_HEADER = "rva_hex\tbefore_hex\tafter_hex\tnote\n"


def ensure_patch_tsv() -> None:
    STATE.mkdir(parents=True, exist_ok=True)
    if not PATCH_TSV.is_file():
        PATCH_TSV.write_text(
            PATCH_HEADER
            + "# empty = no-op. Fill after RE. Example:\n"
            + "# raw=0x1234\t90\t90\tnop placeholder\n",
            encoding="utf-8",
        )


def parse_patches(text: str) -> list[tuple[int, bytes, bytes, str]]:
    rows: list[tuple[int, bytes, bytes, str]] = []
    for i, line in enumerate(text.splitlines()):
        s = line.strip()
        if not s or s.startswith("#") or (i == 0 and s.startswith("rva_hex")):
            continue
        parts = s.split("\t")
        if len(parts) < 3:
            raise ValueError(f"bad patch line: {line!r}")
        off_s, before_s, after_s = parts[0], parts[1], parts[2]
        note = parts[3] if len(parts) > 3 else ""
        if off_s.lower().startswith("raw="):
            off_s = off_s.split("=", 1)[1]
        off = int(off_s, 16)
        before = bytes.fromhex(before_s.replace(" ", ""))
        after = bytes.fromhex(after_s.replace(" ", ""))
        if not before or len(before) != len(after):
            raise ValueError(f"before/after length mismatch at {off:#x}")
        rows.append((off, before, after, note))
    return rows


def read_programdata_slot_from_baseline() -> dict[str, str] | None:
    if not BASELINE.is_file():
        return None
    for i, line in enumerate(BASELINE.read_text(encoding="utf-8").splitlines()):
        if i == 0 and line.startswith("slot\t"):
            continue
        parts = line.split("\t")
        if len(parts) < 9:
            continue
        if parts[0] == "programdata" and parts[2] != "MISSING":
            return {
                "path": parts[1],
                "size": parts[2],
                "crc32": parts[6],
                "md5": parts[7],
                "sha256": parts[8],
            }
    return None


def fingerprint_ok(data: bytes) -> tuple[bool, str]:
    base = read_programdata_slot_from_baseline()
    if not base:
        return False, "missing ngs_fingerprint.tsv programdata row (run ngs_fingerprint.py --write)"
    if not OFFICIAL.is_file():
        return False, "missing ngs_official_crc.tsv (run ngs_fingerprint.py --freeze)"
    sha = hashlib.sha256(data).hexdigest()
    md5 = hashlib.md5(data).hexdigest()
    crc = f"{zlib.crc32(data) & 0xFFFFFFFF:08X}"
    if sha != base["sha256"] or md5 != base["md5"] or crc != base["crc32"]:
        return False, (
            f"target hash != baseline fingerprint "
            f"(file crc={crc} baseline={base['crc32']}; restore or re-freeze)"
        )
    for i, line in enumerate(OFFICIAL.read_text(encoding="utf-8").splitlines()):
        if i == 0 or not line.strip():
            continue
        parts = line.split("\t")
        if len(parts) >= 8 and parts[5] != crc:
            return False, f"official crc row mismatch file={crc} official={parts[5]}"
        break
    return True, "ok"


def apply_patches(
    data: bytearray, patches: list[tuple[int, bytes, bytes, str]], dry: bool
) -> list[str]:
    logs: list[str] = []
    for off, before, after, note in patches:
        end = off + len(before)
        if end > len(data):
            raise RuntimeError(f"offset {off:#x} past EOF size={len(data)}")
        cur = bytes(data[off:end])
        if cur != before:
            raise RuntimeError(
                f"before mismatch at {off:#x}: want {before.hex()} have {cur.hex()} ({note})"
            )
        logs.append(
            f"{'DRY ' if dry else ''}patch {off:#x} {before.hex()} -> {after.hex()} {note}"
        )
        if not dry:
            data[off:end] = after
    return logs


def cmd_restore() -> int:
    if not BACKUP_FILE.is_file():
        print("no backup:", BACKUP_FILE, file=sys.stderr)
        return 2
    shutil.copy2(BACKUP_FILE, TARGET)
    print("restored", TARGET, "from", BACKUP_FILE)
    return 0


def cmd_probe_notes() -> int:
    print(
        """
GA probe notes (manual; NOT RUN by this script):
  1) Ensure ProgramData NGService is patched (non-empty ngs_patch.tsv + --apply).
  2) Start classic client; wait past login ClientFileCRC (game-tree NGService still clean).
  3) From payload / debugger, write a single reversible byte or field in GameAssembly
     (.text page or known data); observe kill / MemoryCrc / logout within ~minutes.
  4) If process dies -> escalate grap-core MemoryCrc (docs/features/security/NGS补丁与CRC.md).
  5) Always --restore NGService after the session if testing.
""".strip()
    )
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="NGService ProgramData patch apply")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--dry-run", action="store_true")
    g.add_argument("--apply", action="store_true")
    g.add_argument("--restore", action="store_true")
    g.add_argument("--probe-notes", action="store_true")
    args = ap.parse_args()

    if args.probe_notes:
        return cmd_probe_notes()
    if args.restore:
        return cmd_restore()

    ensure_patch_tsv()
    if not TARGET.is_file():
        print("missing target", TARGET, file=sys.stderr)
        return 2

    patches = parse_patches(PATCH_TSV.read_text(encoding="utf-8"))
    if not patches:
        print("ngs_patch.tsv has no patch rows — refuse write (fill after RE)")
        print(f"  table -> {PATCH_TSV}")
        return 0

    data = bytearray(TARGET.read_bytes())
    ok, reason = fingerprint_ok(bytes(data))
    if not ok:
        print("fingerprint gate FAIL:", reason, file=sys.stderr)
        return 1
    print("fingerprint gate OK")

    logs = apply_patches(data, patches, dry=args.dry_run)
    for line in logs:
        print(" ", line)

    if args.dry_run:
        print("dry-run done; no file written")
        return 0

    BACKUP_DIR.mkdir(parents=True, exist_ok=True)
    if not BACKUP_FILE.is_file():
        shutil.copy2(TARGET, BACKUP_FILE)
        print("backup ->", BACKUP_FILE)
    TARGET.write_bytes(data)
    print("applied", len(patches), "patch(es) ->", TARGET)
    print("sha256_now", hashlib.sha256(data).hexdigest())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
