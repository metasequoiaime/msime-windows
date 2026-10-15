#!/usr/bin/env python3
"""Build the U 模式部件拆字反查表 from the upstream Rime dictionary.

输入是 mirtlecn/rime-radical-pinyin 发布的 `radical_pinyin.dict.yaml`，数据行格式为
`<汉字>\t<部件拼音>(撇号分隔)\t<字频>`（例如 `晶\tri'ri'ri\t106`）。我们把它压成
引擎运行时用的三列表 `<部件拼音连写>\t<汉字>\t<字频>`，整个文件按字频降序排列，
这样查询端做前缀匹配时天然先取高频字。

上游表随发布版本变化，重建时必须记录版本；`--version` 只是做一个显式校验，避免
拿旧表静默覆盖。上游来源与授权见 ../NOTICE.md。

用法：
    python build_decomposition.py --source radical_pinyin.dict.yaml \
        --output ../decomposition.txt [--version 2026.06.18]
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--version", default="")
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

    # 字频降序；同频按码、字稳定排序，保证可复现。
    entries.sort(key=lambda item: (-item[2], item[1], item[0]))

    header = [
        "# U 模式部件拆字反查表",
        "# 格式：<部件拼音连写>\\t<汉字>\\t<字频>（整表按字频降序）",
        f"# 来源：mirtlecn/rime-radical-pinyin {version}，授权见 NOTICE.md",
        "# 由 scripts/build_decomposition.py 生成，请勿手改。",
    ]
    lines = header + [f"{code}\t{char}\t{rank}" for char, code, rank in entries]
    args.output.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {len(entries)} entries to {args.output} (upstream {version})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
