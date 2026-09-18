#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""经典版 TWMS · GameAssembly remount 入口 + 残槽审计。

产品 = 经典版；只换 dump 能证明的哈希/方法头 RVA，不改业务判定。
体内点（jnz/cmov/E8/RIP 常量框）不自动猜，只对 scripts/data/ga_patch_sites.tsv 核字节。

用法（仓根）：
  python scripts/ga_remount.py map
      旧 dump → 新 dump 建映射，写 Dumps/runtime/_ga_remount_*.tsv（默认 dry-run）
  python scripts/ga_remount.py map --apply-hashes
      只换源码里已映射的类/字段/方法哈希（下次更新日的默认写入）
  python scripts/ga_remount.py map --apply-krva
      只改 constexpr kRva* = 0x…；bind 按 hash/plain 身份改；shared 虚方法禁止唯一改写
  python scripts/ga_remount.py map --apply
      等价 --apply-hashes + --apply-krva。不再改注释/体内点里的裸 0xHEX
  python scripts/ga_remount.py map --apply-all-hex
      旧行为：所有 0xRVA token；哈希已在新 dump 时拒绝
  python scripts/ga_remount.py layout
      字段偏移+类型差，以及 kFb* 是否还等于新偏移（默认 dry-run）
  python scripts/ga_remount.py layout --write
      只改 FALLBACK_STALE 的 constexpr kFb*（用户说 write；TYPE_FLIP 拒绝）
  python scripts/ga_remount.py krva
      kRva* 与同后缀 kHash* 对 dump 方法 RVA（验身份，不只是「是不是方法头」）
  python scripts/ga_remount.py audit
      源码哈希/方法头 RVA vs dump.cs；shape vs dump 字段类型；体内点 vs GameAssembly.dll 字节
  python scripts/ga_remount.py smoke
      扫 x.jsonl + 轮转的 hits=a/b（对照 scripts/data/ga_remount_smoke_expect.tsv）
  python scripts/ga_remount.py dump-check
      下次更新前：查运行时 GA / metadata / Dumper，不写 dump.cs
  python scripts/ga_remount.py dump
      预检 + 打印归档/ForceDump 计划（默认不写）
  python scripts/ga_remount.py dump --archive
      注入前：拷当前 dump.cs/GA 到 _archive_*（不覆盖已有归档）
  python scripts/ga_remount.py dump --process
      注入后：拷客户端 metadata + process_runtime_dump.py（覆盖 out/dump.cs）
  python scripts/ga_remount.py catalog
      体内点 follow 所在方法头窗口（默认 dry-run）；禁止全模块搜 75 07
  python scripts/ga_remount.py apply
      按序 hashes → layout kFb → kRva → catalog（默认 dry-run）
  python scripts/ga_remount.py apply --apply
      用户说 apply/write 才写入；TYPE_FLIP / catalog FAIL 立刻停，不再写后面的步
  python scripts/ga_remount.py selftest
      正则自检：注释里的 constexpr kRva/kFb 不得被当成声明

更新日顺序：dump --archive → 人注 GaRuntimeDump.dll → dump --process → verify → 用户说 apply 再 apply --apply。
  apply --apply = hashes → kFb → kRva → catalog；TYPE_FLIP / catalog FAIL 硬停。禁止 --apply-all-hex 除非用户点名。

Agent 清单：docs/features/ops/GA-remount-Agent清单.md
本机打印：python scripts/ga_remount.py howto
"""
from __future__ import annotations

import argparse
import bisect
import hashlib
import importlib.util
import json
import re
import shutil
import struct
import subprocess
import sys
from collections import Counter, defaultdict
from datetime import datetime
from pathlib import Path

HOWTO = """GA remount howto (classic TWMS). Do not change business logic.

Hard stops:
  - no map --apply* / apply --apply unless the user said apply/write
  - apply --apply writes hashes → kFb → kRva → catalog; TYPE_FLIP / catalog FAIL stops remaining writes
  - --apply = hashes + named kRva* only; never rewrite bare 0xHEX in comments
  - --apply-all-hex only if user named it; on_old_map=0 => REFUSE
  - body-site FAIL => retarget via catalog (parent window) + kRva*; never grep 75 07 in flattened code
  - catalog --write only if the user said write/apply; updates tsv rva/target and constexpr kRva* named in the cpp column (not comments)
  - catalog STALE = GA site ok, constexpr kRva still old; --write only if the user said write
  - layout --write only if the user said write; only FALLBACK_STALE constexpr kFb*; TYPE_FLIP refuses
  - dump --process only if the user said process/ForceDump; never with --archive; never hide in inject
  - layout TYPE_FLIP / FALLBACK_STALE => fix kFb* / field hash; do not ignore
  - do not kill user processes; do not publish

Need:
  Dumps/runtime/out/dump.cs
  Dumps/runtime/GameAssembly.dll          (>=100MB runtime dump, not 36MB disk stub)
  Dumps/runtime/_archive_*/out/dump.cs    (previous dump)

Commands (repo root, this order):
  python scripts/ga_remount.py dump --archive    # before inject; user said archive
  python scripts/ga_remount.py dump --process    # after inject; user said process/ForceDump
  python scripts/ga_remount.py map
  python scripts/ga_remount.py map --apply-hashes
  python scripts/ga_remount.py layout
  python scripts/ga_remount.py map --apply-krva
  python scripts/ga_remount.py krva
  python scripts/ga_remount.py catalog
  python scripts/ga_remount.py audit
  python scripts/ga_remount.py smoke
  python scripts/ga_remount.py dump-check
  python scripts/ga_remount.py dump
  python scripts/ga_remount.py verify            # dry-run selftest+dump-check+map+layout+krva+catalog+audit+smoke; no apply
  python scripts/ga_remount.py apply             # dry-run ordered writes
  python scripts/ga_remount.py apply --apply     # user said apply: hashes→kFb→kRva→catalog
  python scripts/ga_remount.py selftest

Read:
  Dumps/runtime/_ga_remount_audit.txt
  Dumps/runtime/_ga_remount_layout.tsv
  Dumps/runtime/_ga_remount_krva.tsv
  Dumps/runtime/_ga_remount_apply.tsv
  Dumps/runtime/_ga_remount_rva_collision.tsv   (live addrs; do not rewrite)

