"""msime.db：全拼表、自定义词条、读音纠错、86 五笔、空的快捷短语表（出厂不带内置快捷短语），以及日文直出候选表 japanese_lexicon。

对应 Rust 构建器 msime.rs 的 quanpin / custom-words / reading-corrections / wubi / quick-phrases 阶段，但全拼表
不再读 sources/pinyin/ 下的 rime-ice 系列与地名补充，只读 sources/pinyin-next/ 整理好的拼音基础词库（万象加上
rime-ice 有而万象没有的词）。Release 把五笔
拆进了 msime-wubi.db，Windows 端仍然一个库：这里全建在 msime.db 里。98 五笔 Windows 端不读，不建。
"""

from __future__ import annotations

import importlib.util
import re
import sqlite3
from dataclasses import dataclass
from pathlib import Path

from . import textutil

_ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("dictionary_format", _ROOT / "engine/contracts/dictionary/format.py")
_format = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_format)

QUANPIN_TABLES: list[str] = _format.quanpin_tables()
_SHIPPED = frozenset(QUANPIN_TABLES)

# 建表语句按 Python 时代的原文保留：SQLite 把它存进 sqlite_master，表结构本身也是要复现的内容。
CREATE_QUANPIN_TABLE = (
    '\ncreate table if not exists {} (\n   "key" text, -- 全拼拼音\n   "jp" text, -- 全拼简拼\n'
    '   "value" text, -- 对应的汉字或者词组\n   "weight" integer default 0 -- 权重\n);\n'
)
CREATE_WUBI86 = (
    '\n            CREATE TABLE wubi86 (\n                "key" TEXT NOT NULL,\n                "value" TEXT NOT NULL,\n'
    '                "weight" INTEGER NOT NULL DEFAULT 0,\n                UNIQUE("key", "value")\n            )\n            '
)
WUBI86_INDEXES = (
    'CREATE INDEX idx_wubi86_key_weight ON wubi86("key", "weight" DESC)',
    # 按词条反查五笔编码用。
    'CREATE INDEX idx_wubi86_value ON wubi86("value")',
)
CREATE_QUICK_PHRASES = (
    '\n            CREATE TABLE quick_parases (\n                "key" TEXT NOT NULL,\n                "value" TEXT NOT NULL,\n'
    '                "weight" INTEGER NOT NULL DEFAULT 0,\n                UNIQUE("key", "value")\n            )\n            '
)
QUICK_PHRASE_INDEXES = ('CREATE INDEX idx_quick_parases_key_weight ON quick_parases("key", "weight" DESC)',)


def pinyin_table(key: str) -> str | None:
    table = _format.pinyin_table(key)
    return table if table in _SHIPPED else None


def jianpin(key: str) -> str:
    syllables = key.split("'")
    if any(not syllable for syllable in syllables):
        raise ValueError(f"{key!r} has an empty syllable")
    return "".join(syllable[0] for syllable in syllables)


def is_quanpin(key: str) -> bool:
    return all(s and s.isascii() and s.isalpha() and s.islower() for s in key.split("'")) and pinyin_table(key) is not None


# ---- 全拼表 ----

# msime-dictionary 整理好的拼音基础词库：万象全部词表（含已从上游删掉的 lianxiang、renming 里的词）加上 rime-ice
# 有而万象没有的词（权重统一为 1），读音已去调、ü 记为 v、音节以 ' 连接。按字数拆成 1-chars.txt … 8plus-chars.txt，
# 含字母的词另放 letters.txt。每行 `词<TAB>全拼<TAB>权重`，`#` 开头是注释。
# msime-dictionary#148 起这些基础词表在 sources/pinyin-next/base/，旁边的 packs/ 是按需导入的专业词库，不进构建；
# 更早的提交直接放在 sources/pinyin-next/ 下，照旧能重建。
PINYIN_NEXT = "sources/pinyin-next"
PINYIN_NEXT_BASE = f"{PINYIN_NEXT}/base"
PINYIN_NEXT_LETTERS = "letters.txt"


def pinyin_next_base(dictionary: Path) -> str:
    """基础词表所在目录（相对 msime-dictionary 根）：有 base/ 用 base/，否则是旧布局。"""
    return PINYIN_NEXT_BASE if (dictionary / PINYIN_NEXT_BASE).is_dir() else PINYIN_NEXT


