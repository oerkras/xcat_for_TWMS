#!/usr/bin/env python3
"""
Dump Classic TWMS offline tables from Addressables TextAssets → TSV pack.

对照枫星 `dumps/Lua/dataservice` + `pack-dataservice.mjs`：
  - 真源：客户端 StreamingAssets/aa/w/json_*.bundle 内 String TextAsset
  - 产出：dumps/offline_tables/tsv/ + bin/XCat_data/dataservice/ 小包

用法：
  python scripts/dump_offline_tables.py
  python scripts/dump_offline_tables.py --game-root "G:\\Games\\maplestory_classic"
"""
from __future__ import annotations

import argparse
import json
import shutil
from datetime import datetime, timezone
from pathlib import Path

import UnityPy

REPO = Path(__file__).resolve().parents[1]
DEFAULT_GAME = Path(r"G:\Games\maplestory_classic")

STRING_BUNDLE = "json_a2909ccd82f5e94bf57297c362b91c92.bundle"
STRING_TABLES = [
    "Item",
    "Map",
    "Mob",
    "Npc",
    "Skill",
    "SkillBook",
    "Pet",
    "QuestData",
    "WorldMap",
    "WorldName",
    "Book",
    "MonsterBook",
    "Category",
    "Tips",
]


def text_of(data) -> bytes:
    script = getattr(data, "script", None)
    if script is not None:
        return bytes(script) if not isinstance(script, str) else script.encode("utf-8")
    raw = getattr(data, "m_Script", None)
    if raw is None:
        return b""
    return raw.encode("utf-8") if isinstance(raw, str) else bytes(raw)


def item_category(item_id: int) -> tuple[str, str]:
    """Classic id → (category, kind) 粗分，对齐枫星 catalog 列。"""
    # 标准 MapleStory 分段
    if 1000000 <= item_id < 2000000:
        return "Equip", "Equip"
    if 2000000 <= item_id < 3000000:
        return "Consume", "Consume"
    if 3000000 <= item_id < 4000000:
        return "Install", "Install"
    if 4000000 <= item_id < 5000000:
        return "Etc", "Etc"
    if 5000000 <= item_id < 6000000:
        return "Cash", "Cash"
    if item_id < 1000000:
        # 脸型/发型等 Character
        return "Character", "Character"
    return "Other", "Other"


def extract_string_tables(bundle: Path, out_json: Path) -> dict[str, dict]:
    out_json.mkdir(parents=True, exist_ok=True)
    env = UnityPy.load(str(bundle))
    tables: dict[str, dict] = {}
    for obj in env.objects:
        if obj.type.name != "TextAsset":
            continue
        data = obj.read()
        name = getattr(data, "name", None) or getattr(data, "m_Name", None) or ""
        if name not in STRING_TABLES:
            continue
        blob = text_of(data)
        text = blob.decode("utf-8-sig")
        (out_json / f"{name}.json").write_text(text, encoding="utf-8")
        tables[name] = json.loads(text)
        print(f"[json] {name}: keys={len(tables[name])} bytes={len(blob)}")
    return tables


def write_item_catalog(item_table: dict, out_tsv: Path) -> int:
    lines = ["# code\tcategory\tkind\tsubkind\tname_zh\n"]
    n = 0
    for code, row in sorted(item_table.items(), key=lambda kv: int(kv[0]) if str(kv[0]).isdigit() else 0):
        if not str(code).isdigit():
            continue
        item_id = int(code)
        name = ""
        if isinstance(row, dict):
            name = str(row.get("name") or row.get("Name") or "")
        cat, kind = item_category(item_id)
        # TSV escape
        name = name.replace("\t", " ").replace("\n", " ").replace("\r", " ")
        lines.append(f"{item_id}\t{cat}\t{kind}\t\t{name}\n")
        n += 1
    out_tsv.write_text("".join(lines), encoding="utf-8")
    return n


def write_simple_name_tsv(table: dict, out_tsv: Path, name_keys: tuple[str, ...]) -> int:
    lines = ["# code\tname_zh\n"]
    n = 0
    for code, row in sorted(table.items(), key=lambda kv: int(kv[0]) if str(kv[0]).isdigit() else 0):
        if not str(code).isdigit():
            continue
        name = ""
        if isinstance(row, dict):
            for k in name_keys:
                if k in row and row[k]:
                    name = str(row[k])
                    break
        elif isinstance(row, str):
            name = row
        name = name.replace("\t", " ").replace("\n", " ").replace("\r", " ")
        lines.append(f"{int(code)}\t{name}\n")
        n += 1
    out_tsv.write_text("".join(lines), encoding="utf-8")
    return n


def write_map_tsv(table: dict, out_tsv: Path) -> int:
    lines = ["# code\tstreetName\tmapName\tmapDesc\n"]
    n = 0
    for code, row in sorted(table.items(), key=lambda kv: int(kv[0]) if str(kv[0]).isdigit() else 0):
        if not str(code).isdigit() or not isinstance(row, dict):
            continue

        def g(k: str) -> str:
            return str(row.get(k) or "").replace("\t", " ").replace("\n", " ").replace("\r", " ")

        lines.append(f"{int(code)}\t{g('streetName')}\t{g('mapName')}\t{g('mapDesc')}\n")
        n += 1
    out_tsv.write_text("".join(lines), encoding="utf-8")
    return n


