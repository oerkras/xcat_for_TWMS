#!/usr/bin/env python3
"""
Dump Classic TWMS item prices from Addressables Item/*.wzjson (WZJS).

对照枫星：
  - item_value.tsv  — 物值 / 卖店价离线兜底（titlebar 等）
  - shop_prices.tsv — 商店买入价预算表（与 item_value 分表；禁止混用）

真源：StreamingAssets/aa/w/json_*.bundle 内 WzJson（magic WZJS）的 info/price。
经典版 NPC 货架来自 SetShopDlg(InPacket)，客户端无 NpcShop 全量表；
故 shop_prices 只能落 WZ catalog 基线价（非 Commodity 点券、非运行时抓包）。

用法：
  python scripts/dump_item_value_wzjs.py
  python scripts/dump_item_value_wzjs.py --also-bin
"""
from __future__ import annotations

import argparse
import re
import struct
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

import UnityPy

REPO = Path(__file__).resolve().parents[1]
DEFAULT_GAME = Path(r"G:\Games\maplestory_classic")

HDR_FIELDS = [
    "itemCount",
    "itemOffset",
    "itemOrderCount",
    "itemOrderOffset",
    "boolCount",
    "boolOffset",
    "byteCount",
    "byteOffset",
    "shortCount",
    "shortOffset",
    "intCount",
    "intOffset",
    "longCount",
    "longOffset",
    "floatCount",
    "floatOffset",
    "vector2Count",
    "vector2Offset",
    "doubleCount",
    "doubleOffset",
    "vector2IntCount",
    "vector2IntOffset",
    "rectCount",
    "rectOffset",
    "rectIntCount",
    "rectIntOffset",
    "nameCount",
    "nameOffset",
    "nameOffsetOffset",
    "pathCount",
    "pathOffset",
    "pathOffsetOffset",
    "stringCount",
    "stringOffset",
    "stringOffsetOffset",
]

TYPE_PROPERTY = 2
TYPE_BOOL = 3
TYPE_INT = 6
ITEM_SIZE = 0x20


@dataclass
class WzItem:
    index: int
    type: int
    name_index: int
    data_index: int
    child_offset: int
    child_count: int
    parent_index: int
    path_index: int
    order_offset: int


def parse_mb_wzjson(raw: bytes) -> tuple[str, dict, bytes]:
    name_len = struct.unpack_from("<i", raw, 0x1C)[0]
    if name_len <= 0 or name_len > 256:
        raise ValueError(f"bad name_len={name_len}")
    name = raw[0x20 : 0x20 + name_len].decode("utf-8", errors="replace")
    name_padded = (name_len + 3) & ~3
    hdr_off = 0x20 + name_padded
    hdr = {f: struct.unpack_from("<i", raw, hdr_off + i * 4)[0] for i, f in enumerate(HDR_FIELDS)}
    data_len_off = hdr_off + len(HDR_FIELDS) * 4
    data_len = struct.unpack_from("<i", raw, data_len_off)[0]
    data0 = data_len_off + 4
    if raw[data0 : data0 + 4] != b"WZJS":
        raise ValueError(f"bad magic name={name!r}")
    if data_len < 8 or data0 + data_len > len(raw):
        raise ValueError(f"bad data_len={data_len}")
    return name, hdr, raw[data0 : data0 + data_len]


def read_strings(data: bytes, count: int, blob_off: int, table_off: int) -> list[str]:
    if count <= 0:
        return []
    offs = [struct.unpack_from("<i", data, table_off + i * 4)[0] for i in range(count)]
    blob = data[blob_off:]
    out: list[str] = []
    for i, start in enumerate(offs):
        end = offs[i + 1] if i + 1 < len(offs) else len(blob)
        if start < 0 or start >= len(blob) or end < start:
            out.append("")
            continue
        out.append(blob[start:end].split(b"\x00", 1)[0].decode("utf-8", errors="replace"))
    return out


def parse_items(data: bytes, hdr: dict) -> list[WzItem]:
    base = hdr["itemOffset"]
    items: list[WzItem] = []
    for i in range(hdr["itemCount"]):
        vals = struct.unpack_from("<iiiiiiii", data, base + i * ITEM_SIZE)
        items.append(WzItem(i, *vals))
    return items


def parse_int_pool(data: bytes, hdr: dict) -> list[int]:
    base = hdr["intOffset"]
    return [struct.unpack_from("<i", data, base + i * 4)[0] for i in range(max(0, hdr["intCount"]))]