def pinyin_next_files(directory: Path) -> list[Path]:
    """全拼表读的文件：按字数拆的 *-chars.txt，再加 letters.txt——其中读音恰好是小写字母串的（查bug 的 cha'bug、
    打call 的 da'call）照旧当全拼收，大写、数字读音的自然被跳过，只进字母词表。"""
    paths = sorted(directory.glob("*-chars.txt"))
    if not paths:
        raise ValueError(f"{directory} 下没有 *-chars.txt")
    letters = directory / PINYIN_NEXT_LETTERS
    if not letters.is_file():
        raise ValueError(f"{directory} 下没有 {PINYIN_NEXT_LETTERS}")
    return [*paths, letters]


def build_quanpin(connection: sqlite3.Connection, paths: list[Path]) -> str:
    """pinyin-next 按给定顺序读入；同一 (拼音, 词) 只留一行：权重高的，平手留先读到的。"""
    with connection:
        for table in QUANPIN_TABLES:
            connection.execute(f"\ndrop table if exists {table};\n")
            connection.execute(CREATE_QUANPIN_TABLE.format(table))
        count = skipped = 0
        for path in paths:
            rows, file_skipped = read_pinyin_next(path)
            _insert_quanpin_rows(connection, rows)
            count += len(rows)
            skipped += file_skipped
        for table in QUANPIN_TABLES:
            suffix = table.removeprefix("tbl_")
            connection.execute(f"\ncreate index idx_key_{suffix} on {table}(key);\n")
            connection.execute(f"\ncreate index idx_jp_{suffix} on {table}(jp);\n")
            count -= connection.execute(
                f"\ndelete from {table} where exists (select 1 from {table} as kept where kept.key = {table}.key "
                f"and kept.value = {table}.value and (kept.weight > {table}.weight or "
                f"(kept.weight = {table}.weight and kept.rowid < {table}.rowid)));\n").rowcount
    return f"{count} rows from {len(paths)} files, {skipped} readings that are not quanpin left out"


def _insert_quanpin_rows(connection: sqlite3.Connection, rows: list[tuple[str, str, int]]) -> None:
    by_table: dict[str, list[tuple[str, str, str, int]]] = {}
    for value, key, weight in rows:
        by_table.setdefault(pinyin_table(key), []).append((key, jianpin(key), value, weight))
    for table, table_rows in by_table.items():
        connection.executemany(f"\ninsert into {table} (\n    key,\n    jp,\n    value,\n    weight\n) values (?, ?, ?, ?);\n",
                               table_rows)


# 万象词库把嗯、呣记成 n、ng、m，靠方案的拼写运算（wanxiang_algebra.yaml 的 xform/^ng(\d)/eng$1/、
# xform/^n(\d)/en$1/、xform/^m(\d)/me$1/）改写成 eng、en、me，双拼才打得出来。pinyin-next 沿用万象的读音，
# 引擎没有拼写运算，入库时照做。
_ZERO_INITIAL = {"n": "en", "ng": "eng", "m": "me"}


def _pinyin_next_lines(path: Path):
    """逐行给出 (行号, 词, 读音, 权重)。格式错误直接失败。"""
    for number, line in enumerate(textutil.universal_lines(textutil.without_bom(textutil.read(path))), 1):
        if not line.strip() or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 3 or not fields[0] or not fields[1]:
            raise ValueError(f"{path.name}:{number}: 应为 词<TAB>全拼<TAB>权重")
        weight = textutil.parse_int(fields[2])
        if weight is None:
            raise ValueError(f"{path.name}:{number}: 权重 {fields[2]!r} 不是整数")
        yield number, fields[0], fields[1], weight


def read_pinyin_next(path: Path) -> tuple[list[tuple[str, str, int]], int]:
    """返回 ([(词, 全拼, 权重)], 跳过的行数)；跳过的是读音不是全拼的行（80式 的 `80'shi` 这类）。"""
    rows: list[tuple[str, str, int]] = []
    skipped = 0
    for _, value, reading, weight in _pinyin_next_lines(path):
        plain = reading.split("'")
        key = "'".join(_ZERO_INITIAL.get(syllable, syllable) for syllable in plain)
        if not is_quanpin(key):
            skipped += 1
            continue
        rows.append((value, key, weight))
        # 单字另留原读音，全拼打 n、ng 仍出嗯。
        if len(plain) == 1 and plain[0] != key and is_quanpin(plain[0]):
            rows.append((value, plain[0], weight))
    return rows, skipped


# ---- 字母词 ----

