#!/usr/bin/env python3
"""Bake Classic TWMS scroll-drop TTS fragments (zh-CN-XiaoxiaoNeural).

Compose: 掉落 + 部位 + 属性 + 卷轴类 + 成功率.
Irregular names (白衣 / 混沌 / 活动整词) stay as whole clips.
Speech text is Simplified Chinese (same voice as 枫星金价监控).
Also bakes pick_ok = 「拾取成功」、pick_ok_dart = 「雷之镖拾取成功」
(not in map.tsv; loaded by name).
"""
from __future__ import annotations

import argparse
import array
import asyncio
import hashlib
import os
import re
import shutil
import sys
import tempfile
import time
import wave
from collections import Counter
from pathlib import Path

import edge_tts
import miniaudio
import zhconv

REPO = Path(__file__).resolve().parents[1]
CATALOG = REPO / "bin" / "XCat_data" / "dataservice" / "item_catalog.tsv"
FALLBACK_CATALOG = REPO / "dumps" / "offline_tables" / "tsv" / "item_catalog.tsv"
OUT_DEFAULT = REPO / "dumps" / "offline_tables" / "scroll_voice"
BIN_OUT = REPO / "bin" / "XCat_data" / "dataservice" / "scroll_voice"

VOICE = "zh-CN-XiaoxiaoNeural"
# 播报用人声，16 kHz 足够；加载时再线性拉到 waveOut 的 44100。
TARGET_SR = 16000
# edge-tts 短词头尾各约 0.2s / 0.6s 静音；裁掉后拼接才像一句。
TRIM_THRESH = 400
TRIM_PAD_MS = 10

BRACKET_RE = re.compile(r"^(\[[^\]]+\]|【[^】]+】)\s*")
END_RE = re.compile(
    r"^(.*?)(詛咒卷軸|強化卷軸|增幅卷軸|專用特別的卷軸|專用卷軸|的卷軸|卷軸|卷轴)"
    r"\s*(\d+)?\s*%?\s*$"
)

# Longest-first. Catalog is Traditional Chinese.
SLOTS: list[tuple[str, str, str]] = [
    ("單手武器", "weap1h", "单手武器"),
    ("雙手武器", "weap2h", "双手武器"),
    ("武器", "weap", "武器"),
    ("全身盔甲", "overall", "套服"),
    ("巴洛古的鞋子", "balrog_shoes", "巴洛古的鞋子"),
    ("企鵝國王的武器", "penguin_weap", "企鹅国王的武器"),
    ("裝飾品", "acc", "饰品"),
    ("飾品", "acc", "饰品"),
    ("防具", "armor", "防具"),
    ("頭盔", "helm", "头盔"),
    ("上衣", "top", "上衣"),
    ("套服", "overall", "套服"),
    ("褲、裙", "bottom", "裤裙"),
    ("褲/裙", "bottom", "裤裙"),
    ("褲裙", "bottom", "裤裙"),
    ("褲子", "bottom", "裤子"),
    ("鞋子", "shoes", "鞋子"),
    ("手套", "glove", "手套"),
    ("披風", "cape", "披风"),
    ("盾牌", "shield", "盾牌"),
    ("耳環", "earring", "耳环"),
    ("戒子", "ring", "戒指"),
    ("腰帶", "belt", "腰带"),
    ("寵物", "pet", "宠物"),
    ("單手劍", "sword1h", "单手剑"),
    ("單手斧", "axe1h", "单手斧"),
    ("單手棍", "bw1h", "单手棍"),
    ("短劍", "dagger", "短剑"),
    ("短杖", "wand", "短杖"),
    ("長杖", "staff", "长杖"),
    ("雙手劍", "sword2h", "双手剑"),
    ("雙手斧", "axe2h", "双手斧"),
    ("雙手棍", "bw2h", "双手棍"),
    ("指虎", "knuckle", "指虎"),
    ("拳套", "claw", "拳套"),
    ("火槍", "gun", "火枪"),
    ("龍眼鏡", "eye", "龙眼镜"),
    ("槍", "spear", "枪"),
    ("矛", "polearm", "矛"),
    ("弓", "bow", "弓"),
    ("弩", "xbow", "弩"),
]

