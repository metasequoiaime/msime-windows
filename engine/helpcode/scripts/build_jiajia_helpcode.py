#!/usr/bin/env python3
"""按拼音加加的取码规则补齐 jiajia 辅助码表。

取码规则见 ../README.md：按笔顺拆出前两个部件，各取读音首字母；部件按加加的俗称读音取音
（亻=d、扌=t、纟=j、忄=s、攵=f、夂=z、刂=l、彳=s），独体字与部首字取「本字声母 + 起笔」。
本脚本只负责**补**表里没有的字，已有的行原样保留，所以反复运行结果稳定，也不会改动
fzm.bin 对齐过的那批常用字。

拆字和读音都来自 mirtlecn/rime-radical-pinyin 的上游文件：

- `radical.yaml`：`<汉字>\\t<部件1> <部件2> ...`，一个字的多种拆法各占一行。
- `radical_pinyin.dict.yaml`：`<汉字>\\t<部件读音1>'<部件读音2> ...\\t<字频>`。

上游的偏旁有时写成规范部件（人、手、丝、玉），有时写成偏旁形式（亻、扌、⺰、王），
还可能是部首区字符（⺘、⺰）；俗称读音和「位于第几位」在这里补：见 OVERRIDES 与
component_letter。这条规则对已有 7968 字能复现约八成——剩下的两成是加加自己的拆法与
上游不同，或者命中了独体/部首字的例外规则。

目标字符集是 GBK 全部汉字 ∪ 表里已有的字 ∪ 同目录其他辅助码表里的字 ∪ 可选词库里出现的
汉字。GBK 之外、上游没有拆字数据的字会跳过并在报告里列出。

需要 pypinyin（生成时按本字读音在多种拆法里选一种）：

    python -m pip install pypinyin==0.55.0

用法：
    python build_jiajia_helpcode.py \\
        --radicals radical.yaml \\
        --pinyin radical_pinyin.dict.yaml \\
        --base ../helpcodes/jiajia_helpcode.txt \\
        --dictionary ../../../MetasequoiaImeDict/out/msime.db \\
        --version 2026.06.18
"""

from __future__ import annotations

import argparse
import collections
import sqlite3
import sys
import unicodedata
from pathlib import Path

try:
    import pypinyin
    from pypinyin import Style, pinyin
except ImportError:
    print(
        "需要 pypinyin 才能按本字读音挑选拆法：python -m pip install pypinyin==0.55.0",
        file=sys.stderr,
    )
    raise SystemExit(1)

PYPINYIN_VERSION = "0.55.0"

# 加加对成字偏旁的俗称读音。上游 radical.yaml 里同一个偏旁可能写成规范部件，也可能写成
# 偏旁形式（人/亻、手/扌），两种都要登记。
OVERRIDES: dict[str, str] = {
    "人": "d",  # 单人旁
    "亻": "d",
    "手": "t",  # 提手旁
    "扌": "t",
    "丝": "j",  # 绞丝旁
    "糸": "j",
    "糹": "j",
    # 上游还常把绞丝旁写成简体的 纟；pypinyin 给它的是 si，不覆盖就会取成 s。
    "纟": "j",
    "玉": "w",  # 斜玉旁
    "王": "w",
    "水": "s",  # 三点水
    "氵": "s",
    "攴": "f",  # 反文旁
    "攵": "f",
    "夂": "z",  # 折文儿
    "彳": "s",  # 双人旁
    "疒": "b",  # 病字旁
    "阜": "e",  # 左耳旁
    "邑": "e",  # 右耳旁
    "阝": "e",
    "囗": "k",  # 国字框
    "匚": "s",  # 三框
    "网": "s",  # 四字头
    "罒": "s",
    "冖": "t",  # 秃宝盖
    "冂": "t",  # 同字框
    "冫": "l",  # 两点水
    "忄": "s",  # 竖心旁
    "刂": "l",  # 立刀旁
    "灬": "s",  # 四点底
    "⺰": "j",  # 纟的部首形式
    "⺯": "j",
    "⺘": "t",  # 扌的部首形式
    "⺱": "s",  # 罒的部首形式
    "⺲": "s",
    "⺳": "s",
    "⺀": "l",  # 冫的部首形式
    "宀": "b",  # 宝盖头（旧表含 宀 的 61 个字里 60 个取 b，pypinyin 读 mi）
    "丬": "j",  # 将字旁
    "襾": "x",  # 西字头
    "巛": "s",  # 三拐
    "亼": "r",  # 集字头
    "咅": "l",  # 旧表按 立+口 取 l
    "讠": "y",  # 言字旁，读音本来就是 yan
    "訁": "y",
    "钅": "j",  # 金字旁，读音本来就是 jin
    "釒": "j",
    "饣": "s",  # 食字旁，读音本来就是 shi
    "飠": "s",
    "礻": "s",  # 示字旁，读音本来就是 shi
    "衤": "y",  # 衣字旁，读音本来就是 yi
}