# letters.txt 里「拉丁字母 + 汉字」的词（T恤、AA制、卡拉OK）。全拼表不收（读音不是全拼），这里单独建表：
# 每个字母自成一段，T恤 记成 t'xu、卡拉OK 记成 ka'la'o'k，引擎按打出的字母串整串或前缀匹配，再交给词格组句。
# 不进 tbl_*：放进去简拼 tx 会冒出 T恤，i、u、v 开头的词也没有分表可放。
def latin_words_path(dictionary: Path) -> str:
    """letters.txt 相对 msime-dictionary 根的路径，跟着基础词表的目录走。"""
    return f"{pinyin_next_base(dictionary)}/{PINYIN_NEXT_LETTERS}"


CREATE_LATIN_WORDS = (
    '\n            CREATE TABLE latin_words (\n                "key" TEXT NOT NULL,\n                "value" TEXT NOT NULL,\n'
    '                "weight" INTEGER NOT NULL,\n                "exact_only" INTEGER NOT NULL DEFAULT 0,\n'
    '                PRIMARY KEY ("key", "value")\n            ) WITHOUT ROWID\n            '
)
_LATIN_WORD = re.compile(r"[A-Za-z一-鿿]+\Z")
_LATIN_RUN = re.compile(r"[A-Za-z]+")
# 车牌（川A、浙E）占了字母词的三成。收进来，但标成 exact_only：只在整串正好是它时单独出候选，不补全、
# 不进整句——浙E 的 zhe'e 全是完整音节，进了词格会混进普通整句（就浙E养），补全会让 zhe 冒出一串车牌。
_LICENSE_PLATE = re.compile(r"[京津沪渝冀豫云辽黑湘皖鲁新苏浙赣鄂桂甘晋蒙陕吉闽贵粤青藏川宁琼][A-Z]\Z")


def read_latin_words(path: Path) -> tuple[list[tuple[str, str, int, int]], int]:
    """letters.txt 里只含拉丁字母与基本区汉字、两样都有的词。返回 ([(键, 词, 权重, exact_only)], 跳过的行数)。

    读音里与词中字母串原样相同的段是字母（`ka'la'OK` 的 OK），其余是去调拼音、一个汉字一段。带数字、符号的词
    （3C认证、USB-C）和读音对不上字数的行不收；车牌收，exact_only 记 1。文件里的权重几乎都是 0：先按权重降序，
    同权重按文件顺序给递减的权重，同码的词保持文件里的先后。
    """
    entries: list[tuple[str, str, int, int]] = []
    skipped = 0
    for _, value, reading, weight in _pinyin_next_lines(path):
        if not _LATIN_WORD.match(value) or not _LATIN_RUN.search(value) or _LATIN_RUN.fullmatch(value):
            skipped += 1
            continue
        runs = _LATIN_RUN.findall(value)
        syllables: list[str] = []
        for segment in reading.split("'"):
            if runs and segment == runs[0]:
                runs.pop(0)
                continue
            syllables.append(_ZERO_INITIAL.get(segment, segment))
        hanzi = [c for c in value if not c.isascii()]
        if runs or len(syllables) != len(hanzi) or not all(is_quanpin(s) for s in syllables):
            skipped += 1
            continue
        readings = iter(syllables)
        key = "'".join(c.lower() if c.isascii() else next(readings) for c in value)
        entries.append((key, value, weight, 1 if _LICENSE_PLATE.match(value) else 0))
    ordered = sorted(range(len(entries)), key=lambda i: (-entries[i][2], i))
    rows, seen = [], set()
    for rank, index in enumerate(ordered):
        key, value, _, exact_only = entries[index]
        if (key, value) not in seen:
            seen.add((key, value))
            rows.append((key, value, len(entries) - rank, exact_only))
    return rows, skipped


def build_latin_words(connection: sqlite3.Connection, path: Path) -> str:
    rows, skipped = read_latin_words(path)
    with connection:
        connection.execute("DROP TABLE IF EXISTS latin_words")
        connection.execute(CREATE_LATIN_WORDS)
        connection.executemany(
            'INSERT INTO latin_words ("key", "value", "weight", "exact_only") VALUES (?, ?, ?, ?)', rows)
    plates = sum(row[3] for row in rows)
    return (f"{len(rows)} words ({plates} license plates offered only on an exact match), "
            f"{skipped} lines with digits, symbols or an unmatched reading left out")

# ---- 地名补充与自定义词条 ----

@dataclass
class Word:
    value: str
    key: str
    weight: int


