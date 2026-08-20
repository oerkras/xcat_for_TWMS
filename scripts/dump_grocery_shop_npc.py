#!/usr/bin/env python3
"""从离线 DUMP 生成经典版可卖店 NPC 种子（grocery_shop_npc.tsv）。

产品 = 经典版；枫星 = 对照来源（ResolveShop 语义）。

真源（优先序）：
  1) dumps/offline_tables/json/Npc.json 的 func（WZ String · 商人职称）
  2) 无名 func 的 name 兜底（排擋 / 名内嵌「雜貨商人」等）
  3) map_names 店图名规则（补漏无 func 的同店 NPC，如勇士村索非亞）
  4) map_life.tsv 绑定刷点地图；无刷点则跳过（无法赶路）

说明：
  - 经典版真货架仍是服端 SetShopDlg，客户端无 Commodity 全量表；本种子只解决「去哪开店」。
  - 寻店只为就近「能卖」；补给项店内有则买、无则跳过。
"""

from __future__ import annotations

import argparse
import json
import shutil
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TSV = ROOT / "dumps" / "offline_tables" / "tsv"
JSON = ROOT / "dumps" / "offline_tables" / "json"
BIN_DS = ROOT / "bin" / "XCat_data" / "dataservice"

# Npc.json func → tags（sell=可卖装；potion=偏杂货/药；feed=饲料）
SELL_FUNC_TAGS: dict[str, str] = {
    "雜貨商人": "sell|potion",
    "雜貨商": "sell|potion",
    "武器商人": "sell",
    "防具商人": "sell",
    "武器防具商人": "sell",
    "武器/防具商人": "sell",
    "攤販": "sell|potion",
    "商人": "sell|potion",
    "藥水製作師": "sell|potion",
    # 饲料店不要带 sell|potion：通用寻店会当成杂货（BIN 2026-08-20 市集科爾）
    "寵物飼料商人": "feed",
    "寵物商人": "feed",
    "卷軸商人": "sell",
    "捲軸商人": "sell",
}

# 明确不是「能卖装」的店面/职称
EXCLUDE_FUNC = {
    "情報商人",
    "倉庫商人",
    "倉庫老闆",
    "倉庫管理員",
    "倉庫管理人",
    "倉庫保管員",
    "道具製作家",  # BIN 4bb7ea：後街吉姆等，任务/制作人
    "網咖用武器出租",
    "商店主人",  # 自由市场类
    "現金商店小幫手",
}

# 无 func 时按名字兜底（排擋等 WZ 未标职称）
NAME_FALLBACK_TAGS: tuple[tuple[tuple[str, ...], str], ...] = (
    (("排擋", "排档"), "sell|potion"),
    (("自動販賣機", "自动贩卖机"), "sell|potion"),
    (("雜貨商人", "杂货商人"), "sell|potion"),
    (("武器商人",), "sell"),
    (("防具商人",), "sell"),
)

# 地图名命中 → 该图 life NPC 补漏（无 func 的同店伙计）
MAP_RULES = (
    (("雜貨店", "杂货店", "藥店", "药店", "道具店"), "sell|potion"),
    (("武器店", "防具店"), "sell"),
)
MAP_SKIP = ("美髮", "美发", "修理", "被轟炸", "資料商店", "资料商店")

FEED_NAME_KEYS = ("妖精 蓮", "妖精蓮")


def load_tsv_map(path: Path, key_idx: int = 0) -> dict[str, list[str]]:
    out: dict[str, list[str]] = {}
    if not path.is_file():
        return out
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) <= key_idx:
            continue
        key = parts[key_idx].strip()
        if key.isdigit():
            key = str(int(key))
        out[key] = parts
    return out


def norm_id(raw: str) -> str:
    raw = raw.strip()
    if raw.isdigit():
        return str(int(raw))
    return raw


def tags_for_map_name(map_name: str) -> str | None:
    if any(s in map_name for s in MAP_SKIP):
        return None
    for keys, tags in MAP_RULES:
        if any(k in map_name for k in keys):
            return tags
    return None