def write_skill_tsv(table: dict, out_tsv: Path) -> int:
    lines = ["# code\tName\tDesc\n"]
    n = 0
    for code, row in sorted(table.items(), key=lambda kv: int(kv[0]) if str(kv[0]).isdigit() else 0):
        if not str(code).isdigit() or not isinstance(row, dict):
            continue

        def g(k: str) -> str:
            return str(row.get(k) or "").replace("\t", " ").replace("\n", " ").replace("\r", " ")

        lines.append(f"{int(code)}\t{g('Name')}\t{g('Desc')}\n")
        n += 1
    out_tsv.write_text("".join(lines), encoding="utf-8")
    return n


def _cell(v) -> str:
    return str(v or "").replace("\t", " ").replace("\r", " ").replace("\n", "\\n")


def write_pet_tsv(table: dict, out_tsv: Path) -> int:
    lines = ["# code\tname\tdesc\n"]
    n = 0
    for code, row in sorted(table.items(), key=lambda kv: int(kv[0]) if str(kv[0]).isdigit() else 0):
        if not str(code).isdigit() or not isinstance(row, dict):
            continue
        lines.append(f"{int(code)}\t{_cell(row.get('name'))}\t{_cell(row.get('desc'))}\n")
        n += 1
    out_tsv.write_text("".join(lines), encoding="utf-8")
    return n


def write_quest_tsv(table: dict, out_tsv: Path) -> int:
    lines = ["# code\tname\n"]
    n = 0
    for code, row in sorted(table.items(), key=lambda kv: int(kv[0]) if str(kv[0]).isdigit() else 0):
        if not str(code).isdigit() or not isinstance(row, dict):
            continue
        lines.append(f"{int(code)}\t{_cell(row.get('name'))}\n")
        n += 1
    out_tsv.write_text("".join(lines), encoding="utf-8")
    return n


def write_skillbook_tsv(table: dict, out_tsv: Path) -> int:
    lines = ["# code\tName\n"]
    n = 0
    for code, row in sorted(table.items(), key=lambda kv: int(kv[0]) if str(kv[0]).isdigit() else 0):
        if not str(code).isdigit() or not isinstance(row, dict):
            continue
        lines.append(f"{int(code)}\t{_cell(row.get('Name') or row.get('name'))}\n")
        n += 1
    out_tsv.write_text("".join(lines), encoding="utf-8")
    return n


def write_category_tsv(table: dict, out_tsv: Path) -> int:
    lines = ["# code\tcategory\tcategorySub\tname\n"]
    n = 0
    for code, row in sorted(table.items(), key=lambda kv: int(kv[0]) if str(kv[0]).isdigit() else 0):
        if not str(code).isdigit() or not isinstance(row, dict):
            continue
        lines.append(
            f"{int(code)}\t{_cell(row.get('category'))}\t"
            f"{_cell(row.get('categorySub'))}\t{_cell(row.get('name'))}\n"
        )
        n += 1
    out_tsv.write_text("".join(lines), encoding="utf-8")
    return n


def write_monsterbook_tsv(table: dict, out_tsv: Path) -> int:
    lines = ["# code\ttext\n"]
    n = 0
    for code, row in sorted(table.items(), key=lambda kv: int(kv[0]) if str(kv[0]).isdigit() else 0):
        if not str(code).isdigit():
            continue
        text = row if isinstance(row, str) else str(row)
        lines.append(f"{int(code)}\t{_cell(text)}\n")
        n += 1
    out_tsv.write_text("".join(lines), encoding="utf-8")
    return n


def write_tips_tsv(table: dict, out_tsv: Path) -> int:
    lines = ["# code\ttext\n"]
    n = 0
    for code in sorted(table.keys(), key=str):
        row = table[code]
        text = row if isinstance(row, str) else str(row)
        lines.append(f"{_cell(code)}\t{_cell(text)}\n")
        n += 1
    out_tsv.write_text("".join(lines), encoding="utf-8")
    return n


def write_worldname_tsv(table: dict, out_tsv: Path) -> int:
    lines = ["# key\tname\n"]
    n = 0
    for code in sorted(table.keys(), key=str):
        row = table[code]
        name = row if isinstance(row, str) else str(row)
        lines.append(f"{_cell(code)}\t{_cell(name)}\n")
        n += 1
    out_tsv.write_text("".join(lines), encoding="utf-8")
    return n


