#!/usr/bin/env python3
"""
Dump Classic TWMS Map/*.wzjson → travel seed + map meta + life + script portals.

对照枫星：
  - travel_graph.seed.tsv ← MapData portal pn/tm/tn
  - travel_script_portal.tsv ← tm 无效但有 script 的门

用法：
  python scripts/dump_map_tables.py
  python scripts/dump_map_tables.py --also-bin
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
from wzjs_io import TYPE_INT, TYPE_STRING, clean_cell, item_value, parse_mb_wzjson  # noqa: E402

REPO = Path(__file__).resolve().parents[1]
DEFAULT_GAME = Path(r"G:\Games\maplestory_classic")

PORTAL_FIELD_RE = re.compile(
    r"(?:^|/)portal/(\d+)/(pn|pt|tm|tn|script|x|y|delay|hideTooltip|onlyOnce)$"
)
LIFE_FIELD_RE = re.compile(r"(?:^|/)life/(\d+)/(type|id|x|y|fh|cy|f|rx0|rx1)$")
INFO_FIELDS = {
    "returnMap",
    "forcedReturn",
    "mapMark",
    "town",
    "fieldLimit",
    "mobRate",
    "swim",
    "fly",
    "hideMinimap",
    "VRTop",
    "VRLeft",
    "VRBottom",
    "VRRight",
    "version",
    "cloud",
    "bgm",
}


def pad_map(map_id: int) -> str:
    return f"{map_id:09d}"


def is_bogus_tm(tm: int) -> bool:
    return tm <= 0 or tm == 999999999 or tm == -1


def map_id_from_key(key: str) -> int | None:
    name = Path(key.replace("\\", "/")).stem
    if name.isdigit():
        return int(name)
    return None


def iter_map_wzjson(game_root: Path):
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
            if "/Json/Map/Map/" not in norm or not norm.endswith(".wzjson"):
                continue
            if "AreaCode" in norm:
                continue
            mid = map_id_from_key(norm)
            if mid is None:
                continue
            obj = next((o for o in env.objects if o.path_id == info.asset.m_PathID), None)
            if obj is None:
                continue
            try:
                raw = obj.get_raw_data()
            except Exception as e:
                print(f"[skip] {bundle_path.name} {key}: {e}")
                continue
            yield bundle_path.name, norm, mid, raw


def extract_map_fields(raw: bytes) -> tuple[dict[int, dict], dict, dict[int, dict]]:
    doc = parse_mb_wzjson(raw)
    portals: dict[int, dict] = defaultdict(dict)
    life: dict[int, dict] = defaultdict(dict)
    info: dict = {}

    for it in doc.items:
        if not (0 <= it.name_index < len(doc.names)):
            continue
        n = doc.names[it.name_index]
        path = doc.paths[it.path_index] if 0 <= it.path_index < len(doc.paths) else ""
        path = path.replace("\\", "/")
        val = item_value(doc, it)

        pm = PORTAL_FIELD_RE.search(path)
        if pm:
            idx = int(pm.group(1))
            field = pm.group(2)
            if val is not None:
                portals[idx][field] = val
            continue

        lm = LIFE_FIELD_RE.search(path)
        if lm:
            idx = int(lm.group(1))
            field = lm.group(2)
            if val is not None:
                life[idx][field] = val
            continue

        if n in INFO_FIELDS and (path == n or path.endswith("/" + n) or path.startswith("info/")):
            if val is not None:
                info[n] = val
            elif it.type == TYPE_STRING and 0 <= it.data_index < len(doc.strings):
                info[n] = doc.strings[it.data_index]
            elif n == path or path.endswith("/" + n):
                # string typed via name pool fallback already in item_value
                pass

        # relative leaf under info/
        if path.startswith("info/") and n in INFO_FIELDS and val is not None:
            info[n] = val
        if path == "info/" + n and n in INFO_FIELDS:
            if val is not None:
                info[n] = val
            elif it.type == TYPE_STRING and 0 <= it.data_index < len(doc.strings):
                info[n] = doc.strings[it.data_index]

    return dict(portals), info, dict(life)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--game-root", type=Path, default=DEFAULT_GAME)
    ap.add_argument("--out", type=Path, default=REPO / "dumps" / "offline_tables")
    ap.add_argument("--also-bin", action="store_true")
    args = ap.parse_args()

    tsv_dir = args.out / "tsv"
    routes_dir = REPO / "dumps" / "twms_routes"
    tsv_dir.mkdir(parents=True, exist_ok=True)
    routes_dir.mkdir(parents=True, exist_ok=True)

    seed_lines: list[str] = []
    script_lines: list[str] = []
    info_lines = [
        "# map_id\treturnMap\tforcedReturn\tmapMark\ttown\tfieldLimit\tmobRate\tbgm"
        "\tVRTop\tVRLeft\tVRBottom\tVRRight\n"
    ]
    life_lines = ["# map_id\tlife_idx\ttype\tid\tx\ty\n"]

    maps = 0
    edge_n = 0
    script_n = 0
    life_n = 0
    errors = 0
    skipped_sp = skipped_bogus = skipped_self = 0

    for bname, key, mid, raw in iter_map_wzjson(args.game_root):
        try:
            portals, info, life = extract_map_fields(raw)
        except Exception as e:
            errors += 1
            print(f"[fail] {bname} {key}: {e}")
            continue
        maps += 1
        from_key = pad_map(mid)

        def g(k: str) -> str:
            v = info.get(k, "")
            return clean_cell(v)

        info_lines.append(
            f"{mid}\t{g('returnMap')}\t{g('forcedReturn')}\t{g('mapMark')}\t"
            f"{g('town')}\t{g('fieldLimit')}\t{g('mobRate')}\t{g('bgm')}\t"
            f"{g('VRTop')}\t{g('VRLeft')}\t{g('VRBottom')}\t{g('VRRight')}\n"
        )

        for li in sorted(life):
            row = life[li]
            life_lines.append(
                f"{mid}\t{li}\t{clean_cell(row.get('type', ''))}\t"
                f"{clean_cell(row.get('id', ''))}\t"
                f"{clean_cell(row.get('x', ''))}\t{clean_cell(row.get('y', ''))}\n"
            )
            life_n += 1

        for pi in sorted(portals):
            p = portals[pi]
            pn = str(p.get("pn", "")).strip()
            tm = p.get("tm")
            tn = str(p.get("tn", "")).strip()
            script = str(p.get("script", "")).strip()
            pt = p.get("pt", "")
            if not pn or pn == "sp":
                skipped_sp += 1
                continue
            if tm is None or not isinstance(tm, int) or is_bogus_tm(tm):
                skipped_bogus += 1
                if script:
                    script_lines.append(
                        f"{from_key}\t{pn}\t{clean_cell(pt)}\t{clean_cell(script)}\t"
                        f"{clean_cell(p.get('x', ''))}\t{clean_cell(p.get('y', ''))}\n"
                    )
                    script_n += 1
                continue
            dest_key = pad_map(int(tm))
            if dest_key == from_key:
                skipped_self += 1
                continue
            portal_id = f"seed:{from_key}/{pn}"
            # dest 与 destKey 均用 mapId（经典版无枫星 Lua map key）
            seed_lines.append(
                f"{from_key}\t{portal_id}\t{pn}\t1\t0\t{dest_key}\t{dest_key}\n"
            )
            edge_n += 1

        if maps % 100 == 0:
            print(f"[progress] maps={maps} edges={edge_n}")

    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    seed_header = (
        "# travel_graph v3\tfromMap\tportalId\tname\tvis\tfm\tdest\tdestKey"
        "（经典版节点=mapId；对照枫星 seed 格式）\n"
        f"# generated_by=dump_map_tables.py updated={now} maps={maps} edges={edge_n} "
        f"skip_sp={skipped_sp} skip_bogus={skipped_bogus} skip_self={skipped_self}\n"
    )
    script_header = (
        "# travel_script_portal v1\tfromMap\tportalName\tpt\tscript\tx\ty\n"
        f"# generated_by=dump_map_tables.py updated={now} rows={script_n} "
        "（tm 无效；需脚本/运行时解析目标）\n"
    )

    seed_path = routes_dir / "travel_graph.seed.tsv"
    script_path = routes_dir / "travel_script_portal.tsv"
    info_path = tsv_dir / "map_info.tsv"
    life_path = tsv_dir / "map_life.tsv"

    seed_path.write_text(seed_header + "".join(sorted(seed_lines)), encoding="utf-8")
    script_path.write_text(script_header + "".join(sorted(script_lines)), encoding="utf-8")
    info_path.write_text("".join(info_lines), encoding="utf-8")
    life_path.write_text("".join(life_lines), encoding="utf-8")

    print(
        f"[done] maps={maps} edges={edge_n} script={script_n} life={life_n} "
        f"errors={errors} skip_sp={skipped_sp} skip_bogus={skipped_bogus} skip_self={skipped_self}"
    )
    print("[wrote]", seed_path)
    print("[wrote]", script_path)
    print("[wrote]", info_path)
    print("[wrote]", life_path)

    # spot: 000010000 out00 → 000020000
    spot = [ln for ln in seed_lines if ln.startswith("000010000\t") and "\tout00\t" in ln]
    print("[spot]", spot[:3] if spot else "MISSING 000010000/out00")

    if args.also_bin:
        state = REPO / "bin" / "XCat_data" / "state"
        ds = REPO / "bin" / "XCat_data" / "dataservice"
        state.mkdir(parents=True, exist_ok=True)
        ds.mkdir(parents=True, exist_ok=True)
        (state / "travel_graph.seed.tsv").write_text(
            seed_path.read_text(encoding="utf-8"), encoding="utf-8"
        )
        (state / "travel_script_portal.tsv").write_text(
            script_path.read_text(encoding="utf-8"), encoding="utf-8"
        )
        (ds / "map_info.tsv").write_text(info_path.read_text(encoding="utf-8"), encoding="utf-8")
        (ds / "map_life.tsv").write_text(life_path.read_text(encoding="utf-8"), encoding="utf-8")
        print("[bin]", state / "travel_graph.seed.tsv")
        print("[bin]", ds / "map_info.tsv")


if __name__ == "__main__":
    main()
