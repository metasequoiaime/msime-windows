"""msime.db：全拼表、自定义词条、读音纠错、86 五笔、空的快捷短语表（出厂不带内置快捷短语），以及日文直出候选表 japanese_lexicon。

对应 Rust 构建器 msime.rs 的 quanpin / custom-words / reading-corrections / wubi / quick-phrases 阶段，但全拼表
不再读 sources/pinyin/ 下的 rime-ice 系列与地名补充，只读 sources/pinyin/wanxiang/ 的万象词库。Release 把五笔
拆进了 msime-wubi.db，Windows 端仍然一个库：这里全建在 msime.db 里。98 五笔 Windows 端不读，不建。
"""

from __future__ import annotations

import importlib.util
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

WANXIANG = "sources/pinyin/wanxiang"
# 没有权重列（fangyan、mixed 整份都没有）或权重列不是整数的词条取这个权重。
WANXIANG_DEFAULT_WEIGHT = 1


def wanxiang_files(directory: Path) -> list[Path]:
    paths = sorted(directory.glob("*.dict.yaml"))
    if not paths:
        raise ValueError(f"{directory} 下没有 *.dict.yaml")
    return paths


def build_quanpin(connection: sqlite3.Connection, paths: list[Path]) -> str:
    """万象词库按给定顺序读入；同一 (拼音, 词) 只留一行：权重高的，平手留先读到的。"""
    with connection:
        for table in QUANPIN_TABLES:
            connection.execute(f"\ndrop table if exists {table};\n")
            connection.execute(CREATE_QUANPIN_TABLE.format(table))
        count = defaulted = skipped = 0
        for path in paths:
            rows, file_defaulted, file_skipped = read_wanxiang(path)
            _insert_quanpin_rows(connection, rows)
            count += len(rows)
            defaulted += file_defaulted
            skipped += file_skipped
        for table in QUANPIN_TABLES:
            suffix = table.removeprefix("tbl_")
            connection.execute(f"\ncreate index idx_key_{suffix} on {table}(key);\n")
            connection.execute(f"\ncreate index idx_jp_{suffix} on {table}(jp);\n")
            count -= connection.execute(
                f"\ndelete from {table} where exists (select 1 from {table} as kept where kept.key = {table}.key "
                f"and kept.value = {table}.value and (kept.weight > {table}.weight or "
                f"(kept.weight = {table}.weight and kept.rowid < {table}.rowid)));\n").rowcount
    return (f"{count} rows from {len(paths)} files, {defaulted} without a weight set to {WANXIANG_DEFAULT_WEIGHT}, "
            f"{skipped} readings that are not quanpin left out")


def _insert_quanpin_rows(connection: sqlite3.Connection, rows: list[tuple[str, str, int]]) -> None:
    by_table: dict[str, list[tuple[str, str, str, int]]] = {}
    for value, key, weight in rows:
        by_table.setdefault(pinyin_table(key), []).append((key, jianpin(key), value, weight))
    for table, table_rows in by_table.items():
        connection.executemany(f"\ninsert into {table} (\n    key,\n    jp,\n    value,\n    weight\n) values (?, ?, ?, ?);\n",
                               table_rows)


# 万象词库把嗯、呣记成 n、ng、m，靠方案的拼写运算（wanxiang_algebra.yaml 的 xform/^ng(\d)/eng$1/、
# xform/^n(\d)/en$1/、xform/^m(\d)/me$1/）改写成 eng、en、me，双拼才打得出来。引擎没有拼写运算，入库时照做。
_ZERO_INITIAL = {"n": "en", "ng": "eng", "m": "me"}


def _toneless():
    try:
        from pypinyin.contrib.tone_convert import to_normal
    except ImportError as error:
        raise SystemExit("读万象词库需要 pypinyin：python -m pip install pypinyin") from error
    cache: dict[str, str] = {}

    def convert(syllable: str) -> str:
        # 音节种类有限，按音节缓存；ü 写成 v（lǜ -> lv，lüè -> lve），和原先的全拼表一致。
        plain = cache.get(syllable)
        if plain is None:
            plain = cache[syllable] = to_normal(syllable)
        return plain

    return convert


def read_wanxiang(path: Path) -> tuple[list[tuple[str, str, int]], int, int]:
    """Rime 的 `.dict.yaml`：`...` 之后是 `词<TAB>带调拼音（空格分隔）[<TAB>权重[<TAB>...]]`。

    拼音用 pypinyin 去掉声调，音节改用 `'` 连接。返回 ([(词, 全拼, 权重)], 补了默认权重的行数, 跳过的行数)；
    跳过的是没有读音列、或去调后不是全拼的读音（ê、mixed 里的 `A'ban`、`2B` 这类）。
    """
    convert = _toneless()
    rows: list[tuple[str, str, int]] = []
    defaulted = skipped = 0
    in_body = False
    for line in textutil.universal_lines(textutil.without_bom(textutil.read(path))):
        if not in_body:
            in_body = line.strip() == "..."
            continue
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        fields = line.split("\t")
        value = fields[0].strip()
        reading = fields[1].strip() if len(fields) > 1 else ""
        if not value or not reading:
            skipped += 1
            continue
        syllables = reading.replace("'", " ").split()
        # to_normal 把数字当声调去掉（mixed 的 `7z` 会变成 `z`），带数字的读音先挡掉。
        plain = [convert(syllable) for syllable in syllables] if all(s.isalpha() for s in syllables) else [""]
        key = "'".join(_ZERO_INITIAL.get(syllable, syllable) for syllable in plain)
        if not is_quanpin(key):
            skipped += 1
            continue
        weight = textutil.parse_int(fields[2].strip()) if len(fields) > 2 else None
        if weight is None:
            weight = WANXIANG_DEFAULT_WEIGHT
            defaulted += 1
        rows.append((value, key, weight))
        # 单字另留原读音，全拼打 n、ng 仍出嗯。
        if len(plain) == 1 and plain[0] != key and is_quanpin(plain[0]):
            rows.append((value, plain[0], weight))
    if not in_body:
        raise ValueError(f"{path}: 没有 `...`，不是 Rime 词库")
    return rows, defaulted, skipped


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
