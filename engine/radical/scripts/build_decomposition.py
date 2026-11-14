#!/usr/bin/env python3
"""Build the U 模式部件拆字反查表 from the upstream Rime dictionary.

输入是 mirtlecn/rime-radical-pinyin 发布的 `radical_pinyin.dict.yaml`，数据行格式为
`<汉字>\t<部件拼音>(撇号分隔)\t<字频>`（例如 `晶\tri'ri'ri\t106`）。我们把它压成
引擎运行时用的四列表 `<部件拼音连写>\t<汉字>\t<字频>\t<读音>`，整个文件按字频降序排列，
这样查询端做前缀匹配时天然先取高频字。

读音来自 mozillazg/pinyin-data（`--readings-dir` 指向该仓库根目录）：

- 首选读音取 `pinyin.txt` 的第一项（上游合并出的常用读音）。
- 第二读音取现代规范字典里的另一读法：`kHanyuPinlu.txt`（现代汉语频率词典）、
  `kXHC1983.txt`（现代汉语词典）、`kTGHZ2013.txt`（通用规范汉字字典）。常用字要求这个
  读音**至少被两本字典共同收录**，票数高的排前面，票数相同时按上面的顺序取——只出现在
  一本里的多是又音、古读或方言（市 fú、广 ān、鸟 diǎo），标出来反而误导。这些字表之外的
  生僻字才退回 `pinyin.txt` 的第二个读音。轻声音节（无调号）不算独立读音，第二读音跳过。
- 最多两个：候选窗只排得下两种读法，更多读法几乎都是异体或极生僻读音。

多音字在候选上标出两种读法；只取一个又会让「长 zhǎng/cháng」这类字丢掉常用读法。
拆字表里的字比 cpp-pinyin 的词典多得多，读音烘进表里，候选才能反显。

两个上游源都随发布版本变化，重建时必须记录版本；`--version` / `--readings-version`
只是做一个显式校验，避免拿旧表静默覆盖。上游来源与授权见 ../NOTICE.md。

用法：
    python build_decomposition.py --source radical_pinyin.dict.yaml \
        --readings-dir <pinyin-data 仓库根目录> --output ../decomposition.txt \
        [--version 2026.06.18] [--readings-version 0.16.0]
"""

from __future__ import annotations

import argparse
import re
import sys
import unicodedata
from pathlib import Path

READING_LINE = re.compile(r"^U\+([0-9A-Fa-f]+):\s*([^#]*?)\s*(?:#.*)?$")

# 读音来源文件，顺序即第二读音的优先级。所有文件都由 pinyin-data 同一版本发布。
READINGS_PRIMARY = "pinyin.txt"
READINGS_STANDARD = ("kHanyuPinlu.txt", "kXHC1983.txt", "kTGHZ2013.txt")

# 候选窗最多反显两个读音：再多的读法要么极生僻，要么只是异体/通假，标出来反而误导。
MAX_READINGS = 2

# 常用字的第二读音至少要被两本规范字典收录。只出现在一本里的多是又音、古读或方言
# （市 fú、广 ān、鸟 diǎo），标出来对「这个字怎么读」没有帮助。
MIN_STANDARD_VOTES = 2

# 声调符号（组合附加符）：用来区分真正的读音和轻声。ü 的两点不算声调。
TONE_MARKS = {"\u0300", "\u0301", "\u0304", "\u030c"}


def is_pinyin(reading: str) -> bool:
    """读音只允许拼音字母和调号（jīng、de、ê̄、m̀）。

    调号是组合附加符，先拆掉再校验基本字母，避免逐个手写带调字符表。
    """
    if not reading:
        return False
    stripped = unicodedata.normalize("NFD", reading)
    base = "".join(char for char in stripped if not unicodedata.combining(char))
    return bool(base) and base.isascii() and base.isalpha() and base.islower()


def is_han_character(char: str) -> bool:
    """CJK 统一汉字（基本区、扩展 A 及第二、三平面的扩展区）、兼容汉字和〇。

    上游表在 `abc` 码下收了一批数字、拉丁/希腊字母和数学符号，它们没有部件可拆，不该出现在
    拆字候选里。第二、三平面整体按汉字放行，新的扩展区不用逐个登记。
    """
    code = ord(char)
    return (
        code == 0x3007
        or 0x3400 <= code <= 0x4DBF
        or 0x4E00 <= code <= 0x9FFF
        or 0xF900 <= code <= 0xFAFF
        or 0x20000 <= code <= 0x3FFFF
    )


def parse_source(path: Path) -> tuple[str, list[tuple[str, str, int]]]:
    """Return (upstream version, entries). Entries are (char, code_without_apostrophe, weight)."""
    version = ""
    entries: list[tuple[str, str, int]] = []
    seen: set[tuple[str, str]] = set()
    in_body = False
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.rstrip("\n")
        if not in_body:
            # YAML 头部以 `...` 结束，其后才是数据行。
            if line.strip() == "...":
                in_body = True
            elif line.startswith("version:"):
                version = line.split(":", 1)[1].strip().strip('"')
            continue
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        # 数据列数不固定（部分行有多列变体），前三列固定是 汉字 / 码 / 字频。
        if len(parts) < 3:
            continue
        char, code, weight = parts[0], parts[1], parts[2]
        if len(char) != 1 or not is_han_character(char):
            continue
        try:
            rank = int(weight)
        except ValueError:
            continue
        normalized = code.replace("'", "").replace(" ", "")
        if (
            not normalized.isascii()
            or not normalized.isalpha()
            or not normalized.islower()
        ):
            # 只保留纯小写拉丁字母的码：拆字输入只可能是 a-z。
            continue
        key = (normalized, char)
        if key in seen:
            continue
        seen.add(key)
        entries.append((char, normalized, rank))
    return version, entries