Catalog: scripts/data/ga_patch_sites.tsv  (parent + anchor; catalog 子命令 follow 方法头)
Ignore:  scripts/data/ga_remount_ignore.txt
Bind:    scripts/data/ga_krva_bind.tsv          (kRva → hash|plain ident + old RVA; shared=1 禁止唯一改写)
Smoke:   scripts/data/ga_remount_smoke_expect.tsv  (need=req|opt)
Full:    docs/features/ops/GA-remount-Agent*.md
"""

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_NEW = ROOT / "Dumps" / "runtime" / "out" / "dump.cs"
DEFAULT_GA = ROOT / "Dumps" / "runtime" / "GameAssembly.dll"
DEFAULT_OUT = ROOT / "Dumps" / "runtime"
DEFAULT_X = ROOT / "x"
DEFAULT_CATALOG = ROOT / "scripts" / "data" / "ga_patch_sites.tsv"
DEFAULT_SHAPE = ROOT / "x" / "runtime" / "il2cpp_shape.cpp"
DEFAULT_KRVA_BIND = ROOT / "scripts" / "data" / "ga_krva_bind.tsv"
DEFAULT_SMOKE_EXPECT = ROOT / "scripts" / "data" / "ga_remount_smoke_expect.tsv"
DEFAULT_LOG = ROOT / "bin" / "rtcache" / "logs" / "x.jsonl"
LEGACY = ROOT / "Dumps" / "runtime" / "_remount_20260814.py"
CLIENT_META = Path(
    r"G:\Games\maplestory_classic\Maplestory_Classic_Data\il2cpp_data\Metadata\global-metadata.dat"
)
# smoke 必过：登录/战斗/传送/无敌/踢线。进店、F6、旅行等缺组不算红灯。
SMOKE_REQ_TAGS = frozenset(
    {
        "Il2CppBind",
        "AutoEnter",
        "PlayerCombat",
        "Attack",
        "Teleport",
        "Invuln",
        "KickSniff",
    }
)

RE_HASH = re.compile(r"[a-f0-9]{60,64}")
RE_TOKEN_HASH = re.compile(r"\b[a-f0-9]{60,64}\b")
RE_TOKEN_RVA = re.compile(r"\b0x([0-9A-Fa-f]{5,8})\b")
RE_RVA_LINE = re.compile(r"RVA:\s*0x([0-9A-Fa-f]+)")
RE_TDI = re.compile(r"TypeDefIndex:\s*(\d+)")
RE_CLASS = re.compile(r"\b(class|struct|enum|interface)\s+([A-Za-z0-9_.<>-]+)")
RE_NS = re.compile(r"^//\s*Namespace:\s*(.*)$")
RE_METH_BEFORE_PAREN = re.compile(
    r"(\.[A-Za-z]+|[A-Za-z_][A-Za-z0-9_]*|[a-f0-9]{60,64})\s*(?:<[^<>]*>)?\s*\("
)
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
RE_KRVA_NAMED_ASSIGN = re.compile(
    r"(^[ \t]*constexpr\s+(?:const\s+)?(?:uint32_t|uintptr_t|unsigned(?:\s+int)?)\s+"
    r"(kRva\w+)\s*=\s*0x)([0-9A-Fa-f]+)(u?)",
    re.I | re.M,
)
RE_KHASH_DECL = re.compile(
    r"constexpr\s+char\s+kHash(\w+)\[\]\s*=\s*(?:\n\s*)?\"(?:<)?([a-f0-9]{60,64})",
    re.M,
)
RE_KFB_NAMED_ASSIGN = re.compile(
    r"(^[ \t]*constexpr\s+(?:const\s+)?(?:size_t|uint32_t|uintptr_t|unsigned(?:\s+int)?)\s+"
    r"(kFb\w+)\s*=\s*0x)([0-9A-Fa-f]+)(u?)",
    re.I | re.M,
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
    """Only line-start constexpr kRva* = 0x... (skip comments / seeds / masks)."""
    out: list[tuple[str, int, str]] = []
    files = list(xdir.rglob("*.cpp")) + list(xdir.rglob("*.h"))
    for fp in files:
        rel = str(fp.relative_to(ROOT)).replace("\\", "/")
        txt = fp.read_text(encoding="utf-8", errors="replace")
        for m in RE_KRVA_NAMED_ASSIGN.finditer(txt):
            out.append((m.group(2), int(m.group(3), 16), rel))
    return out


def catalog_cpp_names(rec: dict, by_rva: dict[int, list[str]], old_rva: int) -> list[str]:
    raw = (rec.get("cpp") or "").strip()
    if raw:
        names = [
            p.strip()
            for p in re.split(r"[\s,]+", raw)
            if p.strip().startswith("kRva")
        ]
        if names:
            return names
    return list(by_rva.get(old_rva) or [])


def catalog_skip_sets(cat_path: Path) -> tuple[set[int], set[str]]:
    """体内点 RVA 与 cpp 列 kRva 名：apply-krva 禁止走 rva_map。"""
    rvas: set[int] = set()
    names: set[str] = set()
    if not cat_path.is_file():
        return rvas, names
    for rec in load_catalog(cat_path):
        if rec.get("rva_i"):
            rvas.add(int(rec["rva_i"]))
        for n in catalog_cpp_names(rec, {}, rec.get("rva_i") or 0):
            names.add(n.lower())
    return rvas, names


def skip_apply_krva(name: str, rva: int, cat_rvas: set[int], cat_names: set[str]) -> bool:
    nl = name.lower()
    if "seed" in nl:
        return True
    return nl in cat_names or rva in cat_rvas


def patch_named_const_decls(
    xdir: Path, patches: dict[str, tuple[int, int]], rx: re.Pattern
) -> tuple[int, int, list[str]]:
    """只改行首 constexpr Name = 0x…，不改注释/裸 0xHEX。
    patches[name]=(old,new)。当前值已是 new 则跳过；既不是 old 也不是 new 则 WARN 不写。
    """
    if not patches:
        return 0, 0, []
    files = list(xdir.rglob("*.cpp")) + list(xdir.rglob("*.h"))
    n_files = 0
    n_decl = 0
    warns: list[str] = []
    seen: set[str] = set()
    lower = {k.lower(): k for k in patches}

    def sub(m: re.Match) -> str:
        nonlocal n_decl
        name = m.group(2)
        key = name if name in patches else lower.get(name.lower())
        if not key:
            return m.group(0)
        old, new = patches[key]
        cur = int(m.group(3), 16)
        seen.add(key)
        if cur == new:
            return m.group(0)
        if cur != old:
            warns.append(
                "%s have 0x%X (not old 0x%X / new 0x%X)" % (key, cur, old, new)
            )
            return m.group(0)
        src = m.group(3)
        fmt = "%X" % new if any(c.isupper() for c in src) else "%x" % new
        n_decl += 1
        return m.group(1) + fmt + m.group(4)

    for fp in files:
        txt = fp.read_text(encoding="utf-8", errors="replace")
        orig = txt
        txt = rx.sub(sub, txt)
        if txt != orig:
            fp.write_text(txt, encoding="utf-8", newline="\n")
            n_files += 1
            print("patched", str(fp.relative_to(ROOT)).replace("\\", "/"))
    for name in patches:
        if name not in seen:
            warns.append("%s constexpr not found" % name)
    return n_files, n_decl, warns


def patch_named_krva_decls(
    xdir: Path, patches: dict[str, tuple[int, int]]
) -> tuple[int, int, list[str]]:
    return patch_named_const_decls(xdir, patches, RE_KRVA_NAMED_ASSIGN)


def patch_named_kfb_decls(
    xdir: Path, patches: dict[str, tuple[int, int]]
) -> tuple[int, int, list[str]]:
    return patch_named_const_decls(xdir, patches, RE_KFB_NAMED_ASSIGN)


def load_ignore(path: Path) -> set[str]:
    if not path.is_file():
        return set()
    s: set[str] = set()
    for ln in path.read_text(encoding="utf-8").splitlines():
        ln = ln.split("#", 1)[0].strip().lower()
        if ln:
            s.add(ln)
    return s


def load_krva_bind(path: Path) -> dict[str, list[dict]]:
    """kRvaName → 多行（同名不同 RVA，如 GoSetActive vs set_active）。"""
    out: dict[str, list[dict]] = defaultdict(list)
    if not path.is_file():
        return {}
    for ln in path.read_text(encoding="utf-8").splitlines():
        raw = ln.split("#", 1)[0].strip()
        if not raw or raw.startswith("name\t"):
            continue
        parts = raw.split("\t")
        if len(parts) < 3 or not parts[0].startswith("kRva"):
            continue
        name = parts[0]
        if parts[1] in ("hash", "plain"):
            kind = parts[1]
            ident = parts[2]
            rva_s = parts[3] if len(parts) > 3 else ""
            shared = parts[4] == "1" if len(parts) > 4 else False
        elif RE_HASH.fullmatch(parts[1]):
            kind, ident, rva_s = "hash", parts[1], parts[2]
            shared = False
        else:
            continue
        try:
            rva_i = int(rva_s, 16) if rva_s else 0
        except ValueError:
            rva_i = 0
        out[name].append({"kind": kind, "ident": ident, "rva": rva_i, "shared": shared})
    return dict(out)


def write_krva_bind(path: Path, rows: list[tuple[str, str, str, str, str]]) -> None:
    """rows: name, kind, ident, code_rva, shared('0'|'1'). 同名不同 RVA 都保留。"""
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = ["name\tkind\tident\tcode_rva\tshared"]
    seen: set[tuple[str, str]] = set()
    for name, kind, ident, rva, shared in rows:
        key = (name, rva)
        if key in seen or not ident:
            continue
        seen.add(key)
        lines.append("%s\t%s\t%s\t%s\t%s" % (name, kind, ident, rva, shared))
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def pick_bind_rec(bind: dict[str, list[dict]], name: str, old_rva: int) -> dict | None:
    recs = bind.get(name) or []
    for rec in recs:
        if rec.get("rva") == old_rva:
            return rec
    if len(recs) == 1:
        return recs[0]
    return None


def remap_qual_class(qual: str, class_hash: dict[str, str]) -> str:
    def repl(m: re.Match) -> str:
        h = m.group(0)
        return class_hash.get(h, h)

    return RE_HASH.sub(repl, qual)


def resolve_bound_krva(
    rec: dict,
    old_rva: int,
    meth_hash_map: dict[str, str],
    rva_map: dict[int, int],
    new_meth: dict[str, list[int]],
    new_plain: dict[tuple[str, str], list[int]],
    class_hash: dict[str, str],
) -> int | None:
    """按 bind 身份解新 RVA。shared 哈希/明文重载禁止「只剩一个头就全改过去」。"""
    kind = rec.get("kind")
    ident = rec.get("ident") or ""
    shared = bool(rec.get("shared"))
    dump_rvas: list[int] = []
    if kind == "hash":
        nhash = meth_hash_map.get(ident, ident)
        dump_rvas = list(new_meth.get(nhash) or [])
    elif kind == "plain" and "::" in ident:
        qual, meth = ident.split("::", 1)
        nqual = remap_qual_class(qual, class_hash)
        dump_rvas = list(new_plain.get((nqual, meth)) or [])
        if not dump_rvas:
            dump_rvas = list(new_plain.get((nqual.split(".")[-1], meth)) or [])
    else:
        return None
    if not dump_rvas:
        return None
    if len(dump_rvas) == 1 and not shared:
        return dump_rvas[0]
    mapped = rva_map.get(old_rva)
    if mapped in dump_rvas:
        return mapped
    if old_rva in dump_rvas:
        return old_rva
    return None


def count_krva_would_rewrite(
    xdir: Path,
    bind: dict[str, list[dict]],
    meth_hash_map: dict[str, str],
    rva_map: dict[int, int],
    new_meth: dict[str, list[int]],
    new_plain: dict[tuple[str, str], list[int]],
    class_hash: dict[str, str],
    new_rvas: set[int],
    collide: set[int],
    cat_rvas: set[int],
    cat_names: set[str],
) -> tuple[int, int]:
    """与 --apply-krva 同一套规则，只计数不写。"""
    n = 0
    scanned = 0
    files = list(xdir.rglob("*.cpp")) + list(xdir.rglob("*.h"))
    for fp in files:
        txt = fp.read_text(encoding="utf-8", errors="replace")
        for m in RE_KRVA_NAMED_ASSIGN.finditer(txt):
            scanned += 1
            v = int(m.group(3), 16)
            name = m.group(2)
            if skip_apply_krva(name, v, cat_rvas, cat_names):
                continue
            nv: int | None = None
            rec = pick_bind_rec(bind, name, v) if name else None
            if rec:
                nv = resolve_bound_krva(
                    rec, v, meth_hash_map, rva_map, new_meth, new_plain, class_hash
                )
            if nv is None:
                if v in collide or v in new_rvas:
                    continue
                if v in rva_map and rva_map[v] != v:
                    nv = rva_map[v]
                else:
                    continue
            if nv != v:
                n += 1
                if n <= 8:
                    print(
                        "would  %s  0x%X -> 0x%X  %s"
                        % (name, v, nv, str(fp.relative_to(ROOT)).replace("\\", "/"))
                    )
    return n, scanned


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

    bind = load_krva_bind(DEFAULT_KRVA_BIND)
    cat_rvas, cat_names = catalog_skip_sets(DEFAULT_CATALOG)
    slots_new = index_slots(new_path)
    new_meth = slots_new.get("methods") or {}
    new_plain = slots_new.get("plain_methods") or {}
    collide = set(r_collide)
    would, scanned = count_krva_would_rewrite(
        xdir,
        bind,
        meth_hash_map,
        rva_map,
        new_meth,
        new_plain,
        class_hash,
        new_rvas,
        collide,
        cat_rvas,
        cat_names,
    )
    print("would apply-krva rewrites=%d / scanned=%d" % (would, scanned))

    apply_all_hex = bool(getattr(args, "apply_all_hex", False))
    apply_hashes = bool(getattr(args, "apply_hashes", False) or args.apply)
    apply_krva = bool(getattr(args, "apply_krva", False) or args.apply)
    if apply_all_hex:
        apply_hashes = True
        apply_krva = True
    if not (apply_hashes or apply_krva or apply_all_hex):
        print("dry-run（--apply-hashes / --apply-krva / --apply 才写入；--apply 不再改裸 0xHEX）")
        return 0

    if apply_all_hex and on_old == 0 and on_new > 0:
        print(
            "REFUSE --apply-all-hex: source hashes already on NEW dump; "
            "blind RVA rewrite would hit live methods (see _ga_remount_rva_collision.tsv)"
        )
        return 2

    if apply_hashes and on_old == 0:
        print("skip hashes: already on NEW dump (on_old_map=0)")
        apply_hashes = False

    collide = set(r_collide)
    hash_repl = {}
    if apply_hashes:
        hash_repl.update({h: class_hash[h] for h in by["class"]})
        hash_repl.update({h: field_hash_map[h] for h in by["field"]})
        hash_repl.update({h: meth_hash_map[h] for h in by["meth"]})

    files = list(xdir.rglob("*.cpp")) + list(xdir.rglob("*.h"))
    changed = 0
    krva_n = 0
    hex_n = 0
    for fp in files:
        txt = fp.read_text(encoding="utf-8", errors="replace")
        orig = txt
        for oh, nh in hash_repl.items():
            if oh in txt:
                txt = txt.replace(oh, nh)

        if apply_krva:

            def krva_sub(m: re.Match) -> str:
                nonlocal krva_n
                v = int(m.group(3), 16)
                name = m.group(2)
                if skip_apply_krva(name, v, cat_rvas, cat_names):
                    return m.group(0)
                nv: int | None = None
                rec = pick_bind_rec(bind, name, v) if name else None
                if rec:
                    nv = resolve_bound_krva(
                        rec, v, meth_hash_map, rva_map, new_meth, new_plain, class_hash
                    )
                if nv is None:
                    if v in collide or v in new_rvas:
                        return m.group(0)
                    if v in rva_map and rva_map[v] != v:
                        nv = rva_map[v]
                    else:
                        return m.group(0)
                if nv == v:
                    return m.group(0)
                src = m.group(3)
                fmt = "%X" % nv if any(c.isupper() for c in src) else "%x" % nv
                krva_n += 1
                return m.group(1) + fmt + m.group(4)

            txt = RE_KRVA_NAMED_ASSIGN.sub(krva_sub, txt)

        if apply_all_hex:

            def rva_sub(m: re.Match) -> str:
                nonlocal hex_n
                v = int(m.group(1), 16)
                if v in rva_map and rva_map[v] != v:
                    if v in new_rvas:
                        return m.group(0)
                    src = m.group(1)
                    nv = rva_map[v]
                    fmt = "%X" % nv if any(c.isupper() for c in src) else "%x" % nv
                    hex_n += 1
                    return "0x" + fmt
                return m.group(0)

            txt = RE_TOKEN_RVA.sub(rva_sub, txt)

        if txt != orig:
            fp.write_text(txt, encoding="utf-8", newline="\n")
            changed += 1
            print("patched", fp.relative_to(ROOT))
    print(
        "patched files=%d hash_keys=%d krva_rewrites=%d all_hex_rewrites=%d"
        % (changed, len(hash_repl), krva_n, hex_n)
    )
    return 0


# ----- layout (equal-offset / kFb stale) -----

def field_decl_type(ln: str) -> str:
    decl = ln.split("//")[0].strip().rstrip(";").strip()
    if not decl:
        return ""
    bits = decl.rsplit(None, 1)
    return bits[0] if len(bits) == 2 else decl


def meth_ident_from_decl(ln: str) -> str | None:
    decl = ln.split("//")[0]
    m = RE_METH_BEFORE_PAREN.search(decl)
    return m.group(1) if m else None


def index_slots(path: Path) -> dict:
    """One pass: class hashes, method hashes, plaintext Class::Method, field hash → off/kind/type."""
    classes: set[str] = set()
    methods: dict[str, list[int]] = defaultdict(list)
    rva_hashes: dict[int, list[str]] = defaultdict(list)
    plain_methods: dict[tuple[str, str], list[int]] = defaultdict(list)
    rva_plain: dict[int, list[tuple[str, str]]] = defaultdict(list)
    structs: set[str] = set()
    fields: dict[str, list[dict]] = defaultdict(list)
    cur_name = ""
    cur_ns = ""
    cur_tdi = -1
    pending_rva = None
    for ln in path.read_text(encoding="utf-8", errors="replace").splitlines():
        nsm = RE_NS.match(ln.strip()) if ln.lstrip().startswith("//") else None
        if nsm:
            cur_ns = nsm.group(1).strip()
        tm = RE_TDI.search(ln)
        cm = RE_CLASS.search(ln)
        if tm and cm:
            cur_tdi = int(tm.group(1))
            cur_name = cm.group(2)
            tail = cur_name.split(".")[-1]
            if RE_HASH.fullmatch(tail):
                classes.add(tail)
            if cm.group(1) == "struct":
                structs.add(tail)
            pending_rva = None
            continue
        rm = RE_RVA_LINE.search(ln)
        if rm and ln.strip().startswith("//"):
            pending_rva = int(rm.group(1), 16)
            continue
        if pending_rva is not None and "(" in ln:
            ident = meth_ident_from_decl(ln)
            rva = pending_rva
            pending_rva = None
            if ident:
                if RE_HASH.fullmatch(ident):
                    if rva not in methods[ident]:
                        methods[ident].append(rva)
                    if ident not in rva_hashes[rva]:
                        rva_hashes[rva].append(ident)
                else:
                    qual = ("%s.%s" % (cur_ns, cur_name)) if cur_ns else cur_name
                    key = (qual, ident)
                    if rva not in plain_methods[key]:
                        plain_methods[key].append(rva)
                    if key not in rva_plain[rva]:
                        rva_plain[rva].append(key)
                    tail_key = (cur_name.split(".")[-1], ident)
                    if tail_key != key:
                        tlst = plain_methods[tail_key]
                        if rva not in tlst:
                            tlst.append(rva)
            continue
        pending_rva = None
        fm = RE_FIELD_OFF.search(ln)
        if not (fm and ";" in ln):
            continue
        off = int(fm.group(1), 16)
        fh = None
        bm = re.search(r"<([a-f0-9]{60,64})>k__BackingField", ln)
        if bm:
            fh = bm.group(1)
        else:
            hs = RE_HASH.findall(ln.split("//")[0])
            fh = hs[-1] if hs else None
        if not fh:
            continue
        ty = field_decl_type(ln)
        fields[fh].append(
            {
                "off": off,
                "kind": kind_of_csharp(ty, structs),
                "ty": re.sub(r"\s+", " ", ty)[:80],
                "class": cur_name.split(".")[-1][:20],
                "tdi": cur_tdi,
            }
        )
    return {
        "classes": classes,
        "methods": methods,
        "rva_hashes": dict(rva_hashes),
        "plain_methods": dict(plain_methods),
        "rva_plain": dict(rva_plain),
        "fields": dict(fields),
    }


def collect_khash_kfb(xdir: Path) -> tuple[dict[str, str], dict[str, int], dict[str, list[str]]]:
    """kHashFoo / kFbFoo 按后缀配对。hash → suffix；suffix → kFb off；hash → files."""
    suffix_hash: dict[str, str] = {}
    suffix_fb: dict[str, int] = {}
    hash_files: dict[str, list[str]] = defaultdict(list)
    files = list(xdir.rglob("*.cpp")) + list(xdir.rglob("*.h"))
    for fp in files:
        rel = str(fp.relative_to(ROOT)).replace("\\", "/")
        txt = fp.read_text(encoding="utf-8", errors="replace")
        for m in RE_KHASH_DECL.finditer(txt):
            suffix, h = m.group(1), m.group(2)
            suffix_hash[suffix] = h
            if rel not in hash_files[h]:
                hash_files[h].append(rel)
        for m in RE_KFB_NAMED_ASSIGN.finditer(txt):
            name = m.group(2)
            suffix = name[3:] if name.lower().startswith("kfb") else name
            suffix_fb[suffix] = int(m.group(3), 16)
    hash_fb: dict[str, int] = {}
    for suffix, h in suffix_hash.items():
        if suffix in suffix_fb:
            hash_fb[h] = suffix_fb[suffix]
    return suffix_hash, hash_fb, dict(hash_files)


def pick_slot(slots: list[dict] | None) -> dict | None:
    if not slots:
        return None
    if len(slots) == 1:
        return slots[0]
    offs = {s["off"] for s in slots}
    if len(offs) == 1:
        return slots[0]
    return None  # ambiguous offsets


def cmd_layout(args: argparse.Namespace) -> int:
    legacy = load_legacy()
    old_path = Path(args.old) if args.old else default_old_dump()
    new_path = Path(args.new)
    out_dir = Path(args.out_dir)
    xdir = Path(args.x_dir)
    ignore = load_ignore(Path(args.ignore))
    do_write = bool(getattr(args, "write", False))
    out_dir.mkdir(parents=True, exist_ok=True)

    print("old", old_path)
    print("new", new_path)
    print("parse dumps (class map + field types)…")
    old = legacy.parse_dump(old_path)
    new = legacy.parse_dump(new_path)
    hashes_in_code, _rvas = collect_x_tokens(xdir)
    tdi_map = legacy.match_classes(old, new, prefer_names=hashes_in_code)
    _ch, field_hash_map, _mh, _rva, _conf = build_maps(
        legacy, old, new, tdi_map, set()
    )
    rev_field = {v: k for k, v in field_hash_map.items() if v not in field_hash_map}
    old_slots = index_slots(old_path)
    new_slots = index_slots(new_path)
    _suffix_hash, hash_fb, hash_files = collect_khash_kfb(xdir)

    def classify_hash(h: str) -> str:
        in_f = h in old_slots["fields"] or h in new_slots["fields"]
        in_c = h in old_slots["classes"] or h in new_slots["classes"]
        in_m = h in old_slots["methods"] or h in new_slots["methods"]
        if in_f:
            return "field"
        if in_c:
            return "class"
        if in_m:
            return "method"
        return "unknown"

    rows = []
    fail_n = 0
    kfb_patches: dict[str, tuple[int, int]] = {}
    block_write = 0
    counts: Counter[str] = Counter()

    def emit(verdict: str, h: str, **kw) -> None:
        nonlocal fail_n
        hard = any(
            p in verdict.split("|")
            for p in ("TYPE_FLIP", "MOVED_TYPE", "FALLBACK_STALE", "DEAD", "AMBIGUOUS")
        )
        if hard:
            fail_n += 1
        counts[verdict.split("|")[0]] += 1
        src = ";".join(hash_files.get(h, [])[:3])
        old_s = kw.get("old")
        new_s = kw.get("new")
        kfb = kw.get("kfb")
        rows.append(
            "\t".join(
                [
                    verdict,
                    h,
                    src,
                    ("0x%X" % old_s["off"]) if old_s else "",
                    ("0x%X" % new_s["off"]) if new_s else "",
                    (old_s["kind"] if old_s else ""),
                    (new_s["kind"] if new_s else ""),
                    (old_s["ty"] if old_s else ""),
                    (new_s["ty"] if new_s else ""),
                    ("0x%X" % kfb) if kfb is not None else "",
                    (new_s["class"] if new_s else (old_s["class"] if old_s else "")),
                ]
            )
        )
        if hard or verdict.startswith("MOVED") or verdict.startswith("WARN"):
            print(
                "%s  %s  old=%s/%s  new=%s/%s  kFb=%s  %s"
                % (
                    verdict,
                    h[:16],
                    ("0x%X" % old_s["off"]) if old_s else "-",
                    old_s["kind"] if old_s else "-",
                    ("0x%X" % new_s["off"]) if new_s else "-",
                    new_s["kind"] if new_s else "-",
                    ("0x%X" % kfb) if kfb is not None else "-",
                    src.split("/")[-1] if src else "",
                )
            )

    field_hashes = []
    skipped = Counter()
    for h in sorted(hashes_in_code):
        if h.lower() in ignore:
            skipped["ignore"] += 1
            continue
        kind = classify_hash(h)
        if kind != "field":
            skipped[kind] += 1
            continue
        field_hashes.append(h)

    for h in field_hashes:
        new_list = new_slots["fields"].get(h)
        old_list = old_slots["fields"].get(h)
        mapped_old = rev_field.get(h)
        mapped_new = field_hash_map.get(h)
        if not old_list and mapped_old:
            old_list = old_slots["fields"].get(mapped_old)
        if not new_list and mapped_new:
            new_list = new_slots["fields"].get(mapped_new)
        new_s = pick_slot(new_list)
        old_s = pick_slot(old_list)
        kfb = hash_fb.get(h)
        if kfb is None and mapped_old:
            kfb = hash_fb.get(mapped_old)

        flags = []
        if (new_list and pick_slot(new_list) is None) or (
            old_list and pick_slot(old_list) is None
        ):
            flags.append("AMBIGUOUS")
        if not new_list and old_s:
            flags.append("DEAD")
        if old_s and new_s:
            moved = old_s["off"] != new_s["off"]
            flipped = old_s["kind"] != new_s["kind"]
            if moved and flipped:
                flags.append("MOVED_TYPE")
            elif flipped:
                flags.append("TYPE_FLIP")
            elif moved:
                flags.append("MOVED")
        if new_s and kfb is not None and kfb != new_s["off"]:
            flags.append("FALLBACK_STALE")
        suffix_hit = [s for s, hh in _suffix_hash.items() if hh == h]
        # CurFh 是对象指针，RelPos dump 常写成 class 名；只盯镜头/矩形槽被接到数组/字典。
        if new_s and new_s["kind"] == "Ptr" and suffix_hit:
            suf = suffix_hit[0].lower()
            if suf in ("logicalpos", "curpos", "vispos", "pos") and "Vector" not in new_s["ty"]:
                flags.append("WARN_PTR_POS")
        hard_block = [f for f in flags if f in ("TYPE_FLIP", "MOVED_TYPE", "AMBIGUOUS", "DEAD")]
        if hard_block:
            block_write += 1
        elif "FALLBACK_STALE" in flags and new_s is not None and kfb is not None:
            for s in suffix_hit:
                kfb_patches["kFb" + s] = (kfb, new_s["off"])
        if not flags:
            flags.append("OK")
            counts["OK"] += 1
            # OK 行仍进 tsv，不刷屏
            src = ";".join(hash_files.get(h, [])[:3])
            rows.append(
                "\t".join(
                    [
                        "OK",
                        h,
                        src,
                        ("0x%X" % old_s["off"]) if old_s else "",
                        ("0x%X" % new_s["off"]) if new_s else "",
                        (old_s["kind"] if old_s else ""),
                        (new_s["kind"] if new_s else ""),
                        (old_s["ty"] if old_s else ""),
                        (new_s["ty"] if new_s else ""),
                        ("0x%X" % kfb) if kfb is not None else "",
                        (new_s["class"] if new_s else ""),
                    ]
                )
            )
            continue
        emit("|".join(flags), h, old=old_s, new=new_s, kfb=kfb)

    hdr = "verdict\thash\tsrc\told_off\tnew_off\told_kind\tnew_kind\told_ty\tnew_ty\tkfb\tclass"
    outp = out_dir / "_ga_remount_layout.tsv"
    outp.write_text(hdr + "\n" + "\n".join(rows) + "\n", encoding="utf-8")
    print(
        "fields=%d skip=%s counts=%s FAIL=%d would_patch_kfb=%d write=%s block=%d"
        % (
            len(field_hashes),
            dict(skipped),
            dict(counts),
            fail_n,
            len(kfb_patches),
            do_write,
            block_write,
        )
    )
    print("wrote", outp)
    if do_write:
        if block_write:
            print("REFUSE --write (TYPE_FLIP/MOVED_TYPE/DEAD/AMBIGUOUS)")
            return 1
        n_files, n_decl, warns = patch_named_kfb_decls(xdir, kfb_patches)
        print("wrote kFb files=%d decls=%d" % (n_files, n_decl))
        for w in warns:
            print("WARN  kfb  %s" % w)
    elif kfb_patches:
        print("dry-run: pass --write to update constexpr kFb* (not comments)")
    return 1 if fail_n else 0


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
        pr = (rec.get("parent_rva") or "").strip()
        rec["parent_rva_i"] = int(pr, 16) if pr else 0
        ao = (rec.get("anchor_off") or "").strip()
        rec["anchor_off_i"] = int(ao, 0) if ao else 0
        rows.append(rec)
    return rows


def parse_masked_hex(s: str) -> tuple[bytes, bytes]:
    """Hex bytes with ?? wildcards (RIP/call rel32)."""
    pat: list[int] = []
    mask: list[int] = []
    for t in s.replace(",", " ").split():
        t = t.strip()
        if not t:
            continue
        if t in ("??", "?"):
            pat.append(0)
            mask.append(0)
        else:
            pat.append(int(t, 16))
            mask.append(1)
    return bytes(pat), bytes(mask)


def find_masked(blob: bytes, pat: bytes, mask: bytes) -> list[int]:
    n = len(pat)
    if n == 0 or len(blob) < n:
        return []
    hits: list[int] = []
    for i in range(0, len(blob) - n + 1):
        ok = True
        for j in range(n):
            if mask[j] and blob[i + j] != pat[j]:
                ok = False
                break
        if ok:
            hits.append(i)
    return hits


def catalog_method_heads(slots: dict) -> list[int]:
    s: set[int] = set()
    for rvas in (slots.get("methods") or {}).values():
        s.update(rvas)
    for rvas in (slots.get("plain_methods") or {}).values():
        s.update(rvas)
    return sorted(s)


def method_window(heads: list[int], head: int) -> tuple[int, int]:
    """[head, next dump method RVA). Last method caps at +0x8000."""
    i = bisect.bisect_right(heads, head)
    nxt = heads[i] if i < len(heads) else head + 0x8000
    if nxt <= head:
        nxt = head + 0x8000
    return head, nxt


def resolve_catalog_ident(
    ident: str,
    old_rva: int,
    slots: dict,
    bind: dict[str, list[dict]],
    rva_map: dict[int, int],
    meth_hash_map: dict[str, str],
    class_hash: dict[str, str],
    heads: set[int],
) -> int | None:
    ident = (ident or "").strip()
    new_meth = slots.get("methods") or {}
    new_plain = slots.get("plain_methods") or {}
    if ident.startswith("kRva"):
        rec = pick_bind_rec(bind, ident, old_rva)
        if rec:
            got = resolve_bound_krva(
                rec, old_rva, meth_hash_map, rva_map, new_meth, new_plain, class_hash
            )
            if got:
                return got
    h = ident
    if ident.lower().startswith("hash:"):
        h = ident.split(":", 1)[1].strip()
    if RE_HASH.fullmatch(h):
        nh = meth_hash_map.get(h, h)
        rvas = list(new_meth.get(nh) or [])
        if len(rvas) == 1:
            return rvas[0]
        mapped = rva_map.get(old_rva)
        if mapped in rvas:
            return mapped
        if old_rva in rvas:
            return old_rva
        return None
    if old_rva:
        mapped = rva_map.get(old_rva, old_rva)
        if mapped in heads:
            return mapped
    return None


def find_e8_to(blob: bytes, lo: int, target: int) -> list[int]:
    hits: list[int] = []
    for i in range(0, len(blob) - 4):
        if blob[i] != 0xE8:
            continue
        rel = struct.unpack_from("<i", blob, i + 1)[0]
        got = (lo + i + 5 + rel) & 0xFFFFFFFF
        if got == target:
            hits.append(lo + i)
    return hits


def find_rip_data_targets(pe: PeImage, lo: int, hi: int, expect: bytes) -> list[int]:
    blob = pe.read(lo, hi - lo)
    if not blob or not expect:
        return []
    n = len(expect)
    found: dict[int, None] = {}
    for i in range(0, len(blob) - 3):
        disp = struct.unpack_from("<i", blob, i)[0]
        tgt = (lo + i + 4 + disp) & 0xFFFFFFFF
        if lo <= tgt < hi or tgt < 0x10000:
            continue
        raw = pe.read(tgt, n)
        if raw == expect:
            found[tgt] = None
    return sorted(found)


def scan_catalog_site(
    pe: PeImage,
    rec: dict,
    parent_lo: int,
    parent_hi: int,
    target_new: int,
) -> dict:
    """Unique hit inside the parent method window. Never grep the whole image."""
    kind = (rec.get("kind") or "").strip().lower()
    exp = rec.get("expect_b") or b""
    blob = pe.read(parent_lo, parent_hi - parent_lo)
    if blob is None:
        return {"ok": False, "hits": [], "why": "cannot read parent window"}
    if kind == "call":
        if not target_new:
            return {"ok": False, "hits": [], "why": "call target unresolved"}
        hits = find_e8_to(blob, parent_lo, target_new)
        if len(hits) != 1:
            return {
                "ok": False,
                "hits": hits,
                "new_target": target_new,
                "why": "E8 hits=%d want unique -> 0x%X" % (len(hits), target_new),
            }
        return {"ok": True, "hits": hits, "new_rva": hits[0], "new_target": target_new}
    if kind == "data":
        if not exp:
            return {"ok": False, "hits": [], "why": "empty expect"}
        hits = find_rip_data_targets(pe, parent_lo, parent_hi, exp)
        if len(hits) != 1:
            return {"ok": False, "hits": hits, "why": "RIP-data hits=%d" % len(hits)}
        return {"ok": True, "hits": hits, "new_rva": hits[0]}
    anchor = (rec.get("anchor") or "").strip()
    off = int(rec.get("anchor_off_i") or 0)
    if anchor:
        pat, mask = parse_masked_hex(anchor)
        if not pat:
            return {"ok": False, "hits": [], "why": "empty anchor"}
        if off < 0 or off >= len(pat):
            return {"ok": False, "hits": [], "why": "anchor_off out of range"}
        hits = [parent_lo + i + off for i in find_masked(blob, pat, mask)]
    else:
        if not exp:
            return {"ok": False, "hits": [], "why": "empty expect (need anchor)"}
        hits = []
        i = 0
        while True:
            j = blob.find(exp, i)
            if j < 0:
                break
            hits.append(parent_lo + j)
            i = j + 1
    if len(hits) != 1:
        return {
            "ok": False,
            "hits": hits,
            "why": "text hits=%d (flatten grep forbidden)" % len(hits),
        }
    site = hits[0]
    if exp:
        got = pe.read(site, len(exp))
        if got != exp:
            return {
                "ok": False,
                "hits": hits,
                "why": "unique hit 0x%X bytes %s want %s"
                % (site, (got or b"").hex(" "), exp.hex(" ")),
            }
    return {"ok": True, "hits": hits, "new_rva": site}


def write_catalog_tsv(path: Path, hdr: list[str], rows: list[dict]) -> None:
    skip = {"rva_i", "expect_b", "target_i", "parent_rva_i", "anchor_off_i"}
    use = [h for h in hdr if h not in skip]
    lines = ["\t".join(use)]
    for rec in rows:
        lines.append("\t".join(str(rec.get(k, "") or "") for k in use))
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def cmd_catalog(args: argparse.Namespace) -> int:
    """Retarget in-body sites by unique hit in the parent method window."""
    dump_path = Path(args.new)
    ga_path = Path(args.ga)
    cat_path = Path(args.catalog)
    out_dir = Path(args.out_dir)
    xdir = Path(args.x_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    do_write = bool(getattr(args, "write", False))

    print("dump", dump_path)
    print("ga  ", ga_path)
    print("cat ", cat_path)
    if not cat_path.is_file():
        print("FAIL  no catalog")
        return 1
    if not ga_path.is_file():
        print("FAIL  no GameAssembly.dll")
        return 1

    slots = index_slots(dump_path)
    heads = catalog_method_heads(slots)
    head_set = set(heads)
    bind = load_krva_bind(DEFAULT_KRVA_BIND)
    pe = PeImage(ga_path)
    catalog = load_catalog(cat_path)
    hdr = cat_path.read_text(encoding="utf-8").splitlines()[0].split("\t")
    krvas = collect_krva(xdir)
    by_rva: dict[int, list[str]] = defaultdict(list)
    by_name: dict[str, int] = {}
    for name, r, _rel in krvas:
        by_rva[r].append(name)
        by_name[name] = r

    rva_map: dict[int, int] = {}
    meth_hash_map: dict[str, str] = {}
    class_hash: dict[str, str] = {}
    old_arg = getattr(args, "old", "") or ""
    if old_arg:
        legacy = load_legacy()
        old_path = Path(old_arg)
        print("old", old_path, "(rva_map fallback only)")
        old = legacy.parse_dump(old_path)
        new = legacy.parse_dump(dump_path)
        hashes_in_code, _rvas = collect_x_tokens(xdir)
        tdi_map = legacy.match_classes(old, new, prefer_names=hashes_in_code)
        class_hash, _fh, meth_hash_map, rva_map, _conf = build_maps(
            legacy, old, new, tdi_map, set()
        )

    fail = 0
    move = 0
    match_n = 0
    stale_n = 0
    changed = False
    cpp_patches: dict[str, tuple[int, int]] = {}
    report = ["tag\tverdict\tkind\told_rva\tnew_rva\tparent\thits\tdetail"]

    for rec in catalog:
        tag = rec.get("tag") or "?"
        kind = rec.get("kind") or ""
        old_rva = rec["rva_i"]
        parent_ident = (rec.get("parent") or "").strip()
        parent_old = rec.get("parent_rva_i") or 0
        if not parent_ident and not parent_old:
            print("FAIL  %s  no parent (will not flatten-grep)" % tag)
            fail += 1
            report.append(
                "\t".join([tag, "FAIL", kind, "0x%X" % old_rva, "", "", "0", "no parent"])
            )
            continue
        parent_new = resolve_catalog_ident(
            parent_ident,
            parent_old,
            slots,
            bind,
            rva_map,
            meth_hash_map,
            class_hash,
            head_set,
        )
        if not parent_new:
            print("FAIL  %s  parent unresolved %s @0x%X" % (tag, parent_ident, parent_old))
            fail += 1
            report.append(
                "\t".join(
                    [
                        tag,
                        "FAIL",
                        kind,
                        "0x%X" % old_rva,
                        "",
                        parent_ident,
                        "0",
                        "parent unresolved",
                    ]
                )
            )
            continue
        lo, hi = method_window(heads, parent_new)
        target_new = 0
        target_id = (rec.get("target_id") or "").strip()
        if kind.lower() == "call":
            target_new = (
                resolve_catalog_ident(
                    target_id,
                    rec.get("target_i") or 0,
                    slots,
                    bind,
                    rva_map,
                    meth_hash_map,
                    class_hash,
                    head_set,
                )
                or 0
            )
            if not target_new and rec.get("target_i"):
                mapped = rva_map.get(rec["target_i"], rec["target_i"])
                if mapped in head_set:
                    target_new = mapped
        got = scan_catalog_site(pe, rec, lo, hi, target_new)
        hits = got.get("hits") or []
        if not got.get("ok"):
            print(
                "FAIL  %s  %s  parent=[0x%X,0x%X)  %s"
                % (tag, got.get("why"), lo, hi, ",".join("0x%X" % h for h in hits[:8]))
            )
            fail += 1
            report.append(
                "\t".join(
                    [
                        tag,
                        "FAIL",
                        kind,
                        "0x%X" % old_rva,
                        "",
                        "0x%X" % parent_new,
                        str(len(hits)),
                        got.get("why") or "",
                    ]
                )
            )
            continue
        new_rva = int(got["new_rva"])
        new_tgt = int(got.get("new_target") or 0)
        moved = new_rva != old_rva or (
            kind.lower() == "call" and new_tgt and new_tgt != (rec.get("target_i") or 0)
        )
        verdict = "MOVE" if moved else "MATCH"
        if moved:
            move += 1
            changed = True
        else:
            match_n += 1
        cpp_names = catalog_cpp_names(rec, by_rva, old_rva)
        if cpp_names:
            rec["cpp"] = ",".join(cpp_names)
        extra = "parent=0x%X" % parent_new
        if new_tgt:
            extra += " tgt=0x%X" % new_tgt
        if cpp_names:
            extra += " cpp=%s" % ",".join(cpp_names)
            for nm in cpp_names:
                code = by_name.get(nm)
                if code is None:
                    print("WARN  cpp %s constexpr not found" % nm)
                    cpp_patches[nm] = (old_rva, new_rva)
                elif code != new_rva:
                    print("STALE %s constexpr=0x%X site=0x%X" % (nm, code, new_rva))
                    cpp_patches[nm] = (code, new_rva)
                    stale_n += 1
                    changed = True
                    if verdict == "MATCH":
                        verdict = "STALE"
                else:
                    cpp_patches[nm] = (old_rva, new_rva)
                    if new_rva == old_rva:
                        print("cpp   %s  0x%X (same)" % (nm, new_rva))
                    else:
                        print("cpp   %s  0x%X -> 0x%X" % (nm, old_rva, new_rva))
        print("%s  %s  0x%X -> 0x%X  %s" % (verdict, tag, old_rva, new_rva, extra))
        report.append(
            "\t".join(
                [
                    tag,
                    verdict,
                    kind,
                    "0x%X" % old_rva,
                    "0x%X" % new_rva,
                    "0x%X" % parent_new,
                    "1",
                    extra,
                ]
            )
        )
        rec["rva"] = "0x%X" % new_rva
        rec["rva_i"] = new_rva
        rec["parent_rva"] = "0x%X" % parent_new
        rec["parent_rva_i"] = parent_new
        if new_tgt:
            rec["target"] = "0x%X" % new_tgt
            rec["target_i"] = new_tgt

    outp = out_dir / "_ga_remount_catalog.tsv"
    outp.write_text("\n".join(report) + "\n", encoding="utf-8")
    print("wrote", outp)
    cpp_move = sum(1 for old, new in cpp_patches.values() if old != new)
    print(
        "catalog MATCH=%d MOVE=%d STALE=%d FAIL=%d cpp=%d would_patch=%d write=%s"
        % (match_n, move, stale_n, fail, len(cpp_patches), cpp_move, do_write)
    )
    if do_write:
        if fail:
            print("REFUSE --write (FAIL>0)")
            return 1
        want = [
            "tag",
            "kind",
            "rva",
            "expect",
            "target",
            "source",
            "cpp",
            "note",
            "parent",
            "parent_rva",
            "target_id",
            "anchor",
            "anchor_off",
        ]
        for col in want:
            if col not in hdr:
                hdr.append(col)
        write_catalog_tsv(cat_path, hdr, catalog)
        print("wrote catalog", cat_path)
        moving = {n: p for n, p in cpp_patches.items() if p[0] != p[1]}
        n_files, n_decl, warns = patch_named_krva_decls(xdir, moving)
        print("wrote kRva files=%d decls=%d" % (n_files, n_decl))
        for w in warns:
            print("WARN  cpp  %s" % w)
    elif changed or cpp_move or stale_n:
        print("dry-run: pass --write to update tsv rva/target and constexpr kRva* (not comments)")
    return 1 if fail or (stale_n and not do_write) else 0


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


def cmd_krva(args: argparse.Namespace) -> int:
    """kRva* + 同后缀 kHash* vs dump 方法 RVA（身份，不是「碰巧是方法头」）。"""
    dump_path = Path(args.new)
    xdir = Path(args.x_dir)
    out_dir = Path(args.out_dir)
    cat_path = Path(args.catalog)
    ignore = load_ignore(Path(args.ignore))
    out_dir.mkdir(parents=True, exist_ok=True)

    print("dump", dump_path)
    slots = index_slots(dump_path)
    meth = slots["methods"]
    rva_hashes: dict[int, list[str]] = slots.get("rva_hashes") or {}
    rva_plain: dict[int, list[tuple[str, str]]] = slots.get("rva_plain") or {}
    suffix_hash, _fb, hash_files = collect_khash_kfb(xdir)
    krvas = collect_krva(xdir)
    catalog = load_catalog(cat_path) if cat_path.is_file() else []
    catalog_rvas = {r["rva_i"] for r in catalog}

    rows = []
    bind_rows: list[tuple[str, str, str, str, str]] = []
    fail_n = 0
    counts: Counter[str] = Counter()

    for name, r, rel in krvas:
        key = "0x%x" % r
        suffix = name[4:] if name.lower().startswith("krva") else name
        h = suffix_hash.get(suffix)
        src = ";".join(hash_files.get(h, [])[:2]) if h else rel
        dump_rvas = meth.get(h) if h else None
        in_dump = bool(dump_rvas and r in dump_rvas)
        dump_by_rva = rva_hashes.get(r) or []
        plain_by_rva = rva_plain.get(r) or []
        ident = ""
        kind = ""
        shared = False

        if name.lower() in ignore or key in ignore or ("%x" % r) in ignore:
            verdict = "IGN"
        elif r in catalog_rvas:
            verdict = "CATALOG"
        elif "seed" in name.lower():
            verdict = "SEED"
        elif not h:
            if len(dump_by_rva) == 1:
                verdict = "DUMP_ID"
                h = dump_by_rva[0]
                ident = h
                kind = "hash"
                shared = len(meth.get(h) or []) > 1
            elif dump_by_rva:
                verdict = "DUMP_MULTI"
            elif len(plain_by_rva) == 1:
                verdict = "PLAIN_ID"
                qual, mname = plain_by_rva[0]
                ident = "%s::%s" % (qual, mname)
                kind = "plain"
                shared = len(slots["plain_methods"].get((qual, mname)) or []) > 1
            elif plain_by_rva:
                verdict = "PLAIN_MULTI"
            else:
                verdict = "UNBOUND"
        elif dump_rvas is None:
            verdict = "HASH_MISS"
            fail_n += 1
        elif in_dump:
            verdict = "MATCH"
            ident = h
            kind = "hash"
            shared = len(dump_rvas) > 1
        else:
            verdict = "MISMATCH"
            fail_n += 1

        counts[verdict] += 1
        dump_col = ""
        if dump_rvas:
            dump_col = ",".join("0x%X" % x for x in dump_rvas[:6])
            if len(dump_rvas) > 6:
                dump_col += ",+%d" % (len(dump_rvas) - 6)
        elif ident and kind == "plain":
            dump_col = ident
        elif dump_by_rva:
            dump_col = ",".join(x[:12] + "…" for x in dump_by_rva[:4])
            if len(dump_by_rva) > 4:
                dump_col += ",+%d" % (len(dump_by_rva) - 4)
        elif plain_by_rva:
            dump_col = ",".join("%s::%s" % p for p in plain_by_rva[:4])
        rows.append(
            "\t".join(
                [
                    verdict,
                    name,
                    "0x%X" % r,
                    ident or (h or ""),
                    dump_col,
                    src or rel,
                ]
            )
        )
        if verdict in ("MATCH", "DUMP_ID", "PLAIN_ID") and ident:
            bind_rows.append(
                (name, kind, ident, "0x%X" % r, "1" if shared else "0")
            )
        if verdict in ("MISMATCH", "HASH_MISS"):
            print(
                "%s  %s code=0x%X dump=%s hash=%s  %s"
                % (
                    verdict,
                    name,
                    r,
                    dump_col or "-",
                    (h[:16] + "…") if h else "-",
                    rel,
                )
            )

    hdr = "verdict\tname\tcode_rva\thash\tdump_rva\tsrc"
    outp = out_dir / "_ga_remount_krva.tsv"
    outp.write_text(hdr + "\n" + "\n".join(rows) + "\n", encoding="utf-8")
    write_krva_bind(out_dir / "_ga_remount_krva_bind.tsv", bind_rows)
    committed = load_krva_bind(DEFAULT_KRVA_BIND)
    fresh_ni: dict[tuple[str, str], tuple[str, int]] = {}
    for name, kind, ident, rva_s, _sh in bind_rows:
        try:
            rva_i = int(rva_s, 16)
        except ValueError:
            continue
        if ident:
            fresh_ni[(name, ident)] = (kind, rva_i)
    old_ni: dict[tuple[str, str], dict] = {}
    for name, recs in committed.items():
        for rec in recs:
            ident = rec.get("ident") or ""
            if ident:
                old_ni[(name, ident)] = rec
    bind_ok = bind_stale = bind_new = bind_drop = 0
    shown = 0
    for key, (_kind, rva_i) in sorted(fresh_ni.items()):
        old = old_ni.get(key)
        if old is None:
            bind_new += 1
            if shown < 8:
                print("BIND_NEW  %s  %s  0x%X" % (key[0], key[1][:48], rva_i))
                shown += 1
        elif int(old.get("rva") or 0) != rva_i:
            bind_stale += 1
            if shown < 8:
                print(
                    "BIND_STALE  %s  0x%X -> 0x%X  %s"
                    % (key[0], int(old.get("rva") or 0), rva_i, key[1][:40])
                )
                shown += 1
        else:
            bind_ok += 1
    for key in old_ni:
        if key not in fresh_ni:
            bind_drop += 1
    do_bind = bool(getattr(args, "write_bind", False))
    print(
        "bind vs data: ok=%d stale=%d new=%d drop=%d write_bind=%s"
        % (bind_ok, bind_stale, bind_new, bind_drop, do_bind)
    )
    if (bind_stale or bind_new or bind_drop) and not do_bind:
        print("dry-run: pass --write-bind to update scripts/data/ga_krva_bind.tsv")
    if do_bind and fail_n == 0:
        write_krva_bind(DEFAULT_KRVA_BIND, bind_rows)
        print("wrote bind", DEFAULT_KRVA_BIND)
    print("kRva=%d counts=%s FAIL=%d bind=%d" % (len(krvas), dict(counts), fail_n, len(bind_rows)))
    print("wrote", outp)
    return 1 if fail_n else 0


def cmd_smoke(args: argparse.Namespace) -> int:
    """注入后扫 hits=a/b：最后一次 Il2CppBind 会话；对照 expect；空日志不当绿灯。"""
    log_path = Path(args.log)
    files = smoke_log_files(log_path)
    if not files:
        print("no log", log_path)
        return 2
    last, all_last = collect_smoke_session(files)
    re_hits = re.compile(r"hits=(\d+)/(\d+)")
    fail = 0
    n = 0
    skip = 0

    if getattr(args, "write_expect", False):
        exp_path = Path(args.expect) if getattr(args, "expect", "") else DEFAULT_SMOKE_EXPECT
        old_need: dict[tuple[str, str], str] = {}
        if exp_path.is_file():
            for e in load_smoke_expect(exp_path):
                old_need[(e["tag"], e["prefix"])] = e.get("need") or ""
        rows = ["tag\tprefix\ta\tb\tpath\tneed"]
        for key in sorted(last):
            rec = last[key]
            m = re_hits.search(rec["msg"])
            if not m:
                continue
            prefix = smoke_prefix(rec["msg"])
            path = smoke_path(rec["msg"])
            need = old_need.get((rec["tag"], prefix)) or smoke_need_for_tag(rec["tag"])
            rows.append("\t".join([rec["tag"], prefix, m.group(1), m.group(2), path, need]))
        exp_path.parent.mkdir(parents=True, exist_ok=True)
        exp_path.write_text("\n".join(rows) + "\n", encoding="utf-8")
        print("wrote expect groups=%d %s" % (len(rows) - 1, exp_path))
        return 0

    expect = load_smoke_expect(
        Path(args.expect) if getattr(args, "expect", "") else DEFAULT_SMOKE_EXPECT
    )
    if expect:
        by_key = last
        for exp in expect:
            ekey = "%s|%s" % (exp["tag"], exp["prefix"])
            hit = by_key.get(ekey)
            if hit is None:
                # prefix 已规范化（path=*）；再扫一遍兼容旧 expect
                for rec in last.values():
                    if rec["tag"] == exp["tag"] and smoke_prefix(rec["msg"]) == exp["prefix"]:
                        hit = rec
                        break
            if hit is None:
                if exp.get("need") == "req":
                    hit = all_last.get(ekey)
                    if hit is None:
                        for rec in all_last.values():
                            if rec["tag"] == exp["tag"] and smoke_prefix(rec["msg"]) == exp["prefix"]:
                                hit = rec
                                break
                    if hit is not None:
                        print(
                            "WARN  req prior-session  %s | %s"
                            % (exp["tag"], exp["prefix"][:60])
                        )
            if hit is None:
                if exp.get("need") == "opt":
                    print("SKIP  optional missing  %s | %s" % (exp["tag"], exp["prefix"][:60]))
                    skip += 1
                    continue
                print("FAIL  missing  %s | %s" % (exp["tag"], exp["prefix"][:60]))
                fail += 1
                continue
            m = re_hits.search(hit["msg"])
            if not m:
                if "path=fallback" in hit["msg"] and exp["a"] > 0:
                    print("FAIL  fallback  %s" % hit["msg"][:160])
                    fail += 1
                continue
            a, b = int(m.group(1)), int(m.group(2))
            n += 1
            path = smoke_path(hit["msg"])
            # 允许 hits 变好（a>=expect），槽数 b 必须一致；fallback 且 expect>0 为红
            bad = b != exp["b"] or a < exp["a"]
            if path == "fallback" and exp["a"] > 0:
                bad = True
            if bad:
                print(
                    "FAIL  %s expect>=%d/%d got %s"
                    % (exp["tag"], exp["a"], exp["b"], hit["msg"][:160])
                )
                fail += 1
            elif args.verbose:
                print("OK    %s hits=%d/%d" % (exp["tag"], a, b))
        if n == 0:
            print("FAIL  no hits=a/b in last bind session")
            fail += 1
        print(
            "smoke expect=%d seen=%d skip_opt=%d FAIL=%d files=%d"
            % (len(expect), n, skip, fail, len(files))
        )
        return 1 if fail else 0

    for key, rec in sorted(last.items()):
        msg = rec["msg"]
        m = re_hits.search(msg)
        if not m:
            if "path=fallback" in msg:
                print("FAIL  fallback  %s" % msg[:160])
                fail += 1
            continue
        a, b = int(m.group(1)), int(m.group(2))
        n += 1
        bad = (a < b and "fb-open-generic" not in msg) or (
            "path=fallback" in msg and a == 0 and b > 0
        )
        if bad:
            print("FAIL  %s" % msg[:200])
            fail += 1
        elif args.verbose:
            print("OK    %s" % msg[:200])
    if n == 0:
        print("FAIL  no hits=a/b in last bind session (%s)" % log_path)
        fail += 1
    print("smoke groups=%d FAIL=%d files=%d" % (n, fail, len(files)))
    return 1 if fail else 0


def smoke_log_files(log_path: Path) -> list[Path]:
    """RotatingFileHandler: x.jsonl.N 越大越旧 → x.jsonl.1 → x.jsonl。"""
    parent = log_path.parent if log_path.suffix else log_path
    name = log_path.name if log_path.suffix else "x.jsonl"
    if not parent.is_dir():
        return [log_path] if log_path.is_file() else []
    numbered: list[tuple[int, Path]] = []
    current: list[Path] = []
    for p in parent.glob(name + "*"):
        if not p.is_file():
            continue
        if p.name == name:
            current.append(p)
            continue
        suf = p.name[len(name) + 1 :]
        if suf.isdigit():
            numbered.append((int(suf), p))
    numbered.sort(key=lambda x: -x[0])
    return [p for _, p in numbered] + current


def smoke_prefix(msg: str) -> str:
    prefix = msg.split("hits=")[0]
    prefix = re.sub(r"path=\S+", "path=*", prefix)
    return prefix.strip()[:80]


def smoke_path(msg: str) -> str:
    m = re.search(r"path=([^\s]+)", msg)
    return m.group(1) if m else ""


def collect_smoke_session(files: list[Path]) -> tuple[dict[str, dict], dict[str, dict]]:
    """最后一次 Il2CppBind upgrade 之后的 hits=，以及全日志 last-wins（必过组回退）。"""
    session: dict[str, dict] = {}
    all_last: dict[str, dict] = {}
    for fp in files:
        try:
            text = fp.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for ln in text.splitlines():
            if not ln.startswith("{") and "hits=" not in ln and "Il2CppBind" not in ln:
                continue
            tag = ""
            msg = ln
            if ln.startswith("{"):
                try:
                    rec = json.loads(ln)
                    tag = str(rec.get("tag") or "")
                    msg = str(rec.get("msg") or "")
                except Exception:
                    continue
            if tag == "Il2CppBind" and "unity managed upgrade" in msg:
                session = {}
            if "hits=" not in msg and "path=fallback" not in msg:
                continue
            key = "%s|%s" % (tag, smoke_prefix(msg))
            row = {"tag": tag, "msg": msg if msg else ln, "file": fp.name}
            session[key] = row
            all_last[key] = row
    return session, all_last


def smoke_need_for_tag(tag: str) -> str:
    return "req" if tag in SMOKE_REQ_TAGS else "opt"


def load_smoke_expect(path: Path) -> list[dict]:
    if not path.is_file():
        return []
    out: list[dict] = []
    for ln in path.read_text(encoding="utf-8").splitlines():
        raw = ln.split("#", 1)[0].rstrip("\n")
        if not raw.strip() or raw.startswith("tag\t"):
            continue
        parts = raw.split("\t")
        if len(parts) < 4:
            continue
        try:
            a, b = int(parts[2]), int(parts[3])
        except ValueError:
            continue
        need = parts[5].strip().lower() if len(parts) > 5 else ""
        if need not in ("req", "opt"):
            need = smoke_need_for_tag(parts[0])
        out.append(
            {
                "tag": parts[0],
                "prefix": parts[1],
                "a": a,
                "b": b,
                "path": parts[4] if len(parts) > 4 else "",
                "need": need,
            }
        )
    return out


def file_md5(path: Path) -> str:
    h = hashlib.md5()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def latest_archive_dump_cs(rt: Path) -> Path | None:
    cands = sorted(rt.glob("_archive_*/out/dump.cs"))
    return cands[-1] if cands else None


def dump_archive_old_stamp(rt: Path) -> str:
    latest = latest_archive_dump_cs(rt)
    if latest:
        m = re.search(r"pre_(\d{4,8})", latest.parent.parent.name)
        if m:
            s = m.group(1)
            if len(s) == 4:
                return datetime.now().strftime("%Y") + s
            return s
    dump_cs = rt / "out" / "dump.cs"
    if dump_cs.is_file():
        return datetime.fromtimestamp(dump_cs.stat().st_mtime).strftime("%Y%m%d")
    return datetime.now().strftime("%Y%m%d")


def dump_archive_dest(rt: Path) -> Path:
    old = dump_archive_old_stamp(rt)
    new = datetime.now().strftime("%Y%m%d")
    if old == new:
        new = new + "b"
    return rt / ("_archive_%s_pre_%s_update" % (old, new))


DUMP_ARCHIVE_FILES = (
    ("out/dump.cs", "out/dump.cs"),
    ("GameAssembly.dll", "GameAssembly.dll"),
    ("dump_info.txt", "dump_info.txt"),
    ("global-metadata.dat", "global-metadata.dat"),
)


def dump_current_already_archived(rt: Path) -> Path | None:
    cur = rt / "out" / "dump.cs"
    arch = latest_archive_dump_cs(rt)
    if not cur.is_file() or not arch or not arch.is_file():
        return None
    if cur.stat().st_size != arch.stat().st_size:
        return None
    if abs(cur.stat().st_mtime - arch.stat().st_mtime) < 2:
        return arch
    if file_md5(cur) == file_md5(arch):
        return arch
    return None


def dump_preflight(args: argparse.Namespace) -> int:
    """运行时 GA / metadata / Dumper。不写 dump.cs。"""
    rt = ROOT / "Dumps" / "runtime"
    ga = Path(args.ga) if getattr(args, "ga", "") else rt / "GameAssembly.dll"
    meta = rt / "global-metadata.dat"
    info = rt / "dump_info.txt"
    dumper_local = rt / "il2cppdumper_v39" / "win-x64-net8"
    process_py = rt / "process_runtime_dump.py"
    feng_dumper = Path(
        r"c:\Users\kras\Desktop\xcat_for_fengxing\tools\il2cpp\il2cppdumper\Il2CppDumper.exe"
    )
    fail = 0

    def note(ok: bool, label: str, extra: str = "") -> None:
        nonlocal fail
        mark = "OK " if ok else "FAIL"
        if not ok:
            fail += 1
        print("%s  %s%s" % (mark, label, ("  " + extra) if extra else ""))

    if ga.is_file():
        mb = ga.stat().st_size / (1024 * 1024)
        note(mb >= 80, "runtime GameAssembly.dll", "%.1f MB" % mb)
    else:
        note(False, "runtime GameAssembly.dll missing", str(ga))

    note(meta.is_file(), "staged global-metadata.dat", str(meta) if meta.is_file() else "")
    client = Path(args.client_meta) if getattr(args, "client_meta", "") else CLIENT_META
    if client.is_file() and meta.is_file():
        c, s = file_md5(client), file_md5(meta)
        note(c == s, "metadata vs client", "client=%s staged=%s" % (c[:8], s[:8]))
    elif client.is_file():
        note(True, "client metadata present (not staged yet)", str(client))
    else:
        note(False, "client metadata missing", str(client))

    note(info.is_file(), "dump_info.txt")
    if info.is_file():
        txt = info.read_text(encoding="utf-8", errors="replace")
        has_regs = "CodeRegistration=" in txt and "MetadataRegistration=" in txt
        ga_ok = "ga_ok=1" in txt.replace(" ", "")
        if has_regs:
            note(True, "dump_info Registration")
        elif ga_ok:
            print("WARN  dump_info has no Registration (prepare_forcedump can recover)")
        else:
            note(False, "dump_info Registration")

    local_exe = list(dumper_local.glob("Il2CppDumper*.exe")) if dumper_local.is_dir() else []
    note(bool(local_exe) or feng_dumper.is_file(), "Il2CppDumper.exe",
         str(local_exe[0] if local_exe else feng_dumper))
    note(process_py.is_file(), "process_runtime_dump.py")
    dump_cs = rt / "out" / "dump.cs"
    if dump_cs.is_file():
        print(
            "OK   out/dump.cs  %.1f MB  md5=%s"
            % (dump_cs.stat().st_size / (1024 * 1024), file_md5(dump_cs)[:12])
        )
    else:
        note(False, "out/dump.cs missing")
    archives = sorted(rt.glob("_archive_*/out/dump.cs"))
    note(True, "archives", "%d" % len(archives))
    same = dump_current_already_archived(rt)
    if same:
        print("OK   dump.cs already archived", same)
    else:
        print("WARN  out/dump.cs is not identical to latest _archive_* (archive before inject)")
    return fail


def cmd_dump_check(args: argparse.Namespace) -> int:
    """下次更新前预检：运行时 GA / metadata / Dumper。默认不写 dump.cs。"""
    fail = dump_preflight(args)
    print("next: python scripts/ga_remount.py dump --archive   (before inject)")
    print("      inject GaRuntimeDump.dll")
    print("      python scripts/ga_remount.py dump --process  (user said process/ForceDump)")
    print("dump-check does not write dump.cs")
    return 1 if fail else 0


def cmd_dump(args: argparse.Namespace) -> int:
    """预检 + 可选归档 / ForceDump。默认 dry-run。禁止 --archive 与 --process 一起。"""
    rt = ROOT / "Dumps" / "runtime"
    do_archive = bool(getattr(args, "archive", False))
    do_process = bool(getattr(args, "process", False))
    if do_archive and do_process:
        print("REFUSE  do not combine --archive and --process (old dump.cs mixed with new GA)")
        return 2

    fail = dump_preflight(args)
    dest = dump_archive_dest(rt)
    same = dump_current_already_archived(rt)
    print("archive dest", dest)
    for src_rel, dst_rel in DUMP_ARCHIVE_FILES:
        src = rt / src_rel
        extra = ""
        if src.is_file():
            extra = "%.1f MB" % (src.stat().st_size / (1024 * 1024))
        print("  %s  %s  %s" % ("OK " if src.is_file() else "MISS", src_rel, extra))
    idb = rt / "GameAssembly.dll.i64"
    if idb.is_file():
        print(
            "  IDB GameAssembly.dll.i64  %.1f MB  (not copied unless --archive-idb)"
            % (idb.stat().st_size / (1024 * 1024))
        )

    if do_archive:
        if fail:
            print("REFUSE --archive (preflight FAIL)")
            return 1
        if same:
            print("skip --archive: dump.cs already in", same)
        elif dest.exists():
            print("REFUSE --archive dest exists", dest)
            return 2
        else:
            dest.mkdir(parents=True, exist_ok=False)
            for src_rel, dst_rel in DUMP_ARCHIVE_FILES:
                src = rt / src_rel
                if not src.is_file():
                    print("WARN  skip missing", src_rel)
                    continue
                outp = dest / dst_rel
                outp.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src, outp)
                print("copied", src_rel, "->", outp)
            if idb.is_file() and getattr(args, "archive_idb", False):
                shutil.copy2(idb, dest / idb.name)
                print("copied", idb.name)
            elif idb.is_file():
                print("skip IDB (pass --archive-idb to copy %.1f MB)" % (idb.stat().st_size / (1024 * 1024)))
            print("wrote archive", dest)
    elif not do_process:
        print("dry-run: dump --archive before inject; dump --process after inject (user must say process)")

    if do_process:
        if fail:
            print("REFUSE --process (preflight FAIL)")
            return 1
        if not dump_current_already_archived(rt):
            print("REFUSE --process: current out/dump.cs is not in latest _archive_* (run dump --archive first)")
            return 2
        client = Path(args.client_meta) if getattr(args, "client_meta", "") else CLIENT_META
        staged = rt / "global-metadata.dat"
        if not client.is_file():
            print("REFUSE --process: no client metadata", client)
            return 2
        if (not staged.is_file()) or file_md5(client) != file_md5(staged):
            shutil.copy2(client, staged)
            print("staged metadata", staged)
        process_py = rt / "process_runtime_dump.py"
        print("RUN", process_py)
        rc = subprocess.call([sys.executable, str(process_py)], cwd=str(rt))
        print("process_runtime_dump exit", rc)
        return 1 if rc else 0

    return 1 if fail else 0


def cmd_selftest(_args: argparse.Namespace | None = None) -> int:
    """注释里的 constexpr 不得被当成声明；行首声明必须命中。"""
    fail = 0

    def check(ok: bool, msg: str) -> None:
        nonlocal fail
        print("%s  %s" % ("OK " if ok else "FAIL", msg))
        if not ok:
            fail += 1

    krva_hit = "constexpr uint32_t kRvaFoo = 0xABu;"
    krva_ind = "    constexpr uintptr_t kRvaBar = 0x10;"
    krva_cmt = "// constexpr uint32_t kRvaFoo = 0xABu;"
    krva_cmt2 = "  //constexpr uint32_t kRvaFoo = 0x1;"
    kfb_hit = "constexpr size_t kFbFoo = 0xE0;"
    kfb_cmt = "// constexpr size_t kFbFoo = 0xE0;"
    check(bool(RE_KRVA_NAMED_ASSIGN.search(krva_hit)), "kRva line-start decl")
    check(bool(RE_KRVA_NAMED_ASSIGN.search(krva_ind)), "kRva indented decl")
    check(not RE_KRVA_NAMED_ASSIGN.search(krva_cmt), "kRva skip // comment")
    check(not RE_KRVA_NAMED_ASSIGN.search(krva_cmt2), "kRva skip indented //")
    check(bool(RE_KFB_NAMED_ASSIGN.search(kfb_hit)), "kFb line-start decl")
    check(not RE_KFB_NAMED_ASSIGN.search(kfb_cmt), "kFb skip // comment")
    m = RE_KRVA_NAMED_ASSIGN.search(krva_hit)
    check(bool(m and m.group(2) == "kRvaFoo" and m.group(4) == "u"), "kRva keep u suffix")
    print("selftest FAIL=%d" % fail)
    return 1 if fail else 0


def cmd_verify(args: argparse.Namespace) -> int:
    """一次干跑 selftest + dump-check + map + layout + krva + catalog + audit + smoke。不写 dump.cs / 不 apply。"""
    ignore = str(ROOT / "scripts" / "data" / "ga_remount_ignore.txt")
    log = getattr(args, "log", "") or str(DEFAULT_LOG)
    steps: list[tuple[str, object, dict]] = [
        ("selftest", cmd_selftest, {}),
        (
            "dump-check",
            cmd_dump_check,
            {"ga": str(DEFAULT_GA), "client_meta": str(CLIENT_META)},
        ),
        (
            "map",
            cmd_map,
            {
                "old": "",
                "new": str(DEFAULT_NEW),
                "x_dir": str(DEFAULT_X),
                "out_dir": str(DEFAULT_OUT),
                "apply": False,
                "apply_hashes": False,
                "apply_krva": False,
                "apply_all_hex": False,
            },
        ),
        (
            "layout",
            cmd_layout,
            {
                "old": "",
                "new": str(DEFAULT_NEW),
                "x_dir": str(DEFAULT_X),
                "out_dir": str(DEFAULT_OUT),
                "ignore": ignore,
                "write": False,
            },
        ),
        (
            "krva",
            cmd_krva,
            {
                "new": str(DEFAULT_NEW),
                "x_dir": str(DEFAULT_X),
                "out_dir": str(DEFAULT_OUT),
                "catalog": str(DEFAULT_CATALOG),
                "ignore": ignore,
                "write_bind": False,
            },
        ),
        (
            "catalog",
            cmd_catalog,
            {
                "new": str(DEFAULT_NEW),
                "ga": str(DEFAULT_GA),
                "catalog": str(DEFAULT_CATALOG),
                "x_dir": str(DEFAULT_X),
                "out_dir": str(DEFAULT_OUT),
                "old": "",
                "write": False,
            },
        ),
        (
            "audit",
            cmd_audit,
            {
                "new": str(DEFAULT_NEW),
                "ga": str(DEFAULT_GA),
                "x_dir": str(DEFAULT_X),
                "catalog": str(DEFAULT_CATALOG),
                "shape": str(DEFAULT_SHAPE),
                "out_dir": str(DEFAULT_OUT),
                "ignore": ignore,
            },
        ),
        (
            "smoke",
            cmd_smoke,
            {
                "log": log,
                "expect": str(DEFAULT_SMOKE_EXPECT),
                "write_expect": False,
                "verbose": False,
            },
        ),
    ]
    worst = 0
    for name, fn, kw in steps:
        print("========", name)
        rc = int(fn(argparse.Namespace(**kw)))
        print("--------", name, "rc", rc)
        if rc > worst:
            worst = rc
    print("verify worst_rc=%d" % worst)
    return 1 if worst else 0


LAYOUT_BLOCK_FLAGS = ("TYPE_FLIP", "MOVED_TYPE", "AMBIGUOUS", "DEAD")


def summarize_layout_tsv(path: Path) -> dict[str, int | list[str]]:
    block = stale = 0
    samples: list[str] = []
    if not path.is_file():
        return {"block": 0, "stale": 0, "samples": []}
    for ln in path.read_text(encoding="utf-8").splitlines()[1:]:
        if not ln.strip():
            continue
        verdict = ln.split("\t")[0]
        flags = set(verdict.split("|"))
        if flags & set(LAYOUT_BLOCK_FLAGS):
            block += 1
            if len(samples) < 8:
                bits = ln.split("\t")
                samples.append("%s  %s" % (verdict, (bits[1][:20] if len(bits) > 1 else "")))
        if "FALLBACK_STALE" in flags:
            stale += 1
    return {"block": block, "stale": stale, "samples": samples}


def summarize_catalog_report(path: Path) -> dict[str, int]:
    fail = move = stale = match = 0
    if not path.is_file():
        return {"fail": 0, "move": 0, "stale": 0, "match": 0}
    for ln in path.read_text(encoding="utf-8").splitlines()[1:]:
        if not ln.strip():
            continue
        parts = ln.split("\t")
        if len(parts) < 2:
            continue
        v = parts[1]
        if v == "FAIL":
            fail += 1
        elif v == "MOVE":
            move += 1
        elif v == "STALE":
            stale += 1
        elif v == "MATCH":
            match += 1
    return {"fail": fail, "move": move, "stale": stale, "match": match}


def cmd_apply(args: argparse.Namespace) -> int:
    """按序 hashes → kFb → kRva → catalog。默认 dry-run。TYPE_FLIP / catalog FAIL 硬停。"""
    do_write = bool(getattr(args, "apply", False) or getattr(args, "write", False))
    if getattr(args, "apply_all_hex", False):
        print("REFUSE  apply subcommand does not take --apply-all-hex")
        return 2
    ignore = str(ROOT / "scripts" / "data" / "ga_remount_ignore.txt")
    out_dir = Path(getattr(args, "out_dir", "") or DEFAULT_OUT)

    def map_ns(*, hashes: bool = False, krva: bool = False) -> argparse.Namespace:
        return argparse.Namespace(
            old="",
            new=str(DEFAULT_NEW),
            x_dir=str(DEFAULT_X),
            out_dir=str(out_dir),
            apply=False,
            apply_hashes=hashes,
            apply_krva=krva,
            apply_all_hex=False,
        )

    def layout_ns(*, write: bool = False) -> argparse.Namespace:
        return argparse.Namespace(
            old="",
            new=str(DEFAULT_NEW),
            x_dir=str(DEFAULT_X),
            out_dir=str(out_dir),
            ignore=ignore,
            write=write,
        )

    def catalog_ns(*, write: bool = False) -> argparse.Namespace:
        return argparse.Namespace(
            new=str(DEFAULT_NEW),
            ga=str(DEFAULT_GA),
            catalog=str(DEFAULT_CATALOG),
            x_dir=str(DEFAULT_X),
            out_dir=str(out_dir),
            old="",
            write=write,
        )

    def krva_ns(*, write_bind: bool = False) -> argparse.Namespace:
        return argparse.Namespace(
            new=str(DEFAULT_NEW),
            x_dir=str(DEFAULT_X),
            out_dir=str(out_dir),
            catalog=str(DEFAULT_CATALOG),
            ignore=ignore,
            write_bind=write_bind,
        )

    def audit_ns() -> argparse.Namespace:
        return argparse.Namespace(
            new=str(DEFAULT_NEW),
            ga=str(DEFAULT_GA),
            x_dir=str(DEFAULT_X),
            catalog=str(DEFAULT_CATALOG),
            shape=str(DEFAULT_SHAPE),
            out_dir=str(out_dir),
            ignore=ignore,
        )

    print("apply write=%s" % do_write)
    if not do_write:
        print("NOTE  dry-run layout uses current source hashes (apply-hashes not done yet)")
        print("======== apply map")
        cmd_map(map_ns())

    if do_write:
        print("======== apply hashes")
        rc_h = int(cmd_map(map_ns(hashes=True)))
        if rc_h:
            print("REFUSE remaining writes (apply-hashes rc=%d)" % rc_h)
            return rc_h

    print("======== apply layout")
    cmd_layout(layout_ns(write=False))
    lay = summarize_layout_tsv(out_dir / "_ga_remount_layout.tsv")
    print("layout block=%d stale=%d" % (int(lay["block"]), int(lay["stale"])))
    if lay["block"]:
        print("REFUSE remaining writes (TYPE_FLIP/MOVED_TYPE/DEAD/AMBIGUOUS)")
        for s in lay["samples"]:
            print("  ", s)
        return 1
    if do_write and lay["stale"]:
        print("======== apply kFb")
        rc_fb = int(cmd_layout(layout_ns(write=True)))
        if rc_fb:
            print("REFUSE remaining writes (layout --write rc=%d)" % rc_fb)
            return rc_fb

    if do_write:
        print("======== apply krva")
        rc_k = int(cmd_map(map_ns(krva=True)))
        if rc_k:
            print("REFUSE remaining writes (apply-krva rc=%d)" % rc_k)
            return rc_k

    print("======== apply catalog")
    cmd_catalog(catalog_ns(write=False))
    cat = summarize_catalog_report(out_dir / "_ga_remount_catalog.tsv")
    print(
        "catalog MATCH=%d MOVE=%d STALE=%d FAIL=%d"
        % (cat["match"], cat["move"], cat["stale"], cat["fail"])
    )
    if cat["fail"]:
        print("REFUSE catalog --write (FAIL>0); open IDA for those sites")
        return 1
    if do_write and not cat["fail"]:
        print("======== apply catalog --write")
        rc_c = int(cmd_catalog(catalog_ns(write=True)))
        if rc_c:
            return rc_c

    print("======== apply krva-check")
    rc_kr = int(cmd_krva(krva_ns(write_bind=do_write)))
    print("======== apply audit")
    rc_au = int(cmd_audit(audit_ns()))
    worst = max(rc_kr, rc_au)
    if not do_write:
        print(
            "dry-run: pass --apply to write hashes → kFb → kRva → catalog "
            "(stops on TYPE_FLIP / catalog FAIL)"
        )
    print("apply worst_rc=%d write=%s" % (worst, do_write))
    return 1 if worst else 0


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
        help="写入已映射哈希 + constexpr kRva*（不改裸 0xHEX）",
    )
    p_map.add_argument(
        "--apply-hashes",
        action="store_true",
        help="只换类/字段/方法哈希",
    )
    p_map.add_argument(
        "--apply-krva",
        action="store_true",
        help="只改 constexpr kRva* = 0x…",
    )
    p_map.add_argument(
        "--apply-all-hex",
        action="store_true",
        help="旧行为：所有 0xRVA token；哈希已在新 dump 时拒绝",
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

    p_ly = sub.add_parser(
        "layout",
        help="旧/新 dump 字段偏移+类型差，以及 kFb* 是否仍等于新偏移",
    )
    p_ly.add_argument("--old", default="", help="旧 dump.cs（默认最新 _archive_*/out/dump.cs）")
    p_ly.add_argument("--new", default=str(DEFAULT_NEW))
    p_ly.add_argument("--x-dir", default=str(DEFAULT_X))
    p_ly.add_argument("--out-dir", default=str(DEFAULT_OUT))
    p_ly.add_argument(
        "--ignore",
        default=str(ROOT / "scripts" / "data" / "ga_remount_ignore.txt"),
    )
    p_ly.add_argument(
        "--write",
        action="store_true",
        help="只改 FALLBACK_STALE 的 constexpr kFb*（TYPE_FLIP/DEAD 拒绝）",
    )

    p_kr = sub.add_parser("krva", help="kRva* 与同后缀 kHash* 对 dump 方法 RVA")
    p_kr.add_argument("--new", default=str(DEFAULT_NEW))
    p_kr.add_argument("--x-dir", default=str(DEFAULT_X))
    p_kr.add_argument("--out-dir", default=str(DEFAULT_OUT))
    p_kr.add_argument("--catalog", default=str(DEFAULT_CATALOG))
    p_kr.add_argument(
        "--ignore",
        default=str(ROOT / "scripts" / "data" / "ga_remount_ignore.txt"),
    )
    p_kr.add_argument(
        "--write-bind",
        action="store_true",
        help="把 MATCH/DUMP_ID 写入 scripts/data/ga_krva_bind.tsv（下次 apply-krva 用）",
    )

    p_sm = sub.add_parser("smoke", help="扫 x.jsonl(+轮转) 的 hits=a/b（注入后 remount 冒烟）")
    p_sm.add_argument("--log", default=str(DEFAULT_LOG))
    p_sm.add_argument("--expect", default=str(DEFAULT_SMOKE_EXPECT))
    p_sm.add_argument(
        "--write-expect",
        action="store_true",
        help="用当前日志写 expect 基线（不要在更新日红灯时写）",
    )
    p_sm.add_argument("-v", "--verbose", action="store_true")

    p_dc = sub.add_parser("dump-check", help="预检运行时 GA / metadata / Dumper（不写 dump.cs）")
    p_dc.add_argument("--ga", default=str(DEFAULT_GA))
    p_dc.add_argument("--client-meta", default=str(CLIENT_META))

    p_dump = sub.add_parser(
        "dump",
        help="预检 + 可选归档 / ForceDump（默认 dry-run；禁止与 --archive 同时 --process）",
    )
    p_dump.add_argument("--ga", default=str(DEFAULT_GA))
    p_dump.add_argument("--client-meta", default=str(CLIENT_META))
    p_dump.add_argument(
        "--archive",
        action="store_true",
        help="注入前拷 dump.cs/GA 到 _archive_*（已归档则 skip）",
    )
    p_dump.add_argument(
        "--process",
        action="store_true",
        help="注入后拷 metadata 并跑 process_runtime_dump.py（覆盖 out/dump.cs）",
    )
    p_dump.add_argument(
        "--archive-idb",
        action="store_true",
        help="归档时连同 GameAssembly.dll.i64（约 1.7GB，默认不拷）",
    )

    p_cat = sub.add_parser(
        "catalog",
        help="体内点 follow 所在方法头（默认 dry-run；禁止全模块搜 75 07）",
    )
    p_cat.add_argument("--new", default=str(DEFAULT_NEW))
    p_cat.add_argument("--ga", default=str(DEFAULT_GA))
    p_cat.add_argument("--catalog", default=str(DEFAULT_CATALOG))
    p_cat.add_argument("--x-dir", default=str(DEFAULT_X))
    p_cat.add_argument("--out-dir", default=str(DEFAULT_OUT))
    p_cat.add_argument(
        "--old",
        default="",
        help="旧 dump.cs：只给 shared/哈希 miss 做 rva_map 回退",
    )
    p_cat.add_argument(
        "--write",
        action="store_true",
        help="写入 tsv 的 rva/target 以及 catalog 行 cpp 列的 constexpr kRva*（不改注释/裸 HEX）",
    )

    p_vf = sub.add_parser(
        "verify",
        help="干跑 selftest + dump-check + map + layout + krva + catalog + audit + smoke（不 apply）",
    )
    p_vf.add_argument("--log", default=str(DEFAULT_LOG))

    p_ap = sub.add_parser(
        "apply",
        help="按序 hashes→kFb→kRva→catalog（默认 dry-run；TYPE_FLIP / catalog FAIL 硬停）",
    )
    p_ap.add_argument(
        "--apply",
        action="store_true",
        help="写入：hashes → FALLBACK_STALE kFb → kRva → catalog（用户说 apply/write）",
    )
    p_ap.add_argument(
        "--write",
        action="store_true",
        help="同 --apply",
    )
    p_ap.add_argument("--out-dir", default=str(DEFAULT_OUT))

    sub.add_parser("selftest", help="正则自检：注释里的 constexpr kRva/kFb 不得命中")
    sub.add_parser("howto", help="打印 Agent 执行清单（硬停 + 命令）")

    args = ap.parse_args()
    if args.cmd == "map":
        return cmd_map(args)
    if args.cmd == "layout":
        return cmd_layout(args)
    if args.cmd == "krva":
        return cmd_krva(args)
    if args.cmd == "audit":
        return cmd_audit(args)
    if args.cmd == "smoke":
        return cmd_smoke(args)
    if args.cmd == "dump-check":
        return cmd_dump_check(args)
    if args.cmd == "dump":
        return cmd_dump(args)
    if args.cmd == "catalog":
        return cmd_catalog(args)
    if args.cmd == "verify":
        return cmd_verify(args)
    if args.cmd == "apply":
        return cmd_apply(args)
    if args.cmd == "selftest":
        return cmd_selftest(args)
    if args.cmd == "howto":
        sys.stdout.write(HOWTO)
        if not HOWTO.endswith("\n"):
            sys.stdout.write("\n")
        return 0
    ap.print_help()
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