HAN_RANGES = (
    (0x3007, 0x3007),
    (0x3400, 0x4DBF),
    (0x4E00, 0x9FFF),
    (0xF900, 0xFAFF),
    (0x20000, 0x2FA1F),
    (0x30000, 0x323AF),
)


def is_han(char: str) -> bool:
    code = ord(char)
    return any(start <= code <= end for start, end in HAN_RANGES)


def load_base(path: Path) -> dict[str, str]:
    table: dict[str, str] = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        char, code = line.split("=", 1)
        table[char] = code
    return table


def load_radicals(path: Path) -> dict[str, list[list[str]]]:
    table: dict[str, list[list[str]]] = collections.defaultdict(list)
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.rstrip("\n")
        if not line or line.startswith("#") or "\t" not in line:
            continue
        char, decomposition = line.split("\t", 1)
        parts = decomposition.split()
        if len(char) == 1 and parts:
            table[char].append(parts)
    return table


def load_pinyin(path: Path) -> tuple[str, dict[str, list[tuple[list[str], int]]]]:
    version = ""
    table: dict[str, list[tuple[list[str], int]]] = collections.defaultdict(list)
    in_body = False
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.rstrip("\n")
        if not in_body:
            if line.strip() == "...":
                in_body = True
            elif line.startswith("version:"):
                version = line.split(":", 1)[1].strip().strip('"')
            continue
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) < 3:
            continue
        char, code, weight = parts[0], parts[1], parts[2]
        if len(char) != 1 or not is_han(char):
            continue
        syllables = [syllable for syllable in code.split("'") if syllable]
        try:
            rank = int(weight)
        except ValueError:
            continue
        if syllables:
            table[char].append((syllables, rank))
    return version, table


def gbk_han_chars() -> set[str]:
    """GBK 能编码的全部汉字（约 20900 个，即通常说的「大字符集」）。"""
    chars: set[str] = set()
    for lead in range(0x81, 0xFF):
        for trail in range(0x40, 0xFF):
            if trail == 0x7F:
                continue
            try:
                char = bytes((lead, trail)).decode("gbk")
            except UnicodeDecodeError:
                continue
            if len(char) == 1 and is_han(char):
                chars.add(char)
    return chars


def dictionary_chars(path: Path) -> set[str]:
    connection = sqlite3.connect(path)
    try:
        chars: set[str] = set()
        for (table_name,) in connection.execute(
            "SELECT name FROM sqlite_master WHERE type='table'"
        ):
            for (word,) in connection.execute(f'SELECT "value" FROM "{table_name}"'):
                chars.update(char for char in word if is_han(char))
        return chars
    finally:
        connection.close()


def reference_chars(directory: Path, excluded: set[Path]) -> set[str]:
    chars: set[str] = set()
    for path in sorted(directory.glob("*.txt")):
        if path.resolve() in excluded:
            continue
        chars.update(load_base(path))
    return {char for char in chars if is_han(char)}


def component_letter(component: str, index: int, count: int, default: str) -> str:
    if component in OVERRIDES:
        return OVERRIDES[component]
    # 旧表把「尚」按位置分两种：在首位按 小/尚字头 取 x（堂=xt、掌=xt），在次位仍读 shang 取 s
    # （躺=ss、绱=js）；上游把两者都写成同一个 尚，只能按位置区分。
    if component == "尚":
        return "x" if index == 0 else "s"
    # 上游把偏旁统一成成字部件，丢了「忄/心」「刂/刀」「灬/火」的区别，只能按位置分：
    # 忄在左、灬在下、刂在右。
    if component == "心":
        return "s" if index == 0 and count > 1 else "x"
    if component in ("刀", "⺉"):
        return "l" if index == 1 and count > 1 else "d"
    if component in ("火", "⺣"):
        return "s" if index == count - 1 and count > 1 else "h"
    return default