def parse_bool_pool(data: bytes, hdr: dict) -> list[bool]:
    # bools packed as bytes (Unity / span of bool)
    base = hdr["boolOffset"]
    n = hdr["boolCount"]
    return [bool(data[base + i]) for i in range(max(0, n))]


_ITEM_ID_RE = re.compile(r"^0*(\d{7,8})(?:/|$)")


def item_id_from_path(path: str) -> int | None:
    # "02000000/info/price" → 2000000；相对路径 "info/price" 无 ID
    m = _ITEM_ID_RE.match(path.replace("\\", "/"))
    if not m:
        return None
    return int(m.group(1))


def item_id_from_asset_name(name: str) -> int | None:
    # Character 单文件资产名 "01002001" / "01302000"
    if not name or not name.isdigit():
        return None
    return int(name)


def extract_prices_from_wzjs(raw: bytes) -> dict[int, dict]:
    """Return {itemId: {price, notSale, path}} from one WzJson MB blob."""
    asset_name, hdr, data = parse_mb_wzjson(raw)
    names = read_strings(data, hdr["nameCount"], hdr["nameOffset"], hdr["nameOffsetOffset"])
    paths = read_strings(data, hdr["pathCount"], hdr["pathOffset"], hdr["pathOffsetOffset"])
    items = parse_items(data, hdr)
    ints = parse_int_pool(data, hdr)
    bools = parse_bool_pool(data, hdr)
    fallback_id = item_id_from_asset_name(asset_name)

    out: dict[int, dict] = {}

    for it in items:
        if not (0 <= it.name_index < len(names)):
            continue
        n = names[it.name_index]
        path = paths[it.path_index] if 0 <= it.path_index < len(paths) else ""
        iid = item_id_from_path(path)
        if iid is None:
            # Character 单装：path 常为相对 "info/price"
            if n in ("price", "notSale") and (path == n or path.endswith("/" + n) or path == "info/" + n):
                iid = fallback_id
        if iid is None:
            continue

        if n == "price" and it.type == TYPE_INT:
            if 0 <= it.data_index < len(ints):
                rec = out.setdefault(iid, {})
                rec["price"] = ints[it.data_index]
                rec["price_path"] = path
        elif n == "notSale":
            rec = out.setdefault(iid, {})
            if it.type == TYPE_BOOL and 0 <= it.data_index < len(bools):
                rec["notSale"] = 1 if bools[it.data_index] else 0
            elif it.type == TYPE_INT and 0 <= it.data_index < len(ints):
                rec["notSale"] = 1 if ints[it.data_index] else 0
            elif it.type == TYPE_PROPERTY:
                # presence-only flag in some WZ trees
                rec["notSale"] = 1
            else:
                rec["notSale"] = 1

    return out


