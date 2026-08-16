#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""经典版 TWMS · GameAssembly remount 入口 + 残槽审计。

产品 = 经典版；只换 dump 能证明的哈希/方法头 RVA，不改业务判定。
体内点（jnz/cmov/E8/RIP 常量框）不自动猜，只对 scripts/data/ga_patch_sites.tsv 核字节。

用法（仓根）：
  python scripts/ga_remount.py map
      旧 dump → 新 dump 建映射，写 Dumps/runtime/_ga_remount_*.tsv（默认 dry-run）
  python scripts/ga_remount.py map --apply
      源码仍是旧 dump 哈希时才写入；哈希已在新 dump 上则拒绝。
      旧地址若在新 dump 仍是方法头（数字碰巧重用）绝不改，避免改坏业务 RVA。
  python scripts/ga_remount.py audit
      源码哈希/方法头 RVA vs dump.cs；shape vs dump 字段类型；体内点 vs GameAssembly.dll 字节

更新日顺序：归档旧 dump.cs → Il2CppDumper 出新 dump → map（看 miss）→ audit → 只对红灯开 IDA → 确认后再 map --apply。

Agent 清单：docs/features/ops/GA-remount-Agent清单.md
本机打印：python scripts/ga_remount.py howto
"""
from __future__ import annotations

import argparse
import importlib.util
import re
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

HOWTO = """GA remount howto (classic TWMS). Do not change business logic.

Hard stops:
  - no map --apply unless the user said apply/write AND on_old_map>0
  - on_old_map=0 + already_on_new>0 => already remounted; REFUSE apply
  - body-site FAIL => retarget catalog + kRva*; never grep 75 07 in flattened code
  - do not kill user processes; do not publish

Need:
  Dumps/runtime/out/dump.cs
  Dumps/runtime/GameAssembly.dll          (>=100MB runtime dump, not 36MB disk stub)
  Dumps/runtime/_archive_*/out/dump.cs    (previous dump)

Commands (repo root, this order):
  python scripts/ga_remount.py map
  python scripts/ga_remount.py audit
  # then only if user asked AND on_old_map>0:
  python scripts/ga_remount.py map --apply
  python scripts/ga_remount.py audit

Read:
  Dumps/runtime/_ga_remount_audit.txt
  Dumps/runtime/_ga_remount_apply.tsv
  Dumps/runtime/_ga_remount_rva_collision.tsv   (live addrs; do not rewrite)

