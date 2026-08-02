#!/usr/bin/env python3
"""
Dump Classic TWMS mob_stats + skill_levels from Addressables WZJS.

  Mob/*.wzjson  → tsv/mob_stats.tsv
  Skill/*.wzjson → tsv/skill_levels.tsv（长表 skill/level/key/value）
                 → tsv/skill_meta.tsv（每技能一行摘要）

用法：
  python scripts/dump_mob_skill_tables.py
  python scripts/dump_mob_skill_tables.py --also-bin
"""
from __future__ import annotations

import argparse
import re
import sys
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path

import UnityPy

sys.path.insert(0, str(Path(__file__).resolve().parent))
from wzjs_io import TYPE_FLOAT, TYPE_INT, TYPE_STRING, clean_cell, item_value, parse_mb_wzjson  # noqa: E402

REPO = Path(__file__).resolve().parents[1]
DEFAULT_GAME = Path(r"G:\Games\maplestory_classic")

MOB_INFO_KEYS = (
    "level",
    "maxHP",
    "maxMP",
    "exp",
    "PADamage",
    "PDDamage",
    "MADamage",
    "MDDamage",
    "acc",
    "eva",
    "speed",
    "pushed",
    "undead",
    "bodyAttack",
    "summonType",
    "mobType",
    "fs",
)

LEVEL_FIELD_RE = re.compile(r"(?:^|/)skill/(\d+)/level/(\d+)/([\w]+)$")
META_FIELD_RE = re.compile(
    r"(?:^|/)skill/(\d+)/(masterLevel|maxLevel|combatOrders|psd|invisible|magicDamageCheck)$"
)
def iter_wzjson(game_root: Path, folder_token: str):
    """folder_token e.g. '/Json/Mob/' or '/Json/Skill/'."""
    w = game_root / "Maplestory_Classic_Data" / "StreamingAssets" / "aa" / "w"
    for bundle_path in sorted(w.glob("json_*.bundle"), key=lambda p: p.stat().st_size):
        env = UnityPy.load(str(bundle_path))
        ab_objs = [o for o in env.objects if o.type.name == "AssetBundle"]
        if not ab_objs:
            continue
        ab = ab_objs[0].read()
        container = ab.m_Container
        pairs = list(container.items()) if hasattr(container, "items") else list(container)
        for key, info in pairs:
            norm = key.replace("\\", "/")
            if folder_token not in norm or not norm.endswith(".wzjson"):
                continue
            if "QuestCountGroup" in norm:
                continue
            obj = next((o for o in env.objects if o.path_id == info.asset.m_PathID), None)
            if obj is None:
                continue
            try:
                raw = obj.get_raw_data()
            except Exception as e:
                print(f"[skip] {bundle_path.name} {key}: {e}")
                continue
            yield bundle_path.name, norm, raw


def mob_id_from_key(key: str) -> int | None:
    stem = Path(key).stem
    if stem.isdigit():
        return int(stem)
    return None


def extract_mob_stats(raw: bytes, fallback_id: int | None) -> dict | None:
    doc = parse_mb_wzjson(raw)
    mid = fallback_id
    if doc.asset_name.isdigit():
        mid = int(doc.asset_name)
    if mid is None:
        return None
    row: dict = {"mob_id": mid}
    for it in doc.items:
        n = doc.names[it.name_index] if 0 <= it.name_index < len(doc.names) else ""
        path = (doc.paths[it.path_index] if 0 <= it.path_index < len(doc.paths) else "").replace(
            "\\", "/"
        )
        if n not in MOB_INFO_KEYS:
            continue
        if not (path == f"info/{n}" or path.endswith(f"/info/{n}") or path == n):
            continue
        val = item_value(doc, it)
        if val is None and it.type == TYPE_STRING and 0 <= it.data_index < len(doc.strings):
            val = doc.strings[it.data_index]
        if val is not None:
            row[n] = val
    return row