def iter_item_wzjson_raw(game_root: Path):
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
            is_item = "/Json/Item/" in norm and norm.endswith(".wzjson")
            # 装备数值在 Character/*.wzjson（非 String；含 info/price）
            is_equip = "/Json/Character/" in norm and norm.endswith(".wzjson")
            if not (is_item or is_equip):
                continue
            # skip non-item tables
            if any(x in norm for x in ("ItemOption", "PetTable", "LinkCash", "MaplePoint")):
                continue
            obj = next((o for o in env.objects if o.path_id == info.asset.m_PathID), None)
            if obj is None:
                continue
            try:
                raw = obj.get_raw_data()
            except Exception as e:
                print(f"[skip] {bundle_path.name} {key}: {e}")
                continue
            yield bundle_path.name, key, raw


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--game-root", type=Path, default=DEFAULT_GAME)
    ap.add_argument("--out", type=Path, default=REPO / "dumps" / "offline_tables")
    ap.add_argument("--also-bin", action="store_true")
    args = ap.parse_args()

    merged: dict[int, dict] = {}
    per_bundle: dict[str, int] = {}
    errors = 0

    for bname, key, raw in iter_item_wzjson_raw(args.game_root):
        try:
            part = extract_prices_from_wzjs(raw)
        except Exception as e:
            errors += 1
            print(f"[fail] {bname} {key}: {e}")
            continue
        per_bundle[bname] = per_bundle.get(bname, 0) + len(part)
        for iid, rec in part.items():
            # later files win only if they carry price
            cur = merged.get(iid, {})
            if "price" in rec:
                cur["price"] = rec["price"]
            if "notSale" in rec:
                cur["notSale"] = rec["notSale"]
            merged[iid] = cur
        if part and "/Character/" not in key.replace("\\", "/"):
            print(f"[ok] {key}: +{len(part)}")
    print(f"[bundles] {dict(per_bundle)}")

    tsv_dir = args.out / "tsv"
    tsv_dir.mkdir(parents=True, exist_ok=True)
    # 两表同数字、不同语义：加载侧严禁用 shop 填 sell。
    value_lines = [
        "# item_code\tprice\tsource=twms-wzjs-info-price\n",
        "# 物值/卖店离线兜底；运行时优先 ItemBundle.nSellPrice → Info.price\n",
    ]
    shop_lines = [
        "# item_code\tprice\tsource=twms-wzjs-info-price-catalog\n",
        "# 经典版无客户端 NpcShop 全量表（UIShopDialog.SetShopDlg=InPacket）。\n",
        "# 本表=WZ info/price 目录基线，供买入预算等；非 Commodity 点券、非抓包店表。\n",
    ]
    kept = skipped_ns = skipped_zero = skipped_noprice = 0
    for iid in sorted(merged):
        rec = merged[iid]
        if "price" not in rec:
            skipped_noprice += 1
            continue
        price = int(rec["price"])
        if rec.get("notSale"):
            skipped_ns += 1
            continue
        if price <= 0:
            skipped_zero += 1
            continue
        row = f"{iid}\t{price}\n"
        value_lines.append(row)
        shop_lines.append(row)
        kept += 1

    out_value = tsv_dir / "item_value.tsv"
    out_shop = tsv_dir / "shop_prices.tsv"
    out_value.write_text("".join(value_lines), encoding="utf-8")
    out_shop.write_text("".join(shop_lines), encoding="utf-8")

    # spot checks
    spots = {2000000, 2000001, 2000002, 2000003, 4000000, 4000019, 1002000, 1002001, 1302000}
    print("--- spots ---")
    for iid in sorted(spots):
        print(iid, merged.get(iid))

    print(
        f"[done] kept={kept} notSale={skipped_ns} zero={skipped_zero} "
        f"noprice={skipped_noprice} errors={errors} total_ids={len(merged)}"
    )
    print("[wrote]", out_value)
    print("[wrote]", out_shop)

    source = args.out / "SOURCE.md"
    now = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%SZ")
    price_note = (
        f"\n## item_value / shop_prices（{now}）\n\n"
        f"- 脚本：`scripts/dump_item_value_wzjs.py`\n"
        f"- 行数：各 **{kept}**（跳过 notSale={skipped_ns} / price≤0={skipped_zero}）\n"
        f"- 字段：WZJS `info/price`；`notSale` 有则丢弃\n"
        f"- `item_value`：物值离线兜底（对齐 `Info.price` / `GetItemPrice`）\n"
        f"- `shop_prices`：WZ catalog 买入基线；**非**服端 `SetShopDlg` 实店、**非** Commodity\n"
        f"- 铁律：加载侧禁止用 shop 填 sell\n"
        f"- 抽检：2000000 → {merged.get(2000000)}\n"
    )
    if source.is_file():
        text = source.read_text(encoding="utf-8")
        text = re.sub(
            r"- `item_value\.tsv`[^\n]*\n",
            f"- `item_value.tsv`：{kept} 行（WZJS `info/price`，见下节）\n",
            text,
            count=1,
        )
        text = re.sub(
            r"- `shop_prices\.tsv`[^\n]*\n",
            f"- `shop_prices.tsv`：{kept} 行（WZ catalog 基线，见下节）\n",
            text,
            count=1,
        )
        text = re.sub(
            r"\n## 尚未抽出\n\n- `shop_prices\.tsv`[^\n]*\n?",
            "\n",
            text,
            count=1,
        )
        text = re.sub(
            r"\n## item_value(?: / shop_prices)?（.*",
            "\n" + price_note.lstrip(),
            text,
            flags=re.S,
            count=1,
        )
        if "## item_value" not in text and "## item_value / shop_prices" not in text:
            text = text.rstrip() + "\n" + price_note
        source.write_text(text, encoding="utf-8")
    else:
        source.write_text(
            "# 经典版 TWMS · 离线表 DUMP\n" + price_note,
            encoding="utf-8",
        )

    if args.also_bin:
        bin_ds = REPO / "bin" / "XCat_data" / "dataservice"
        bin_ds.mkdir(parents=True, exist_ok=True)
        (bin_ds / "item_value.tsv").write_text("".join(value_lines), encoding="utf-8")
        (bin_ds / "shop_prices.tsv").write_text("".join(shop_lines), encoding="utf-8")
        if source.is_file():
            (bin_ds / "SOURCE.md").write_text(source.read_text(encoding="utf-8"), encoding="utf-8")
        print("[bin]", bin_ds / "item_value.tsv")
        print("[bin]", bin_ds / "shop_prices.tsv")


if __name__ == "__main__":
    main()
