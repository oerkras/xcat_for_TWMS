"""Shared WZJS (Unity WzJson MonoBehaviour) reader for Classic TWMS dumps."""
from __future__ import annotations

import struct
from dataclasses import dataclass

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
TYPE_FLOAT = 8  # WzJsonType.Float
TYPE_STRING = 11  # WzJsonType.String in CMS/TWMS
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


@dataclass
class WzJsonDoc:
    asset_name: str
    hdr: dict
    data: bytes
    names: list[str]
    paths: list[str]
    strings: list[str]
    ints: list[int]
    floats: list[float]
    bools: list[bool]
    items: list[WzItem]


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


def parse_mb_wzjson(raw: bytes) -> WzJsonDoc:
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
    data = raw[data0 : data0 + data_len]
    items: list[WzItem] = []
    base = hdr["itemOffset"]
    for i in range(max(0, hdr["itemCount"])):
        vals = struct.unpack_from("<iiiiiiii", data, base + i * ITEM_SIZE)
        items.append(WzItem(i, *vals))
    return WzJsonDoc(
        asset_name=name,
        hdr=hdr,
        data=data,
        names=read_strings(data, hdr["nameCount"], hdr["nameOffset"], hdr["nameOffsetOffset"]),
        paths=read_strings(data, hdr["pathCount"], hdr["pathOffset"], hdr["pathOffsetOffset"]),
        strings=read_strings(data, hdr["stringCount"], hdr["stringOffset"], hdr["stringOffsetOffset"]),
        ints=[
            struct.unpack_from("<i", data, hdr["intOffset"] + i * 4)[0]
            for i in range(max(0, hdr["intCount"]))
        ],
        floats=[
            struct.unpack_from("<f", data, hdr["floatOffset"] + i * 4)[0]
            for i in range(max(0, hdr["floatCount"]))
        ],
        bools=[bool(data[hdr["boolOffset"] + i]) for i in range(max(0, hdr["boolCount"]))],
        items=items,
    )


def item_value(doc: WzJsonDoc, it: WzItem):
    if it.type == TYPE_INT and 0 <= it.data_index < len(doc.ints):
        return doc.ints[it.data_index]
    if it.type == TYPE_FLOAT and 0 <= it.data_index < len(doc.floats):
        return doc.floats[it.data_index]
    if it.type == TYPE_STRING and 0 <= it.data_index < len(doc.strings):
        return doc.strings[it.data_index]
    if it.type == TYPE_BOOL and 0 <= it.data_index < len(doc.bools):
        return 1 if doc.bools[it.data_index] else 0
    return None


def clean_cell(s: str) -> str:
    return str(s).replace("\t", " ").replace("\r", " ").replace("\n", "\\n")