def extract_skill_tables(raw: bytes) -> tuple[list[tuple], dict[int, dict]]:
    """Return (level_rows as (sid,lv,key,val), meta_by_sid)."""
    doc = parse_mb_wzjson(raw)
    level_rows: list[tuple] = []
    meta: dict[int, dict] = defaultdict(dict)
    max_lv: dict[int, int] = defaultdict(int)

    for it in doc.items:
        n = doc.names[it.name_index] if 0 <= it.name_index < len(doc.names) else ""
        path = (doc.paths[it.path_index] if 0 <= it.path_index < len(doc.paths) else "").replace(
            "\\", "/"
        )
        val = item_value(doc, it)

        lm = LEVEL_FIELD_RE.search(path)
        if lm:
            sid, lv, key = int(lm.group(1)), int(lm.group(2)), lm.group(3)
            max_lv[sid] = max(max_lv[sid], lv)
            # 等级数值：int / float / string（跳过纯属性节点）
            if it.type in (TYPE_INT, TYPE_FLOAT, TYPE_STRING) and val is not None:
                if isinstance(val, float):
                    val = round(val, 6)
                level_rows.append((sid, lv, key, val))
            continue

        mm = META_FIELD_RE.search(path)
        if mm and val is not None:
            sid = int(mm.group(1))
            meta[sid][mm.group(2)] = val

    for sid, lv in max_lv.items():
        meta[sid]["maxLevelObserved"] = lv
    return level_rows, dict(meta)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--game-root", type=Path, default=DEFAULT_GAME)
    ap.add_argument("--out", type=Path, default=REPO / "dumps" / "offline_tables")
    ap.add_argument("--also-bin", action="store_true")
    args = ap.parse_args()

    tsv_dir = args.out / "tsv"
    tsv_dir.mkdir(parents=True, exist_ok=True)

    # --- mobs ---
    mobs: dict[int, dict] = {}
    mob_err = 0
    for bname, key, raw in iter_wzjson(args.game_root, "/Json/Mob/"):
        mid = mob_id_from_key(key)
        try:
            row = extract_mob_stats(raw, mid)
        except Exception as e:
            mob_err += 1
            print(f"[mob-fail] {key}: {e}")
            continue
        if row:
            mobs[int(row["mob_id"])] = row
    print(f"[mob] kept={len(mobs)} errors={mob_err}")

    mob_header = (
        "# mob_id\t"
        + "\t".join(MOB_INFO_KEYS)
        + "\tsource=twms-wzjs-mob-info\n"
    )
    mob_lines = [mob_header]
    for mid in sorted(mobs):
        r = mobs[mid]
        cells = [str(mid)] + [clean_cell(r.get(k, "")) for k in MOB_INFO_KEYS]
        mob_lines.append("\t".join(cells) + "\n")
    mob_path = tsv_dir / "mob_stats.tsv"
    mob_path.write_text("".join(mob_lines), encoding="utf-8")

    # --- skills ---
    all_levels: list[tuple] = []
    all_meta: dict[int, dict] = {}
    skill_files = 0
    skill_err = 0
    for bname, key, raw in iter_wzjson(args.game_root, "/Json/Skill/"):
        skill_files += 1
        try:
            rows, meta = extract_skill_tables(raw)
        except Exception as e:
            skill_err += 1
            print(f"[skill-fail] {key}: {e}")
            continue
        all_levels.extend(rows)
        for sid, m in meta.items():
            all_meta.setdefault(sid, {}).update(m)
        if skill_files % 20 == 0:
            print(f"[skill] files={skill_files} level_rows={len(all_levels)}")

    # dedupe level rows (last wins)
    level_map: dict[tuple[int, int, str], object] = {}
    for sid, lv, key, val in all_levels:
        level_map[(sid, lv, key)] = val

    skill_lv_path = tsv_dir / "skill_levels.tsv"
    skill_lv_lines = ["# skill_id\tlevel\tkey\tvalue\tsource=twms-wzjs-skill-level\n"]
    for sid, lv, key in sorted(level_map.keys()):
        skill_lv_lines.append(f"{sid}\t{lv}\t{key}\t{clean_cell(level_map[(sid, lv, key)])}\n")
    skill_lv_path.write_text("".join(skill_lv_lines), encoding="utf-8")

    meta_keys = ("masterLevel", "maxLevel", "maxLevelObserved", "combatOrders", "psd", "invisible")
    skill_meta_path = tsv_dir / "skill_meta.tsv"
    meta_lines = ["# skill_id\t" + "\t".join(meta_keys) + "\tsource=twms-wzjs-skill-meta\n"]
    for sid in sorted(all_meta):
        m = all_meta[sid]
        meta_lines.append(
            f"{sid}\t" + "\t".join(clean_cell(m.get(k, "")) for k in meta_keys) + "\n"
        )
    skill_meta_path.write_text("".join(meta_lines), encoding="utf-8")

    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    print(
        f"[done] {now} mobs={len(mobs)} skill_files={skill_files} "
        f"skill_ids={len(all_meta)} level_kv={len(level_map)} skill_err={skill_err}"
    )
    print("[wrote]", mob_path)
    print("[wrote]", skill_lv_path)
    print("[wrote]", skill_meta_path)

    # spots
    print("[spot mob 100100]", mobs.get(100100))
    for sid in (1001004, 1001005, 2001002):
        sample = [(lv, k, level_map[(sid, lv, k)]) for (s, lv, k) in level_map if s == sid][:6]
        print(f"[spot skill {sid}]", sample)

    if args.also_bin:
        ds = REPO / "bin" / "XCat_data" / "dataservice"
        ds.mkdir(parents=True, exist_ok=True)
        for p in (mob_path, skill_lv_path, skill_meta_path):
            (ds / p.name).write_text(p.read_text(encoding="utf-8"), encoding="utf-8")
            print("[bin]", ds / p.name)


if __name__ == "__main__":
    main()
