#!/usr/bin/env python3
"""生成经典版 TWMS 功能目录表（对照枫星 travel_catalog / skill_catalog_full 列契约）。

产品 = 经典版；枫星 = 对照来源（列名与打包路径对齐，不硬套协议）。

输入（已有离线 TSV）：
  dumps/offline_tables/tsv/map_names.tsv
  dumps/offline_tables/tsv/map_info.tsv
  dumps/offline_tables/tsv/skill_names.tsv
  dumps/offline_tables/tsv/skill_meta.tsv
  dumps/offline_tables/tsv/skill_levels.tsv
  dumps/offline_tables/tsv/skillbook_names.tsv

输出：
  dumps/twms_routes/travel_catalog.tsv
      → bin/XCat_data/state/travel_catalog.tsv
  dumps/offline_tables/tsv/skill_catalog_full.tsv
      → bin/XCat_data/skill_catalog/skill_catalog_full.tsv
      → bin/XCat_data/dataservice/skill_catalog_full.tsv
"""
from __future__ import annotations

import argparse
import shutil
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TSV = ROOT / "dumps" / "offline_tables" / "tsv"
ROUTES = ROOT / "dumps" / "twms_routes"
BIN_STATE = ROOT / "bin" / "XCat_data" / "state"
BIN_SKILL = ROOT / "bin" / "XCat_data" / "skill_catalog"
BIN_DS = ROOT / "bin" / "XCat_data" / "dataservice"

# mapMark → 繁中大区（streetName 缺失时兜底；对照经典版地图）
MARK_REGION = {
    "MushroomVillage": "楓之島",
    "SouthPerry": "楓之島",
    "Amherst": "彩虹之地",
    "Henesys": "維多利亞",
    "Ellinia": "維多利亞",
    "Perion": "維多利亞",
    "KerningCity": "維多利亞",
    "KerningParty": "維多利亞",
    "Nautilus": "維多利亞",
    "Dungeon": "維多利亞",
    "Rith": "冰原雪域",
    "Orbis": "冰原雪域",
    "FreeMarket": "自由市場",
    "Wedding": "結婚村莊",
    "Quest": "任務地圖",
    "None": "其他",
}

ATTACK_KEYS = {
    "damage",
    "damageX",
    "mobCount",
    "attackCount",
    "bulletCount",
    "bulletConsume",
    "cooltimeMS",  # alone not attack; filtered below
}
ATTACK_KEYS_STRICT = {
    "damage",
    "damageX",
    "mobCount",
    "attackCount",
    "bulletCount",
    "bulletConsume",
}
SUPPORT_KEYS = {
    "time",
    "x",
    "y",
    "z",
    "pad",
    "mad",
    "pdd",
    "mdd",
    "acc",
    "eva",
    "speed",
    "jump",
    "hp",
    "mp",
    "hpCon",
    "mpCon",
    "prop",
    "itemCon",
    "itemConNo",
}


def read_tsv(path: Path) -> list[list[str]]:
    if not path.is_file():
        raise FileNotFoundError(path)
    rows: list[list[str]] = []
    for ln in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not ln.strip() or ln.startswith("#"):
            continue
        rows.append(ln.split("\t"))
    return rows


def pad_map_key(map_id: int | str) -> str:
    try:
        n = int(str(map_id).strip(), 10)
    except ValueError:
        return str(map_id).strip()
    return f"{n:09d}"


def fmt_skill_code(sid: int) -> str:
    """对齐枫星习惯：新手 1xxx 补 7 位；其它保持十进制串。"""
    if 1000 <= sid < 1000000:
        return f"{sid:07d}"
    return str(sid)


def job_from_skill_id(sid: int) -> int:
    if sid < 1000000:
        return 0
    return sid // 10000


