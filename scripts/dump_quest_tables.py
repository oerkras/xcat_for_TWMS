#!/usr/bin/env python3
"""
Dump Classic TWMS quest deep tables.

WZJS（Quest/*.wzjson）:
  quest_act.tsv / quest_check.tsv / quest_say_keys.tsv
  quest_exclusive.tsv / quest_pquest.tsv / quest_pquest_search.tsv

String（QuestData.json，已由 dump_offline_tables 抽出）:
  quest_info.tsv / quest_say_text.tsv  （任务 Info/Say 正文）

用法：
  python scripts/dump_quest_tables.py
  python scripts/dump_quest_tables.py --also-bin
"""
from __future__ import annotations

import argparse
import json
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

# questId / stage / rest…
QPATH_RE = re.compile(r"^(\d+)(?:/(\d+))?(?:/(.+))?$")


def load_quest_wzjson(game_root: Path) -> dict[str, object]:
    w = game_root / "Maplestory_Classic_Data" / "StreamingAssets" / "aa" / "w"
    out: dict[str, object] = {}
    for bundle_path in w.glob("json_*.bundle"):
        env = UnityPy.load(str(bundle_path))
        ab_objs = [o for o in env.objects if o.type.name == "AssetBundle"]
        if not ab_objs:
            continue
        ab = ab_objs[0].read()
        container = ab.m_Container
        pairs = list(container.items()) if hasattr(container, "items") else list(container)
        for key, info in pairs:
            norm = key.replace("\\", "/")
            if "/Json/Quest/" not in norm:
                continue
            name = Path(norm).stem  # Act / Check / Say / …
            obj = next((o for o in env.objects if o.path_id == info.asset.m_PathID), None)
            if obj is None:
                continue
            try:
                raw = obj.get_raw_data()
                out[name] = parse_mb_wzjson(raw)
            except Exception as e:
                print(f"[skip] {norm}: {e}")
    return out


def fmt_val(v) -> str:
    if isinstance(v, float):
        return clean_cell(round(v, 6))
    return clean_cell(v)


def dump_stage_kv(doc, out_path: Path, source: str) -> tuple[int, int]:
    """Long table: quest_id, stage, path, value. stage 空表示挂在 quest 根上。"""
    lines = [f"# quest_id\tstage\tpath\tvalue\tsource=twms-wzjs-quest-{source}\n"]
    n = 0
    quest_ids: set[int] = set()
    for it in doc.items:
        path = (doc.paths[it.path_index] if 0 <= it.path_index < len(doc.paths) else "").replace(
            "\\", "/"
        )
        if not path or path in ("Quest/" + source, source):
            continue
        m = QPATH_RE.match(path)
        if not m:
            continue
        qid = int(m.group(1))
        stage = m.group(2) or ""
        rest = m.group(3) or ""
        if not rest:
            # 纯属性节点（如 1001/0/item）无标量值则跳过
            val = item_value(doc, it)
            if val is None:
                continue
            rest = doc.names[it.name_index] if 0 <= it.name_index < len(doc.names) else ""
        else:
            val = item_value(doc, it)
        if val is None:
            continue
        if it.type not in (TYPE_INT, TYPE_FLOAT, TYPE_STRING):
            continue
        quest_ids.add(qid)
        lines.append(f"{qid}\t{stage}\t{rest}\t{fmt_val(val)}\n")
        n += 1
    out_path.write_text("".join(lines), encoding="utf-8")
    return n, len(quest_ids)


def dump_exclusive(doc, out_path: Path) -> int:
    lines = ["# group_id\tquest_id\tsource=twms-wzjs-quest-Exclusive\n"]
    n = 0
    for it in doc.items:
        path = (doc.paths[it.path_index] if 0 <= it.path_index < len(doc.paths) else "").replace(
            "\\", "/"
        )
        # 0/10415
        m = re.match(r"^(\d+)/(\d+)$", path)
        if not m:
            continue
        lines.append(f"{m.group(1)}\t{m.group(2)}\n")
        n += 1
    out_path.write_text("".join(lines), encoding="utf-8")
    return n


def dump_pquest_kv(doc, out_path: Path, kind: str) -> int:
    lines = [f"# path\tkey\tvalue\tsource=twms-wzjs-quest-{kind}\n"]
    n = 0
    for it in doc.items:
        path = (doc.paths[it.path_index] if 0 <= it.path_index < len(doc.paths) else "").replace(
            "\\", "/"
        )
        name = doc.names[it.name_index] if 0 <= it.name_index < len(doc.names) else ""
        if not path or path in (kind, f"Quest/{kind}"):
            continue
        val = item_value(doc, it)
        if val is None or it.type not in (TYPE_INT, TYPE_FLOAT, TYPE_STRING):
            continue
        lines.append(f"{clean_cell(path)}\t{clean_cell(name)}\t{fmt_val(val)}\n")
        n += 1
    out_path.write_text("".join(lines), encoding="utf-8")
    return n


def flatten_obj(prefix: str, obj, out: list[tuple[str, str]]) -> None:
    if isinstance(obj, dict):
        for k, v in obj.items():
            flatten_obj(f"{prefix}.{k}" if prefix else str(k), v, out)
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            flatten_obj(f"{prefix}[{i}]", v, out)
    else:
        out.append((prefix, str(obj)))