# Longest-first rest-after-slot.
STATS: list[tuple[str, str, str]] = [
    ("魔法攻擊力", "mad", "魔力"),
    ("攻擊力", "atk", "攻击"),
    ("移動速度", "speed", "速度"),
    ("敏捷性", "dex", "敏捷"),
    ("敏捷", "dex", "敏捷"),
    ("跳躍力", "jump", "跳跃"),
    ("命中率", "acc", "命中"),
    ("魔力", "mad", "魔力"),
    ("魔防", "mdd", "魔防"),
    ("防禦", "def", "防御"),
    ("力量", "str", "力量"),
    ("智力", "int", "智力"),
    ("幸運", "luk", "幸运"),
    ("生命", "hp", "生命"),
    ("跳躍", "jump", "跳跃"),
    ("速度", "speed", "速度"),
    ("命中", "acc", "命中"),
    ("攻擊", "atk", "攻击"),
    ("體力", "hp", "体力"),
    ("迴避", "avoid", "回避"),
    ("防滑", "nonskid", "防滑"),
    ("防寒", "warm", "防寒"),
]

# 非 204、整句口播（叮咚后直接念，不再拆零件）。目录是「雷之鏢」不是「標」。
SPECIAL_ANNOUNCE: list[tuple[int, str, str]] = [
    (2070005, "eq_thunder_dart", "掉落雷之镖"),
]

# 不进 map.tsv：卷軸/雷之鏢从池消失后的确认口播（与掉落叮咚成对）。
EXTRA_SPEECH: list[tuple[str, str]] = [
    ("pick_ok", "拾取成功"),
    ("pick_ok_dart", "雷之镖拾取成功"),
]

TAILS = {
    "詛咒卷軸": ("curse", "诅咒卷轴"),
    "強化卷軸": ("enhance", "强化卷轴"),
    "增幅卷軸": ("amplify", "增幅卷轴"),
    "專用特別的卷軸": ("special", "专用卷轴"),
    "專用卷軸": ("scroll", "卷轴"),
    "的卷軸": ("scroll", "卷轴"),
    "卷軸": ("scroll", "卷轴"),
    "卷轴": ("scroll", "卷轴"),
}

PCT_SPEECH = {
    1: "百分之一",
    3: "百分之三",
    5: "百分之五",
    10: "百分之十",
    15: "百分之十五",
    20: "百分之二十",
    30: "百分之三十",
    40: "百分之四十",
    50: "百分之五十",
    60: "百分之六十",
    65: "百分之六十五",
    70: "百分之七十",
    100: "百分之百",
}

LEAD_PREFIXES: list[tuple[str, str, str]] = [
    ("詛咒的", "pfx_curse", "诅咒的"),
    ("一級", "pfx_g1", "一级"),
    ("企鵝國王的", "pfx_penguin", "企鹅国王的"),
    ("楓之谷四週年慶", "pfx_4th", "枫之谷四周年庆"),
    ("受到咀咒的包包", "whole_cursed_bag", "受到诅咒的包包"),
    ("驚訝的混沌", "whole_chaos", "惊讶的混沌"),
    ("白衣", "whole_white", "白衣"),
    ("幸運智力", "stat_lukint", "幸运智力"),
]


def to_cn(s: str) -> str:
    return zhconv.convert(s, "zh-cn")


def load_204(path: Path) -> list[tuple[int, str]]:
    rows: list[tuple[int, str]] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        cols = line.split("\t")
        if len(cols) < 5 or not cols[0].isdigit():
            continue
        code = int(cols[0])
        if 2040000 <= code < 2050000:
            rows.append((code, cols[4].strip()))
    return rows


def pct_key(n: int) -> str:
    return f"pct_{n}"


