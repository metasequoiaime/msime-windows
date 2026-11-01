"""others.db 的符号部分：表情面板的 symbol_catalog 与 S 模式、符号混输查的 symbol_code。

源数据在 msime-dictionary 的 sources/symbols/：
    wanxiang_symbols.yaml  rime-wanxiang 的符号表，取 symbols: 下 `'/编码': [符号, ...]` 分组
    codex_sym.txt          rime-wanxiang 从 typst/codex 生成的 `名称<TAB>符号`
    wanxiang-unrenderable.txt  Windows 自带字体显示不了的符号（scripts/check-symbol-rendering.py 生成），一律不入库
    symbol-names.tsv       msime 的打拼音出符号名称表：`名称<TAB>全拼<TAB>符号[<空格>符号...]`（逗号 → ，,，书名号 → 《》〈〉），
                           万象只按分组码收标点，标点和数学符号的拼音读音从这里来

万象表里不是符号的分组不收：Emoji 分类（E 模式和表情面板已有 emoji）、元素周期表（元素信息文本）、
XCompose 合成（编码含标点，S 模式只能输入字母）。
"""

from __future__ import annotations

import re
import sqlite3
from dataclasses import dataclass

from . import textutil
from .others_db import Pinyin

WANXIANG = "sources/symbols/wanxiang_symbols.yaml"
CODEX = "sources/symbols/codex_sym.txt"
UNRENDERABLE = "sources/symbols/wanxiang-unrenderable.txt"
READINGS = "sources/symbols/symbol-names.tsv"

_GROUP_LINE = re.compile(r"""^ {4}(['"])/(.*?)\1:\s*\[(.*)\]\s*(?:#.*)?$""")
_COMMENT_LINE = re.compile(r"^ {4}#\s*(.*?)\s*$")
_CODE = re.compile(r"[A-Za-z]+\Z")
_CODEPOINT = re.compile(r"U\+([0-9A-F]{4,6})\Z")

# 万象用数字作编码的「同一个数的各种写法」：S 模式只能输入字母，改用读音作编码。
DIGIT_CODES = {"0": "ling", "1": "yi", "2": "er", "3": "san", "4": "si", "5": "wu", "6": "liu", "7": "qi", "8": "ba",
               "9": "jiu", "10": "shi"}# Dingbats 区的无衬线带圈数字 ➀–➉、➊–➓ 和 ①–⑩、❶–❿ 在候选窗里看不出区别，万象两套都收了，
# 列在一起就像同一个符号出现了两次，所以不收。
_VISUAL_DUPLICATES = ((0x2780, 0x2793),)
# 数字别名混进普通候选时只取序号写法：带圈 ①–⑩、带括号 ⑴–⑽、带点 ⒈–⒑、带圈零 ⓪。
_SEQUENCE_NUMBERS = ((0x2460, 0x2469), (0x2474, 0x247D), (0x2488, 0x2491), (0x24EA, 0x24EA))
# 单音节中文名称默认不收（见 parse_readings），这里列出的照收，接受它们在混输时紧跟首选：打 dian 出间隔号 ·，
# 以及 #928 列出的运算、方位与图形单字（jia +、cheng ×、du °、zuo ←、yuan ○ 等）。
SINGLE_SYLLABLE_NAMES = frozenset({
    "点", "度", "加", "正", "乘", "叉", "错", "角", "分", "秒", "左", "右", "上", "下", "圆", "圈",
})

# 万象的 /a…/z 里也收了数学字母（𝐚 𝑎 𝒂…），面板的「拉丁扩展」不重复列出，它们在「数学字母」标签页。
_MATH_ALPHANUMERIC = (0x1D400, 0x1D7FF)
_LATIN = tuple(code for letter in "abcdefghijklmnopqrstuvwxyz" for code in (letter, letter.upper()))
_MATH_STYLES = (("粗体", "sxc"), ("斜体", "sxx"), ("粗斜体", "sxz"), ("花体", "sxh"), ("花体粗体", "sxhc"),
                ("哥特体", "sxg"), ("哥特粗体", "sxgc"), ("双线体", "sxs"), ("无衬线体", "sxw"), ("无衬线粗体", "sxwc"),
                ("无衬线斜体", "sxwx"), ("无衬线粗斜体", "sxwz"), ("等宽体", "sxd"), ("希腊粗体", "xlc"),
                ("希腊斜体", "xlx"), ("希腊粗斜体", "xlz"), ("希腊无衬线粗体", "xlwc"), ("希腊无衬线粗斜体", "xlwz"))