def parse_word_list(text: str, name: str) -> list[Word]:
    """custom/words.txt 与 sources/pinyin/places.txt 的格式。格式错误直接失败；重复的 (拼音, 词) 留权重高的。"""
    entries: dict[tuple[str, str], Word] = {}
    for number, line in enumerate(text.splitlines(), start=1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        fields = stripped.split("\t")
        if len(fields) != 3:
            raise ValueError(f"{name}:{number}: expected word, pinyin and weight: {line!r}")
        value, key, weight_text = (field.strip() for field in fields)
        if not value:
            raise ValueError(f"{name}:{number}: the word is empty")
        if not all(s and s.isascii() and s.isalpha() and s.islower() for s in key.split("'")):
            raise ValueError(f"{name}:{number}: {key!r} is not quanpin separated by \"'\"")
        if pinyin_table(key) is None:
            raise ValueError(f"{name}:{number}: {key!r} maps to no quanpin table")
        weight = textutil.parse_int(weight_text)
        if weight is None:
            raise ValueError(f"{name}:{number}: weight {weight_text!r} is not an integer")
        if weight < 1:
            raise ValueError(f"{name}:{number}: weight {weight} is below 1")
        previous = entries.get((key, value))
        if previous is None or previous.weight < weight:
            entries[(key, value)] = Word(value, key, weight)
    return list(entries.values())


def apply_words(connection: sqlite3.Connection, words: list[Word]) -> tuple[int, int, int]:
    """只升不降：没有的插入，已有且权重更低的调高，已有且权重更高的不动。返回 (插入, 调高, 不变)。"""
    inserted = promoted = unchanged = 0
    with connection:
        for word in words:
            table = pinyin_table(word.key)
            jp = jianpin(word.key)
            row = connection.execute(f"select weight from {table} where key = ? and value = ?",
                                     (word.key, word.value)).fetchone()
            if row is None:
                connection.execute(f"insert into {table} (key, jp, value, weight) values (?, ?, ?, ?)",
                                   (word.key, jp, word.value, word.weight))
                inserted += 1
            elif row[0] < word.weight:
                connection.execute(f"update {table} set weight = ?, jp = ? where key = ? and value = ?",
                                   (word.weight, jp, word.key, word.value))
                promoted += 1
            else:
                unchanged += 1
    return inserted, promoted, unchanged


# ---- 读音纠错 ----

def parse_reading_corrections(text: str, name: str) -> list[tuple[str, str, str]]:
    """`词<TAB>错误拼音<TAB>正确拼音`。"""
    entries = []
    seen = set()
    for number, line in enumerate(text.splitlines(), start=1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        fields = [field.strip() for field in stripped.split("\t")]
        if len(fields) != 3:
            raise ValueError(f"{name}:{number}: expected word, wrong pinyin and correct pinyin: {line!r}")
        value, wrong, correct = fields
        if not value:
            raise ValueError(f"{name}:{number}: the word is empty")
        for key in (wrong, correct):
            if not is_quanpin(key):
                raise ValueError(f"{name}:{number}: {key!r} is not quanpin separated by \"'\"")
        if wrong == correct:
            raise ValueError(f"{name}:{number}: the wrong and correct readings are both {wrong!r}")
        if (value, wrong) in seen:
            raise ValueError(f"{name}:{number}: {value} {wrong} is listed twice")
        seen.add((value, wrong))
        entries.append((value, wrong, correct))
    return entries


def apply_reading_corrections(connection: sqlite3.Connection, entries: list[tuple[str, str, str]]) -> tuple[int, int, int]:
    """删掉错误读音的行。纠错表是对着 rime-ice 写的，换成万象后多数条目删不到行，跳过即可；
    词在正确读音下没有行时也不删，免得词打不出来。返回 (删掉的行, 删不到行的条目, 缺正确读音而保留的条目)。"""
    removed = stale = kept = 0
    with connection:
        for value, wrong, correct in entries:
            if not connection.execute(f"select exists(select 1 from {pinyin_table(wrong)} where key = ? and value = ?)",
                                      (wrong, value)).fetchone()[0]:
                stale += 1
                continue
            if not connection.execute(f"select exists(select 1 from {pinyin_table(correct)} where key = ? and value = ?)",
                                      (correct, value)).fetchone()[0]:
                kept += 1
                continue
            removed += connection.execute(f"delete from {pinyin_table(wrong)} where key = ? and value = ?",
                                          (wrong, value)).rowcount
    return removed, stale, kept


# ---- 编码表：86 五笔与快捷短语 ----

def _parse_code_line(line: str) -> tuple[str, str, int] | None:
    """五笔是 `词<TAB>编码<TAB>权重[...]`。无效行返回 None（计入跳过），和 Python 时代的导入器一样。"""
    if not line or line.startswith("#"):
        return None
    columns = line.split("\t")
    if len(columns) < 3:
        return None
    value, key, weight = columns[0], columns[1], columns[2]
    key = key.strip().lower()
    if not value or not textutil.is_ascii_alpha(key):
        return None
    parsed = textutil.parse_int(weight.strip())
    return (key, value, parsed) if parsed is not None and parsed >= 0 else None


def _write_code_table(connection: sqlite3.Connection, name: str, create: str, indexes, rows) -> tuple[int, int]:
    imported = skipped = 0
    with connection:
        connection.execute(f"DROP TABLE IF EXISTS {name}")
        connection.execute(create)
        for index in indexes:
            connection.execute(index)
        insert = (f'\nINSERT INTO {name} ("key", "value", "weight")\nVALUES (?, ?, ?)\nON CONFLICT("key", "value") '
                  f'DO UPDATE SET\n    "weight" = MAX("weight", excluded."weight")\n')
        for row in rows:
            if row is None:
                skipped += 1
                continue
            connection.execute(insert, row)
            imported += 1
    return imported, skipped


def _outside_basic_cjk(value: str) -> bool:
    """扩展 A 区和 BMP 之外的字：候选窗字体大多画不出来，86 极点表末尾的大字集全是这些，不收。"""
    return any(0x3400 <= ord(c) <= 0x4DBF or ord(c) >= 0x10000 for c in value)


def build_wubi86(connection: sqlite3.Connection, paths: list[Path]) -> tuple[int, int, int]:
    """极点 86 表在前、生成的补充表在后：同权重按 rowid 排，补充行留在原有行之后。返回 (导入, 跳过, 去掉的扩展区行)。"""
    outside = 0

    def rows():
        nonlocal outside
        for path in paths:
            for line in textutil.universal_lines(textutil.without_bom(textutil.read(path))):
                row = _parse_code_line(line)
                if row is not None and _outside_basic_cjk(row[1]):
                    outside += 1
                    continue
                yield row

    imported, skipped = _write_code_table(connection, "wubi86", CREATE_WUBI86, WUBI86_INDEXES, rows())
    return imported, skipped, outside


def build_quick_phrases(connection: sqlite3.Connection) -> None:
    """出厂不带任何快捷短语，只建空表：K 模式和设置页的用户快捷短语都写在这张表里，安装回放也往这里写。"""
    _write_code_table(connection, "quick_parases", CREATE_QUICK_PHRASES, QUICK_PHRASE_INDEXES, ())


# ---- 日文直出候选 ----

def read_jp_sela(path: Path) -> list[tuple[str, str, int]]:
    """rime-jp_sela 的 jp_sela.dict.yaml，解析与 MSIME-Engine 的 build_japanese_db.py 一致。"""
    entries = []
    in_body = False
    with path.open("r", encoding="utf-8-sig") as stream:
        for line_number, raw_line in enumerate(stream, start=1):
            line = raw_line.rstrip("\r\n")
            if not in_body:
                in_body = line.strip() == "..."
                continue
            if not line or line.lstrip().startswith("#"):
                continue
            columns = line.split("\t")
            if len(columns) < 2:
                continue
            value, code = columns[0].strip(), columns[1].strip()
            if not value or not code or any(ord(ch) > 127 for ch in code):
                continue
            try:
                weight = int(columns[2]) if len(columns) >= 3 else 1_000_000 - line_number
            except ValueError:
                weight = 1_000_000 - line_number
            entries.append((code, value, weight))
    return entries


def build_japanese_lexicon(connection: sqlite3.Connection, path: Path) -> int:
    entries = read_jp_sela(path)
    if not entries:
        raise ValueError(f"no Japanese entries parsed from {path}")
    with connection:
        connection.executescript("""
            DROP TABLE IF EXISTS japanese_lexicon;
            CREATE TABLE japanese_lexicon (
                code TEXT NOT NULL,
                value TEXT NOT NULL,
                weight INTEGER NOT NULL DEFAULT 0,
                PRIMARY KEY (code, value)
            );
        """)
        connection.executemany(
            "INSERT INTO japanese_lexicon(code, value, weight) VALUES (?, ?, ?) "
            "ON CONFLICT(code, value) DO UPDATE SET weight=MAX(weight, excluded.weight)", entries)
        connection.execute("CREATE INDEX idx_japanese_lexicon_code_weight ON japanese_lexicon(code, weight DESC)")
    return len(entries)