def parse_name(name: str) -> tuple[list[str], dict[str, str]] | None:
    """Return (frag_ids after 'drop', extra speech map) or None to mark irregular."""
    extra: dict[str, str] = {}
    name = BRACKET_RE.sub("", name).strip()

    tm = END_RE.search(name)
    if not tm:
        return None
    body, tail, pct_s = tm.group(1), tm.group(2), tm.group(3)
    body = body.strip()
    pct = int(pct_s) if pct_s else None
    tail_id, tail_speech = TAILS[tail]
    extra[tail_id] = tail_speech

    frags: list[str] = []

    for tw, fid, speech in LEAD_PREFIXES:
        if body.startswith(tw):
            frags.append(fid)
            extra[fid] = speech
            body = body[len(tw) :].strip()
            break

    if body:
        hit_slot = None
        for tw, fid, speech in SLOTS:
            if body.startswith(tw):
                hit_slot = (fid, speech, tw)
                break
        if hit_slot:
            fid, speech, tw = hit_slot
            frags.append(f"slot_{fid}")
            extra[f"slot_{fid}"] = speech
            body = body[len(tw) :].strip()
        else:
            return None

    if body:
        hit_stat = None
        for tw, fid, speech in STATS:
            if body.startswith(tw):
                hit_stat = (fid, speech, tw)
                break
        if hit_stat:
            fid, speech, tw = hit_stat
            frags.append(f"stat_{fid}")
            extra[f"stat_{fid}"] = speech
            body = body[len(tw) :].strip()
            if body:
                return None
        else:
            return None

    frags.append(tail_id)
    if pct is not None:
        if pct not in PCT_SPEECH:
            extra[pct_key(pct)] = f"百分之{to_cn(str(pct))}"
        else:
            extra[pct_key(pct)] = PCT_SPEECH[pct]
        frags.append(pct_key(pct))
    return frags, extra


def whole_key(name: str) -> str:
    digest = hashlib.sha1(to_cn(name).encode("utf-8")).hexdigest()[:12]
    return "w_" + digest


def build_maps(rows: list[tuple[int, str]]) -> tuple[dict[int, list[str]], dict[str, str], dict[str, int]]:
    item_frags: dict[int, list[str]] = {}
    speech: dict[str, str] = {"drop": "掉落"}
    stats = Counter()

    for code, name in rows:
        parsed = parse_name(name)
        if parsed is None:
            key = whole_key(name)
            speech[key] = to_cn(name)
            item_frags[code] = ["drop", key]
            stats["whole"] += 1
            continue
        rest, extra = parsed
        speech.update(extra)
        item_frags[code] = ["drop", *rest]
        stats["compose"] += 1

    for code, fid, text in SPECIAL_ANNOUNCE:
        speech[fid] = text
        item_frags[code] = [fid]
        stats["special"] += 1

    for fid, text in EXTRA_SPEECH:
        speech[fid] = text
        stats["extra"] = stats.get("extra", 0) + 1

    stats["items"] = len(item_frags)
    stats["frags"] = len(speech)
    return item_frags, speech, dict(stats)


def write_tables(out: Path, item_frags: dict[int, list[str]], speech: dict[str, str]) -> None:
    out.mkdir(parents=True, exist_ok=True)
    frag_lines = ["# id\tspeech_zh_cn\n"]
    for fid in sorted(speech):
        frag_lines.append(f"{fid}\t{speech[fid]}\n")
    (out / "fragments.tsv").write_text("".join(frag_lines), encoding="utf-8")

    map_lines = ["# item_id\tfrags\n"]
    for code in sorted(item_frags):
        map_lines.append(f"{code}\t{','.join(item_frags[code])}\n")
    (out / "map.tsv").write_text("".join(map_lines), encoding="utf-8")


def trim_pcm_i16(samples: array.array, sr: int) -> array.array:
    pad = sr * TRIM_PAD_MS // 1000
    n = len(samples)
    i = 0
    while i < n and abs(samples[i]) < TRIM_THRESH:
        i += 1
    j = n
    while j > i and abs(samples[j - 1]) < TRIM_THRESH:
        j -= 1
    if i >= j:
        return samples
    i = max(0, i - pad)
    j = min(n, j + pad)
    return samples[i:j]


def write_wav_i16(path: Path, samples: array.array, sr: int) -> None:
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(samples.tobytes())


def trim_wav_file(path: Path) -> bool:
    with wave.open(str(path), "rb") as w:
        sr = w.getframerate()
        ch = w.getnchannels()
        sw = w.getsampwidth()
        raw = w.readframes(w.getnframes())
    if ch != 1 or sw != 2:
        return False
    samples = array.array("h")
    samples.frombytes(raw)
    trimmed = trim_pcm_i16(samples, sr)
    if len(trimmed) >= len(samples):
        return False
    write_wav_i16(path, trimmed, sr)
    return True