# 表情面板符号页：父标签（面板上显示为图标，取第一个符号）→ 小标题 → 万象分组编码。没列出的分组只供 S 模式输入。
PANEL_TABS = (
    ("数学", (("数学符号", ("sx",)), ("大于小于", ("dy", "xy")), ("因为所以", ("yw", "sy")), ("上标", ("sb",)),
              ("下标", ("xb",)))),
    ("序号", (("带圈数字", ("szq",)), ("带括号数字", ("szh",)), ("带点数字", ("szd",)), ("罗马数字", ("lmd", "lm")),
              ("带圈字母", ("zmq",)), ("带括号字母", ("zmh",)), ("带圈汉字", ("hzq",)), ("带括号汉字", ("hzh",)),
              ("苏州码", ("szm",)), ("编号", ("num",)))),
    ("字母", (("希腊字母", ("xl", "xld")), ("俄文字母", ("ey", "eyd")), ("拼音", ("py", "pyd")), ("声调", ("sd",)),
              ("注音符号", ("zy",)), ("英文音标", ("yb",)), ("拉丁扩展", _LATIN))),
    ("数学字母", tuple((name, (code,)) for name, code in _MATH_STYLES)),
    ("单位", (("单位", ("dw",)), ("货币", ("hb",)), ("月份", ("yf",)), ("日期", ("rf",)), ("时间", ("sj",)))),
    ("箭头与图形", (("箭头", ("jt",)), ("几何图形", ("jh",)), ("方块", ("fk",)), ("五角星", ("wjx",)), ("星号", ("xh",)),
                    ("圆点", ("yd",)), ("叉号", ("ih",)))),
    ("标点", (("点号", ("dd",)), ("非夹注符号", ("fj",)), ("夹注符号", ("jz",)), ("行间符号", ("hj",)),
              ("常用标点", ("bd",)), ("竖排标点", ("bdz",)), ("中英标点", ("bdzy",)))),
    ("杂项", (("常用符号", ("fh", "tsfh")), ("电脑按键", ("dn", "aj")), ("象棋", ("xq",)), ("麻将", ("mj",)),
              ("骰子", ("sz",)), ("扑克", ("pk",)), ("音乐", ("yy",)), ("性别", ("lx",)), ("天体", ("tt",)),
              ("星座", ("xz",)), ("六十四卦", ("lssg",)), ("太玄经", ("txj",)))),
    ("汉字部件", (("汉字结构", ("jg",)), ("偏旁", ("pp",)), ("康熙部首", ("kx",)), ("笔画", ("bh",)))),
    ("假名与韩文", (("平假名", ("jm",)), ("片假名", ("pjm",)), ("带圈假名", ("jmq",)), ("半角假名", ("jmbj",)),
                    ("韩文字母", ("hw",)), ("带圈韩文", ("hwq",)), ("带括号韩文", ("hwh",)))),
)


@dataclass
class Group:
    code: str
    items: list[str]


def _flow_items(text: str, where: str) -> list[str]:
    """YAML 行内序列 `a, 'b', "c"`：单引号里 '' 表示一个单引号，双引号里认 \\" 与 \\\\。"""
    items: list[str] = []
    index, length = 0, len(text)
    while index < length:
        while index < length and text[index] == " ":
            index += 1
        if index >= length:
            break
        quote = text[index]
        if quote in "'\"":
            index += 1
            value = []
            while True:
                if index >= length:
                    raise ValueError(f"{where}: unterminated quote")
                char = text[index]
                if quote == "'" and char == "'":
                    if index + 1 < length and text[index + 1] == "'":
                        value.append("'")
                        index += 2
                        continue
                    index += 1
                    break
                if quote == '"' and char == "\\" and index + 1 < length:
                    value.append(text[index + 1])
                    index += 2
                    continue
                if quote == '"' and char == '"':
                    index += 1
                    break
                value.append(char)
                index += 1
            items.append("".join(value))
            while index < length and text[index] == " ":
                index += 1
            if index < length and text[index] != ",":
                raise ValueError(f"{where}: expected ',' after quoted item")
            index += 1
        else:
            end = text.find(",", index)
            end = length if end < 0 else end
            value = text[index:end].strip()
            if value:
                items.append(value)
            index = end + 1
    return items


