#!/usr/bin/env python3
"""
Dump Classic TWMS action delay bases + skill→action map from Addressables WZJS.

  Character/00002000.wzjson  → tsv/action_delay_base.tsv
  Skill/*.wzjson action/0    → tsv/skill_action.tsv

离线存的是 **未乘 ActionSpeed** 的 baseDelay 加总（正帧之和）。
运行时：delay' = base * 100 / clamp(ActionSpeed,70..140)，再换算 ms。
攻速类 buff（改 GetActionSpeed / SS+0x80/+0x84/+0x1BC）必须运行时再乘才正确；
Booster（武器档 B 系统）不进 Prepare，不影响本表。

用法：
  python scripts/dump_action_delay_tables.py
  python scripts/dump_action_delay_tables.py --also-bin
"""
from __future__ import annotations

import argparse
import sys
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from dump_mob_skill_tables import iter_wzjson  # noqa: E402
from wzjs_io import item_value, parse_mb_wzjson  # noqa: E402

REPO = Path(__file__).resolve().parents[1]
DEFAULT_GAME = Path(r"G:\Games\maplestory_classic")


def dump_action_delays(game_root: Path) -> list[tuple[str, int, int, int]]:
    """Return rows: action_name, frames, sum_pos, sum_all."""
    rows: list[tuple[str, int, int, int]] = []
    for _b, key, raw in iter_wzjson(game_root, "/Json/Character/"):
        if not key.endswith("/00002000.wzjson"):
            continue
        doc = parse_mb_wzjson(raw)
        by: dict[str, list[int]] = defaultdict(list)
        for it in doc.items:
            path = (
                doc.paths[it.path_index] if 0 <= it.path_index < len(doc.paths) else ""
            ).replace("\\", "/")
            if not path.endswith("/delay"):
                continue
            v = item_value(doc, it)
            if isinstance(v, (int, float)):
                by[path.split("/")[0]].append(int(v))
        for act in sorted(by.keys()):
            vals = by[act]
            sum_all = sum(vals)
            sum_pos = sum(v for v in vals if v > 0)
            rows.append((act, len(vals), sum_pos, sum_all))
        break
    return rows


def dump_skill_actions(game_root: Path) -> list[tuple[int, str]]:
    """skill_id → first action/0 string (if present)."""
    out: dict[int, str] = {}
    for _b, key, raw in iter_wzjson(game_root, "/Json/Skill/"):
        if "Attacktype" in key or "QuestCount" in key:
            continue
        doc = parse_mb_wzjson(raw)
        for it in doc.items:
            path = (
                doc.paths[it.path_index] if 0 <= it.path_index < len(doc.paths) else ""
            ).replace("\\", "/")
            # skill/0001000/action/0 or skill/1001003/action/0
            parts = path.split("/")
            if len(parts) >= 4 and parts[0] == "skill" and parts[2] == "action":
                if not parts[1].isdigit():
                    continue
                sid = int(parts[1])
                # only first slot action/0 (or bare index 0)
                if parts[3] != "0":
                    continue
                val = item_value(doc, it)
                if isinstance(val, str) and val:
                    out.setdefault(sid, val)
    return sorted(out.items())


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--game-root", type=Path, default=DEFAULT_GAME)
    ap.add_argument("--out", type=Path, default=REPO / "dumps" / "offline_tables")
    ap.add_argument("--also-bin", action="store_true")
    args = ap.parse_args()

    tsv_dir = args.out / "tsv"
    tsv_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%MZ")

    actions = dump_action_delays(args.game_root)
    act_path = tsv_dir / "action_delay_base.tsv"
    lines = [
        f"# action\tframes\tdelay_sum_pos\tdelay_sum_all\tsource=twms-wzjs-character-00002000\tutc={stamp}\n",
        "# delay_sum_pos = sum(delay>0); Prepare busy ≈ sum_pos after ActionSpeed scale\n",
    ]
    for act, n, spos, sall in actions:
        lines.append(f"{act}\t{n}\t{spos}\t{sall}\n")
    act_path.write_text("".join(lines), encoding="utf-8")
    print(f"wrote {act_path} rows={len(actions)}")

    skills = dump_skill_actions(args.game_root)
    sk_path = tsv_dir / "skill_action.tsv"
    slines = [
        f"# skill_id\taction\tsource=twms-wzjs-skill-action0\tutc={stamp}\n",
    ]
    for sid, act in skills:
        slines.append(f"{sid}\t{act}\n")
    sk_path.write_text("".join(slines), encoding="utf-8")
    print(f"wrote {sk_path} rows={len(skills)}")

    if args.also_bin:
        bin_dir = REPO / "bin" / "XCat_data" / "dataservice"
        bin_dir.mkdir(parents=True, exist_ok=True)
        (bin_dir / "action_delay_base.tsv").write_text("".join(lines), encoding="utf-8")
        (bin_dir / "skill_action.tsv").write_text("".join(slines), encoding="utf-8")
        print(f"also-bin → {bin_dir}")


if __name__ == "__main__":
    main()