async def synth_one(text: str, wav: Path) -> None:
    last_err: Exception | None = None
    for attempt in range(4):
        fd_mp3, mp3_name = tempfile.mkstemp(suffix=".mp3")
        fd_wav, wav_name = tempfile.mkstemp(suffix=".wav")
        os.close(fd_mp3)
        os.close(fd_wav)
        ascii_mp3 = Path(mp3_name)
        ascii_wav = Path(wav_name)
        try:
            await edge_tts.Communicate(text, VOICE).save(str(ascii_mp3))
            dec = miniaudio.decode_file(
                str(ascii_mp3),
                output_format=miniaudio.SampleFormat.SIGNED16,
                nchannels=1,
                sample_rate=TARGET_SR,
            )
            miniaudio.wav_write_file(str(ascii_wav), dec)
            trim_wav_file(ascii_wav)
            shutil.copyfile(ascii_wav, wav)
            return
        except Exception as exc:  # noqa: BLE001
            last_err = exc
            await asyncio.sleep(0.8 * (attempt + 1))
        finally:
            ascii_mp3.unlink(missing_ok=True)
            ascii_wav.unlink(missing_ok=True)
    raise RuntimeError(f"synth failed: {text!r} ({last_err})")


async def bake(speech: dict[str, str], out: Path, jobs: int) -> None:
    sem = asyncio.Semaphore(max(1, jobs))
    todo = [(fid, text) for fid, text in sorted(speech.items())]
    done = 0
    t0 = time.perf_counter()

    async def run(fid: str, text: str) -> None:
        nonlocal done
        wav = out / f"{fid}.wav"
        if wav.exists() and wav.stat().st_size > 44:
            async with sem:
                done += 1
            return
        async with sem:
            await synth_one(text, wav)
            done += 1
            print(f"  [{done}/{len(todo)}] {fid}  {text}", flush=True)

    results = await asyncio.gather(*(run(fid, text) for fid, text in todo), return_exceptions=True)
    errs = [r for r in results if isinstance(r, Exception)]
    for e in errs[:8]:
        print(f"ERROR {e}", flush=True)
    print(f"bake done {len(todo) - len(errs)}/{len(todo)} clips in {time.perf_counter() - t0:.1f}s")
    if errs:
        raise RuntimeError(f"{len(errs)} fragment(s) failed")


def copy_runtime(src: Path, dst: Path) -> None:
    dst.mkdir(parents=True, exist_ok=True)
    for p in src.iterdir():
        if p.suffix.lower() in {".wav", ".tsv"}:
            target = dst / p.name
            target.write_bytes(p.read_bytes())


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, default=OUT_DEFAULT)
    ap.add_argument("--jobs", type=int, default=10)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true", help="re-synth even if wav exists")
    args = ap.parse_args()

    catalog = CATALOG if CATALOG.is_file() else FALLBACK_CATALOG
    if not catalog.is_file():
        print(f"missing catalog: {catalog}", file=sys.stderr)
        return 2

    rows = load_204(catalog)
    item_frags, speech, stats = build_maps(rows)
    args.out.mkdir(parents=True, exist_ok=True)
    write_tables(args.out, item_frags, speech)

    print(
        f"204+special items={stats['items']} compose={stats.get('compose', 0)} "
        f"whole={stats.get('whole', 0)} special={stats.get('special', 0)} "
        f"extra={stats.get('extra', 0)} fragments={stats['frags']}"
    )
    if args.dry_run:
        for fid, text in sorted(speech.items()):
            print(f"  {fid}\t{text}")
        return 0

    if args.force:
        for p in args.out.glob("*.wav"):
            p.unlink()
        for p in args.out.glob("*.mp3"):
            p.unlink()
        for p in args.out.glob("*.part.mp3"):
            p.unlink()

    asyncio.run(bake(speech, args.out, args.jobs))
    if BIN_OUT != args.out:
        copy_runtime(args.out, BIN_OUT)
        print(f"copied to {BIN_OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