def likely_passive_id(sid: int) -> bool:
    # 经典 WZ：xxxx0yyy 多为精通/被动；xxxx1yyy 多为主动
    if sid < 1000000:
        return False
    return ((sid // 1000) % 10) == 0


def zh_type_hint(name: str, desc: str) -> int | None:
    text = f"{name}\n{desc}"
    if any(k in text for k in ("被動", "被动", "精通", "熟練度", "熟练度")):
        return 0
    if any(k in text for k in ("傷害", "伤害", "攻擊", "攻击", "擊中", "击中", "投擲", "投掷")):
        return 2
    if any(
        k in text
        for k in (
            "提升",
            "增加",
            "恢復",
            "恢复",
            "治癒",
            "治愈",
            "瞬移",
            "移動",
            "移动",
            "跳躍",
            "跳跃",
            "召喚",
            "召唤",
            "持續",
            "持续",
            "冷卻",
            "冷却",
        )
    ):
        return 1
    return None


def build_travel_catalog() -> tuple[Path, int]:
    names: dict[int, tuple[str, str]] = {}
    for p in read_tsv(TSV / "map_names.tsv"):
        if len(p) < 3:
            continue
        try:
            mid = int(p[0], 10)
        except ValueError:
            continue
        street, mname = p[1].strip(), p[2].strip()
        names[mid] = (street, mname)

    info_rows = read_tsv(TSV / "map_info.tsv")
    rows: list[tuple[str, str, str, str, str]] = []
    seen: set[str] = set()
    for p in info_rows:
        if len(p) < 5:
            continue
        try:
            mid = int(p[0], 10)
        except ValueError:
            continue
        mark = (p[3] if len(p) > 3 else "").strip()
        town = (p[4] if len(p) > 4 else "0").strip()
        street, mname = names.get(mid, ("", ""))
        region = street or MARK_REGION.get(mark, mark or "其他")
        sub = street or MARK_REGION.get(mark, mark or region)
        key = pad_map_key(mid)
        if key in seen:
            continue
        seen.add(key)
        disp = mname or key
        map_type = "WORLD" if town == "1" else "FIELD"
        rows.append((region, sub, key, disp, map_type))

    # 名称表有、但 Map WZ 无 info 的图：标 NAME_ONLY，便于目录检索（不进赶路图）
    for mid, (street, mname) in sorted(names.items()):
        key = pad_map_key(mid)
        if key in seen:
            continue
        seen.add(key)
        region = street or "其他"
        rows.append((region, region, key, mname or key, "NAME_ONLY"))

    rows.sort(key=lambda r: (r[0], r[1], r[2]))
    ROUTES.mkdir(parents=True, exist_ok=True)
    out = ROUTES / "travel_catalog.tsv"
    lines = ["#region\tsub\tkey\tdisp\tmapType"]
    lines += [f"{a}\t{b}\t{c}\t{d}\t{e}" for a, b, c, d, e in rows]
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return out, len(rows)


def build_skill_catalog_full() -> tuple[Path, int, dict[str, int]]:
    names: dict[int, tuple[str, str]] = {}
    for p in read_tsv(TSV / "skill_names.tsv"):
        if len(p) < 2:
            continue
        try:
            sid = int(p[0], 10)
        except ValueError:
            continue
        names[sid] = (p[1].strip(), (p[2] if len(p) > 2 else "").strip())

    meta: dict[int, dict[str, str]] = {}
    for p in read_tsv(TSV / "skill_meta.tsv"):
        if not p:
            continue
        try:
            sid = int(p[0], 10)
        except ValueError:
            continue
        # skill_id masterLevel maxLevel maxLevelObserved combatOrders psd invisible
        invisible = (p[6] if len(p) > 6 else "").strip()
        meta[sid] = {"invisible": invisible}

    # skill_id → set(keys at any level)
    keys_by: dict[int, set[str]] = defaultdict(set)
    for p in read_tsv(TSV / "skill_levels.tsv"):
        if len(p) < 3:
            continue
        try:
            sid = int(p[0], 10)
        except ValueError:
            continue
        keys_by[sid].add(p[2].strip())

    job_names = {0: "冒險之技"}
    for p in read_tsv(TSV / "skillbook_names.tsv"):
        if len(p) < 2:
            continue
        try:
            job_names[int(p[0], 10)] = p[1].strip()
        except ValueError:
            pass

    all_ids = sorted(set(names) | set(meta) | set(keys_by))
    rows: list[tuple[str, str, int, int, int, int, int]] = []
    stats = {"attack": 0, "support": 0, "passive": 0, "via_keys": 0, "via_text": 0, "via_id": 0}

    for sid in all_ids:
        name, desc = names.get(sid, ("", ""))
        if not name:
            name = str(sid)
        code = fmt_skill_code(sid)
        job = job_from_skill_id(sid)
        kset = keys_by.get(sid, set())
        inv = meta.get(sid, {}).get("invisible", "") == "1"

        typ: int | None = None
        if kset & ATTACK_KEYS_STRICT:
            typ = 2
            stats["via_keys"] += 1
        else:
            hint = zh_type_hint(name, desc)
            if hint is not None:
                typ = hint
                stats["via_text"] += 1
            elif kset & SUPPORT_KEYS and not likely_passive_id(sid):
                typ = 1
                stats["via_keys"] += 1
            elif inv or likely_passive_id(sid):
                typ = 0
                stats["via_id"] += 1
            else:
                typ = 0
                stats["via_id"] += 1

        passive = 1 if typ == 0 else 0
        if typ == 2:
            stats["attack"] += 1
        elif typ == 1:
            stats["support"] += 1
        else:
            stats["passive"] += 1

        name = name.replace("\t", " ").replace("\n", " ").replace("\r", " ")
        rows.append((code, name, job, typ, passive, 0, 0))

    TSV.mkdir(parents=True, exist_ok=True)
    out = TSV / "skill_catalog_full.tsv"
    header = [
        "# code\tname\tjob\ttype\tpassive\tlearned\tlevel",
        "# 经典版离线：skill_names + skill_meta + skill_levels 启发式",
        "# type: 0被动/未知 1辅助 2攻击；learned/level 离线恒 0（运行时覆盖）",
        f"# jobs_known={len(job_names)} source=twms-offline",
    ]
    body = [f"{c}\t{n}\t{j}\t{t}\t{p}\t{l}\t{lv}" for c, n, j, t, p, l, lv in rows]
    out.write_text("\n".join(header + body) + "\n", encoding="utf-8")
    return out, len(rows), stats


def sync_bin(travel: Path, skill: Path) -> list[str]:
    synced: list[str] = []
    pairs = [
        (travel, BIN_STATE / "travel_catalog.tsv"),
        (skill, BIN_SKILL / "skill_catalog_full.tsv"),
        (skill, BIN_DS / "skill_catalog_full.tsv"),
    ]
    for src, dst in pairs:
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        synced.append(str(dst.relative_to(ROOT)))
    return synced


def update_source_md(travel_n: int, skill_n: int) -> None:
    path = ROOT / "dumps" / "offline_tables" / "SOURCE.md"
    if not path.is_file():
        return
    text = path.read_text(encoding="utf-8")
    marker = "## 功能目录（travel / skill）"
    block = f"""{marker}

- 脚本：`scripts/dump_feature_catalogs.py`（依赖既有 map/skill 离线 TSV，不扫客户端）
- 对照枫星列契约；**产品=经典版**

| 表 | 行数 | 说明 |
|---|---:|---|
| `dumps/twms_routes/travel_catalog.tsv` | {travel_n} | region/sub/key/disp/mapType；WORLD=城镇 FIELD=野外 NAME_ONLY=仅字符串 |
| `skill_catalog_full.tsv` | {skill_n} | code/name/job/type/passive/learned/level；type 启发式 |

- 运行时：`bin/XCat_data/state/travel_catalog.tsv`、`bin/XCat_data/skill_catalog/skill_catalog_full.tsv`

"""
    if marker in text:
        # replace from marker to next ## or end
        i = text.index(marker)
        j = text.find("\n## ", i + 1)
        if j < 0:
            text = text[:i] + block
        else:
            text = text[:i] + block + text[j:]
    else:
        # insert before ## 重跑
        key = "## 重跑"
        if key in text:
            text = text.replace(key, block + key)
        else:
            text = text.rstrip() + "\n\n" + block
    if "dump_feature_catalogs.py" not in text.split("## 重跑")[-1]:
        text = text.replace(
            "python scripts/dump_quest_tables.py --also-bin\n```",
            "python scripts/dump_quest_tables.py --also-bin\n"
            "python scripts/dump_feature_catalogs.py --also-bin\n```",
        )
    path.write_text(text, encoding="utf-8")
    ds = BIN_DS / "SOURCE.md"
    if ds.parent.is_dir():
        shutil.copy2(path, ds)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--also-bin", action="store_true", help="同步到 bin/XCat_data")
    args = ap.parse_args()

    travel_path, travel_n = build_travel_catalog()
    skill_path, skill_n, stats = build_skill_catalog_full()
    print(f"travel_catalog.tsv → {travel_n} 行  {travel_path}")
    print(f"skill_catalog_full.tsv → {skill_n} 行  {skill_path}")
    print(
        f"  type: attack={stats['attack']} support={stats['support']} "
        f"passive={stats['passive']} "
        f"(via_keys={stats['via_keys']} via_text={stats['via_text']} via_id={stats['via_id']})"
    )

    # 抽检
    for mid in (100000000, 104000000, 60000):
        key = pad_map_key(mid)
        hit = [ln for ln in travel_path.read_text(encoding="utf-8").splitlines() if f"\t{key}\t" in ln]
        print(f"  travel spot {key}: {hit[0] if hit else 'MISS'}")
    skill_lines = skill_path.read_text(encoding="utf-8").splitlines()
    for code in ("0001000", "1001004", "12"):
        hit = [ln for ln in skill_lines if ln.split("\t", 1)[0] == code]
        print(f"  skill spot {code}: {hit[0] if hit else 'MISS'}")

    if args.also_bin:
        synced = sync_bin(travel_path, skill_path)
        for s in synced:
            print(f"synced → {s}")
    update_source_md(travel_n, skill_n)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
