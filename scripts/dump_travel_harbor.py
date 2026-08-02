#!/usr/bin/env python3
"""生成经典版 TWMS travel_harbor.tsv（跨板块码头种子）。

列契约对照枫星 dumps/fengxing_routes/travel_harbor.tsv：
  fromPlate \\t toPlate \\t harborKey \\t harborName \\t note

语义：goto 跨板块时只赶到 harborKey，摆渡由用户自行完成（不自动上船）。

经典版事实（map_info 实证，勿硬套枫星 Orbis 枢纽）：
  - 客户端几乎无 Orbis/Ludi/Leafre/… 码头图（Orbis 仅见公会本部等）
  - 真跨板块摆渡主路径：楓之島 000060000 ↔ 維多利亞港 104000000
  - 地铁售票处 / 鯨魚號碼頭 属维多利亚岛同板块，不进本表

板块名：
  maple     — 图号 < 100000000（楓之島 / 彩虹之地教程带）
  victoria  — 100000000..199999999
  其余枫星 plate 名保留解析兼容，但本客户端无对应码头则不写边
"""
from __future__ import annotations

import argparse
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TSV = ROOT / "dumps" / "offline_tables" / "tsv"
ROUTES = ROOT / "dumps" / "twms_routes"
BIN_STATE = ROOT / "bin" / "XCat_data" / "state"
BIN_DS = ROOT / "bin" / "XCat_data" / "dataservice"

# (from, to, map_id, name, note) — name 可空，空则从 map_names 补
CURATED: list[tuple[str, str, int, str, str]] = [
    (
        "maple",
        "victoria",
        60000,
        "楓之港",
        "楓之島→維多利亞（乘船；擺渡自行）",
    ),
    (
        "victoria",
        "maple",
        104000000,
        "維多利亞港",
        "維多利亞→楓之島（乘船；擺渡自行）",
    ),
]


def load_map_info_ids() -> set[int]:
    path = TSV / "map_info.tsv"
    out: set[int] = set()
    for ln in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not ln.strip() or ln.startswith("#"):
            continue
        try:
            out.add(int(ln.split("\t", 1)[0], 10))
        except ValueError:
            pass
    return out


def load_map_names() -> dict[int, str]:
    path = TSV / "map_names.tsv"
    out: dict[int, str] = {}
    for ln in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not ln.strip() or ln.startswith("#"):
            continue
        p = ln.split("\t")
        if len(p) < 3:
            continue
        try:
            mid = int(p[0], 10)
        except ValueError:
            continue
        out[mid] = p[2].strip() or p[1].strip()
    return out


def pad_key(mid: int) -> str:
    return f"{mid:09d}"


def build() -> tuple[Path, int, list[str]]:
    info = load_map_info_ids()
    names = load_map_names()
    warnings: list[str] = []
    rows: list[str] = []
    for frm, to, mid, harbor_name, note in CURATED:
        if mid not in info:
            warnings.append(f"SKIP {frm}->{to} harbor {mid}: 不在 map_info")
            continue
        key = pad_key(mid)
        disp = harbor_name or names.get(mid, key)
        rows.append(f"{frm}\t{to}\t{key}\t{disp}\t{note}")

    ROUTES.mkdir(parents=True, exist_ok=True)
    out = ROUTES / "travel_harbor.tsv"
    header = [
        "# fromPlate\ttoPlate\tharborKey\tharborName\tnote",
        "# 跨板块不自动摆渡：goto 只赶到 harborKey，摆渡由用户自行完成。",
        "# 产品=经典版 TWMS；枫星=对照列契约。Orbis 枢纽边省略（本客户端无对应码头图）。",
        "# plate: maple=<100000000；victoria=1xxxxxxxx；移植 travel 时需解析 maple。",
        "# source=twms-curated+map_info-verified",
    ]
    out.write_text("\n".join(header + rows) + "\n", encoding="utf-8")
    return out, len(rows), warnings


def update_source(n: int) -> None:
    path = ROOT / "dumps" / "offline_tables" / "SOURCE.md"
    if not path.is_file():
        return
    text = path.read_text(encoding="utf-8")
    marker = "## 跨板块码头（travel_harbor）"
    block = f"""{marker}

- 脚本：`scripts/dump_travel_harbor.py`
- 产出：`dumps/twms_routes/travel_harbor.tsv` → **{n}** 边（枫之岛↔维多利亚港）
- 运行时：`bin/XCat_data/state/travel_harbor.tsv`
- 说明：经典版无 Orbis/玩具城等枢纽码头图，**不**复制枫星 Orbis hub 边；地铁/鲸鱼号同属维多利亚板块，不入表

"""
    if marker in text:
        i = text.index(marker)
        j = text.find("\n## ", i + 1)
        text = text[:i] + block + (text[j:] if j >= 0 else "")
    else:
        key = "## 重跑"
        if key in text:
            text = text.replace(key, block + key)
        else:
            text += "\n" + block
    if "dump_travel_harbor.py" not in text:
        text = text.replace(
            "python scripts/dump_feature_catalogs.py --also-bin\n```",
            "python scripts/dump_feature_catalogs.py --also-bin\n"
            "python scripts/dump_travel_harbor.py --also-bin\n```",
        )
    path.write_text(text, encoding="utf-8")
    if BIN_DS.is_dir():
        shutil.copy2(path, BIN_DS / "SOURCE.md")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--also-bin", action="store_true")
    args = ap.parse_args()

    out, n, warnings = build()
    for w in warnings:
        print("WARN:", w)
    print(f"travel_harbor.tsv → {n} 边  {out}")
    for ln in out.read_text(encoding="utf-8").splitlines():
        if ln.startswith("#") or not ln.strip():
            continue
        print(" ", ln)

    if args.also_bin:
        dst = BIN_STATE / "travel_harbor.tsv"
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(out, dst)
        print(f"synced → {dst.relative_to(ROOT)}")

    update_source(n)
    return 0 if n > 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