def tags_for_npc(name: str, func: str) -> str | None:
    if func in EXCLUDE_FUNC:
        return None
    if func in SELL_FUNC_TAGS:
        return SELL_FUNC_TAGS[func]
    for keys, tags in NAME_FALLBACK_TAGS:
        if any(k in name for k in keys):
            return tags
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--also-bin", action="store_true", help="同步到 bin/XCat_data/dataservice")
    args = ap.parse_args()

    npc_json_path = JSON / "Npc.json"
    if not npc_json_path.is_file():
        raise SystemExit(f"missing {npc_json_path}")
    npc_json: dict = json.loads(npc_json_path.read_text(encoding="utf-8"))

    map_names = load_tsv_map(TSV / "map_names.tsv")
    npc_names = load_tsv_map(TSV / "npc_names.tsv")

    # npc_id -> [map_id...]
    life_by_npc: dict[str, list[str]] = defaultdict(list)
    # map_id -> [(npc_id, name)]
    life_by_map: dict[str, list[tuple[str, str]]] = defaultdict(list)
    life_path = TSV / "map_life.tsv"
    for line in life_path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        p = line.split("\t")
        if len(p) < 4 or p[2] != "n":
            continue
        mid, nid = norm_id(p[0]), norm_id(p[3])
        name = ""
        if nid in npc_json and isinstance(npc_json[nid], dict):
            name = str(npc_json[nid].get("name") or "")
        if not name and nid in npc_names and len(npc_names[nid]) >= 2:
            name = npc_names[nid][1]
        life_by_npc[nid].append(mid)
        life_by_map[mid].append((nid, name))

    # rows: (npc_id, map_id, tags, note)
    rows: list[tuple[str, str, str, str]] = []
    seen: set[tuple[str, str]] = set()

    def add(npc_id: str, map_id: str, tags: str, note: str) -> None:
        if not map_id:
            return
        key = (npc_id, map_id)
        if key in seen:
            return
        seen.add(key)
        name = ""
        if npc_id in npc_json and isinstance(npc_json[npc_id], dict):
            name = str(npc_json[npc_id].get("name") or "")
        if not name and npc_id in npc_names and len(npc_names[npc_id]) >= 2:
            name = npc_names[npc_id][1]
        if any(k in name for k in FEED_NAME_KEYS) and "feed" not in tags:
            tags = f"{tags}|feed" if tags else "sell|potion|feed"
        rows.append((npc_id, map_id, tags, note or name))

    # 1) Npc.json func / name 真源
    skipped_no_map = 0
    for nid, obj in npc_json.items():
        if not isinstance(obj, dict):
            continue
        nid = norm_id(nid)
        name = str(obj.get("name") or "")
        func = str(obj.get("func") or "")
        tags = tags_for_npc(name, func)
        if not tags:
            continue
        maps = sorted(set(life_by_npc.get(nid, [])), key=lambda x: int(x) if x.isdigit() else 0)
        if not maps:
            skipped_no_map += 1
            continue
        why = f"func={func}" if func in SELL_FUNC_TAGS else "name"
        for mid in maps:
            mname = map_names.get(mid, ["", "", ""])
            mlabel = mname[2] if len(mname) > 2 else mid
            add(nid, mid, tags, f"{mlabel} {name} [{why}]".strip())

    # 2) 店图名补漏：同图无 func 的伙计（如索非亞）
    for mid, parts in map_names.items():
        street = parts[1] if len(parts) > 1 else ""
        mname = parts[2] if len(parts) > 2 else ""
        blob = street + mname
        tags = tags_for_map_name(blob)
        if not tags:
            continue
        for nid, name in life_by_map.get(mid, []):
            # 已由 func 收录则跳过；排除明确非店
            func = ""
            if nid in npc_json and isinstance(npc_json[nid], dict):
                func = str(npc_json[nid].get("func") or "")
            if func in EXCLUDE_FUNC:
                continue
            add(nid, mid, tags, f"{mname} {name} [map]".strip())

    rows.sort(key=lambda r: (int(r[1]) if r[1].isdigit() else 0, int(r[0]) if r[0].isdigit() else 0))

    out_path = TSV / "grocery_shop_npc.tsv"
    lines = [
        "# Classic TWMS grocery/sell NPC seed — generated by scripts/dump_grocery_shop_npc.py",
        "# Sources: Npc.json(func) + map_life + map_names（无 SetShopDlg 货架表）",
        "# npc_id\tmap_id\ttags\tnote",
        "# tags: sell=可卖装, potion=药水杂货, feed=饲料优先",
    ]
    for npc_id, map_id, tags, note in rows:
        note = note.replace("\t", " ").strip()
        lines.append(f"{npc_id}\t{map_id}\t{tags}\t{note}")
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(
        f"wrote {out_path} rows={len(rows)} unique_npc={len({r[0] for r in rows})} "
        f"skipped_no_map={skipped_no_map}"
    )

    if args.also_bin:
        BIN_DS.mkdir(parents=True, exist_ok=True)
        dst = BIN_DS / "grocery_shop_npc.tsv"
        shutil.copy2(out_path, dst)
        print(f"copied {dst}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