Catalog: scripts/data/ga_patch_sites.tsv
Ignore:  scripts/data/ga_remount_ignore.txt
Full:    docs/features/ops/GA-remount-Agent*.md
"""

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_NEW = ROOT / "Dumps" / "runtime" / "out" / "dump.cs"
DEFAULT_GA = ROOT / "Dumps" / "runtime" / "GameAssembly.dll"
DEFAULT_OUT = ROOT / "Dumps" / "runtime"
DEFAULT_X = ROOT / "x"
DEFAULT_CATALOG = ROOT / "scripts" / "data" / "ga_patch_sites.tsv"
DEFAULT_SHAPE = ROOT / "x" / "runtime" / "il2cpp_shape.cpp"
LEGACY = ROOT / "Dumps" / "runtime" / "_remount_20260814.py"

RE_HASH = re.compile(r"[a-f0-9]{60,64}")
RE_TOKEN_HASH = re.compile(r"\b[a-f0-9]{60,64}\b")
RE_TOKEN_RVA = re.compile(r"\b0x([0-9A-Fa-f]{5,8})\b")
RE_RVA_LINE = re.compile(r"RVA:\s*0x([0-9A-Fa-f]+)")
RE_TDI = re.compile(r"TypeDefIndex:\s*(\d+)")
RE_CLASS = re.compile(r"\b(class|struct|enum|interface)\s+([A-Za-z0-9_.<>-]+)")
RE_FIELD_OFF = re.compile(r"//\s*0x([0-9A-Fa-f]+)\s*$")
RE_SHAPE_HASH = re.compile(
    r"constexpr char (kHash\w+)\[\]\s*=\s*\n?\s*\"([a-f0-9]{60,64})\""
)
RE_SHAPE_FIELDS = re.compile(
    r"constexpr FieldShape (k\w+Fields)\[\] = \{((?:[^{}]|\{[^}]*\})*)\}",
    re.S,
)
RE_SHAPE_FIELD = re.compile(
    r"\{0x([0-9A-Fa-f]+),\s*FieldKind::(Ptr|I32|I64|Bool|ValueTypeApprox)"
)
RE_KRVA = re.compile(
    r"constexpr\s+(?:const\s+)?(?:uint32_t|uintptr_t|unsigned(?:\s+int)?)\s+"
    r"(kRva\w+)\s*=\s*0x([0-9A-Fa-f]+)",
    re.I,
)

# shape 表：源码哈希符号 → FieldShape 数组名（与 il2cpp_shape.cpp 对齐）
SHAPE_PAIRS = (
    ("kHashWorldManager", "kWmFields"),
    ("kHashUserLocal", "kUlFields"),
    ("kHashNetworkManager", "kNmFields"),
    ("kHashNetworkManagerFacade", "kNmFacadeFields"),
    ("kHashSecAttack", "kSaFields"),
    ("kHashSceneLogin", "kSlFields"),
)

ACCESS = ("public", "private", "protected", "internal")


def default_old_dump() -> Path:
    cands = sorted((ROOT / "Dumps" / "runtime").glob("_archive_*/out/dump.cs"))
    if not cands:
        raise SystemExit("找不到归档 dump.cs，请传 --old")
    return cands[-1]


def load_legacy():
    if not LEGACY.is_file():
        raise SystemExit("missing %s" % LEGACY)
    spec = importlib.util.spec_from_file_location("ga_remount_legacy", LEGACY)
    if spec is None or spec.loader is None:
        raise SystemExit("cannot load %s" % LEGACY)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def collect_x_tokens(xdir: Path) -> tuple[set[str], set[int]]:
    hashes: set[str] = set()
    rvas: set[int] = set()
    files = list(xdir.rglob("*.cpp")) + list(xdir.rglob("*.h"))
    for fp in files:
        txt = fp.read_text(encoding="utf-8", errors="replace")
        hashes.update(RE_TOKEN_HASH.findall(txt))
        for m in RE_TOKEN_RVA.finditer(txt):
            v = int(m.group(1), 16)
            if v >= 0x10000:
                rvas.add(v)
    return hashes, rvas


def collect_krva(xdir: Path) -> list[tuple[str, int, str]]:
    """Only constexpr kRva* = 0x... (skip comments / seeds / masks)."""
    out: list[tuple[str, int, str]] = []
    files = list(xdir.rglob("*.cpp")) + list(xdir.rglob("*.h"))
    for fp in files:
        rel = str(fp.relative_to(ROOT)).replace("\\", "/")
        txt = fp.read_text(encoding="utf-8", errors="replace")
        for m in RE_KRVA.finditer(txt):
            out.append((m.group(1), int(m.group(2), 16), rel))
    return out


def load_ignore(path: Path) -> set[str]:
    if not path.is_file():
        return set()
    s: set[str] = set()
    for ln in path.read_text(encoding="utf-8").splitlines():
        ln = ln.split("#", 1)[0].strip().lower()
        if ln:
            s.add(ln)
    return s


def build_maps(legacy, old, new, tdi_map, rvas_in_code: set[int]):
    class_hash: dict[str, str] = {}
    field_hash_map: dict[str, str] = {}
    meth_hash_map: dict[str, str] = {}
    rva_map: dict[int, int] = {}
    rva_conflict = 0

    def hash_tail(name: str) -> str | None:
        tail = name.split(".")[-1]
        gm = re.match(r"([a-f0-9]{60,64})", tail)
        if gm:
            return gm.group(1)
        return name if RE_HASH.fullmatch(name) else None

    for otdi, ntdi in tdi_map.items():
        oc, nc = old[otdi], new[ntdi]
        oh, nh = hash_tail(oc["name"]), hash_tail(nc["name"])
        if oh and nh:
            class_hash[oh] = nh
        if RE_HASH.fullmatch(oc["name"]) and RE_HASH.fullmatch(nc["name"]):
            class_hash[oc["name"]] = nc["name"]
        nf = {off: fh for off, fh in nc["fields"]}
        for off, fh in oc["fields"]:
            nhf = nf.get(off)
            if fh and nhf and fh != nhf:
                field_hash_map[fh] = nhf
        ooffs = oc["off_set"]
        noffs = nc["off_set"]
        unmatched_old = [o for o in ooffs if o not in noffs]
        unmatched_new = [o for o in noffs if o not in ooffs]
        if unmatched_old and unmatched_new:
            cnt: Counter[int] = Counter()
            for oo in unmatched_old:
                for no in unmatched_new:
                    d = no - oo
                    if abs(d) <= 0x100:
                        cnt[d] += 1
            if cnt:
                dlt, n = cnt.most_common(1)[0]
                if n >= 4 and n >= len(unmatched_old) * 0.45:
                    for off, fh in oc["fields"]:
                        if not fh or fh in field_hash_map:
                            continue
                        nhf = nf.get(off + dlt)
                        if nhf:
                            field_hash_map[fh] = nhf
        for i, j in legacy.align_methods(oc["methods"], nc["methods"]):
            orva, _osig, oh = oc["methods"][i]
            nrva, _nsig, nh = nc["methods"][j]
            if orva in rva_map and rva_map[orva] != nrva:
                rva_conflict += 1
            else:
                rva_map[orva] = nrva
            if oh and nh and oh != nh:
                meth_hash_map[oh] = nh

    new_rvas = dump_method_rvas(new)
    for old_r, new_r in list(rva_map.items()):
        # anti_macro 等曾把 Unity dump RVA 写成 +0x1000000；新 dump 里已是真方法头的数字不能当别名。
        alias = old_r + 0x1000000
        if alias in rvas_in_code and alias not in rva_map and alias not in new_rvas:
            rva_map[alias] = new_r
    return class_hash, field_hash_map, meth_hash_map, rva_map, rva_conflict


def dump_method_rvas(parsed: dict) -> set[int]:
    s: set[int] = set()
    for k, c in parsed.items():
        if not isinstance(k, int):
            continue
        for rva, _sig, _h in c.get("methods", []):
            s.add(rva)
    return s


def cmd_map(args: argparse.Namespace) -> int:
    legacy = load_legacy()
    old_path = Path(args.old) if args.old else default_old_dump()
    new_path = Path(args.new)
    out_dir = Path(args.out_dir)
    xdir = Path(args.x_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    print("old", old_path)
    print("new", new_path)
    print("parse OLD")
    old = legacy.parse_dump(old_path)
    print("parse NEW")
    new = legacy.parse_dump(new_path)
    old_n = sum(1 for k in old if isinstance(k, int))
    new_n = sum(1 for k in new if isinstance(k, int))
    print("classes old=%d new=%d" % (old_n, new_n))

    hashes_in_code, rvas_in_code = collect_x_tokens(xdir)
    print("match classes by shape...")
    tdi_map = legacy.match_classes(old, new, prefer_names=hashes_in_code)
    print("paired TDI=%d" % len(tdi_map))

    class_hash, field_hash_map, meth_hash_map, rva_map, rva_conflict = build_maps(
        legacy, old, new, tdi_map, rvas_in_code
    )
    print(
        "maps class=%d field=%d meth=%d rva=%d rva_conflict=%d"
        % (
            len(class_hash),
            len(field_hash_map),
            len(meth_hash_map),
            len(rva_map),
            rva_conflict,
        )
    )

    def bucket(h: str) -> str:
        if h in class_hash:
            return "class"
        if h in field_hash_map:
            return "field"
        if h in meth_hash_map:
            return "meth"
        return "miss"

    by: dict[str, list[str]] = defaultdict(list)
    for h in hashes_in_code:
        by[bucket(h)].append(h)
    print(
        "CODE hashes=%d class=%d field=%d meth=%d miss=%d"
        % (
            len(hashes_in_code),
            len(by["class"]),
            len(by["field"]),
            len(by["meth"]),
            len(by["miss"]),
        )
    )
    new_rvas = dump_method_rvas(new)
    r_ch = [r for r in rvas_in_code if r in rva_map and rva_map[r] != r]
    r_same = [r for r in rvas_in_code if r in rva_map and rva_map[r] == r]
    r_miss = [r for r in rvas_in_code if r not in rva_map]
    r_collide = [r for r in r_ch if r in new_rvas]
    r_move = [r for r in r_ch if r not in new_rvas]
    valset = (
        set(class_hash.values())
        | set(field_hash_map.values())
        | set(meth_hash_map.values())
    )
    on_new = sum(1 for h in hashes_in_code if h in valset)
    on_old = len(by["class"]) + len(by["field"]) + len(by["meth"])
    print(
        "CODE rvas=%d changed=%d collide=%d move=%d same=%d miss=%d"
        % (len(rvas_in_code), len(r_ch), len(r_collide), len(r_move), len(r_same), len(r_miss))
    )
    print("CODE hashes on_old_map=%d already_on_new=%d" % (on_old, on_new))

    (out_dir / "_ga_remount_miss_hashes.txt").write_text(
        "\n".join(sorted(by["miss"])) + "\n", encoding="utf-8"
    )
    (out_dir / "_ga_remount_miss_rvas.txt").write_text(
        "\n".join("0x%X" % r for r in sorted(r_miss)) + "\n", encoding="utf-8"
    )
    rows = []
    for h in sorted(hashes_in_code):
        k = bucket(h)
        if k == "miss":
            continue
        mp = {"class": class_hash, "field": field_hash_map, "meth": meth_hash_map}[k]
        rows.append("%s\t%s\t%s" % (k, h, mp[h]))
    (out_dir / "_ga_remount_apply.tsv").write_text(
        "kind\told\tnew\n" + "\n".join(rows) + "\n", encoding="utf-8"
    )
    (out_dir / "_ga_remount_rva.tsv").write_text(
        "old\tnew\n"
        + "\n".join(
            "0x%X\t0x%X" % (a, rva_map[a]) for a in sorted(rvas_in_code) if a in rva_map
        )
        + "\n",
        encoding="utf-8",
    )
    (out_dir / "_ga_remount_rva_collision.tsv").write_text(
        "src\twould_map_to\n"
        + "\n".join("0x%X\t0x%X" % (a, rva_map[a]) for a in sorted(r_collide))
        + "\n",
        encoding="utf-8",
    )
    print("wrote", out_dir / "_ga_remount_apply.tsv")
    print("sample miss hashes", sorted(by["miss"])[:8])
    print("sample miss rvas", ["0x%X" % x for x in sorted(r_miss)[:12]])
    if r_collide:
        print(
            "RVA collision (live in NEW dump, will not rewrite):",
            ["0x%X" % x for x in sorted(r_collide)[:12]],
        )

    if not args.apply:
        print("dry-run（加 --apply 才改 x/ 哈希和 0xRVA；不改业务判定）")
        return 0

    if on_old == 0 and on_new > 0:
        print(
            "REFUSE --apply: source hashes already on NEW dump; "
            "blind RVA rewrite would hit live methods (see _ga_remount_rva_collision.tsv)"
        )
        return 2

    hash_repl = {}
    hash_repl.update({h: class_hash[h] for h in by["class"]})
    hash_repl.update({h: field_hash_map[h] for h in by["field"]})
    hash_repl.update({h: meth_hash_map[h] for h in by["meth"]})
    files = list(xdir.rglob("*.cpp")) + list(xdir.rglob("*.h"))
    changed = 0
    for fp in files:
        txt = fp.read_text(encoding="utf-8", errors="replace")
        orig = txt
        for oh, nh in hash_repl.items():
            if oh in txt:
                txt = txt.replace(oh, nh)

        def rva_sub(m: re.Match) -> str:
            v = int(m.group(1), 16)
            if v in rva_map and rva_map[v] != v:
                if v in new_rvas:
                    return m.group(0)
                src = m.group(1)
                nv = rva_map[v]
                fmt = "%X" % nv if any(c.isupper() for c in src) else "%x" % nv
                return "0x" + fmt
            return m.group(0)

        txt = RE_TOKEN_RVA.sub(rva_sub, txt)
        if txt != orig:
            fp.write_text(txt, encoding="utf-8", newline="\n")
            changed += 1
            print("patched", fp.relative_to(ROOT))
    print("patched files=%d" % changed)
    return 0


# ----- audit -----


class PeImage:
    def __init__(self, path: Path):
        self.data = path.read_bytes()
        self.sections: list[tuple[int, int, int, int]] = []
        if self.data[:2] != b"MZ":
            raise SystemExit("not PE: %s" % path)
        e_lfanew = struct.unpack_from("<I", self.data, 0x3C)[0]
        if self.data[e_lfanew : e_lfanew + 4] != b"PE\0\0":
            raise SystemExit("not PE: %s" % path)
        coff = e_lfanew + 4
        nsec = struct.unpack_from("<H", self.data, coff + 2)[0]
        opt_size = struct.unpack_from("<H", self.data, coff + 16)[0]
        sec = coff + 20 + opt_size
        for i in range(nsec):
            off = sec + i * 40
            vsize = struct.unpack_from("<I", self.data, off + 8)[0]
            va = struct.unpack_from("<I", self.data, off + 12)[0]
            raw_size = struct.unpack_from("<I", self.data, off + 16)[0]
            raw_ptr = struct.unpack_from("<I", self.data, off + 20)[0]
            self.sections.append((va, vsize, raw_ptr, raw_size))

    def to_off(self, rva: int) -> int | None:
        for va, vsize, raw_ptr, raw_size in self.sections:
            span = max(vsize, raw_size)
            if raw_ptr and va <= rva < va + span:
                return raw_ptr + (rva - va)
        return None

    def read(self, rva: int, n: int) -> bytes | None:
        off = self.to_off(rva)
        if off is None or off < 0 or off + n > len(self.data):
            return None
        return self.data[off : off + n]


def parse_hex_bytes(s: str) -> bytes:
    s = s.strip()
    if not s:
        return b""
    return bytes(int(p, 16) for p in s.split())


def load_catalog(path: Path) -> list[dict]:
    rows = []
    lines = path.read_text(encoding="utf-8").splitlines()
    if not lines:
        return rows
    hdr = lines[0].split("\t")
    for ln in lines[1:]:
        if not ln.strip() or ln.startswith("#"):
            continue
        parts = ln.split("\t")
        rec = {hdr[i]: (parts[i] if i < len(parts) else "") for i in range(len(hdr))}
        rec["rva_i"] = int(rec["rva"], 16)
        rec["expect_b"] = parse_hex_bytes(rec.get("expect", ""))
        rec["target_i"] = int(rec["target"], 16) if rec.get("target", "").strip() else 0
        rows.append(rec)
    return rows


def classify_rva(r: int) -> str:
    if r in (0xFFFFFFFF, 0x7FFFFFFF, 0xFFFFFFF) or r >= 0xFFFF0000:
        return "imm_seed"
    if r < 0x10000:
        return "junk"
    try:
        b = r.to_bytes(4, "little")
        if all(0x20 < x < 0x7F for x in b):
            return "fourcc"
    except OverflowError:
        pass
    return "unknown"


def kind_of_csharp(ty: str, structs: set[str]) -> str:
    t = re.sub(r"\s+", " ", ty).strip()
    t = re.sub(r"^((public|private|protected|internal|static|readonly|volatile)\s+)+", "", t)
    base = t.split("<", 1)[0].strip().rstrip("&").rstrip("*")
    if base in ("bool", "Boolean"):
        return "Bool"
    if base in (
        "int",
        "uint",
        "Int32",
        "UInt32",
        "short",
        "ushort",
        "Int16",
        "UInt16",
        "byte",
        "sbyte",
        "char",
    ):
        return "I32"
    if base in ("long", "ulong", "Int64", "UInt64"):
        return "I64"
    if base in (
        "float",
        "double",
        "Vector2",
        "Vector3",
        "Rect",
        "Color",
        "Quaternion",
        "Nullable",
    ):
        return "ValueTypeApprox"
    if base in structs:
        return "ValueTypeApprox"
    return "Ptr"


def index_dump(path: Path) -> dict:
    hashes: set[str] = set()
    method_rvas: set[int] = set()
    structs: set[str] = set()
    class_fields: dict[str, dict[int, str]] = {}
    raw_fields: dict[str, list[tuple[int, str]]] = {}
    cur_name = ""
    pending_rva = None
    fields: list[tuple[int, str]] = []

    def flush():
        nonlocal fields, cur_name
        key = cur_name.split(".")[-1] if cur_name else ""
        if key and RE_HASH.fullmatch(key):
            raw_fields[key] = list(fields)
        fields = []

    text = path.read_text(encoding="utf-8", errors="replace")
    for ln in text.splitlines():
        hashes.update(RE_HASH.findall(ln))
        tm = RE_TDI.search(ln)
        cm = RE_CLASS.search(ln)
        if tm and cm:
            flush()
            cur_name = cm.group(2)
            if cm.group(1) == "struct":
                structs.add(cur_name.split(".")[-1])
            pending_rva = None
            continue
        rm = RE_RVA_LINE.search(ln)
        if rm and ln.strip().startswith("//"):
            pending_rva = int(rm.group(1), 16)
            continue
        if pending_rva is not None and "(" in ln:
            method_rvas.add(pending_rva)
            pending_rva = None
            continue
        pending_rva = None
        fm = RE_FIELD_OFF.search(ln)
        if fm and ";" in ln:
            off = int(fm.group(1), 16)
            decl = ln.split("//")[0].strip().rstrip(";")
            if not decl:
                continue
            bits = decl.rsplit(None, 1)
            ty = bits[0] if len(bits) == 2 else decl
            fields.append((off, ty))
    flush()
    for key, fl in raw_fields.items():
        class_fields[key] = {off: kind_of_csharp(ty, structs) for off, ty in fl}
    return {
        "hashes": hashes,
        "method_rvas": method_rvas,
        "structs": structs,
        "class_fields": class_fields,
    }


def parse_shape(path: Path) -> list[tuple[str, str, list[tuple[int, str]]]]:
    txt = path.read_text(encoding="utf-8", errors="replace")
    hashes = {k: h for k, h in RE_SHAPE_HASH.findall(txt)}
    blocks = {name: body for name, body in RE_SHAPE_FIELDS.findall(txt)}
    out = []
    for hk, fk in SHAPE_PAIRS:
        h = hashes.get(hk)
        body = blocks.get(fk)
        if not h or body is None:
            out.append((hk, h or "", []))
            continue
        fields = [(int(a, 16), b) for a, b in RE_SHAPE_FIELD.findall(body)]
        out.append((hk, h, fields))
    return out


def cmd_audit(args: argparse.Namespace) -> int:
    dump_path = Path(args.new)
    ga_path = Path(args.ga)
    xdir = Path(args.x_dir)
    cat_path = Path(args.catalog)
    shape_path = Path(args.shape)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    print("dump", dump_path)
    print("ga  ", ga_path)
    idx = index_dump(dump_path)
    hashes_in_code, _rvas_all = collect_x_tokens(xdir)
    krvas = collect_krva(xdir)
    catalog = load_catalog(cat_path) if cat_path.is_file() else []
    catalog_rvas = {r["rva_i"] for r in catalog}
    ignore = load_ignore(Path(args.ignore))
    ga_size = ga_path.stat().st_size if ga_path.is_file() else 0x8000000

    lines: list[str] = []
    fail = 0

    def note(s: str, is_fail: bool = False) -> None:
        nonlocal fail
        if is_fail:
            fail += 1
            s = "FAIL  " + s
        print(s)
        lines.append(s)

    # 1) hashes in source vs dump
    miss_h = sorted(h for h in hashes_in_code if h not in idx["hashes"])
    ign_h = [h for h in miss_h if h.lower() in ignore]
    real_h = [h for h in miss_h if h.lower() not in ignore]
    note("[hash] source=%d dump_miss=%d ignore=%d" % (len(hashes_in_code), len(real_h), len(ign_h)))
    for h in ign_h:
        note("IGN   hash  %s" % h)
    for h in real_h:
        note("hash not in dump.cs  %s" % h, True)

    # 2) constexpr kRva* vs dump method table
    head_miss = []
    skipped = Counter()
    for name, r, rel in krvas:
        key = "0x%x" % r
        if r in catalog_rvas:
            skipped["catalog"] += 1
            continue
        if key in ignore or ("%x" % r) in ignore:
            skipped["ignore"] += 1
            continue
        # 种子 RVA 在 .data，不是 dump 方法头。
        if "seed" in name.lower():
            skipped["seed"] += 1
            continue
        kind = classify_rva(r)
        if kind != "unknown":
            skipped[kind] += 1
            continue
        if ga_size and r >= ga_size:
            skipped["beyond_image"] += 1
            continue
        if r not in idx["method_rvas"]:
            head_miss.append((name, r, rel))
    note(
        "[rva-head] kRva*=%d catalog=%d skip=%s dump_miss=%d"
        % (len(krvas), skipped["catalog"], dict(skipped), len(head_miss))
    )
    for name, r, rel in head_miss:
        note("kRva not in dump.cs  %s=0x%X  %s" % (name, r, rel), True)

    # 3) shape
    if shape_path.is_file():
        shapes = parse_shape(shape_path)
        note("[shape] tables=%d" % len(shapes))
        for name, h, fields in shapes:
            if not h:
                note("shape %s: hash symbol missing in cpp" % name, True)
                continue
            cf = idx["class_fields"].get(h)
            if cf is None:
                note("shape %s: class hash not in dump  %s" % (name, h), True)
                continue
            bad = 0
            for off, want in fields:
                got = cf.get(off)
                if got is None:
                    note("shape %s: no field @0x%X (want %s)" % (name, off, want), True)
                    bad += 1
                elif got != want:
                    # dump 把混淆 valuetype 写成 class 名时会报 Ptr；不当红灯。
                    if want == "ValueTypeApprox" and got == "Ptr":
                        note(
                            "WARN  shape %s: @0x%X dump=Ptr (hashed) shape=ValueTypeApprox"
                            % (name, off)
                        )
                    else:
                        note(
                            "shape %s: @0x%X dump=%s shape=%s" % (name, off, got, want),
                            True,
                        )
                        bad += 1
            if fields and bad == 0:
                note("[shape %s] %d fields ok" % (name, len(fields)))
    else:
        note("[shape] skip (no %s)" % shape_path)

    # 4) in-body catalog vs GA bytes
    if not catalog:
        note("[body] no catalog %s" % cat_path, True)
    elif not ga_path.is_file():
        note("[body] no GameAssembly.dll %s" % ga_path, True)
    else:
        pe = PeImage(ga_path)
        note("[body] sites=%d ga=%s" % (len(catalog), ga_path.name))
        for rec in catalog:
            rva = rec["rva_i"]
            kind = rec["kind"]
            if kind == "call":
                raw = pe.read(rva, 5)
                if not raw:
                    note("%s: cannot read 0x%X" % (rec["tag"], rva), True)
                    continue
                want0 = rec["expect_b"][:1] or b"\xE8"
                if raw[0:1] != want0:
                    note(
                        "%s: 0x%X byte=%s want=%s"
                        % (rec["tag"], rva, raw[:1].hex(), want0.hex()),
                        True,
                    )
                    continue
                rel = struct.unpack_from("<i", raw, 1)[0]
                got_tgt = (rva + 5 + rel) & 0xFFFFFFFF
                if rec["target_i"] and got_tgt != rec["target_i"]:
                    note(
                        "%s: 0x%X call tgt=0x%X want=0x%X"
                        % (rec["tag"], rva, got_tgt, rec["target_i"]),
                        True,
                    )
                    continue
                note("[%s] E8 -> 0x%X" % (rec["tag"], got_tgt))
                continue
            exp = rec["expect_b"]
            if not exp:
                note("%s: empty expect" % rec["tag"], True)
                continue
            raw = pe.read(rva, len(exp))
            if raw is None:
                note("%s: cannot read 0x%X" % (rec["tag"], rva), True)
                continue
            if raw != exp:
                note(
                    "%s: 0x%X have %s want %s"
                    % (rec["tag"], rva, raw.hex(" "), exp.hex(" ")),
                    True,
                )
            else:
                note("[%s] %d bytes @0x%X" % (rec["tag"], len(exp), rva))

    report = out_dir / "_ga_remount_audit.txt"
    report.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("wrote", report)
    print("FAIL count=%d" % fail)
    return 1 if fail else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_map = sub.add_parser("map", help="old dump.cs → new dump.cs 哈希/方法头 RVA 映射")
    p_map.add_argument("--old", default="", help="旧 dump.cs（默认最新 Dumps/runtime/_archive_*/out/dump.cs）")
    p_map.add_argument("--new", default=str(DEFAULT_NEW))
    p_map.add_argument("--x-dir", default=str(DEFAULT_X))
    p_map.add_argument("--out-dir", default=str(DEFAULT_OUT))
    p_map.add_argument(
        "--apply",
        action="store_true",
        help="写入 x/ 已映射哈希和 0xRVA；默认只出 tsv",
    )

    p_au = sub.add_parser("audit", help="残槽：哈希 / 方法头 / shape / 体内点字节")
    p_au.add_argument("--new", default=str(DEFAULT_NEW), help="新 dump.cs")
    p_au.add_argument("--ga", default=str(DEFAULT_GA))
    p_au.add_argument("--x-dir", default=str(DEFAULT_X))
    p_au.add_argument("--catalog", default=str(DEFAULT_CATALOG))
    p_au.add_argument("--shape", default=str(DEFAULT_SHAPE))
    p_au.add_argument("--out-dir", default=str(DEFAULT_OUT))
    p_au.add_argument(
        "--ignore",
        default=str(ROOT / "scripts" / "data" / "ga_remount_ignore.txt"),
        help="已知不在 dump 的哈希/RVA（每行一项）",
    )

    sub.add_parser("howto", help="打印 Agent 执行清单（硬停 + 命令）")

    args = ap.parse_args()
    if args.cmd == "map":
        return cmd_map(args)
    if args.cmd == "audit":
        return cmd_audit(args)
    if args.cmd == "howto":
        sys.stdout.write(HOWTO)
        if not HOWTO.endswith("\n"):
            sys.stdout.write("\n")
        return 0
    ap.print_help()
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