class Generator:
    def __init__(
        self,
        radicals: dict[str, list[list[str]]],
        pinyin: dict[str, list[tuple[list[str], int]]],
    ):
        self.radicals = radicals
        self.pinyin = pinyin
        self.component_readings = self._component_readings()

    def _component_readings(self) -> dict[str, str]:
        readings: dict[str, str] = {}
        for variants in self.radicals.values():
            for components in variants:
                for component in components:
                    if component in readings:
                        continue
                    # 上游码表给部分部件（見、王、彖……）的不是本字读音而是拆法，不能拿来
                    # 跟整字的音节对齐，所以优先用 pypinyin 的本字读音。
                    candidates = pinyin(
                        component,
                        style=Style.NORMAL,
                        heteronym=True,
                        errors="ignore",
                    )
                    if candidates and candidates[0]:
                        readings[component] = candidates[0][0][0]
                        continue
                    if component in self.pinyin:
                        counts = collections.Counter(
                            syllables[0][0]
                            for syllables, _ in self.pinyin[component]
                            if len(syllables) == 1
                        )
                        if counts:
                            readings[component] = counts.most_common(1)[0][0]
        return readings

    def _score(
        self, char: str, syllables: list[str], weight: int
    ) -> tuple[bool, int, bool, int, int]:
        aligned = [
            components
            for components in self.radicals.get(char, [])
            if len(components) == len(syllables)
        ]
        agreement = 0
        if aligned:
            for index, syllable in enumerate(syllables):
                component = aligned[0][index]
                default = self.component_readings.get(component, syllable[0])
                agreement += syllable[0] == component_letter(
                    component, index, len(syllables), default
                )
        # 加加取的是「前两个部件」；同样能对上读音时，部件少的拆法更接近这个粗粒度。
        return (
            len(syllables) == 2,
            -len(syllables),
            bool(aligned),
            agreement,
            weight,
        )

    def code_for(self, char: str) -> str | None:
        variants = [
            variant for variant in self.pinyin.get(char, []) if len(variant[0]) >= 2
        ]
        if not variants:
            return None
        syllables, _ = max(variants, key=lambda variant: self._score(char, *variant))
        components = next(
            (
                parts
                for parts in self.radicals.get(char, [])
                if len(parts) == len(syllables)
            ),
            None,
        )
        letters = []
        for index in range(2):
            default = syllables[index][0]
            letters.append(
                component_letter(components[index], index, len(syllables), default)
                if components
                else default
            )
        return "".join(letters)


def generate(
    char: str, generator: Generator, seen: set[str], known: dict[str, str]
) -> str | None:
    if char in seen:
        return None
    seen.add(char)
    # 兼容汉字（U+F900–FAFF）是同一批字的另一种编码，优先沿用正字的既有码。
    canonical = unicodedata.normalize("NFKC", char)
    if len(canonical) == 1 and canonical != char and canonical in known:
        return known[canonical]
    code = generator.code_for(char)
    if code:
        return code
    if len(canonical) == 1 and canonical != char:
        return generate(canonical, generator, seen, known)
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--radicals", required=True, type=Path)
    parser.add_argument("--pinyin", required=True, type=Path)
    parser.add_argument(
        "--base", required=True, type=Path, help="现有码表，已有条目原样保留"
    )
    parser.add_argument("--output", type=Path, help="默认写回 --base")
    parser.add_argument(
        "--reference-dir",
        type=Path,
        help="扫描哪个目录下的 *.txt 作为目标字符集，默认 --base 所在目录",
    )
    parser.add_argument(
        "--dictionary", type=Path, help="可选：从 msime.db 的 value 列补字符集"
    )
    parser.add_argument(
        "--version", default="", help="校验上游 radical_pinyin.dict.yaml 的版本"
    )
    args = parser.parse_args()

    version, pinyin = load_pinyin(args.pinyin)
    if not version:
        print("could not read upstream version from the pinyin table", file=sys.stderr)
        return 1
    if args.version and version != args.version:
        print(
            f"upstream version mismatch: expected {args.version}, got {version}",
            file=sys.stderr,
        )
        return 1
    if getattr(pypinyin, "__version__", "") != PYPINYIN_VERSION:
        print(
            f"pypinyin version mismatch: expected {PYPINYIN_VERSION}, got {getattr(pypinyin, '__version__', 'unknown')}",
            file=sys.stderr,
        )
        return 1

    base = load_base(args.base)
    output = args.output or args.base
    generator = Generator(load_radicals(args.radicals), pinyin)

    excluded = {args.base.resolve(), output.resolve()}
    reference_dir = args.reference_dir or args.base.parent
    target = set(base) | gbk_han_chars() | reference_chars(reference_dir, excluded)
    if args.dictionary:
        target |= dictionary_chars(args.dictionary)
    target = {char for char in target if is_han(char)}

    codes: dict[str, str] = {}
    failed: list[str] = []
    for char in sorted(target - set(base), reverse=True):
        code = generate(char, generator, set(), base)
        if code:
            codes[char] = code
        else:
            failed.append(char)

    merged = dict(base)
    merged.update(codes)
    output.write_text(
        "\n".join(f"{char}={merged[char]}" for char in sorted(merged, reverse=True))
        + "\n",
        encoding="utf-8",
        # 仓里这份表是 LF，且 .gitattributes 钉了 eol=lf；不写死换行符时 Windows 上会写出 CRLF。
        newline="\n",
    )

    print(
        f"base {len(base)}, added {len(codes)}, total {len(merged)}, skipped {len(failed)} (upstream {version})"
    )
    if failed:
        print("no decomposition data: " + "".join(failed))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