def parse_wanxiang(text: str) -> list[Group]:
    """symbols: 下的符号分组，按文件顺序。分段注释（Emoji 分类、元素周期表、XCompose）决定哪些分组不收。"""
    lines = textutil.universal_lines(textutil.without_bom(text))
    try:
        start = lines.index("symbols:") + 1
    except ValueError as error:
        raise ValueError(f"{WANXIANG}: no top-level symbols: section") from error
    groups: list[Group] = []
    section = ""
    for number, line in enumerate(lines[start:], start=start + 1):
        match = _COMMENT_LINE.match(line)
        if match:
            for marker, name in (("Emoji 分类", "emoji"), ("元素周期表", "elements"), ("XCompose", "xcompose")):
                if match.group(1).startswith(marker):
                    section = name
            continue
        match = _GROUP_LINE.match(line)
        if not match:
            continue
        code = match.group(2)
        items = [item for item in _flow_items(match.group(3), f"{WANXIANG}:{number}") if not _visual_duplicate(item)]
        if not items or section in ("elements", "xcompose") or (section == "emoji" and code.startswith("e")):
            continue
        groups.append(Group(code, items))
    return groups


def _visual_duplicate(symbol: str) -> bool:
    return any(low <= ord(char) <= high for char in symbol for low, high in _VISUAL_DUPLICATES)


def parse_codex(text: str) -> list[tuple[str, str]]:
    """`名称<TAB>符号`；名称是 typst 的点分路径，如 arrow.r.double。"""
    entries = []
    for number, line in enumerate(textutil.universal_lines(textutil.without_bom(text)), start=1):
        if not line or line.startswith("#"):
            continue
        name, separator, symbol = line.partition("\t")
        if not separator or not name or not symbol:
            raise ValueError(f"{CODEX}:{number}: expected name<TAB>symbol")
        if not _visual_duplicate(symbol):
            entries.append((name, symbol))
    return entries


def parse_unrenderable(text: str) -> set[str]:
    """首列是空格分隔的码位（U+1F1E8 U+1F1F3），因为不可见字符没法直接写在行里。"""
    symbols = set()
    for number, line in enumerate(textutil.universal_lines(textutil.without_bom(text)), start=1):
        if not line or line.startswith("#"):
            continue
        codepoints = line.split("\t", 1)[0].split(" ")
        try:
            symbols.add("".join(chr(int(_CODEPOINT.match(cp).group(1), 16)) for cp in codepoints))
        except AttributeError as error:
            raise ValueError(f"{UNRENDERABLE}:{number}: expected U+XXXX code points") from error
    return symbols


def parse_readings(text: str) -> list[tuple[str, str]]:
    """symbol-names.tsv 的 `(编码, 符号)`，按文件顺序；同一编码的多行混输时第一行紧跟首选。

    中文名称的编码是全拼去掉 `'`（逗号 dou'hao → douhao），拉丁名称（拼音列是 `-`）的编码就是名称本身（gamma）。
    单音节的中文名称（派 pai、度 du、套 tao、一 yi）不收：msime 引擎只让它们跟在那个字后面、最早排第 4 位，
    这里的混输把第一行放在首选后面，收进来就会在打常用音节时冒出符号；数字序号仍由万象分组与 DIGIT_CODES 给出。
    SINGLE_SYLLABLE_NAMES 里的名称（点 dian → ·、加 jia → ＋ 等）例外。
    """
    entries = []
    for number, line in enumerate(textutil.universal_lines(textutil.without_bom(text)), start=1):
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 3 or not fields[0] or not fields[2]:
            raise ValueError(f"{READINGS}:{number}: expected name<TAB>pinyin<TAB>symbols")
        name, pinyin, symbols = fields
        if pinyin == "-":
            code = name
        else:
            syllables = pinyin.split("'")
            if len(syllables) == 1 and name not in SINGLE_SYLLABLE_NAMES:
                continue
            code = "".join(syllables)
        if not re.fullmatch(r"[a-z]+", code):
            raise ValueError(f"{READINGS}:{number}: expected a lowercase pinyin or Latin name")
        entries.extend((code, symbol) for symbol in symbols.split(" "))
    return entries


def input_code(code: str) -> str | None:
    """S 模式能输入的编码：只含字母，统一小写（万象的 /a 与 /A 合成一个 a）；数字编码改用读音。"""
    if _CODE.match(code):
        return code.lower()
    return DIGIT_CODES.get(code)