def dump_questdata_string(quest_json: Path, out_info: Path, out_say: Path) -> tuple[int, int]:
    data = json.loads(quest_json.read_text(encoding="utf-8"))
    info_lines = ["# quest_id\tpath\ttext\tsource=twms-string-QuestData.Info\n"]
    say_lines = ["# quest_id\tpath\ttext\tsource=twms-string-QuestData.Say\n"]
    ni = ns = 0
    for code, row in sorted(data.items(), key=lambda kv: int(kv[0]) if str(kv[0]).isdigit() else 0):
        if not str(code).isdigit() or not isinstance(row, dict):
            continue
        qid = int(code)
        for field, out_lines, counter_name in (
            ("Info", info_lines, "info"),
            ("Say", say_lines, "say"),
        ):
            raw = row.get(field)
            if raw is None or raw == "":
                continue
            if isinstance(raw, str):
                try:
                    parsed = json.loads(raw.replace("'", '"')) if raw.startswith("{") else None
                except Exception:
                    parsed = None
                if parsed is None:
                    try:
                        import ast

                        parsed = ast.literal_eval(raw)
                    except Exception:
                        parsed = None
                obj = parsed if parsed is not None else {"_raw": raw}
            elif isinstance(raw, dict):
                obj = raw
            else:
                obj = {"_raw": raw}
            flat: list[tuple[str, str]] = []
            flatten_obj("", obj, flat)
            for path, text in flat:
                if not path:
                    continue
                line = f"{qid}\t{clean_cell(path)}\t{clean_cell(text)}\n"
                out_lines.append(line)
                if counter_name == "info":
                    ni += 1
                else:
                    ns += 1
    out_info.write_text("".join(info_lines), encoding="utf-8")
    out_say.write_text("".join(say_lines), encoding="utf-8")
    return ni, ns


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--game-root", type=Path, default=DEFAULT_GAME)
    ap.add_argument("--out", type=Path, default=REPO / "dumps" / "offline_tables")
    ap.add_argument("--also-bin", action="store_true")
    args = ap.parse_args()

    tsv_dir = args.out / "tsv"
    tsv_dir.mkdir(parents=True, exist_ok=True)

    docs = load_quest_wzjson(args.game_root)
    print("[quest wzjson]", sorted(docs.keys()))

    stats: dict[str, int] = {}

    if "Act" in docs:
        n, q = dump_stage_kv(docs["Act"], tsv_dir / "quest_act.tsv", "Act")
        stats["quest_act"] = n
        print(f"[Act] rows={n} quests≈{q}")
    if "Check" in docs:
        n, q = dump_stage_kv(docs["Check"], tsv_dir / "quest_check.tsv", "Check")
        stats["quest_check"] = n
        print(f"[Check] rows={n} quests≈{q}")
    if "Say" in docs:
        n, q = dump_stage_kv(docs["Say"], tsv_dir / "quest_say_keys.tsv", "Say")
        stats["quest_say_keys"] = n
        print(f"[Say keys] rows={n} quests≈{q}")
    if "QuestInfo" in docs:
        n, q = dump_stage_kv(docs["QuestInfo"], tsv_dir / "quest_wz_info.tsv", "QuestInfo")
        stats["quest_wz_info"] = n
        print(f"[QuestInfo] rows={n} quests≈{q}")
    if "Exclusive" in docs:
        n = dump_exclusive(docs["Exclusive"], tsv_dir / "quest_exclusive.tsv")
        stats["quest_exclusive"] = n
        print(f"[Exclusive] rows={n}")
    if "PQuest" in docs:
        n = dump_pquest_kv(docs["PQuest"], tsv_dir / "quest_pquest.tsv", "PQuest")
        stats["quest_pquest"] = n
        print(f"[PQuest] rows={n}")
    if "PQuestSearch" in docs:
        n = dump_pquest_kv(docs["PQuestSearch"], tsv_dir / "quest_pquest_search.tsv", "PQuestSearch")
        stats["quest_pquest_search"] = n
        print(f"[PQuestSearch] rows={n}")

    # String QuestData Info/Say 正文
    qjson = args.out / "json" / "QuestData.json"
    if not qjson.is_file():
        # 尝试从 String bundle 再抽一次最小集
        print("[warn] missing json/QuestData.json — run dump_offline_tables.py first if needed")
    else:
        ni, ns = dump_questdata_string(
            qjson, tsv_dir / "quest_info.tsv", tsv_dir / "quest_say_text.tsv"
        )
        stats["quest_info"] = ni
        stats["quest_say_text"] = ns
        print(f"[QuestData string] info={ni} say={ns}")

    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    print(f"[done] {now} {stats}")

    # spots
    def spot(path: Path, prefix: str, limit: int = 8) -> None:
        if not path.is_file():
            return
        hits = [
            ln
            for ln in path.read_text(encoding="utf-8").splitlines()
            if ln.startswith(prefix + "\t")
        ]
        print(f"[spot {path.name} {prefix}]", hits[:limit])

    spot(tsv_dir / "quest_act.tsv", "1001")
    spot(tsv_dir / "quest_check.tsv", "1001")
    spot(tsv_dir / "quest_say_text.tsv", "1000")

    if args.also_bin:
        ds = REPO / "bin" / "XCat_data" / "dataservice"
        ds.mkdir(parents=True, exist_ok=True)
        for name in (
            "quest_act.tsv",
            "quest_check.tsv",
            "quest_say_keys.tsv",
            "quest_wz_info.tsv",
            "quest_exclusive.tsv",
            "quest_pquest.tsv",
            "quest_pquest_search.tsv",
            "quest_info.tsv",
            "quest_say_text.tsv",
        ):
            src = tsv_dir / name
            if src.is_file():
                (ds / name).write_text(src.read_text(encoding="utf-8"), encoding="utf-8")
                print("[bin]", ds / name)


if __name__ == "__main__":
    main()
