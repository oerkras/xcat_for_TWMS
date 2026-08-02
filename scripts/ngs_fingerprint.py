#!/usr/bin/env python3
"""经典版 TWMS · NGS（NGService.exe）版本指纹检测。

产品 = 经典版；NGS = Nexon Game Security 服务二进制（非枫星）。
同行只有一份 NGS.EXE.CRC；本仓 --freeze 写入 state/ngs_official_crc.tsv + state/NGS.EXE.CRC。

候选路径（存在则采）：
  1) C:\\ProgramData\\Nexon\\NGS\\NGService.exe          （服务 ImagePath）
  2) <game>\\Maplestory_Classic_Data\\Plugins\\x86_64\\grap\\NGService.exe
     game 默认读环境变量 XCAT_TWMS_GAME_ROOT，否则试 G:\\Games\\maplestory_classic

用法：
  python scripts/ngs_fingerprint.py              # 扫并与 baseline 比对；无 baseline 则写入
  python scripts/ngs_fingerprint.py --write      # 强制把当前结果写成 baseline
  python scripts/ngs_fingerprint.py --freeze     # 写单行 ngs_official_crc.tsv + NGS.EXE.CRC
  python scripts/ngs_fingerprint.py --write --freeze
  python scripts/ngs_fingerprint.py --json       # JSON 打到 stdout

退出码：
  0  与 baseline 一致，或首次写入 / --write / --freeze 成功
  1  相对 baseline 有变化（路径缺失、hash/版本/大小变）
  2  本机找不到任何 NGService.exe
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
import time
import zlib
from dataclasses import asdict, dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STATE_DIR = ROOT / "bin" / "XCat_data" / "state"
BASELINE = STATE_DIR / "ngs_fingerprint.tsv"
STATUS = STATE_DIR / "ngs_status.txt"
OFFICIAL_CRC = STATE_DIR / "ngs_official_crc.tsv"
SIDECAR_CRC = STATE_DIR / "NGS.EXE.CRC"

PROGRAMDATA_NGS = Path(r"C:\ProgramData\Nexon\NGS\NGService.exe")

TSV_HEADER = (
    "slot\tpath\tsize\tmtime_utc\tfile_version\tproduct_version"
    "\tcrc32\tmd5\tsha256\n"
)
OFFICIAL_HEADER = "rel_path\tsource_slot\tpath\tsize\tfile_version\tcrc32\tmd5\tsha256\n"


@dataclass(frozen=True)
class Fingerprint:
    slot: str
    path: str
    size: int
    mtime_utc: str
    file_version: str
    product_version: str
    crc32: str
    md5: str
    sha256: str
    present: bool = True

    def identity_key(self) -> tuple:
        """版本判定主键：内容 hash + 资源版本（忽略 mtime/path 文本漂移）。"""
        return (self.sha256, self.md5, self.crc32, self.size, self.file_version, self.product_version)


def _game_ngs_candidates() -> list[Path]:
    roots: list[Path] = []
    env = os.environ.get("XCAT_TWMS_GAME_ROOT", "").strip()
    if env:
        roots.append(Path(env))
    roots.append(Path(r"G:\Games\maplestory_classic"))
    out: list[Path] = []
    seen: set[str] = set()
    for root in roots:
        p = root / "Maplestory_Classic_Data" / "Plugins" / "x86_64" / "grap" / "NGService.exe"
        key = str(p).lower()
        if key in seen:
            continue
        seen.add(key)
        out.append(p)
    return out


def _pe_file_versions(path: Path) -> tuple[str, str]:
    """ctypes + version.dll；失败返回空串。"""
    try:
        import ctypes
        from ctypes import wintypes
    except Exception:
        return "", ""

    ver = ctypes.WinDLL("version")
    get_size = ver.GetFileVersionInfoSizeW
    get_size.argtypes = [wintypes.LPCWSTR, wintypes.LPDWORD]
    get_size.restype = wintypes.DWORD
    get_info = ver.GetFileVersionInfoW
    get_info.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID]
    get_info.restype = wintypes.BOOL
    ver_query = ver.VerQueryValueW
    ver_query.argtypes = [
        wintypes.LPCVOID,
        wintypes.LPCWSTR,
        ctypes.POINTER(ctypes.c_void_p),
        ctypes.POINTER(wintypes.UINT),
    ]
    ver_query.restype = wintypes.BOOL

    dummy = wintypes.DWORD(0)
    size = get_size(str(path), ctypes.byref(dummy))
    if not size:
        return "", ""
    buf = (ctypes.c_char * size)()
    if not get_info(str(path), 0, size, buf):
        return "", ""

    class VS_FIXEDFILEINFO(ctypes.Structure):
        _fields_ = [
            ("dwSignature", wintypes.DWORD),
            ("dwStrucVersion", wintypes.DWORD),
            ("dwFileVersionMS", wintypes.DWORD),
            ("dwFileVersionLS", wintypes.DWORD),
            ("dwProductVersionMS", wintypes.DWORD),
            ("dwProductVersionLS", wintypes.DWORD),
            ("dwFileFlagsMask", wintypes.DWORD),
            ("dwFileFlags", wintypes.DWORD),
            ("dwFileOS", wintypes.DWORD),
            ("dwFileType", wintypes.DWORD),
            ("dwFileSubtype", wintypes.DWORD),
            ("dwFileDateMS", wintypes.DWORD),
            ("dwFileDateLS", wintypes.DWORD),
        ]

    def dotted(ms: int, ls: int) -> str:
        return f"{(ms >> 16) & 0xFFFF}.{ms & 0xFFFF}.{(ls >> 16) & 0xFFFF}.{ls & 0xFFFF}"

    pblock = ctypes.c_void_p()
    plen = wintypes.UINT(0)
    if not ver_query(buf, r"\\", ctypes.byref(pblock), ctypes.byref(plen)):
        return "", ""
    info = ctypes.cast(pblock, ctypes.POINTER(VS_FIXEDFILEINFO)).contents
    return (
        dotted(info.dwFileVersionMS, info.dwFileVersionLS),
        dotted(info.dwProductVersionMS, info.dwProductVersionLS),
    )


def fingerprint(slot: str, path: Path) -> Fingerprint:
    if not path.is_file():
        return Fingerprint(
            slot=slot,
            path=str(path),
            size=0,
            mtime_utc="",
            file_version="",
            product_version="",
            crc32="",
            md5="",
            sha256="",
            present=False,
        )
    data = path.read_bytes()
    st = path.stat()
    mtime = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(st.st_mtime))
    fv, pv = _pe_file_versions(path)
    return Fingerprint(
        slot=slot,
        path=str(path),
        size=len(data),
        mtime_utc=mtime,
        file_version=fv,
        product_version=pv,
        crc32=f"{zlib.crc32(data) & 0xFFFFFFFF:08X}",
        md5=hashlib.md5(data).hexdigest(),
        sha256=hashlib.sha256(data).hexdigest(),
        present=True,
    )


def collect() -> list[Fingerprint]:
    rows = [fingerprint("programdata", PROGRAMDATA_NGS)]
    for i, p in enumerate(_game_ngs_candidates()):
        slot = "game_grap" if i == 0 else f"game_grap_{i}"
        rows.append(fingerprint(slot, p))
    return rows


def to_tsv(rows: list[Fingerprint]) -> str:
    lines = [TSV_HEADER]
    for r in rows:
        if not r.present:
            lines.append(f"{r.slot}\t{r.path}\tMISSING\t\t\t\t\t\t\n")
            continue
        lines.append(
            f"{r.slot}\t{r.path}\t{r.size}\t{r.mtime_utc}\t{r.file_version}\t"
            f"{r.product_version}\t{r.crc32}\t{r.md5}\t{r.sha256}\n"
        )
    return "".join(lines)


def parse_tsv(text: str) -> dict[str, Fingerprint]:
    out: dict[str, Fingerprint] = {}
    for i, line in enumerate(text.splitlines()):
        if i == 0 and line.startswith("slot\t"):
            continue
        if not line.strip():
            continue
        parts = line.split("\t")
        if len(parts) < 9:
            continue
        slot, path, size_s = parts[0], parts[1], parts[2]
        if size_s == "MISSING":
            out[slot] = Fingerprint(
                slot=slot,
                path=path,
                size=0,
                mtime_utc="",
                file_version="",
                product_version="",
                crc32="",
                md5="",
                sha256="",
                present=False,
            )
            continue
        out[slot] = Fingerprint(
            slot=slot,
            path=path,
            size=int(size_s),
            mtime_utc=parts[3],
            file_version=parts[4],
            product_version=parts[5],
            crc32=parts[6],
            md5=parts[7],
            sha256=parts[8],
            present=True,
        )
    return out


def diff(current: list[Fingerprint], baseline: dict[str, Fingerprint]) -> list[str]:
    msgs: list[str] = []
    cur_map = {r.slot: r for r in current}
    for slot, old in baseline.items():
        new = cur_map.get(slot)
        if new is None:
            msgs.append(f"{slot}: baseline 有、当前扫描无此 slot")
            continue
        if old.present != new.present:
            msgs.append(
                f"{slot}: present {old.present} -> {new.present} path={new.path}"
            )
            continue
        if not new.present:
            continue
        if old.identity_key() != new.identity_key():
            msgs.append(
                f"{slot}: CHANGED file_version={old.file_version}->{new.file_version} "
                f"sha256={old.sha256[:12]}..->{new.sha256[:12]}.. size={old.size}->{new.size}"
            )
        elif old.path != new.path:
            msgs.append(f"{slot}: path moved {old.path} -> {new.path} (content same)")
    for slot, new in cur_map.items():
        if slot not in baseline:
            msgs.append(f"{slot}: NEW path={new.path} present={new.present}")
    return msgs


def write_status(rows: list[Fingerprint], changes: list[str], mode: str) -> None:
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    present = [r for r in rows if r.present]
    primary = next((r for r in present if r.slot == "programdata"), None)
    if primary is None and present:
        primary = present[0]
    lines = [
        f"mode={mode}",
        f"checked_utc={time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}",
        f"baseline={BASELINE}",
        f"present_count={len(present)}/{len(rows)}",
    ]
    if primary:
        lines += [
            f"primary_slot={primary.slot}",
            f"primary_path={primary.path}",
            f"file_version={primary.file_version}",
            f"product_version={primary.product_version}",
            f"size={primary.size}",
            f"crc32={primary.crc32}",
            f"md5={primary.md5}",
            f"sha256={primary.sha256}",
        ]
    else:
        lines.append("primary_slot=")
    if changes:
        lines.append("status=CHANGED")
        lines.append(f"change_count={len(changes)}")
        for c in changes:
            lines.append(f"change={c}")
    else:
        lines.append("status=OK")
        lines.append("change_count=0")

    # same-bytes check between programdata and first game copy
    pd = next((r for r in rows if r.slot == "programdata" and r.present), None)
    gg = next((r for r in rows if r.slot.startswith("game_grap") and r.present), None)
    if pd and gg:
        lines.append(f"programdata_eq_game={'1' if pd.sha256 == gg.sha256 else '0'}")
    STATUS.write_text("\n".join(lines) + "\n", encoding="utf-8")


def freeze_official(primary: Fingerprint) -> None:
    """Single-file official CRC anchor (peer: only NGS.EXE.CRC)."""
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    line = (
        f"NGService.exe\t{primary.slot}\t{primary.path}\t{primary.size}\t"
        f"{primary.file_version}\t{primary.crc32}\t{primary.md5}\t{primary.sha256}\n"
    )
    OFFICIAL_CRC.write_text(OFFICIAL_HEADER + line, encoding="utf-8")
    crc_u32 = int(primary.crc32, 16)
    SIDECAR_CRC.write_bytes(struct.pack("<I", crc_u32))
    print(f"NGS: official CRC frozen -> {OFFICIAL_CRC}")
    print(f"NGS: sidecar -> {SIDECAR_CRC} ({primary.crc32} LE)")


def main() -> int:
    ap = argparse.ArgumentParser(description="NGS NGService.exe fingerprint")
    ap.add_argument("--write", action="store_true", help="force rewrite baseline")
    ap.add_argument(
        "--freeze",
        action="store_true",
        help="write single-row ngs_official_crc.tsv + NGS.EXE.CRC from programdata (or first present)",
    )
    ap.add_argument("--json", action="store_true", help="print JSON to stdout")
    args = ap.parse_args()

    rows = collect()
    present = [r for r in rows if r.present]
    if not present:
        write_status(rows, ["no NGService.exe found"], "missing")
        print("NGS: no NGService.exe found in candidate paths", file=sys.stderr)
        for r in rows:
            print(f"  miss {r.slot}: {r.path}", file=sys.stderr)
        return 2

    if args.json:
        print(json.dumps([asdict(r) for r in rows], ensure_ascii=False, indent=2))

    STATE_DIR.mkdir(parents=True, exist_ok=True)

    primary = next((r for r in present if r.slot == "programdata"), present[0])

    if args.write or not BASELINE.is_file():
        BASELINE.write_text(to_tsv(rows), encoding="utf-8")
        write_status(rows, [], "baseline_written")
        print(f"NGS: baseline written -> {BASELINE}")
        for r in present:
            print(
                f"  [{r.slot}] ver={r.file_version} size={r.size} "
                f"crc32={r.crc32} sha256={r.sha256[:16]}…\n    {r.path}"
            )
        if args.freeze:
            freeze_official(primary)
        return 0

    baseline = parse_tsv(BASELINE.read_text(encoding="utf-8"))
    changes = diff(rows, baseline)
    write_status(rows, changes, "compare")

    print(
        f"NGS: {primary.file_version or '?'} size={primary.size} "
        f"crc32={primary.crc32} status={'CHANGED' if changes else 'OK'}"
    )
    print(f"  status -> {STATUS}")
    if args.freeze:
        if changes:
            print(
                "NGS: refuse --freeze while fingerprint CHANGED (re--write baseline first)",
                file=sys.stderr,
            )
            return 1
        freeze_official(primary)
    if changes:
        for c in changes:
            print(f"  ! {c}")
        return 1
    print("  matches baseline")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