def code_rows(groups: list[Group], readings: list[tuple[str, str]], codex: list[tuple[str, str]],
              excluded: set[str]) -> list[tuple]:
    """symbol_code 的行：(编码, 符号, sort_order, 可混输)。万象分组在前、按文件顺序，然后是 symbol-names.tsv 的拼音读音，
    codex 名称在后。

    可混输：符号混输只在普通拼音候选里认整串精确匹配，所以读得成拼音的编码（douhao、xi）也照样混进来，
    只有单字母编码不混（混输从两个字母起才查）。数字别名（yi、er）只混序号写法（① ⑴ ⒈ ⓪），
    一、壹、¹ 这些仍只在 S 模式里出。
    """
    rows, seen = [], set()

    def add(code: str, symbol: str, digit: bool = False) -> None:
        if symbol in excluded or (code, symbol) in seen:
            return
        seen.add((code, symbol))
        if digit:
            mixed = 1 if len(symbol) == 1 and any(low <= ord(symbol) <= high for low, high in _SEQUENCE_NUMBERS) else 0
        else:
            mixed = 1 if len(code) >= 2 else 0
        rows.append((code, symbol, len(rows), mixed))

    for group in groups:
        code = input_code(group.code)
        if code is None:
            continue
        for symbol in group.items:
            add(code, symbol, digit=group.code in DIGIT_CODES)
    for code, symbol in readings:
        add(code, symbol)
    for name, symbol in codex:
        code = name.replace(".", "").lower()
        if _CODE.match(code):
            add(code, symbol)
    return rows


def catalog_rows(pinyin: Pinyin, groups: list[Group], codex: list[tuple[str, str]], excluded: set[str]) -> list[tuple]:
    """symbol_catalog 的行：(符号, 小标题, 父标签, sort_order, 关键词)，同一父标签、同一小标题的行连续。

    关键词给面板搜索用：小标题、父标签和它们的全拼与首字母，加上这个符号所在的全部万象编码与 codex 名称。
    """
    by_code = {group.code: group for group in groups}
    search: dict[str, list[str]] = {}
    for group in groups:
        code = input_code(group.code)
        for symbol in group.items:
            if code:
                search.setdefault(symbol, []).append(code)
    for name, symbol in codex:
        search.setdefault(symbol, []).append(name)

    def spelled(text: str) -> list[str]:
        full, initials = pinyin.full_and_initials(text)
        return [full, initials] if initials != full else [full]

    rows = []
    for parent, headings in PANEL_TABS:
        for heading, codes in headings:
            missing = [code for code in codes if code not in by_code]
            if missing:
                raise ValueError(f"{WANXIANG}: panel heading {heading!r} names missing groups {missing}")
            base = [heading, *spelled(heading), parent, *spelled(parent)]
            seen = set()
            for code in codes:
                for symbol in by_code[code].items:
                    if symbol in excluded or symbol in seen:
                        continue
                    if heading == "拉丁扩展" and any(_MATH_ALPHANUMERIC[0] <= ord(c) <= _MATH_ALPHANUMERIC[1] for c in symbol):
                        continue
                    seen.add(symbol)
                    keywords = " ".join(dict.fromkeys([*base, *search.get(symbol, [])]))
                    rows.append((symbol, heading, parent, len(rows), keywords))
    return rows


def build(connection: sqlite3.Connection, catalog: list[tuple], codes: list[tuple]) -> None:
    with connection:
        connection.execute("DROP TABLE IF EXISTS symbol_catalog")
        connection.execute("\n            CREATE TABLE symbol_catalog (\n                symbol TEXT NOT NULL,\n"
                           "                category TEXT NOT NULL,\n                parent_category TEXT NOT NULL,\n"
                           "                sort_order INTEGER NOT NULL,\n                keywords TEXT NOT NULL,\n"
                           "                PRIMARY KEY (symbol, category)\n            ) WITHOUT ROWID\n            ")
        connection.execute("CREATE INDEX idx_symbol_catalog_parent_order ON symbol_catalog(parent_category, sort_order)")
        connection.executemany("INSERT INTO symbol_catalog (symbol, category, parent_category, sort_order, keywords) "
                               "VALUES (?, ?, ?, ?, ?)", catalog)
        connection.execute("DROP TABLE IF EXISTS symbol_code")
        connection.execute("\n            CREATE TABLE symbol_code (\n                code TEXT NOT NULL,\n"
                           "                symbol TEXT NOT NULL,\n                sort_order INTEGER NOT NULL,\n"
                           "                mixed INTEGER NOT NULL,\n"
                           "                PRIMARY KEY (code, symbol)\n            ) WITHOUT ROWID\n            ")
        connection.executemany("INSERT INTO symbol_code (code, symbol, sort_order, mixed) VALUES (?, ?, ?, ?)", codes)
        textutil.analyze(connection, optimize=False)