def parse_reading_file(path: Path) -> dict[str, list[str]]:
    """Return char -> readings in the file's order.

    行格式是 `U+4E00: yī,yí  # 一`（有的文件用空格分隔读音）。标不出拼音的项丢弃，
    标点、数字、注释不会混进读音列。
    """
    readings: dict[str, list[str]] = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        match = READING_LINE.match(line)
        if not match:
            continue
        values = [
            item.strip() for item in re.split(r"[,\s]+", match.group(2)) if item.strip()
        ]
        valid = [item for item in values if is_pinyin(item)]
        if valid:
            readings[chr(int(match.group(1), 16))] = valid
    return readings


def is_toned(reading: str) -> bool:
    """带声调符号的读音。轻声（de、le）在单字候选上不是独立读音。"""
    return any(char in TONE_MARKS for char in unicodedata.normalize("NFD", reading))


def collected_readings(
    tables: list[dict[str, list[str]]], char: str
) -> list[tuple[str, int]]:
    """把各本规范字典给出的读音合并成 [(读音, 票数)]。

    被多本字典同时收录的读音更可能是现代常用读音，排前面；票数相同时按字典顺序
    （频率词典 → 现代汉语词典 → 通用规范字典）保留先出现的那个。
    """
    votes: dict[str, int] = {}
    order: list[str] = []
    for table in tables:
        for reading in table.get(char, []):
            if reading not in votes:
                votes[reading] = 0
                order.append(reading)
            votes[reading] += 1
    ordered = sorted(order, key=lambda reading: (-votes[reading], order.index(reading)))
    return [(reading, votes[reading]) for reading in ordered]


def pick_second_reading(values: list[tuple[str, int]], first: str) -> str | None:
    """选一个不同于首选的读音：跳过轻声音节，票数不足的候选由调用方过滤。"""
    for value, _ in values:
        if value == first:
            continue
        if not is_toned(value):
            continue
        return value
    return None


def load_readings(directory: Path) -> tuple[str, dict[str, dict[str, list[str]]]]:
    """Return (pinyin-data version, file name -> char -> readings)."""
    version = ""
    primary_path = directory / READINGS_PRIMARY
    if not primary_path.is_file():
        raise FileNotFoundError(f"readings file not found: {primary_path}")
    for raw in primary_path.read_text(encoding="utf-8").splitlines():
        if raw.startswith("# version:"):
            version = raw.split(":", 1)[1].strip()
            break

    tables: dict[str, dict[str, list[str]]] = {}
    for name in (READINGS_PRIMARY, *READINGS_STANDARD):
        path = directory / name
        if not path.is_file():
            raise FileNotFoundError(f"readings file not found: {path}")
        tables[name] = parse_reading_file(path)
    return version, tables


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--readings-dir", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--version", default="")
    parser.add_argument("--readings-version", default="")
    args = parser.parse_args()

    if not args.source.is_file():
        print(f"source not found: {args.source}", file=sys.stderr)
        return 1

    version, entries = parse_source(args.source)
    if not version:
        print("could not read upstream version from source header", file=sys.stderr)
        return 1
    if args.version and version != args.version:
        print(
            f"upstream version mismatch: expected {args.version}, got {version}",
            file=sys.stderr,
        )
        return 1

    try:
        readings_version, readings = load_readings(args.readings_dir)
    except FileNotFoundError as error:
        print(error, file=sys.stderr)
        return 1
    if not readings_version:
        print("could not read data version from readings header", file=sys.stderr)
        return 1
    if args.readings_version and readings_version != args.readings_version:
        print(
            f"readings version mismatch: expected {args.readings_version}, got {readings_version}",
            file=sys.stderr,
        )
        return 1

    primary = readings[READINGS_PRIMARY]
    standard = [readings[name] for name in READINGS_STANDARD]

    def readings_for(char: str) -> list[str]:
        values = primary.get(char)
        if not values:
            return []
        first = values[0]
        # 常用字只在现代规范字典里找第二读音，且要求至少两本字典共同收录；
        # 规范字典没收录的生僻字才退回 pinyin-data 的第二项（那里没有票数可依）。
        if any(char in table for table in standard):
            candidates = [
                (reading, votes)
                for reading, votes in collected_readings(standard, char)
                if votes >= MIN_STANDARD_VOTES
            ]
        else:
            candidates = [(reading, 1) for reading in values[1:]]
        second = pick_second_reading(candidates, first)
        return [first, second] if second else [first]

    # 字频降序；同频按码、字稳定排序，保证可复现。
    entries.sort(key=lambda item: (-item[2], item[1], item[0]))

    def reading_column(char: str) -> str:
        return ",".join(readings_for(char)[:MAX_READINGS])

    entries_with_reading = sum(1 for char, _, _ in entries if char in primary)
    header = [
        "# U 模式部件拆字反查表",
        "# 格式：<部件拼音连写>\\t<汉字>\\t<字频>\\t<读音>（整表按字频降序；读音为空或逗号分隔的多个）",
        f"# 来源：mirtlecn/rime-radical-pinyin {version}，授权见 NOTICE.md",
        f"# 读音：mozillazg/pinyin-data {readings_version}，最多 {MAX_READINGS} 个读音（首选 + 规范字典次读音），授权见 NOTICE.md",
        "# 由 scripts/build_decomposition.py 生成，请勿手改。",
    ]
    lines = header + [
        f"{code}\t{char}\t{rank}\t{reading_column(char)}"
        for char, code, rank in entries
    ]
    args.output.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(
        f"wrote {len(entries)} entries to {args.output} "
        f"(upstream {version}, readings {readings_version}, {entries_with_reading} entries with reading)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