def write_worldmap_tsv(table: dict, out_tsv: Path) -> int:
    lines = ["# world_id\tentry_key\ttext\n"]
    n = 0
    for wid, row in sorted(table.items(), key=lambda kv: int(kv[0]) if str(kv[0]).isdigit() else 0):
        if not isinstance(row, dict):
            continue
        values = row.get("values")
        if isinstance(values, str):
            try:
                import ast

                values = ast.literal_eval(values)
            except Exception:
                values = {"_raw": values}
        if not isinstance(values, dict):
            continue
        for ek, text in sorted(values.items(), key=lambda kv: str(kv[0])):
            lines.append(f"{_cell(wid)}\t{_cell(ek)}\t{_cell(text)}\n")
            n += 1
    out_tsv.write_text("".join(lines), encoding="utf-8")
    return n


def write_book_tsv(table: dict, out_tsv: Path) -> int:
    """Book pages are nested lists; keep first plain text as title hint."""
    lines = ["# code\ttitle_hint\n"]
    n = 0
    for code, row in sorted(table.items(), key=lambda kv: int(kv[0]) if str(kv[0]).isdigit() else 0):
        if not str(code).isdigit():
            continue
        title = ""
        if isinstance(row, list) and row:
            page0 = row[0]
            if isinstance(page0, list):
                for seg in page0:
                    if isinstance(seg, dict) and seg.get("text"):
                        title = str(seg["text"])
                        break
        lines.append(f"{int(code)}\t{_cell(title)}\n")
        n += 1
    out_tsv.write_text("".join(lines), encoding="utf-8")
    return n


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--game-root", type=Path, default=DEFAULT_GAME)
    ap.add_argument("--out", type=Path, default=REPO / "dumps" / "offline_tables")
    ap.add_argument("--also-bin", action="store_true", help="同步拷到 bin/XCat_data/dataservice")
    args = ap.parse_args()

    bundle = args.game_root / "Maplestory_Classic_Data" / "StreamingAssets" / "aa" / "w" / STRING_BUNDLE
    if not bundle.is_file():
        raise SystemExit(f"找不到 String bundle: {bundle}")

    out = args.out
    json_dir = out / "json"
    tsv_dir = out / "tsv"
    tsv_dir.mkdir(parents=True, exist_ok=True)

    tables = extract_string_tables(bundle, json_dir)
    if "Item" not in tables:
        raise SystemExit("String bundle 缺少 Item TextAsset")

    stats = {}
    stats["item_catalog"] = write_item_catalog(tables["Item"], tsv_dir / "item_catalog.tsv")
    if "Mob" in tables:
        stats["mob_names"] = write_simple_name_tsv(tables["Mob"], tsv_dir / "mob_names.tsv", ("name", "Name"))
    if "Npc" in tables:
        stats["npc_names"] = write_simple_name_tsv(tables["Npc"], tsv_dir / "npc_names.tsv", ("name", "Name"))
    if "Map" in tables:
        stats["map_names"] = write_map_tsv(tables["Map"], tsv_dir / "map_names.tsv")
    if "Skill" in tables:
        stats["skill_names"] = write_skill_tsv(tables["Skill"], tsv_dir / "skill_names.tsv")
    if "Pet" in tables:
        stats["pet_names"] = write_pet_tsv(tables["Pet"], tsv_dir / "pet_names.tsv")
    if "QuestData" in tables:
        stats["quest_names"] = write_quest_tsv(tables["QuestData"], tsv_dir / "quest_names.tsv")
    if "SkillBook" in tables:
        stats["skillbook_names"] = write_skillbook_tsv(tables["SkillBook"], tsv_dir / "skillbook_names.tsv")
    if "Category" in tables:
        stats["cash_category"] = write_category_tsv(tables["Category"], tsv_dir / "cash_category.tsv")
    if "MonsterBook" in tables:
        stats["monster_book"] = write_monsterbook_tsv(tables["MonsterBook"], tsv_dir / "monster_book.tsv")
    if "Tips" in tables:
        stats["tips"] = write_tips_tsv(tables["Tips"], tsv_dir / "tips.tsv")
    if "WorldName" in tables:
        stats["world_names"] = write_worldname_tsv(tables["WorldName"], tsv_dir / "world_names.tsv")
    if "WorldMap" in tables:
        stats["worldmap_labels"] = write_worldmap_tsv(tables["WorldMap"], tsv_dir / "worldmap_labels.tsv")
    if "Book" in tables:
        stats["book_titles"] = write_book_tsv(tables["Book"], tsv_dir / "book_titles.tsv")

    print("[stats]", stats)

    if args.also_bin:
        bin_ds = REPO / "bin" / "XCat_data" / "dataservice"
        bin_ds.mkdir(parents=True, exist_ok=True)
        for name in (
            "item_catalog.tsv",
            "mob_names.tsv",
            "npc_names.tsv",
            "map_names.tsv",
            "skill_names.tsv",
            "pet_names.tsv",
            "quest_names.tsv",
            "skillbook_names.tsv",
            "cash_category.tsv",
            "monster_book.tsv",
            "tips.tsv",
            "world_names.tsv",
            "worldmap_labels.tsv",
            "book_titles.tsv",
        ):
            src = tsv_dir / name
            if src.is_file():
                shutil.copy2(src, bin_ds / name)
                print(f"[bin] {bin_ds / name}")


if __name__ == "__main__":
    main()
